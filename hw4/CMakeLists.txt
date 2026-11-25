cmake_minimum_required(VERSION 3.10)
project(hw4 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(hw4
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
    src/net.c
)

target_include_directories(hw4 PRIVATE include)

add_executable(hw4_worker
    src/worker.c
    src/port.c
    src/types.c
    src/berth.c
    src/tug.c
    src/crane.c
    src/warehouse.c
    src/ship.c
    src/dispatcher.c
    src/log.c
    src/net.c
)

target_include_directories(hw4_worker PRIVATE include)

add_executable(hw4_observer
    src/observer.c
    src/port.c
    src/types.c
    src/berth.c
    src/tug.c
    src/crane.c
    src/warehouse.c
    src/ship.c
    src/dispatcher.c
    src/log.c
    src/net.c
)

target_include_directories(hw4_observer PRIVATE include)
