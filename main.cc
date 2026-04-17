#include "buffer.hh"
#include "pipewire/properties.h"
#include "pipewire/stream.h"
#include "screencast_portal.hh"
#include "shm.hh"
#include "spa/utils/defs.h"
#include "util.hh"
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <sys/time.h>

#include <array>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

static std::atomic<bool> g_done = false;

struct VideoFrame {
    // FIXME: We could create an array that is the size of width*height*4 and
    // index into it globally to prevent over allocation
    std::array<uint8_t, 4096 * 4096 * 4> data;
    int w, h, stride;
    uint64_t ts; // timestamp
};

static std::vector<VideoFrame> g_v_frame_data;
static int g_v_frame_idx = 0;

struct AudioFrame {
    std::vector<uint8_t> data;
    uint32_t n_samples;
};

static RingBuffer<VideoFrame*, 16> g_video_frame_data;
static RingBuffer<AudioFrame, 256> g_audio_frame_data;

struct VideoRecordData {
    struct pw_main_loop* loop;
    struct pw_stream* stream;
    struct spa_video_info format;
};

static uint64_t g_time_scale_mul = 0;
static uint64_t first = 0;

// --------------------------------------------------------------
// ENCODING
// --------------------------------------------------------------
struct EncoderContext {
    // Video
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* cod_ctx = nullptr;
    AVStream* stream = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int64_t pts = 0;

    int64_t prev_frame_pts = 0;

    // Audio
    AVCodecContext* audio_cod_ctx = nullptr;
    AVStream* audio_stream = nullptr;
    AVFrame* audio_frame = nullptr;
    int64_t audio_pts = 0;
    SwrContext* swr_ctx = nullptr;

    void init(const char* output_path, int width, int height) {
        avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, output_path);

        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        stream = avformat_new_stream(fmt_ctx, codec);
        cod_ctx = avcodec_alloc_context3(codec);

        cod_ctx->width = round(width / 2) * 2;
        cod_ctx->height = round(height / 2) * 2;

        cod_ctx->time_base = (AVRational) { 1, 1000 };
        cod_ctx->framerate = (AVRational) { 60, 1 };
        g_time_scale_mul = (1000.0f / 60);

        cod_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        // keyframe
        cod_ctx->gop_size = 10;

