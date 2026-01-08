#include "conn.h"
#include <unistd.h>
#include <poll.h>
#include <iostream>
#include <cerrno>
#include <sys/mman.h>
#include <cstdlib>

int Conn::pipe_h2c[2] = {-1, -1};
int Conn::pipe_c2h[2] = {-1, -1};
void* Conn::sem_ptr = nullptr;

void Conn::pre_fork_init() {
    if (pipe(pipe_h2c) == -1) {
        perror("pipe h2c");
        exit(1);
    }
    if (pipe(pipe_c2h) == -1) {
        perror("pipe c2h");
        exit(1);
    }
    const size_t sem_size = sizeof(SemShared);
    sem_ptr = mmap(nullptr, sem_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sem_ptr == MAP_FAILED) {
        perror("mmap sem");
        exit(1);
    }
}

Conn::Conn(bool is_host) : is_host_(is_host) {
    SemShared* ss = static_cast<SemShared*>(sem_ptr);
    if (is_host_) {
        close(pipe_h2c[0]);
        close(pipe_c2h[1]);
        read_fd_ = pipe_c2h[0];
        write_fd_ = pipe_h2c[1];
        sem_read_ = &ss->sem_c2h;
        sem_write_ = &ss->sem_h2c;
        sem_init(&ss->sem_h2c, 1, 0);
        sem_init(&ss->sem_c2h, 1, 0);
    } else {
        close(pipe_h2c[1]);
        close(pipe_c2h[0]);
        read_fd_ = pipe_h2c[0];
        write_fd_ = pipe_c2h[1];
        sem_read_ = &ss->sem_h2c;
        sem_write_ = &ss->sem_c2h;
    }
}

Conn::~Conn() {
    close(read_fd_);
    close(write_fd_);
    SemShared* ss = static_cast<SemShared*>(sem_ptr);
    if (is_host_) {
        sem_destroy(&ss->sem_h2c);
        sem_destroy(&ss->sem_c2h);
    }
    munmap(sem_ptr, sizeof(SemShared));
}

bool Conn::Read(void* buf, size_t count) {
    if (sem_wait(sem_read_) == -1) {
        return false;
    }
    if (!running.load()) {
        return false;
    }   
    struct pollfd pfd = {read_fd_, POLLIN, 0};
    int ret = poll(&pfd, 1, 0); 
    if (ret <= 0) {
        if (ret == 0) std::cerr << "poll no data after sem" << std::endl;
        else perror("poll");
        return false;
    }
    ssize_t n = read(read_fd_, buf, count);
    if (n != static_cast<ssize_t>(count)) {
        if (n < 0) perror("read");
        else std::cerr << "read incomplete" << std::endl;
        return false;
    }
    return true;
}

bool Conn::Write(const void* buf, size_t count) {
    ssize_t n = write(write_fd_, buf, count);
    if (n != static_cast<ssize_t>(count)) {
        if (n < 0) perror("write");
        else std::cerr << "write incomplete" << std::endl;
        return false;
    }
    sem_post(sem_write_);
    return true;
}