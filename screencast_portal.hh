#pragma once

#include <cstdint>

struct _XdpSession;
struct ScreencastPortalData {
    int fd;
    uint32_t node_id;
};

bool create_screencast_portal(ScreencastPortalData* data_out);
