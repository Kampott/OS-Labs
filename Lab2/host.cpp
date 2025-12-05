#include <iostream>
#include <string>
#include <thread>
#include <signal.h>
#include <ctime>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <cerrno>
#include <chrono>

#include "conn.h"

Conn* conn;
pid_t client_pid;
bool is_host;
std::atomic<bool> running(true);
std::atomic<bool> graceful_shutdown{false};
time_t last_message_time;

#if defined(TYPE_MMAP) || defined(TYPE_SHM)
Shared* shared = nullptr;
#else
SemShared* semshared = nullptr;
#endif

void sigchld_handler(int sig) {
    running.store(false);
    graceful_shutdown.store(true); 
#if defined(TYPE_PIPE)
    sem_post(&semshared->sem_c2h);
#else
    sem_post(&shared->sem_c2h);
#endif
}

void display(const Message& msg) {
    char time_buf[26];
    ctime_r(&msg.timestamp, time_buf);
    time_buf[strlen(time_buf) - 1] = '\0'; // Remove newline
    std::cout << time_buf << " Sender " << msg.sender << ": " << msg.text << std::endl;
}

void monitor_inactivity() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (time(nullptr) - last_message_time > 60) {
            std::cerr << "Client inactive, sending SIGKILL" << std::endl;
            kill(client_pid, SIGKILL);
            running.store(false);
            // No need for sem_post here, sigchld_handler will handle
            break;
        }
    }
}

void input_thread() {
    while (running) {
        std::string line;
        if (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            Message msg{};
            msg.timestamp = time(nullptr);
            msg.sender = is_host ? 0 : 1;
            msg.type = 0; // Default general
            msg.to_id = -1;
            if (line.rfind("@0 ", 0) == 0) {
                msg.type = 1;
                msg.to_id = 0;
                strncpy(msg.text, line.substr(3).c_str(), sizeof(msg.text) - 1);
            } else if (line.rfind("@1 ", 0) == 0) {
                msg.type = 1;
                msg.to_id = 1;
                strncpy(msg.text, line.substr(3).c_str(), sizeof(msg.text) - 1);
            } else {
                strncpy(msg.text, line.c_str(), sizeof(msg.text) - 1);
            }
            msg.text[sizeof(msg.text) - 1] = '\0';

            if (is_host) {
                bool should_display = (msg.type == 0) || (msg.type == 1 && (msg.to_id == 0 || msg.to_id == 1));
                bool send_to_client = (msg.type == 0) || (msg.type == 1 && msg.to_id == 1);
                if (should_display) {
                    display(msg);
                }
                if (send_to_client) {
                    if (!conn->Write(&msg, sizeof(Message))) {
                        std::cerr << "Host write failed" << std::endl;
                        running = false;
                    }
                }
            } else {
                if (!conn->Write(&msg, sizeof(Message))) {
                    std::cerr << "Client write failed" << std::endl;
                    running = false;
                }
            }
        } else {
            running = false;
        }
    }
}

