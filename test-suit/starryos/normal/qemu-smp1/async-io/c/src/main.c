#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* syscall numbers */
#ifndef SYS_async_setup
#define SYS_async_setup 461
#endif
#ifndef SYS_async_read
#define SYS_async_read 462
#endif
#ifndef SYS_async_write
#define SYS_async_write 463
#endif

#define MAX_TMP 16

/* handler state — kernel delivers one CQ entry at a time */
static volatile int         handler_called;
static volatile int         handler_count;
static volatile uint64_t    handler_userdata[MAX_TMP];
static volatile int64_t     handler_result[MAX_TMP];

static void async_handler(uint64_t userdata, int64_t result)
{
    int idx = handler_count;
    if (idx < MAX_TMP) {
        handler_userdata[idx] = userdata;
        handler_result[idx] = result;
    }
    handler_count++;
    handler_called = 1;
}

static void reset_handler(void)
{
    int i;
    handler_called = 0;
    handler_count = 0;
    for (i = 0; i < MAX_TMP; i++) {
        handler_userdata[i] = 0;
        handler_result[i]  = 0;
    }
}

/* spin-wait: yield until handler fires (max 200 iterations).
   Caller must set handler_called = 0 before calling. */
static void drain_completions(void)
{
    int i;
    for (i = 0; i < 200; i++) {
        if (handler_called)
            return;
        syscall(SYS_sched_yield);
    }
}

/* ───── part 1: async_setup error cases ───── */
static int part_setup_errors(void)
{
    TEST_START("async_setup error handling");

    CHECK_ERR(syscall(SYS_async_setup, 0),
              EINVAL, "rejects null handler");

    return 0;
}

/* ───── part 2: async_read immediate error returns ───── */
static int part_read_errors(void)
{
    TEST_START("async_read immediate error returns");

    CHECK_ERR(syscall(SYS_async_read, -1, 0, 0, -1, 0),
              EBADF, "invalid fd -1");
    CHECK_ERR(syscall(SYS_async_read, 999, (uintptr_t)"x", 1, -1, 0),
              EBADF, "unopened fd 999");
    CHECK_ERR(syscall(SYS_async_write, -1, 0, 0, -1, 0),
              EBADF, "async_write with bad fd");

    int fds[2];
    CHECK_RET(pipe(fds), 0, "create pipe for error tests");
    CHECK_ERR(syscall(SYS_async_read, fds[0], 0, 4, -1, 0),
              EFAULT, "null buf returns EFAULT");
    CHECK_RET(close(fds[0]), 0, "close read end");
    CHECK_RET(close(fds[1]), 0, "close write end");

    return 0;
}

/* ───── part 3: async read from pipe (data pre-written) ───── */
static int part_pipe_read(void)
{
    TEST_START("async read from pipe");

    int fds[2];
    CHECK_RET(pipe(fds), 0, "create pipe");

    int fl = fcntl(fds[0], F_GETFL);
    CHECK(fl >= 0, "get flags");
    CHECK_RET(fcntl(fds[0], F_SETFL, fl | O_NONBLOCK), 0,
              "set read end nonblocking");

    const char *data = "HelloAsync";
    CHECK_RET(write(fds[1], data, 10), 10, "pre-write data to pipe");

    char buf[64];
    memset(buf, 0, sizeof(buf));
    reset_handler();
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)buf, 10, -1, 0x42),
              0, "async_read submitted");

    drain_completions();

    CHECK(handler_called, "completion handler called");
    CHECK(handler_count == 1, "1 completion delivered");
    CHECK(handler_userdata[0] == 0x42, "userdata preserved");
    CHECK(handler_result[0] == 10, "result = 10 bytes");
    CHECK(memcmp(buf, "HelloAsync", 10) == 0, "buf content matches");

    CHECK_RET(close(fds[0]), 0, "close read end");
    CHECK_RET(close(fds[1]), 0, "close write end");
    return 0;
}

