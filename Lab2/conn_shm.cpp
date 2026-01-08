#include "conn.h"
#include <cstring>
#include <cerrno>
#include <iostream>
#include <sys/mman.h>
#include <fcntl.h>
#include <cstdlib>
#include <unistd.h>

void* Conn::shared_ptr = nullptr; // Not used, but declared for compilation

void Conn::pre_fork_init() {
    // Empty for SHM
}

Conn::Conn(bool is_host) : is_host_(is_host) {
    const char* shm_name = "/lab_shm";
    const size_t shared_size = sizeof(Shared);
    int fd = shm_open(shm_name, is_host_ ? O_CREAT | O_RDWR : O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        exit(1);
    }
    if (is_host_) {
        if (ftruncate(fd, shared_size) == -1) {
            perror("ftruncate");
            exit(1);
        }
    }
    void* shm_ptr = mmap(nullptr, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    close(fd);
    shared_ = static_cast<Shared*>(shm_ptr);
    if (is_host_) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&shared_->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        sem_init(&shared_->sem_h2c, 1, 0);
        sem_init(&shared_->sem_c2h, 1, 0);
    }
}

Conn::~Conn() {
    if (is_host_) {
        pthread_mutex_destroy(&shared_->mutex);
        sem_destroy(&shared_->sem_h2c);
        sem_destroy(&shared_->sem_c2h);
        shm_unlink("/lab_shm");
    }
    munmap(shared_, sizeof(Shared));
}

bool Conn::Read(void* buf, size_t count) {
    sem_t* sem = is_host_ ? &shared_->sem_c2h : &shared_->sem_h2c;
    Message* msg_ptr = is_host_ ? &shared_->msg_c2h : &shared_->msg_h2c;
    if (sem_wait(sem) == -1) {
        return false;
    }
    if (!running.load()) {
        return false;
    }
    pthread_mutex_lock(&shared_->mutex);
    memcpy(buf, msg_ptr, count);
    pthread_mutex_unlock(&shared_->mutex);
    return true;
}

bool Conn::Write(const void* buf, size_t count) {
    sem_t* sem = is_host_ ? &shared_->sem_h2c : &shared_->sem_c2h;
    Message* msg_ptr = is_host_ ? &shared_->msg_h2c : &shared_->msg_c2h;
    pthread_mutex_lock(&shared_->mutex);
    memcpy(msg_ptr, buf, count);
    pthread_mutex_unlock(&shared_->mutex);
    sem_post(sem);
    return true;
}