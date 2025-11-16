#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <stdatomic.h>
#include <signal.h>

#define PORT 9000
#define MAX_EVENTS 1024
#define SOCK_BUF_SIZE 512

void handle_sigint(int signum);

void bind_thread_to_core(int core_id);

int make_listen_socket();

void *shard_thread(void *);

atomic_int running = 1;

int main() {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    int num_cores = (int) sysconf(_SC_NPROCESSORS_ONLN);
    printf("Detected %d CPU cores\n", num_cores);
    if (num_cores < 1) {
        num_cores = 1;
    }

    const int NUM_SHARDS = num_cores;
    printf("Set NUM_SHARDS to %d\n", NUM_SHARDS);

    pthread_t threads[NUM_SHARDS];
    int shard_ids[NUM_SHARDS];

    for (int i = 0; i < NUM_SHARDS; i++) {
        shard_ids[i] = i;

        if (pthread_create(&threads[i], NULL, shard_thread, &shard_ids[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    printf("Press Enter to stop server...\n");
    getchar();
    running = 0;

    for (int i = 0; i < NUM_SHARDS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("Server stopped\n");
    return 0;
}

void handle_sigint(int signum) {
    (void) signum;
    running = 0;
    printf("SIGINT received, shutting down...\n");
}

void bind_thread_to_core(int core_id) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core_id, &cpu_set);

    pthread_t thread = pthread_self();
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpu_set) != 0) {
        perror("pthread_setaffinity_np");
    } else {
        printf("Shard[%d] affinity set\n", core_id);
    }
}

int make_listen_socket() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &(int) {1}, sizeof(int));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &(int) {1}, sizeof(int));

    struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = INADDR_ANY,
            .sin_port = htons(PORT)
    };

    if (bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        exit(1);
    }

    fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    return listen_fd;
}


void *shard_thread(void *args) {
    int shard_id = *((int *) args);

    bind_thread_to_core(shard_id);

    int listen_fd = make_listen_socket();
    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];

    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("Shard[%d] started, listening on port %d\n", shard_id, PORT);

    while (running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr *) &client_addr, &addr_len);
                if (client_fd < 0) {
                    printf("Shard[%d] unable to accept client connection", shard_id);
                    continue;
                }
                fcntl(client_fd, F_SETFL, O_NONBLOCK);
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                printf("Shard[%d] accepted connection id=%d\n", shard_id, client_fd);
            } else {
                char buf[SOCK_BUF_SIZE];
                size_t count = read(events[i].data.fd, buf, sizeof(buf));
                if (count <= 0) {
                    close(events[i].data.fd);
                    printf("Shard[%d] closed connection id=%d\n", shard_id, events[i].data.fd);
                } else {
                    write(events[i].data.fd, buf, count);
                    printf("Shard[%d] written to id=%d\n", shard_id, events[i].data.fd);
                }
            }
        }
    }

    close(listen_fd);
    close(epoll_fd);
    printf("Shard[%d] stopped\n", shard_id);
    return NULL;
}