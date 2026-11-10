cmake_minimum_required(VERSION 3.10)
project(hw3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(hw3
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
    src/ipc.c
)

target_include_directories(hw3 PRIVATE include)
target_link_libraries(hw3 PRIVATE Threads::Threads)
target_compile_definitions(hw3 PRIVATE INDEPENDENT_WORKERS)

add_executable(hw3_worker
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
    src/ipc.c
)

target_include_directories(hw3_worker PRIVATE include)
target_link_libraries(hw3_worker PRIVATE Threads::Threads)

add_executable(hw3_named
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
    src/ipc.c
)

target_include_directories(hw3_named PRIVATE include)
target_link_libraries(hw3_named PRIVATE Threads::Threads)

add_executable(hw3_unnamed
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
    src/ipc.c
)

target_include_directories(hw3_unnamed PRIVATE include)
target_link_libraries(hw3_unnamed PRIVATE Threads::Threads)
target_compile_definitions(hw3_unnamed PRIVATE UNNAMED_SEMAPHORES)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(hw3 PRIVATE rt)
    target_link_libraries(hw3_worker PRIVATE rt)
    target_link_libraries(hw3_named PRIVATE rt)
    target_link_libraries(hw3_unnamed PRIVATE rt)
endif()