int main() {
#if defined(TYPE_MMAP) || defined(TYPE_SHM)
    void* shm_ptr = nullptr;
    const size_t shared_size = sizeof(Shared);
#if defined(TYPE_SHM)
    int fd;
    const char* shm_name = "/lab_shm";
#endif
#endif
#if defined(TYPE_PIPE)
    int pipe_h2c[2];
    int pipe_c2h[2];
    void* sem_ptr = nullptr;
    const size_t sem_size = sizeof(SemShared);
#endif

#if defined(TYPE_MMAP)
    shm_ptr = mmap(nullptr, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    shared = static_cast<Shared*>(shm_ptr);
#elif defined(TYPE_SHM)
    fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return 1;
    }
    if (ftruncate(fd, shared_size) == -1) {
        perror("ftruncate");
        return 1;
    }
    shm_ptr = mmap(nullptr, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    close(fd);
    shared = static_cast<Shared*>(shm_ptr);
#elif defined(TYPE_PIPE)
    if (pipe(pipe_h2c) == -1 || pipe(pipe_c2h) == -1) {
        perror("pipe");
        return 1;
    }
    sem_ptr = mmap(nullptr, sem_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sem_ptr == MAP_FAILED) {
        perror("mmap sem");
        return 1;
    }
    semshared = static_cast<SemShared*>(sem_ptr);
#endif

    // sigchild before forks
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    client_pid = fork();
    if (client_pid == -1) {
        perror("fork");
        return 1;
    }

    is_host = (client_pid > 0);
    if (is_host) {
        std::cerr << "Client PID: " << client_pid << std::endl;
    }

#if defined(TYPE_PIPE)
    if (is_host) {
        close(pipe_h2c[0]);
        close(pipe_c2h[1]);
    } else {
        close(pipe_h2c[1]);
        close(pipe_c2h[0]);
    }
#elif defined(TYPE_SHM)
    if (!is_host) {
        int fd = shm_open(shm_name, O_RDWR, 0);
        if (fd == -1) {
            perror("shm_open client");
            return 1;
        }
        shm_ptr = mmap(nullptr, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (shm_ptr == MAP_FAILED) {
            perror("mmap client");
            return 1;
        }
        close(fd);
        shared = static_cast<Shared*>(shm_ptr);
    }
#endif

#if defined(TYPE_PIPE)
    conn = new Conn(is_host, pipe_h2c, pipe_c2h, semshared);
#else
    conn = new Conn(is_host, shared);
#endif

#if !defined(TYPE_PIPE)
    if (is_host) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&shared->mutex, &attr);
        sem_init(&shared->sem_h2c, 1, 0);
        sem_init(&shared->sem_c2h, 1, 0);
        pthread_mutexattr_destroy(&attr);
    }
#else
    if (is_host) {
        sem_init(&semshared->sem_h2c, 1, 0);
        sem_init(&semshared->sem_c2h, 1, 0);
    }
#endif

    std::thread input_th(input_thread);
    std::thread monitor_th;
    if (is_host) {
        std::cerr << "Host initialized" << std::endl;
        last_message_time = time(nullptr);
        monitor_th = std::thread(monitor_inactivity);
    } else {
        std::cerr << "Client initialized" << std::endl;
    }

    while (running) {
        Message msg;
        if (conn->Read(&msg, sizeof(Message))) {
            if (is_host) {
                last_message_time = time(nullptr);
                bool send_to_client = false;
                if (msg.type == 0) {
                    display(msg);
                    send_to_client = true;
                } else if (msg.type == 1) {
                    if (msg.to_id == 0) display(msg);
                    if (msg.to_id == 1) send_to_client = true;
                    else send_to_client = true;
                }
                if (send_to_client) {
                    conn->Write(&msg, sizeof(Message));
                }
            } else {
                display(msg);
            }
        } else {
            if (graceful_shutdown.load()) {
                break;  // нормальное завершение
            } else {
                std::cerr << "Read failed or error" << std::endl;
                running = false;
            }
        }
    }

    running.store(false);
    graceful_shutdown.store(true);
    close(0);  // ломает getline в input_thread

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (input_th.joinable()) input_th.detach();
    if (is_host && monitor_th.joinable()) monitor_th.join();

    delete conn;

    if (is_host) {
        wait(nullptr);
#if defined(TYPE_SHM)
        shm_unlink(shm_name);
#endif
#if defined(TYPE_MMAP) || defined(TYPE_SHM)
        munmap(shm_ptr, shared_size);
#endif
#if defined(TYPE_PIPE)
        munmap(sem_ptr, sem_size);
#endif
    } else {
#if defined(TYPE_MMAP) || defined(TYPE_SHM)
        munmap(shm_ptr, shared_size);
#endif
#if defined(TYPE_PIPE)
        munmap(sem_ptr, sem_size);
#endif
    }

    return 0;
}