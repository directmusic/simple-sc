#pragma once

// contains info about the current capture
#include <sys/types.h>

struct SharedMemory {
    pid_t pid;
};

extern bool handle_exists();
// returns 0 if failed
extern pid_t get_other_instance_pid();
extern SharedMemory* create_handle_with_pid();
extern void delete_handle();