/* ───── part 4: async write + async read pipe roundtrip ───── */
static int part_write_read(void)
{
    TEST_START("async write then async read");

    int fds[2];
    CHECK_RET(pipe(fds), 0, "create pipe");

    int fl;
    fl = fcntl(fds[0], F_GETFL);
    CHECK_RET(fcntl(fds[0], F_SETFL, fl | O_NONBLOCK), 0,
              "set read nonblocking");
    fl = fcntl(fds[1], F_GETFL);
    CHECK_RET(fcntl(fds[1], F_SETFL, fl | O_NONBLOCK), 0,
              "set write nonblocking");

    const char *data = "AsyncIO";
    reset_handler();
    CHECK_RET(syscall(SYS_async_write, fds[1], (uintptr_t)data, 7, -1, 0x99),
              0, "async_write submitted");
    drain_completions();
    CHECK(handler_called, "write completion handler called");
    CHECK(handler_count == 1, "one write completion");
    CHECK(handler_result[0] == 7, "write completed 7 bytes");

    char buf[64];
    memset(buf, 0, sizeof(buf));
    reset_handler();
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)buf, 7, -1, 0xAA),
              0, "async_read submitted");
    drain_completions();
    CHECK(handler_called, "read completion handler called");
    CHECK(handler_count == 1, "one read completion");
    CHECK(handler_result[0] == 7, "read completed 7 bytes");
    CHECK(memcmp(buf, "AsyncIO", 7) == 0, "read data matches written");

    CHECK_RET(close(fds[0]), 0, "close read end");
    CHECK_RET(close(fds[1]), 0, "close write end");
    return 0;
}

/* ───── part 5: three concurrent reads batched ───── */
static int part_batch(void)
{
    TEST_START("batch completions");

    int fds[2];
    CHECK_RET(pipe(fds), 0, "create pipe");
    int fl = fcntl(fds[0], F_GETFL);
    CHECK_RET(fcntl(fds[0], F_SETFL, fl | O_NONBLOCK), 0,
              "set read nonblocking");

    unsigned char wbuf[64];
    unsigned int i;
    memset(wbuf, 0, sizeof(wbuf));
    CHECK_RET(write(fds[1], wbuf, sizeof(wbuf)), 64,
              "pre-write 64 bytes");

    unsigned char b1[16], b2[16], b3[32];
    memset(b1, 0, sizeof(b1));
    memset(b2, 0, sizeof(b2));
    memset(b3, 0, sizeof(b3));

    reset_handler();
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)b1, 16, -1, 1),
              0, "async_read #1 (16b)");
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)b2, 16, -1, 2),
              0, "async_read #2 (16b)");
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)b3, 32, -1, 3),
              0, "async_read #3 (32b)");

    /* drain up to all 3 completions */
    int total = 0;
    while (total < 3) {
        drain_completions();
        if (handler_called) {
            total = handler_count;
            handler_called = 0;
        }
    }
    printf("  INFO | total completions: %d\n", total);

    CHECK(total == 3, "all 3 completions received");
    unsigned int seen[4] = {0};
    for (i = 0; i < (unsigned int)total && i < MAX_TMP; i++) {
        uint64_t ud = handler_userdata[i];
        int64_t  r  = handler_result[i];
        int exp = 0;
        if (ud == 1) { exp = 16; }
        else if (ud == 2) { exp = 16; }
        else if (ud == 3) { exp = 32; }
        if (ud >= 1 && ud <= 3 && !seen[ud]) {
            seen[ud] = 1;
            if (r == exp)
                printf("  PASS | userdata=%lu result=%ld OK\n", ud, r);
            else {
                printf("  FAIL | userdata=%lu result=%ld expected=%d\n",
                       ud, (long)r, exp);
                __fail++;
            }
        } else {
            printf("  FAIL | unexpected userdata=%lu\n", ud);
            __fail++;
        }
    }
    /* data integrity: pipe consumed all bytes, verify by sum */
    int64_t total_bytes = 0;
    for (i = 0; i < (unsigned int)total && i < MAX_TMP; i++)
        total_bytes += handler_result[i];
    CHECK(total_bytes == 64, "total bytes read = 64");

    CHECK_RET(close(fds[0]), 0, "close read end");
    CHECK_RET(close(fds[1]), 0, "close write end");
    return 0;
}

