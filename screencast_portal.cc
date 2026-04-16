#include "screencast_portal.hh"

#include <glib-2.0/gio/gio.h>
#include <glib-2.0/glib.h>
#include <glib.h>
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstdint>
#include <thread>

XdpSession* g_session = nullptr;
GMainLoop* g_gio_main_loop = nullptr;

enum class XdpScreencastPortalStatus {
    kInit,
    kRunning,
    kCancelled,
};

XdpScreencastPortalStatus g_status = XdpScreencastPortalStatus::kInit;

static int g_pipewire_fd = 0;
static uint32_t g_pipewire_node_id = 0;
static struct pw_properties* stream_properties = nullptr;

static void session_create_cb(GObject* source_object, GAsyncResult* result, gpointer user_data) {
    g_autoptr(GError) error = nullptr;
    g_session = xdp_portal_create_screencast_session_finish(
        XDP_PORTAL(source_object),
        result,
        &error);
    if (!g_session) {
        g_print("Failed to create screencast session: %s\n", error->message);
        return;
    }
}

std::thread payload_start_portal_gio_mainloop_thread() {
    // this thread is going to be trapped in the gio mainloop
    std::thread portal_gio_mainloop_thread = std::thread([]() {
        g_main_loop_run(g_gio_main_loop);
    });
    while (!g_session) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::move(portal_gio_mainloop_thread);
}

static void session_start_cb(GObject* source_object, GAsyncResult* result, gpointer user_data) {
    GError* error = nullptr;
    if (!xdp_session_start_finish(XDP_SESSION(source_object), result, &error)) {
        g_warning("Failed to start screencast session: %s", error->message);
        g_error_free(error);
        g_status = XdpScreencastPortalStatus::kCancelled;
        g_main_loop_quit(g_gio_main_loop);
        return;
    }

    g_status = XdpScreencastPortalStatus::kRunning;
    g_pipewire_fd = xdp_session_open_pipewire_remote(XDP_SESSION(source_object));

    GVariant* streams = xdp_session_get_streams(XDP_SESSION(source_object));
    GVariantIter iter;
    g_variant_iter_init(&iter, streams);
    int n_streams = g_variant_iter_n_children(&iter);

    if (n_streams != 1) {
        for (int i = 0; i < n_streams - 1; i++) {
            GVariant* dummy = nullptr;
            uint32_t dummy_id;
            g_variant_iter_loop(&iter, "(u@a{sv})", &dummy_id, &dummy);
        }
    }

    GVariant* props_variant = nullptr;
    g_variant_iter_loop(&iter, "(u@a{sv})", &g_pipewire_node_id, &props_variant);
    if (props_variant) g_variant_unref(props_variant);

    g_main_loop_quit(g_gio_main_loop);
}

// reuturns true on success
bool create_screencast_portal(ScreencastPortalData* data_out) {
    g_gio_main_loop = g_main_loop_new(NULL, FALSE);
    auto portal = xdp_portal_new();
    xdp_portal_create_screencast_session(
        portal,
        (XdpOutputType)(XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW),
        XDP_SCREENCAST_FLAG_NONE,
        XDP_CURSOR_MODE_EMBEDDED,
        XDP_PERSIST_MODE_NONE,
        NULL,
        NULL,
        session_create_cb,
        nullptr);

    std::thread portal_gio_mainloop_thread = payload_start_portal_gio_mainloop_thread();

    g_main_context_invoke(g_main_loop_get_context(g_gio_main_loop), [](gpointer) -> gboolean {
        xdp_session_start(g_session, NULL, NULL, session_start_cb, nullptr);
        return G_SOURCE_REMOVE; }, nullptr);

    portal_gio_mainloop_thread.join();

    if (g_status == XdpScreencastPortalStatus::kCancelled) return 1;

    if (g_pipewire_fd == -1) {
        fprintf(stderr, "Failed to open pipewire remote\n");
        return false;
    }

    data_out->fd = g_pipewire_fd;
    data_out->node_id = g_pipewire_node_id;

    return true;
}
