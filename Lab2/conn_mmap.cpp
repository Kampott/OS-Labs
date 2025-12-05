#include "conn.h"
#include <cstring>
#include <cerrno>
#include <iostream>

Conn::Conn(bool ih, Shared* sh) : is_host(ih), shared(sh) {}

Conn::~Conn() {}

bool Conn::Read(void* buf, size_t count) {
    sem_t* sem = is_host ? &shared->sem_c2h : &shared->sem_h2c;
    Message* msg_ptr = is_host ? &shared->msg_c2h : &shared->msg_h2c;
    if (sem_wait(sem) == -1) {
        return false;
    }
    if (!running.load()) {
    return false;
    }
    pthread_mutex_lock(&shared->mutex);
    memcpy(buf, msg_ptr, count);
    pthread_mutex_unlock(&shared->mutex);
    return true;
}

bool Conn::Write(const void* buf, size_t count) {
    sem_t* sem = is_host ? &shared->sem_h2c : &shared->sem_c2h;
    Message* msg_ptr = is_host ? &shared->msg_h2c : &shared->msg_c2h;
    pthread_mutex_lock(&shared->mutex);
    memcpy(msg_ptr, buf, count);
    pthread_mutex_unlock(&shared->mutex);
    sem_post(sem);
    return true;
}