/* ───── part 6: CQ wrap with many requests ───── */
static int part_wrap(void)
{
    TEST_START("CQ wrap-around");

    int fds[2];
    CHECK_RET(pipe(fds), 0, "create pipe");
    int fl = fcntl(fds[0], F_GETFL);
    CHECK_RET(fcntl(fds[0], F_SETFL, fl | O_NONBLOCK), 0,
              "set read nonblocking");

    unsigned char wbuf[256];
    unsigned int i;
    for (i = 0; i < sizeof(wbuf); i++)
        wbuf[i] = (unsigned char)i;
    CHECK_RET(write(fds[1], wbuf, sizeof(wbuf)), 256,
              "pre-write 256 bytes");

    unsigned char bufs[4][64];
    reset_handler();
    for (i = 0; i < 4; i++) {
        memset(bufs[i], 0, sizeof(bufs[i]));
        CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)bufs[i], 64, -1,
                          (uint64_t)(100 + i)),
                  0, "async_read submitted (64b)");
    }

    /* drain all 4 completions (256 bytes = pipe capacity) */
    int total = 0;
    while (total < 4) {
        drain_completions();
        if (handler_called) {
            total = handler_count;
            handler_called = 0;
        }
    }
    printf("  INFO | total completions: %d\n", total);

    CHECK(total == 4, "all 4 completions received");
    /* data integrity: pipe consumed all bytes, verify by sum */
    int64_t total_bytes_w = 0;
    for (i = 0; i < (unsigned int)total && i < MAX_TMP; i++)
        total_bytes_w += handler_result[i];
    CHECK(total_bytes_w == 256, "total bytes read = 256");

    CHECK_RET(close(fds[0]), 0, "close read end");
    CHECK_RET(close(fds[1]), 0, "close write end");
    return 0;
}

/* ───── part N: handler-return correctness ───── */
/* Verify that after the handler fires and returns, the program can
   continue normally (subsequent syscalls, stack ops, etc.).
   Uses sched_yield() instead of usleep() to avoid timer dependency. */
