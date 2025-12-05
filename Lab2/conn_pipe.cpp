#include "conn.h"
#include <unistd.h>
#include <poll.h>
#include <iostream>
#include <cerrno>

bool Conn::is_host;
int Conn::read_fd = -1;
int Conn::write_fd = -1;

Conn::Conn(bool ih, int h2c[2], int c2h[2], SemShared* ss) {
    is_host = ih;
    if (is_host) {
        read_fd = c2h[0];
        write_fd = h2c[1];
    } else {
        read_fd = h2c[0];
        write_fd = c2h[1];
    }
    sem_read = is_host ? &ss->sem_c2h : &ss->sem_h2c;
    sem_write = is_host ? &ss->sem_h2c : &ss->sem_c2h;
}

Conn::~Conn() {
    if (read_fd != -1) close(read_fd);
    if (write_fd != -1) close(write_fd);
}

bool Conn::Read(void* buf, size_t count) {
    if (sem_wait(sem_read) == -1) {
        return false;
    }
    if (!running.load()) {
        return false;
    }   
    struct pollfd pfd = {read_fd, POLLIN, 0};
    int ret = poll(&pfd, 1, 0); 
    if (ret <= 0) {
        if (ret == 0) std::cerr << "poll no data after sem" << std::endl;
        else perror("poll");
        return false;
    }
    ssize_t n = read(read_fd, buf, count);
    if (n != static_cast<ssize_t>(count)) {
        if (n < 0) perror("read");
        else std::cerr << "read incomplete" << std::endl;
        return false;
    }
    return true;
}

bool Conn::Write(const void* buf, size_t count) {
    ssize_t n = write(write_fd, buf, count);
    if (n != static_cast<ssize_t>(count)) {
        if (n < 0) perror("write");
        else std::cerr << "write incomplete" << std::endl;
        return false;
    }
    sem_post(sem_write);
    return true;
}