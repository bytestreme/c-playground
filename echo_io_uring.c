#include <liburing.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef BACKLOG
#define BACKLOG  1024
#endif
#ifndef QUEUE_DEPTH
#define QUEUE_DEPTH 4096
#endif
#ifndef MAX_FDS
#define MAX_FDS  65536
#endif
#ifndef MAX_MESSAGE_LEN
#define MAX_MESSAGE_LEN 2048
#endif
#ifndef DEFAULT_PORT
#define DEFAULT_PORT 9000
#endif

// Toggle SQPOLL (requires CAP_SYS_NICE for long polling or RLIMIT_MEMLOCK setup)
//#define USE_SQPOLL

// From linux/io_uring.h if not included:
#ifndef IORING_FEAT_FAST_POLL
#define IORING_FEAT_FAST_POLL (1u << 5)
#endif

typedef enum {
    EV_ACCEPT = 1,
    EV_READ = 2,
    EV_WRITE = 3
} conn_info_type;

typedef struct conn_info {
    int fd;
    uint32_t type;
} conn_info;

typedef struct thread_ctx {
    int core_id;
    int port;
    int listen_fd;
    struct io_uring ring;
    struct io_uring_params params;

    conn_info *conns;
    char *bufs;
    size_t bufs_stride;

    volatile bool *stop_flag;
} thread_ctx;

static volatile bool g_stop = false;

static void on_sigint(int signo) {
    (void) signo;
    g_stop = true;
}

static inline conn_info *ci(thread_ctx *tc, int fd) {
    return &tc->conns[fd];
}

static inline char *cbuf(thread_ctx *tc, int fd) {
    return tc->bufs + ((size_t) fd * tc->bufs_stride);
}

static void add_accept(thread_ctx *tc, int fd, struct sockaddr *addr, socklen_t *addrlen) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&tc->ring);
    assert(sqe);
    io_uring_prep_accept(sqe, fd, addr, addrlen, 0);
    conn_info *c = ci(tc, fd);
    c->fd = fd;
    c->type = EV_ACCEPT;
    io_uring_sqe_set_data(sqe, c);
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "ConstantParameter"

static void add_recv(thread_ctx *tc, int fd, size_t size) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&tc->ring);
    assert(sqe);
    io_uring_prep_recv(sqe, fd, cbuf(tc, fd), size, 0);
    conn_info *c = ci(tc, fd);
    c->fd = fd;
    c->type = EV_READ;
    io_uring_sqe_set_data(sqe, c);
}

#pragma clang diagnostic pop

static void add_send(thread_ctx *tc, int fd, size_t size) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&tc->ring);
    assert(sqe);
    io_uring_prep_send(sqe, fd, cbuf(tc, fd), size, 0);
    conn_info *c = ci(tc, fd);
    c->fd = fd;
    c->type = EV_WRITE;
    io_uring_sqe_set_data(sqe, c);
}

static int make_reuseport_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void pin_thread_to_core(int core_id) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        fprintf(stderr, "[core %d] WARN: pthread_setaffinity_np failed: %s\n", core_id, strerror(rc));
    }
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "ConstantFunctionResult"

