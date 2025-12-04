#!/bin/bash

# g++ compiler
g++ -Wall -Werror -o mydaemon daemon.cpp

# cleanup
rm -f *.o

echo "Build complete. Executable: mydaemon"
