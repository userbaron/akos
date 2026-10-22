cmake_minimum_required(VERSION 3.10)
project(hw2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(hw2
    src/main.c
    src/port.c
    src/types.c
    src/berth.c
    src/tug.c
    src/crane.c
    src/warehouse.c
    src/ship.c
    src/dispatcher.c
    src/log.c
)

target_include_directories(hw2 PRIVATE include)
target_link_libraries(hw2 PRIVATE Threads::Threads)
