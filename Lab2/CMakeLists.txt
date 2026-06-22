cmake_minimum_required(VERSION 3.10)
project(LabIPC)

set(CMAKE_CXX_STANDARD 11)
add_compile_options(-Wall)

add_executable(host_mmap host.cpp conn_mmap.cpp)
target_compile_definitions(host_mmap PRIVATE TYPE_MMAP)
target_link_libraries(host_mmap PRIVATE pthread rt)

add_executable(host_shm host.cpp conn_shm.cpp)
target_compile_definitions(host_shm PRIVATE TYPE_SHM)
target_link_libraries(host_shm PRIVATE pthread rt)

add_executable(host_pipe host.cpp conn_pipe.cpp)
target_compile_definitions(host_pipe PRIVATE TYPE_PIPE)
target_link_libraries(host_pipe PRIVATE pthread rt)