find_package(Libevent REQUIRED)

add_executable(http_file_server server.c)
target_link_libraries(http_file_server PRIVATE Libevent::Libevent)