        av_opt_set(cod_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(cod_ctx->priv_data, "tune", "zerolatency", 0);

        if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            cod_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        avcodec_open2(cod_ctx, codec, NULL);
        avcodec_parameters_from_context(stream->codecpar, cod_ctx);
        stream->time_base = cod_ctx->time_base;

        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = width;
        frame->height = height;
        av_frame_get_buffer(frame, 0);

        packet = av_packet_alloc();

        sws_ctx = sws_getContext(
            width, height, AV_PIX_FMT_BGRA, // FIXME: might need to be changed depending on the format coming in
            width,
            height,
            AV_PIX_FMT_YUV420P, // destination format
            SWS_BILINEAR,
            NULL,
            NULL,
            NULL);

        // AUDIO
        const AVCodec* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        audio_stream = avformat_new_stream(fmt_ctx, audio_codec);
        audio_cod_ctx = avcodec_alloc_context3(audio_codec);

        audio_cod_ctx->sample_rate = 48000;
        audio_cod_ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        audio_cod_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio_cod_ctx->bit_rate = 128000;
        audio_cod_ctx->time_base = { 1, 48000 };

        if (fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            audio_cod_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        avcodec_open2(audio_cod_ctx, audio_codec, NULL);
        avcodec_parameters_from_context(audio_stream->codecpar, audio_cod_ctx);
        audio_stream->time_base = audio_cod_ctx->time_base;

        // frame sized to the codec's required frame size
        audio_frame = av_frame_alloc();
        audio_frame->format = AV_SAMPLE_FMT_FLTP;
        audio_frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        audio_frame->sample_rate = 48000;
        audio_frame->nb_samples = audio_cod_ctx->frame_size;
        av_frame_get_buffer(audio_frame, 0);

        swr_alloc_set_opts2(&swr_ctx,
                            &audio_cod_ctx->ch_layout,
                            AV_SAMPLE_FMT_FLTP,
                            48000, // out
                            &audio_cod_ctx->ch_layout,
                            AV_SAMPLE_FMT_S16,
                            48000, // in
                            0,
                            NULL);
        swr_init(swr_ctx);

        avio_open(&fmt_ctx->pb, output_path, AVIO_FLAG_WRITE);
        avformat_write_header(fmt_ctx, NULL);
    }

    void submit_video(const VideoFrame* raw) {
        // convert BGR -> YUV420P
        const uint8_t* src_slices[] = { raw->data.data() };
        const int src_stride[] = { raw->stride };

        av_frame_make_writable(frame);
        sws_scale(sws_ctx,
                  src_slices,
                  src_stride,
                  0,
                  raw->h,
                  frame->data,
                  frame->linesize);

        frame->pts = pts++;

        avcodec_send_frame(cod_ctx, frame);

        // drain
        while (avcodec_receive_packet(cod_ctx, packet) == 0) {
            packet->pts = raw->ts * g_time_scale_mul;
            if (prev_frame_pts >= packet->pts) {
                av_packet_unref(packet);
                continue;
            }
            packet->dts = raw->ts * g_time_scale_mul;
            packet->duration = g_time_scale_mul;
            packet->stream_index = stream->index;
            av_interleaved_write_frame(fmt_ctx, packet);
            av_packet_unref(packet);
            prev_frame_pts = packet->pts;
        }
    }

    void submit_audio(const AudioFrame& raw) {
        // input: interleaved S16 pointer
        const uint8_t* in[] = { raw.data.data() };

        av_frame_make_writable(audio_frame);
        swr_convert(swr_ctx,
                    audio_frame->data,
                    audio_frame->nb_samples,
                    in,
                    raw.n_samples);

        audio_frame->pts = audio_pts;
        audio_pts += audio_frame->nb_samples;

        avcodec_send_frame(audio_cod_ctx, audio_frame);
        while (avcodec_receive_packet(audio_cod_ctx, packet) == 0) {
            av_packet_rescale_ts(packet,
                                 audio_cod_ctx->time_base,
                                 audio_stream->time_base);
            packet->stream_index = audio_stream->index;
            av_interleaved_write_frame(fmt_ctx, packet);
            av_packet_unref(packet);
        }
    }

    void close() {
        // flush video
        avcodec_send_frame(cod_ctx, NULL);
        while (avcodec_receive_packet(cod_ctx, packet) == 0) {
            av_packet_rescale_ts(packet, cod_ctx->time_base, stream->time_base);
            packet->stream_index = stream->index;
            av_interleaved_write_frame(fmt_ctx, packet);
            av_packet_unref(packet);
        }

        // flush audio
        avcodec_send_frame(audio_cod_ctx, NULL);
        while (avcodec_receive_packet(audio_cod_ctx, packet) == 0) {
            av_packet_rescale_ts(packet, audio_cod_ctx->time_base, audio_stream->time_base);
            packet->stream_index = audio_stream->index;
            av_interleaved_write_frame(fmt_ctx, packet);
            av_packet_unref(packet);
        }

        av_write_trailer(fmt_ctx);

        sws_freeContext(sws_ctx);
        swr_free(&swr_ctx);
        av_frame_free(&frame);
        av_frame_free(&audio_frame);
        av_packet_free(&packet);
        avcodec_free_context(&cod_ctx);
        avcodec_free_context(&audio_cod_ctx);
        avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
    }
};

static void on_process(void* userdata) {
    struct VideoRecordData* data = (struct VideoRecordData*)userdata;

    struct pw_buffer* b;
    if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
        pw_log_warn("out of buffers: %m");
        return;
    }

    if (b->buffer->datas[0].data == NULL) return;

    struct spa_buffer* buf = b->buffer;
    struct spa_data* d = &buf->datas[0];

