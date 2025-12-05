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
#ifdef TYPE_PIPE
    Conn(bool ih, int h2c[2], int c2h[2], SemShared* ss);
#else
    Conn(bool ih, Shared* sh);
#endif
    ~Conn();
    bool Read(void* buf, size_t count);
    bool Write(const void* buf, size_t count);
#ifdef TYPE_PIPE
    static bool is_host;
    static int read_fd;
    static int write_fd;
    sem_t *sem_read;
    sem_t *sem_write;
#else
private:
    bool is_host;
    Shared* shared;
#endif
};

#endif