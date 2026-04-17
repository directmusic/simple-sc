#include "shm.hh"
#include <unistd.h>

#include <fcntl.h>
#include <sys/mman.h>

const char* const SHM_NAME = "simple-sc-instance-data";

bool handle_exists() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    return fd >= 0;
}

pid_t get_other_instance_pid() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, sizeof(SharedMemory));
        auto ptr = (SharedMemory*)mmap(nullptr, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        return ptr->pid;
    }

    return 0;
}

SharedMemory* create_handle_with_pid() {
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, sizeof(SharedMemory));
    } else {
        // We failed to create here so lets try not to create and instead open the already existing.
        shm_unlink(SHM_NAME);
        fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            ftruncate(fd, sizeof(SharedMemory));
        }
    }

    auto ptr = (SharedMemory*)mmap(nullptr, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ptr->pid = getpid();
    return ptr;
}

void delete_handle() { shm_unlink(SHM_NAME); }
