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

// Singleton Daemon class
class Daemon {
private:
    static Daemon* instance;
    std::string configPath;
    std::string dir1;
    std::string dir2;
    int interval;
    pid_t pid;
    std::string pidFile = "/var/run/mydaemon.pid";

    Daemon() : interval(30) {}

    
    void daemonize() {
        pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "Fork failed");
            exit(EXIT_FAILURE);
        }
        if (pid > 0) {
            exit(EXIT_SUCCESS);
        }

        umask(0);
        if (setsid() < 0) {
            syslog(LOG_ERR, "setsid failed");
            exit(EXIT_FAILURE);
        }

        if (chdir("/") < 0) {
            syslog(LOG_ERR, "chdir failed");
            exit(EXIT_FAILURE);
        }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }

    void checkAndKillExisting() {
        std::ifstream pidStream(pidFile);
        if (pidStream.good()) {
            pid_t oldPid;
            pidStream >> oldPid;
            pidStream.close();

            std::string procPath = "/proc/" + std::to_string(oldPid);
            struct stat statbuf;
            if (stat(procPath.c_str(), &statbuf) == 0) {
                kill(oldPid, SIGTERM);
                sleep(1); // Wait a bit for the old process to exit
            }
        }
    }

    void writePid() {
        std::ofstream pidStream(pidFile);
        if (pidStream.good()) {
            pidStream << getpid();
            pidStream.close();
        } else {
            syslog(LOG_ERR, "Failed to write PID file");
            exit(EXIT_FAILURE);
        }
    }

    void readConfig() {
        std::ifstream configFile(configPath);
        if (!configFile.good()) {
            syslog(LOG_ERR, "Failed to open config file: %s", configPath.c_str());
            return;
        }

        std::string line;
        while (std::getline(configFile, line)) {
            if (line.find("dir1=") == 0) {
                dir1 = line.substr(5);
            } else if (line.find("dir2=") == 0) {
                dir2 = line.substr(5);
            } else if (line.find("interval=") == 0) {
                interval = std::stoi(line.substr(9));
            }
        }
        configFile.close();
        syslog(LOG_INFO, "Config loaded: dir1=%s, dir2=%s, interval=%d", dir1.c_str(), dir2.c_str(), interval);
    }

    void moveFiles(const std::string& srcDir, const std::string& destDir, bool olderThan10Min) {
        DIR* dir = opendir(srcDir.c_str());
        if (!dir) {
            syslog(LOG_ERR, "Failed to open directory: %s", srcDir.c_str());
            return;
        }

        struct dirent* entry;
        time_t now = time(nullptr);
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_REG) continue; 

            std::string fileName = entry->d_name;
            std::string srcPath = srcDir + "/" + fileName;

            struct stat statbuf;
            if (stat(srcPath.c_str(), &statbuf) != 0) continue;

            double ageSeconds = difftime(now, statbuf.st_mtime);
            bool shouldMove = olderThan10Min ? (ageSeconds > 600) : (ageSeconds < 600);

            if (shouldMove) {
                std::string destPath = destDir + "/" + fileName;
                if (rename(srcPath.c_str(), destPath.c_str()) == 0) {
                    syslog(LOG_INFO, "Moved %s to %s", srcPath.c_str(), destPath.c_str());
                } else {
                    syslog(LOG_ERR, "Failed to move %s to %s", srcPath.c_str(), destPath.c_str());
                }
            }
        }
        closedir(dir);
    }

    void performAction() {
        moveFiles(dir1, dir2, true);  // From dir1 to dir2: older than 10 min
        moveFiles(dir2, dir1, false); // From dir2 to dir1: younger than 10 min
    }

    static void signalHandler(int sig) {
        if (sig == SIGHUP) {
            instance->readConfig();
        } else if (sig == SIGTERM) {
            syslog(LOG_INFO, "Received SIGTERM, exiting");
            unlink(instance->pidFile.c_str());
            closelog();
            exit(EXIT_SUCCESS);
        }
    }

public:
    static Daemon* getInstance() {
        if (!instance) {
            instance = new Daemon();
        }
        return instance;
    }

    void start(const std::string& config) {
        char absPath[PATH_MAX];
        if (realpath(config.c_str(), absPath) == nullptr) {
            std::cerr << "Failed to get absolute path of config" << std::endl;
            exit(EXIT_FAILURE);
        }
        configPath = absPath;

        openlog("mydaemon", LOG_PID | LOG_CONS, LOG_DAEMON);
        syslog(LOG_INFO, "Daemon starting");

        checkAndKillExisting();

        daemonize();

        writePid();

        readConfig();

        signal(SIGHUP, signalHandler);
        signal(SIGTERM, signalHandler);

        while (true) {
            performAction();
            sleep(interval);
        }
    }
};

Daemon* Daemon::instance = nullptr;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return EXIT_FAILURE;
    }

    Daemon::getInstance()->start(argv[1]);
    return EXIT_SUCCESS;
}