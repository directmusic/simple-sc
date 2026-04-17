#pragma once

#include <cstdint>

struct _XdpSession;
struct ScreencastPortalData {
    int fd;
    uint32_t node_id;
};

enum class ScreencastPortalStatus {
    Cancelled = -1,
    Error = 0,
    Success = 1,
};
ScreencastPortalStatus create_screencast_portal(ScreencastPortalData* data_out);
