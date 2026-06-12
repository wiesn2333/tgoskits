#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_async_setup
#define SYS_async_setup 461
#endif
#ifndef SYS_async_write
#define SYS_async_write 463
#endif
#ifndef SYS_async_connect
#define SYS_async_connect 466
#endif

#define MAX_CONNS 4096
#define ST_CONNECT 0
#define ST_WRITE   1
#define ST_DONE    2

struct task {
    int fd;
    volatile int state;
    volatile int64_t result;
    char msg[32];
    int msg_len;
    char buf[128];
};

static struct task tasks[MAX_CONNS];
static volatile int write_done;

static int n_pass;
static int n_fail;

#define CHECK(cond, fmt, ...) do {                                      \
    if (cond) {                                                         \
        printf("  PASS | %s:%d | " fmt "\n", __FILE__, __LINE__,        \
               ##__VA_ARGS__);                                          \
        n_pass++;                                                       \
    } else {                                                            \
        printf("  FAIL | %s:%d | " fmt "\n", __FILE__, __LINE__,        \
               ##__VA_ARGS__);                                          \
        n_fail++;                                                       \
    }                                                                   \
} while (0)

static void async_handler(uint64_t userdata, int64_t result)
{
    int idx = (int)userdata;

    if (result < 0) {
        tasks[idx].result = result;
        tasks[idx].state = ST_DONE;
        __sync_fetch_and_add(&write_done, 1);
        return;
    }

    switch (tasks[idx].state) {
    case ST_CONNECT:
        tasks[idx].state = ST_WRITE;
        tasks[idx].result = result;
        syscall(SYS_async_write, tasks[idx].fd,
                (uintptr_t)tasks[idx].msg, tasks[idx].msg_len, -1, idx);
        break;

    case ST_WRITE:
        tasks[idx].state = ST_DONE;
        tasks[idx].result = result;
        __sync_fetch_and_add(&write_done, 1);
        break;
    }
}

static ssize_t read_all(int fd, void *buf, size_t len)
{
    size_t left = len;
    char *p = (char *)buf;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                poll(&pfd, 1, -1);
                continue;
            }
            return -1;
        }
        p += n;
        left -= n;
    }
    return len - left;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc < 2) {
        printf("usage: crawl-async <concurrency>\n");
        return 1;
    }
    int n = atoi(argv[1]);
    if (n < 1 || n > MAX_CONNS) {
        printf("concurrency must be 1..%d\n", MAX_CONNS);
        return 1;
    }

    printf("================================================\n");
    printf("  StarryOS Async Crawl Test\n");
    printf("  concurrency = %d\n", n);
    printf("================================================\n");

    if (syscall(SYS_async_setup, (uintptr_t)async_handler) != 0) {
        printf("FAIL sys_async_setup\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(8080);
    addr.sin_addr.s_addr = inet_addr("10.0.2.2");

    for (int i = 0; i < n; i++) {
        tasks[i].fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (tasks[i].fd < 0) {
            printf("FAIL socket %d: errno=%d\n", i, errno);
            return 1;
        }
        tasks[i].msg_len = snprintf(tasks[i].msg, sizeof(tasks[i].msg), "%d", i);
        tasks[i].state = ST_CONNECT;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < n; i++) {
        syscall(SYS_async_connect, tasks[i].fd,
                (uintptr_t)&addr, sizeof(addr), i);
    }

    while (write_done < n)
        syscall(SYS_sched_yield);

    for (int i = 0; i < n; i++) {
        if (tasks[i].result < 0) continue;
        int fl = fcntl(tasks[i].fd, F_GETFL);
        fcntl(tasks[i].fd, F_SETFL, fl & ~O_NONBLOCK);
        memset(tasks[i].buf, 0, sizeof(tasks[i].buf));
        read_all(tasks[i].fd, tasks[i].buf, sizeof(tasks[i].buf) - 1);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;

    printf("--- verify %d tasks ---\n", n);
    for (int i = 0; i < n; i++) {
        if (tasks[i].result < 0) {
            CHECK(0, "task %d connect/write result=%ld", i, tasks[i].result);
            continue;
        }

        ssize_t r = strlen(tasks[i].buf);
        int expected_len = 6 + tasks[i].msg_len + 1;
        CHECK(r == expected_len,
              "task %d read %zd bytes (expected %d)", i, r, expected_len);
        char expected[128];
        int elen = snprintf(expected, sizeof(expected), "echo: %s\n", tasks[i].msg);
        CHECK(memcmp(tasks[i].buf, expected, elen) == 0,
              "task %d echo match", i);
    }

    for (int i = 0; i < n; i++) {
        if (tasks[i].fd >= 0)
            close(tasks[i].fd);
    }

    printf("  DONE: %d pass, %d fail\n", n_pass, n_fail);
    printf("BENCH crawl-async: %d conns, %.2f s, %.1f conns/s\n",
           n, wall, n / wall);
    printf("BENCH crawl-async: done\n");
    return n_fail > 0 ? 1 : 0;
}