    struct spa_meta_sync_timeline* synctimeline
        = (struct spa_meta_sync_timeline*)spa_buffer_find_meta_data(b->buffer, SPA_META_SyncTimeline, sizeof(struct spa_meta_sync_timeline));

    VideoFrame* frame = &g_v_frame_data[g_v_frame_idx];

    frame->w = data->format.info.raw.size.width;
    frame->h = data->format.info.raw.size.height;
    frame->stride = d->chunk->stride;
    // frame.data.assign((uint8_t*)d->data, (uint8_t*)d->data + d->chunk->size);
    memcpy(frame->data.data(), d->data, d->chunk->size);

    if (first == 0) first = get_timestamp_ms();
    frame->ts = get_timestamp_ms() - first;

    g_video_frame_data.write(&g_v_frame_data[g_v_frame_idx]);

    g_v_frame_idx += 1;
    g_v_frame_idx %= g_v_frame_data.size();

    pw_stream_queue_buffer(data->stream, b);
}

static void on_param_changed(void* userdata, uint32_t id, const struct spa_pod* param) {
    struct VideoRecordData* data = (struct VideoRecordData*)userdata;

    if (id == SPA_PARAM_EnumFormat) {
        // This shows what the portal CAN provide
        printf("available format from portal\n");
    }

    if (param == NULL || id != SPA_PARAM_Format)
        return;

    if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
        return;

    if (data->format.media_type != SPA_MEDIA_TYPE_video || data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    if (spa_format_video_raw_parse(param, &data->format.info.raw) < 0)
        return;

    printf("got video format:\n");
    printf("  format: %d (%s)\n", data->format.info.raw.format, spa_debug_type_find_name(spa_type_video_format, data->format.info.raw.format));
    printf("  size: %dx%d\n", data->format.info.raw.size.width, data->format.info.raw.size.height);
    printf("  framerate: %d/%d\n", data->format.info.raw.framerate.num, data->format.info.raw.framerate.denom);
}

static void on_state_changed(void* userdata, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        g_done = true;
    }

    printf("stream state: %s -> %s (error: %s)\n",
           pw_stream_state_as_string(old),
           pw_stream_state_as_string(state),
           error ? error : "none");
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static int next_power_of_two(int val) {
    unsigned int v;

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

// ----------------------------------------------
// AUDIO
// ----------------------------------------------

struct pw_stream* g_audio_stream = nullptr;

static void on_audio_process(void* userdata) {
    struct pw_buffer* b = pw_stream_dequeue_buffer(g_audio_stream);
    if (!b) return;

    struct spa_data* d = &b->buffer->datas[0];
    if (d->data && d->chunk->size > 0) {
        AudioFrame frame;
        frame.n_samples = d->chunk->size / (2 * sizeof(int16_t)); // stereo S16
        frame.data.assign(
            (uint8_t*)d->data,
            (uint8_t*)d->data + d->chunk->size);

        g_audio_frame_data.write(std::move(frame));
    }

    pw_stream_queue_buffer(g_audio_stream, b);
}

static void on_state_changed_audio(void* userdata, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    printf("audio state: %s -> %s (error: %s)\n",
           pw_stream_state_as_string(old),
           pw_stream_state_as_string(state),
           error ? error : "none");
}

struct pw_stream* audio_stream;
struct spa_audio_info audio_format;

static void on_param_changed_audio(void* _data, uint32_t id, const struct spa_pod* param) {
    /* NULL means to clear the format */
    if (param == NULL || id != SPA_PARAM_Format)
        return;

    if (spa_format_parse(param, &audio_format.media_type, &audio_format.media_subtype) < 0)
        return;

    /* only accept raw audio */
    if (audio_format.media_type != SPA_MEDIA_TYPE_audio || audio_format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    /* call a helper function to parse the format for us. */
    spa_format_audio_raw_parse(param, &audio_format.info.raw);

    fprintf(stdout, "capturing rate:%d channels:%d\n", audio_format.info.raw.rate, audio_format.info.raw.channels);
}

static const struct pw_stream_events audio_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed_audio,
    .param_changed = on_param_changed_audio,
    .process = on_audio_process,
};

const char* g_output_file_path = nullptr;

static void on_sigint(int dummy) {
    (void)dummy;
    g_done = true;
}

int main(int argc, char* argv[]) {

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printf("Usage: simple-sc [options]\n");
            printf("Options:\n");
            printf("  -h, --help    Show this help message\n");
            printf("  -o, --output  Change the output file path. (defaults to $HOME/Videos/[timestamp].mp4)\n");
            printf("  -s, --stop    Stops other instances of simple-sc that may be in progress.\n");
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "[ERR] -o/--output flag requires a path\n");
                return 1;
            }
            g_output_file_path = argv[i + 1];
            i++;
        } else if (arg == "-s" || arg == "--stop") {
            pid_t pid = get_other_instance_pid();
            printf("Stopping recording from pid: %d\n", pid);
            kill(pid, SIGINT);
            delete_handle();
            return 0;
        } else {
            fprintf(stderr, "[ERR] Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (handle_exists()) {
        fprintf(stderr, "[WARN] a screen recording is already in progress. Run with the -s/--stop flag to stop in progress recordings");
        return 1;
    }

    // add the signal handler
    signal(SIGINT, on_sigint);

    // opens the portal dialog
    ScreencastPortalData portal_data;
    auto status = create_screencast_portal(&portal_data);
    if (status == ScreencastPortalStatus::Error) {
        fprintf(stderr, "[ERR] Couldn't get access to org.freedesktop.portal.ScreenCast\n");
        return 1;
    } else if (status == ScreencastPortalStatus::Cancelled) {
        fprintf(stdout, "[WARN] ScreenCast cancelled\n");
        return 0;
    }

    g_v_frame_data.resize(16);
    struct VideoRecordData data = { 0 };

    const struct spa_pod* audio_params[1];
    uint8_t audio_buffer[1024];
    struct spa_pod_builder audio_b = SPA_POD_BUILDER_INIT(audio_buffer, sizeof(audio_buffer));

    const struct spa_pod* video_params[1];
    uint8_t video_buffer[1024];
    struct spa_pod_builder video_b = SPA_POD_BUILDER_INIT(video_buffer, sizeof(video_buffer));

    pw_init(NULL, NULL);

    data.loop = pw_main_loop_new(NULL);
    struct pw_main_loop* loop;
    struct pw_loop* pw_mainloop_loop = pw_main_loop_get_loop(data.loop);
    struct pw_context* context = pw_context_new(pw_mainloop_loop, NULL, 0);
    struct pw_core* core = pw_context_connect_fd(context, portal_data.fd, NULL, 0);

    struct pw_properties* video_props
        = pw_properties_new(PW_KEY_MEDIA_TYPE,
                            "Video",
                            PW_KEY_MEDIA_CATEGORY,
                            "Capture",
                            PW_KEY_MEDIA_ROLE,
                            "Screen",
                            NULL);

    data.stream = pw_stream_new(core, "pipewire-portal-screencast", video_props);

    struct spa_hook stream_listener;
    pw_stream_add_listener(data.stream, &stream_listener, &stream_events, &data);

    auto default_rect = SPA_RECTANGLE(320, 240);
    auto min_rect = SPA_RECTANGLE(1, 1);
    auto max_rect = SPA_RECTANGLE(4096, 4096);

    auto default_fps = SPA_FRACTION(25, 1);
    auto min_fps = SPA_FRACTION(0, 1);
    auto max_fps = SPA_FRACTION(1000, 1);

    video_params[0] = reinterpret_cast<spa_pod*>(spa_pod_builder_add_object(&video_b,
                                                                            SPA_TYPE_OBJECT_Format,
                                                                            SPA_PARAM_EnumFormat,
                                                                            SPA_FORMAT_mediaType,
                                                                            SPA_POD_Id(SPA_MEDIA_TYPE_video),
                                                                            SPA_FORMAT_mediaSubtype,
                                                                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                                                                            SPA_FORMAT_VIDEO_format,
                                                                            SPA_POD_CHOICE_ENUM_Id(6,
                                                                                                   SPA_VIDEO_FORMAT_RGB,
                                                                                                   SPA_VIDEO_FORMAT_BGR,
                                                                                                   SPA_VIDEO_FORMAT_RGBA,
                                                                                                   SPA_VIDEO_FORMAT_BGRA,
                                                                                                   SPA_VIDEO_FORMAT_RGBx,
                                                                                                   SPA_VIDEO_FORMAT_BGRx),
                                                                            SPA_FORMAT_VIDEO_size,
                                                                            SPA_POD_CHOICE_RANGE_Rectangle(
                                                                                &default_rect,
                                                                                &min_rect,
                                                                                &max_rect),
                                                                            SPA_FORMAT_VIDEO_framerate,
                                                                            SPA_POD_CHOICE_RANGE_Fraction(
                                                                                &default_fps,
                                                                                &min_fps,
                                                                                &max_fps)));

    // AUDIO
    struct pw_properties* audio_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,
        "Audio",
        PW_KEY_MEDIA_CATEGORY,
        "Capture",
        PW_KEY_MEDIA_ROLE,
        "Screen",
        PW_KEY_STREAM_CAPTURE_SINK,
        "true",
        NULL);

    g_audio_stream = pw_stream_new_simple(pw_main_loop_get_loop(data.loop), "audio-capture", audio_props, &audio_stream_events, nullptr);

    // g_audio_stream = pw_stream_new(core, "audio-capture", audio_props);
    // struct spa_hook listener;
    // pw_stream_add_listener(g_audio_stream, &listener, &audio_stream_events, nullptr);

    // request interleaved 16-bit stereo at 48kHz
    audio_params[0] = (const struct spa_pod*)spa_pod_builder_add_object(&audio_b,
                                                                        SPA_TYPE_OBJECT_Format,
                                                                        SPA_PARAM_EnumFormat,
                                                                        SPA_FORMAT_mediaType,
                                                                        SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                                                                        SPA_FORMAT_mediaSubtype,
                                                                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                                                                        SPA_FORMAT_AUDIO_format,
                                                                        SPA_POD_Id(SPA_AUDIO_FORMAT_S16),
                                                                        SPA_FORMAT_AUDIO_channels,
                                                                        SPA_POD_Int(2),
                                                                        SPA_FORMAT_AUDIO_rate,
                                                                        SPA_POD_Int(48000));

    if (pw_stream_connect(g_audio_stream,
                          PW_DIRECTION_INPUT,
                          PW_ID_ANY,
                          (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                          audio_params,
                          1)
        != 0) {
        printf("Couldn't connect to audio stream\n");
    }

    if (pw_stream_connect(data.stream,
                          PW_DIRECTION_INPUT,
                          portal_data.node_id,
                          (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                          video_params,
                          1)
        != 0) {
        printf("Couldn't connect to video stream\n");
    }

    std::thread render_thread(std::move([data]() {
        bool initialized = false;
        EncoderContext enc;
        while (!g_done) {
            while (!g_video_frame_data.empty()) {
                auto frame = g_video_frame_data.read();
                if (!initialized) {
                    if (!g_output_file_path) {
                        char* home = getenv("HOME");
                        enc.init((std::string(home) + "/Videos/" + make_date_time_string() + ".mp4").c_str(), frame->w, frame->h);
                    } else {
                        enc.init(g_output_file_path, frame->w, frame->h);
                    }
                    initialized = true;
                }
                enc.submit_video(frame);

                while (!g_audio_frame_data.empty()) {
                    auto frame = g_audio_frame_data.read();
                    enc.submit_audio(frame);
                }
            }
        }

        // cleanup
        enc.close();
        pw_main_loop_quit(data.loop);
        delete_handle();
    }));

    create_handle_with_pid();

    pw_main_loop_run(data.loop);

    // cleanup
    render_thread.join();
    pw_stream_destroy(data.stream);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(data.loop);
    delete_handle();

    return 0;
}
