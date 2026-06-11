#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static int __pass = 0;
static int __fail = 0;

#define CHECK(cond, msg) do {                                           \
    if (cond) {                                                         \
        printf("  PASS | %s:%d | %s\n", __FILE__, __LINE__, msg);      \
        __pass++;                                                       \
    } else {                                                            \
        printf("  FAIL | %s:%d | %s\n", __FILE__, __LINE__, msg);      \
        __fail++;                                                       \
    }                                                                   \
} while (0)

#define CHECK_RET(call, expected, msg) do {                             \
    errno = 0;                                                          \
    long _r = (long)(call);                                             \
    long _e = (long)(expected);                                         \
    if (_r == _e) {                                                     \
        printf("  PASS | %s:%d | %s (ret=%ld)\n",                       \
               __FILE__, __LINE__, msg, _r);                            \
        __pass++;                                                       \
    } else {                                                            \
        printf("  FAIL | %s:%d | %s | expected=%ld got=%ld | errno=%d (%s)\n", \
               __FILE__, __LINE__, msg, _e, _r, errno, strerror(errno)); \
        __fail++;                                                       \
    }                                                                   \
} while (0)

#define CHECK_ERR(call, exp_errno, msg) do {                            \
    errno = 0;                                                          \
    long _r = (long)(call);                                             \
    if (_r == -1 && errno == (exp_errno)) {                             \
        printf("  PASS | %s:%d | %s (errno=%d as expected)\n",          \
               __FILE__, __LINE__, msg, errno);                         \
        __pass++;                                                       \
    } else {                                                            \
        printf("  FAIL | %s:%d | %s | expected errno=%d got ret=%ld errno=%d (%s)\n", \
               __FILE__, __LINE__, msg, (int)(exp_errno), _r, errno,     \
               strerror(errno));                                        \
        __fail++;                                                       \
    }                                                                   \
} while (0)

#define TEST_START(name)                                                \
    do {                                                                \
        printf("================================================\n");   \
        printf("  TEST: %s\n", name);                                   \
        printf("================================================\n");   \
    } while (0)

#ifndef SYS_async_setup
#define SYS_async_setup 461
#endif
#ifndef SYS_async_read
#define SYS_async_read 462
#endif
#ifndef SYS_async_write
#define SYS_async_write 463
#endif
#ifndef SYS_async_connect
#define SYS_async_connect 466
#endif

static volatile int         handler_called;
static volatile uint64_t    handler_userdata;
static volatile int64_t     handler_result;

static void async_handler(uint64_t userdata, int64_t result)
{
    handler_userdata = userdata;
    handler_result   = result;
    handler_called   = 1;
}

static void reset_handler(void)
{
    handler_called   = 0;
    handler_userdata = 0;
    handler_result   = 0;
}

static int drain_one(void)
{
    int i;
    for (i = 0; i < 2000; i++) {
        if (handler_called)
            return 1;
        syscall(SYS_sched_yield);
    }
    return 0;
}

static int part_error_cases(void)
{
    TEST_START("async_connect immediate errors");

    CHECK_ERR(syscall(SYS_async_connect, -1, 0, 0, 0),
              EBADF, "invalid fd -1");

    int fds[2];
    CHECK_RET(pipe(fds), 0, "create pipe for ENOTSOCK test");
    CHECK_ERR(syscall(SYS_async_connect, fds[0], 0, 0, 0),
              ENOTSOCK, "pipe fd returns ENOTSOCK");
    CHECK_RET(close(fds[0]), 0, "close pipe read end");
    CHECK_RET(close(fds[1]), 0, "close pipe write end");

    return 0;
}

static int part_tcp_echo(void)
{
    TEST_START("async TCP loopback echo");

    char buf[16];
    long ret;

    ret = syscall(SYS_async_setup, (uintptr_t)async_handler);
    CHECK_RET(ret, 0, "async_setup");

    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(sock >= 0, "socket creation");
    if (sock < 0)
        return 1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(49152);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    reset_handler();
    ret = syscall(SYS_async_connect, sock,
                  (uintptr_t)&addr, sizeof(addr), 0x42);
    CHECK_RET(ret, 0, "async_connect submitted");
    CHECK(drain_one(), "async_connect completed");
    CHECK(handler_result == 0,
          "connect succeeded (result=0)");

    reset_handler();
    ret = syscall(SYS_async_write, sock, (uintptr_t)"hello", 5, -1, 0x43);
    CHECK_RET(ret, 0, "async_write submitted");
    CHECK(drain_one(), "async_write completed");
    CHECK(handler_result == 5,
          "write returned 5 bytes");

    memset(buf, 0, sizeof(buf));
    reset_handler();
    ret = syscall(SYS_async_read, sock, (uintptr_t)buf, 5, -1, 0x44);
    CHECK_RET(ret, 0, "async_read submitted");
    CHECK(drain_one(), "async_read completed");
    CHECK(handler_result == 5,
          "read returned 5 bytes");
    CHECK(memcmp(buf, "hello", 5) == 0,
          "echo data matches");

    close(sock);
    return 0;
}

int main(void)
{
    printf("================================================\n");
    printf("  StarryOS Async Connect Test Suite\n");
    printf("================================================\n");

    part_error_cases();
    part_tcp_echo();

    printf("================================================\n");
    printf("  DONE: %d pass, %d fail\n", __pass, __fail);
    printf("================================================\n");
    return __fail > 0 ? 1 : 0;
}
