#ifndef CONN_H
#define CONN_H

#include <cstddef>
#include <ctime>   
#include <pthread.h>
#include <semaphore.h>
#include <atomic>

struct Message {
    int type; // 0: general, 1: personal
    int to_id;
    time_t timestamp;
    int sender;
    char text[1024];
};

struct Shared {
    pthread_mutex_t mutex;
    sem_t sem_h2c;
    sem_t sem_c2h;
    Message msg_h2c;
    Message msg_c2h;
};

struct SemShared {
    sem_t sem_h2c;
    sem_t sem_c2h;
};

extern std::atomic<bool> running;

class Conn {
public:
    Conn(bool is_host);
    ~Conn();
    bool Read(void* buf, size_t count);
    bool Write(const void* buf, size_t count);
    static void pre_fork_init();

private:
#ifdef TYPE_PIPE
    bool is_host_;
    int read_fd_;
    int write_fd_;
    sem_t *sem_read_;
    sem_t *sem_write_;
    static int pipe_h2c[2];
    static int pipe_c2h[2];
    static void* sem_ptr;
#else
    bool is_host_;
    Shared* shared_;
#if defined(TYPE_MMAP) || defined(TYPE_SHM)
    static void* shared_ptr;
#endif
#endif
};

#endif