static void *thread_main(void *arg) {
    thread_ctx *tc = (thread_ctx *) arg;
    pin_thread_to_core(tc->core_id);

    tc->listen_fd = make_reuseport_listener((uint16_t) tc->port);
    if (tc->listen_fd < 0) {
        perror("listen socket");
        return NULL;
    }

    memset(&tc->params, 0, sizeof(tc->params));
#ifdef USE_SQPOLL
    unsigned int flags = IORING_SETUP_SQPOLL; // kernel thread polls SQ
#else
#endif
    int ret = io_uring_queue_init_params(QUEUE_DEPTH, &tc->ring, &tc->params);
    if (ret < 0) {
        fprintf(stderr, "[core %d] io_uring_queue_init_params: %s\n", tc->core_id, strerror(-ret));
        return NULL;
    }

    if (!(tc->params.features & IORING_FEAT_FAST_POLL)) {
        fprintf(stderr, "[core %d] FAST_POLL not available; continuing but expect higher latency.\n", tc->core_id);
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    add_accept(tc, tc->listen_fd, (struct sockaddr *) &client_addr, &client_len);

    fprintf(stderr, "[core %d] ring online, listening on port %d (fd=%d)\n", tc->core_id, tc->port, tc->listen_fd);

    while (!*(tc->stop_flag)) {
        io_uring_submit(&tc->ring);

        struct io_uring_cqe *cqe = NULL;
        int rc = io_uring_wait_cqe(&tc->ring, &cqe);
        if (rc < 0) {
            if (rc == -EINTR) continue;
            fprintf(stderr, "[core %d] wait_cqe: %s\n", tc->core_id, strerror(-rc));
            break;
        }

        struct io_uring_cqe *cqes[BACKLOG];
        unsigned int n = io_uring_peek_batch_cqe(&tc->ring, cqes, (int) (sizeof(cqes) / sizeof(cqes[0])));
        if (n <= 0) {
            cqes[0] = cqe;
            n = 1;
        }

        for (int i = 0; i < n; ++i) {
            struct io_uring_cqe *x = cqes[i];
            conn_info *ud = (conn_info *) io_uring_cqe_get_data(x);
            int res = x->res;

            if (!ud) {
                io_uring_cqe_seen(&tc->ring, x);
                continue;
            }

            switch (ud->type) {
                case EV_ACCEPT: {
                    if (res < 0) {
                        add_accept(tc, tc->listen_fd, (struct sockaddr *) &client_addr, &client_len);
                        break;
                    }
                    int cfd = res;
                    if (cfd >= MAX_FDS) {
                        close(cfd);
                    } else {
                        add_recv(tc, cfd, MAX_MESSAGE_LEN);
                    }
                    add_accept(tc, tc->listen_fd, (struct sockaddr *) &client_addr, &client_len);
                    break;
                }
                case EV_READ: {
                    if (res <= 0) {
                        shutdown(ud->fd, SHUT_RDWR);
                        close(ud->fd);
                    } else {
                        add_send(tc, ud->fd, (size_t) res);
                    }
                    break;
                }
                case EV_WRITE: {
                    add_recv(tc, ud->fd, MAX_MESSAGE_LEN);
                    break;
                }
                default:
                    break;
            }
            io_uring_cqe_seen(&tc->ring, x);
        }
    }

    fprintf(stderr, "[core %d] shutting down...\n", tc->core_id);
    io_uring_queue_exit(&tc->ring);
    close(tc->listen_fd);
    return NULL;
}

#pragma clang diagnostic pop

void cleanup(pthread_t *ths, thread_ctx *tcs) {
    free(ths);
    free(tcs);
}

int main() {
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    struct rlimit rl = {.rlim_cur = MAX_FDS * 2, .rlim_max = MAX_FDS * 2};
    setrlimit(RLIMIT_NOFILE, &rl);

    int nprocs = get_nprocs();
    if (nprocs <= 0) {
        nprocs = 1;
    }
    fprintf(stderr, "Starting per-core echo with %d cores on port %d\n", nprocs, DEFAULT_PORT);

    pthread_t *ths = calloc((size_t) nprocs, sizeof(pthread_t));
    thread_ctx *tcs = calloc((size_t) nprocs, sizeof(thread_ctx));

    for (int i = 0; i < nprocs; ++i) {
        thread_ctx *tc = &tcs[i];
        tc->core_id = i;
        tc->port = DEFAULT_PORT;
        tc->stop_flag = &g_stop;

        tc->conns = aligned_alloc(64, sizeof(conn_info) * (size_t) MAX_FDS);
        tc->bufs_stride = MAX_MESSAGE_LEN;
        tc->bufs = aligned_alloc(64, (size_t) MAX_FDS * tc->bufs_stride);
        if (!tc->conns || !tc->bufs) {
            fprintf(stderr, "allocation failed\n");
            cleanup(ths, tcs);
            return 2;
        }
        memset(tc->conns, 0, sizeof(conn_info) * (size_t) MAX_FDS);
    }

    for (int i = 0; i < nprocs; ++i) {
        int rc = pthread_create(&ths[i], NULL, thread_main, &tcs[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            cleanup(ths, tcs);
            return 3;
        }
    }

    for (int i = 0; i < nprocs; ++i) {
        pthread_join(ths[i], NULL);
    }

    for (int i = 0; i < nprocs; ++i) {
        free(tcs[i].conns);
        free(tcs[i].bufs);
    }
    cleanup(ths, tcs);

    return 0;
}

