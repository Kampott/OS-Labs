#create
g++ -Wall -Werror -o mydaemon main.cpp Daemon.cpp -std=c++14

rm -f *.o

echo "Build complete. Executable: mydaemon"