static int part_handler_return(void)
{
    TEST_START("handler return correctness");

    int fds[2];
    CHECK_RET(pipe(fds), 0, "create pipe");
    int fl = fcntl(fds[0], F_GETFL);
    CHECK_RET(fcntl(fds[0], F_SETFL, fl | O_NONBLOCK), 0,
              "set read end nonblocking");

    /* ---- round 1: single async read ---- */
    const char *data = "RoundOne";
    CHECK_RET(write(fds[1], data, 8), 8, "pre-write 8 bytes");

    char buf1[64];
    memset(buf1, 0, sizeof(buf1));
    reset_handler();
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)buf1, 8, -1, 0x77),
              0, "async_read #1 submitted");

    /* spin-wait: yield until handler fires (max ~5000 yields) */
    int spins = 0;
    while (!handler_called && spins < 5000) {
        syscall(SYS_sched_yield);
        spins++;
    }
    printf("  INFO | round1 spins=%d\n", spins);

    CHECK(handler_called, "round1: handler called");
    CHECK(handler_count == 1, "round1: 1 completion");
    CHECK(handler_userdata[0] == 0x77, "round1: userdata=0x77");
    CHECK(handler_result[0] == 8, "round1: result=8 bytes");
    CHECK(memcmp(buf1, "RoundOne", 8) == 0, "round1: data matches");

    /* After first handler returns, verify normal syscall still works */
    long pid1 = syscall(SYS_getpid);
    printf("  INFO | round1: getpid returned %ld\n", pid1);
    CHECK(pid1 > 0, "round1: getpid works after handler");

    /* ---- round 2: another async read on same pipe ---- */
    const char *data2 = "RoundTwo";
    CHECK_RET(write(fds[1], data2, 8), 8, "pre-write 8 bytes for round2");

    char buf2[64];
    memset(buf2, 0, sizeof(buf2));
    reset_handler();
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)buf2, 8, -1, 0x88),
              0, "async_read #2 submitted");

    spins = 0;
    while (!handler_called && spins < 5000) {
        syscall(SYS_sched_yield);
        spins++;
    }
    printf("  INFO | round2 spins=%d\n", spins);

    CHECK(handler_called, "round2: handler called");
    CHECK(handler_count == 1, "round2: 1 completion");
    CHECK(handler_userdata[0] == 0x88, "round2: userdata=0x88");
    CHECK(handler_result[0] == 8, "round2: result=8 bytes");
    CHECK(memcmp(buf2, "RoundTwo", 8) == 0, "round2: data matches");

    /* Verify second getpid works too */
    long pid2 = syscall(SYS_getpid);
    printf("  INFO | round2: getpid returned %ld (expected %ld)\n", pid2, pid1);
    CHECK(pid2 == pid1, "round2: pid unchanged after second handler");

    /* ---- round 3: overlapping 3 reads to test CQ batching ---- */
    unsigned char wbuf3[48];
    memset(wbuf3, 0x10, sizeof(wbuf3));
    CHECK_RET(write(fds[1], wbuf3, sizeof(wbuf3)), 48,
              "pre-write 48 bytes for round3");

    unsigned char b3a[16], b3b[16], b3c[16];
    memset(b3a, 0, sizeof(b3a));
    memset(b3b, 0, sizeof(b3b));
    memset(b3c, 0, sizeof(b3c));

    reset_handler();
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)b3a, 16, -1, 0x31),
              0, "async_read #3a (16b)");
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)b3b, 16, -1, 0x32),
              0, "async_read #3b (16b)");
    CHECK_RET(syscall(SYS_async_read, fds[0], (uintptr_t)b3c, 16, -1, 0x33),
              0, "async_read #3c (16b)");

    int got = 0;
    while (got < 3) {
        handler_called = 0;
        spins = 0;
        while (!handler_called && spins < 5000) {
            syscall(SYS_sched_yield);
            spins++;
        }
        if (handler_called)
            got = handler_count;
    }
    printf("  INFO | round3 completions: %d\n", got);

    CHECK(got == 3, "round3: all 3 completions received");
    /* data integrity: pipe consumed all bytes, verify by sum */
    unsigned int ri;
    int64_t total_bytes_r3 = 0;
    for (ri = 0; ri < (unsigned int)got && ri < MAX_TMP; ri++)
        total_bytes_r3 += handler_result[ri];
    CHECK(total_bytes_r3 == 48, "round3: total bytes read = 48");

    /* Final continuity check */
    long pid3 = syscall(SYS_getpid);
    printf("  INFO | round3: getpid returned %ld (expected %ld)\n", pid3, pid1);
    CHECK(pid3 == pid1, "round3: pid still unchanged");

    CHECK_RET(close(fds[0]), 0, "close read end");
    CHECK_RET(close(fds[1]), 0, "close write end");
    return 0;
}

/* ───── part 7: second async_setup should fail ───── */
static int part_double_setup(void)
{
    TEST_START("double async_setup rejected");
    CHECK_ERR(syscall(SYS_async_setup, (uintptr_t)async_handler),
              EEXIST, "second async_setup fails with EEXIST");
    return 0;
}

int main(void)
{
    printf("================================================\n");
    printf("  StarryOS Async I/O Test Suite\n");
    printf("================================================\n");

    /* part 1: error cases before CQ exists */
    if (part_setup_errors()) return 1;

    /* create main CQ */
    if (syscall(SYS_async_setup, (uintptr_t)async_handler) != 0) {
        printf("FATAL: async_setup failed: %s\n", strerror(errno));
        return 1;
    }
    printf("  CQ created\n\n");

    /* remaining parts */
    part_double_setup();
    part_read_errors();
    part_pipe_read();
    part_write_read();
    part_batch();
    part_wrap();
    part_handler_return();

    printf("================================================\n");
    printf("  DONE: %d pass, %d fail\n", __pass, __fail);
    printf("================================================\n");
    return __fail > 0 ? 1 : 0;
}
