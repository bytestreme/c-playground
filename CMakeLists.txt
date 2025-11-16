cmake_minimum_required(VERSION 3.28)
project(c_playground C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(LibUring REQUIRED)

set(CMAKE_C_STANDARD 11)

add_definitions(-D_GNU_SOURCE)

add_executable(echo_epoll echo_epoll.c)

add_executable(echo_io_uring echo_io_uring.c)
target_link_libraries(echo_io_uring PRIVATE LibUring::LibUring)

add_subdirectory(http_file_server)
