#include "Daemon.h"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return EXIT_FAILURE;
    }

    Daemon::getInstance()->start(argv[1]);
    return EXIT_SUCCESS;
}