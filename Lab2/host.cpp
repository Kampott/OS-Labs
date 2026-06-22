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

void sigchld_handler(int sig) {
    running.store(false);
    graceful_shutdown.store(true); 
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
    Conn::pre_fork_init();

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

    conn = new Conn(is_host);

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
    }

    return 0;
}