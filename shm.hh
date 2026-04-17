#pragma once

// contains info about the current capture
#include <sys/types.h>

struct SharedMemory {
    pid_t pid;
};

extern bool shm_handle_exists();
// returns 0 if failed
extern pid_t shm_get_other_instance_pid();
extern SharedMemory* shm_create_handle_with_pid();
extern void shm_delete_handle();
