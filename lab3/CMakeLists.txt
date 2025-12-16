
cmake_minimum_required(VERSION 3.10)

project(LabOS VERSION 1.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Add executable
add_executable(lab lab.cpp)

# Link pthread library
target_link_libraries(lab PRIVATE pthread)

# Treat warnings as errors
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(lab PRIVATE -Wall -Wextra -Werror)
endif()