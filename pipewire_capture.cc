#include "buffer.hh"
#include "pipewire/properties.h"
#include "pipewire/stream.h"
#include "spa/utils/defs.h"
#include <array>
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <string>
#include <sys/time.h>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

struct VideoFrame {
    std::array<uint8_t, 4096 * 4096 * 4> data;
    int w, h, stride;
    uint64_t ts;
};

static std::vector<VideoFrame> g_v_frame_data;
static int g_v_frame_idx = 0;

struct AudioFrame {
    std::vector<uint8_t> data;
    uint32_t n_samples;
};

RingBuffer<VideoFrame*, 16> g_video_frame_data;
RingBuffer<AudioFrame, 256> g_audio_frame_data;

struct VideoRecordData {
    struct pw_main_loop* loop;
    struct pw_stream* stream;
    struct spa_video_info format;
};

uint64_t g_time_scale_mul = 0;
uint64_t first = 0;

uint64_t get_timestamp_ms() {
    struct timeval tp;
    gettimeofday(&tp, NULL);
    return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

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

    if (first == 0) {
        first = get_timestamp_ms();
    }

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

static bool done = false;
static void on_state_changed(void* userdata, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        done = true;
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

    // Audio
    AVCodecContext* audio_cod_ctx = nullptr;
    AVStream* audio_stream = nullptr;
    AVFrame* audio_frame = nullptr;
    int64_t audio_pts = 0;
    SwrContext* swr_ctx = nullptr;
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

EncoderContext encoder_init(const char* output_path, int width, int height) {
    EncoderContext enc;

    avformat_alloc_output_context2(&enc.fmt_ctx, NULL, NULL, output_path);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    enc.stream = avformat_new_stream(enc.fmt_ctx, codec);
    enc.cod_ctx = avcodec_alloc_context3(codec);

    enc.cod_ctx->width = round(width / 2) * 2;
    enc.cod_ctx->height = round(height / 2) * 2;

    enc.cod_ctx->time_base = (AVRational) { 1, 1000 };
    enc.cod_ctx->framerate = (AVRational) { 60, 1 };
    g_time_scale_mul = (1000.0f / 60);

    enc.cod_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    // keyframe
    enc.cod_ctx->gop_size = 10;

    av_opt_set(enc.cod_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(enc.cod_ctx->priv_data, "tune", "zerolatency", 0);

    if (enc.fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc.cod_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    avcodec_open2(enc.cod_ctx, codec, NULL);
    avcodec_parameters_from_context(enc.stream->codecpar, enc.cod_ctx);
    enc.stream->time_base = enc.cod_ctx->time_base;

    enc.frame = av_frame_alloc();
    enc.frame->format = AV_PIX_FMT_YUV420P;
    enc.frame->width = width;
    enc.frame->height = height;
    av_frame_get_buffer(enc.frame, 0);

    enc.packet = av_packet_alloc();

    enc.sws_ctx = sws_getContext(
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
    enc.audio_stream = avformat_new_stream(enc.fmt_ctx, audio_codec);
    enc.audio_cod_ctx = avcodec_alloc_context3(audio_codec);

    enc.audio_cod_ctx->sample_rate = 48000;
    enc.audio_cod_ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    enc.audio_cod_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc.audio_cod_ctx->bit_rate = 128000;
    enc.audio_cod_ctx->time_base = { 1, 48000 };

    if (enc.fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        enc.audio_cod_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    avcodec_open2(enc.audio_cod_ctx, audio_codec, NULL);
    avcodec_parameters_from_context(enc.audio_stream->codecpar, enc.audio_cod_ctx);
    enc.audio_stream->time_base = enc.audio_cod_ctx->time_base;

    // frame sized to the codec's required frame size
    enc.audio_frame = av_frame_alloc();
    enc.audio_frame->format = AV_SAMPLE_FMT_FLTP;
    enc.audio_frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    enc.audio_frame->sample_rate = 48000;
    enc.audio_frame->nb_samples = enc.audio_cod_ctx->frame_size;
    av_frame_get_buffer(enc.audio_frame, 0);

    swr_alloc_set_opts2(&enc.swr_ctx,
                        &enc.audio_cod_ctx->ch_layout,
                        AV_SAMPLE_FMT_FLTP,
                        48000, // out
                        &enc.audio_cod_ctx->ch_layout,
                        AV_SAMPLE_FMT_S16,
                        48000, // in
                        0,
                        NULL);
    swr_init(enc.swr_ctx);

    avio_open(&enc.fmt_ctx->pb, output_path, AVIO_FLAG_WRITE);
    avformat_write_header(enc.fmt_ctx, NULL);

    return enc;
}

void encoder_submit_frame(EncoderContext& enc, const VideoFrame* raw) {
    // convert BGR -> YUV420P
    const uint8_t* src_slices[] = { raw->data.data() };
    const int src_stride[] = { raw->stride };

    av_frame_make_writable(enc.frame);
    sws_scale(enc.sws_ctx,
              src_slices,
              src_stride,
              0,
              raw->h,
              enc.frame->data,
              enc.frame->linesize);

    enc.frame->pts = enc.pts++;

    avcodec_send_frame(enc.cod_ctx, enc.frame);

    // drain
    while (avcodec_receive_packet(enc.cod_ctx, enc.packet) == 0) {
        enc.packet->pts = raw->ts * g_time_scale_mul;
        enc.packet->dts = raw->ts * g_time_scale_mul;
        enc.packet->duration = g_time_scale_mul;
        enc.packet->stream_index = enc.stream->index;
        av_interleaved_write_frame(enc.fmt_ctx, enc.packet);
        av_packet_unref(enc.packet);
    }
}

void encoder_close(EncoderContext& enc) {
    // flush video
    avcodec_send_frame(enc.cod_ctx, NULL);
    while (avcodec_receive_packet(enc.cod_ctx, enc.packet) == 0) {
        av_packet_rescale_ts(enc.packet, enc.cod_ctx->time_base, enc.stream->time_base);
        enc.packet->stream_index = enc.stream->index;
        av_interleaved_write_frame(enc.fmt_ctx, enc.packet);
        av_packet_unref(enc.packet);
    }

    // flush audio
    avcodec_send_frame(enc.audio_cod_ctx, NULL);
    while (avcodec_receive_packet(enc.audio_cod_ctx, enc.packet) == 0) {
        av_packet_rescale_ts(enc.packet, enc.audio_cod_ctx->time_base, enc.audio_stream->time_base);
        enc.packet->stream_index = enc.audio_stream->index;
        av_interleaved_write_frame(enc.fmt_ctx, enc.packet);
        av_packet_unref(enc.packet);
    }

    av_write_trailer(enc.fmt_ctx);

    sws_freeContext(enc.sws_ctx);
    swr_free(&enc.swr_ctx);
    av_frame_free(&enc.frame);
    av_frame_free(&enc.audio_frame);
    av_packet_free(&enc.packet);
    avcodec_free_context(&enc.cod_ctx);
    avcodec_free_context(&enc.audio_cod_ctx);
    avio_closep(&enc.fmt_ctx->pb);
    avformat_free_context(enc.fmt_ctx);
}

// ----------------------------------------------
// AUDIO
// ----------------------------------------------

struct pw_stream* g_audio_stream = nullptr;

void encoder_submit_audio(EncoderContext& enc, const AudioFrame& raw) {
    // input: interleaved S16 pointer
    const uint8_t* in[] = { raw.data.data() };

    av_frame_make_writable(enc.audio_frame);
    swr_convert(enc.swr_ctx,
                enc.audio_frame->data,
                enc.audio_frame->nb_samples,
                in,
                raw.n_samples);

    enc.audio_frame->pts = enc.audio_pts;
    enc.audio_pts += enc.audio_frame->nb_samples;

    avcodec_send_frame(enc.audio_cod_ctx, enc.audio_frame);
    while (avcodec_receive_packet(enc.audio_cod_ctx, enc.packet) == 0) {
        av_packet_rescale_ts(enc.packet,
                             enc.audio_cod_ctx->time_base,
                             enc.audio_stream->time_base);
        enc.packet->stream_index = enc.audio_stream->index;
        av_interleaved_write_frame(enc.fmt_ctx, enc.packet);
        av_packet_unref(enc.packet);
    }
}

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

inline std::string make_date_time_string() {
    char buffer[32];
    time_t now = time(nullptr);
    struct tm* local_time = localtime(&now);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", local_time);
    return buffer;
}

void start_pipewire_capture(int fd, uint32_t node_id, struct pw_properties* stream_properties, XdpSession* g_session) {
    g_v_frame_data.resize(16);
    struct VideoRecordData data = { 0 };
    const struct spa_pod* audio_params[1];
    const struct spa_pod* video_params[1];
    uint8_t audio_buffer[1024];
    uint8_t video_buffer[1024];
    struct spa_pod_builder audio_b = SPA_POD_BUILDER_INIT(audio_buffer, sizeof(audio_buffer));
    struct spa_pod_builder video_b = SPA_POD_BUILDER_INIT(video_buffer, sizeof(video_buffer));

    pw_init(NULL, NULL);

    data.loop = pw_main_loop_new(NULL);
    struct pw_main_loop* loop;
    struct pw_loop* pw_mainloop_loop = pw_main_loop_get_loop(data.loop);
    struct pw_context* context = pw_context_new(pw_mainloop_loop, NULL, 0);
    struct pw_core* core = pw_context_connect_fd(context, fd, NULL, 0);

    data.stream = pw_stream_new(core, "pipewire-portal-screencast", stream_properties);

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

    if (pw_stream_connect(g_audio_stream, PW_DIRECTION_INPUT, PW_ID_ANY, (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), audio_params, 1) != 0) {
        printf("Couldn't connect to audio stream\n");
    }

    pw_stream_connect(data.stream, PW_DIRECTION_INPUT, node_id, (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), video_params, 1);

    std::thread th(std::move([data]() {
        bool initialized = false;
        EncoderContext enc;
        while (!done) {
            while (!g_video_frame_data.empty()) {
                auto frame = g_video_frame_data.read();
                if (!initialized) {
                    char* home = getenv("HOME");
                    enc = encoder_init((std::string(home) + "/Videos/" + make_date_time_string() + ".mp4").c_str(), frame->w, frame->h);
                    initialized = true;
                }
                encoder_submit_frame(enc, frame);

                while (!g_audio_frame_data.empty()) {
                    auto frame = g_audio_frame_data.read();
                    encoder_submit_audio(enc, frame);
                }
            }
        }

        encoder_close(enc);
        pw_main_loop_quit(data.loop);
    }));

    pw_main_loop_run(data.loop);
    th.join();
    pw_stream_destroy(data.stream);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(data.loop);
}
