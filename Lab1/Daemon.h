#ifndef DAEMON_H
#define DAEMON_H

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <limits.h>
#include <memory>

class Daemon {
public:
    static Daemon* getInstance();

    void start(const std::string& config);

private:
    static std::unique_ptr<Daemon> instance;
    std::string configPath;
    std::string dir1;
    std::string dir2;
    int interval;
    pid_t pid;
    std::string pidFile = "/var/run/mydaemon.pid";

    Daemon();

    void daemonize();
    void checkAndKillExisting();
    void writePid();
    void readConfig();
    void moveFiles(const std::string& srcDir, const std::string& destDir, bool olderThan10Min) const;
    void performAction() const;
    static void signalHandler(int sig);
};

#endif