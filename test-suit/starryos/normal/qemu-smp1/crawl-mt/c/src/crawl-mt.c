#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SERVER_IP "10.0.2.2"
#define SERVER_PORT 8080
#define RETRY_MAX 3
#define RETRY_SLEEP_S 1

static double *c_times;
static double *rw_times;
static volatile int conn_ok;

static double ns_to_s(long sec, long nsec)
{
    return (double)sec + (double)nsec * 1e-9;
}

static int connect_with_retry(struct sockaddr_in *addr)
{
    for (int attempt = 0;; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        if (connect(fd, (struct sockaddr *)addr, sizeof(*addr)) == 0)
            return fd;

        int e = errno;
        close(fd);
        if (e != ECONNREFUSED || attempt >= RETRY_MAX - 1)
            return -1;
        sleep(RETRY_SLEEP_S);
    }
}

static void *worker(void *arg)
{
    long idx = (long)arg;
    struct timespec t0, t1, t2;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    int fd = connect_with_retry(&addr);
    if (fd < 0) {
        printf("  FAIL crawl-mt: worker %ld connect errno=%d\n", idx, errno);
        return NULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    c_times[idx] = ns_to_s(t1.tv_sec - t0.tv_sec, t1.tv_nsec - t0.tv_nsec);

    char msg[32];
    int msg_len = snprintf(msg, sizeof(msg), "%ld", idx);
    char buf[128];
    write(fd, msg, (size_t)msg_len);
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    rw_times[idx] = ns_to_s(t2.tv_sec - t1.tv_sec, t2.tv_nsec - t1.tv_nsec);

    if (r > 0) {
        buf[r] = 0;
        while (read(fd, buf, sizeof(buf)) > 0);
    }

    __sync_fetch_and_add(&conn_ok, 1);
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc < 2) {
        printf("usage: crawl-mt <num_connections>\n");
        return 1;
    }
    int n = atoi(argv[1]);
    if (n < 1 || n > 4096) {
        printf("num_connections must be 1..4096\n");
        return 1;
    }

    c_times = calloc((size_t)n, sizeof(double));
    rw_times = calloc((size_t)n, sizeof(double));
    conn_ok = 0;

    pthread_t *tids = calloc((size_t)n, sizeof(pthread_t));

    struct timespec wall0;
    clock_gettime(CLOCK_MONOTONIC, &wall0);

    for (long i = 0; i < n; i++) {
        c_times[i] = -1;
        rw_times[i] = -1;
        pthread_create(&tids[i], NULL, worker, (void *)i);
    }
    for (int i = 0; i < n; i++)
        pthread_join(tids[i], NULL);

    struct timespec wall1;
    clock_gettime(CLOCK_MONOTONIC, &wall1);
    double wall = ns_to_s(wall1.tv_sec - wall0.tv_sec, wall1.tv_nsec - wall0.tv_nsec);

    double c_min = 999, c_max = 0, c_sum = 0;
    double rw_min = 999, rw_max = 0, rw_sum = 0;
    int valid = 0;
    for (int i = 0; i < n; i++) {
        if (rw_times[i] < 0 || c_times[i] < 0) continue;
        valid++;
        if (c_times[i] < c_min) c_min = c_times[i];
        if (c_times[i] > c_max) c_max = c_times[i];
        c_sum += c_times[i];
        if (rw_times[i] < rw_min) rw_min = rw_times[i];
        if (rw_times[i] > rw_max) rw_max = rw_times[i];
        rw_sum += rw_times[i];
    }

    printf("BENCH crawl-mt: %d conns, %.2f s, %.1f conns/s\n",
           conn_ok, wall, conn_ok / wall);
    if (valid > 0) {
        printf("  | connect min %.4f | max %.4f | avg %.4f\n",
               c_min, c_max, c_sum / valid);
        printf("  | write+read min %.4f | max %.4f | avg %.4f\n",
               rw_min, rw_max, rw_sum / valid);
    }
    printf("BENCH crawl-mt: done\n");

    free(tids);
    free(c_times);
    free(rw_times);
    return conn_ok == n ? 0 : 1;
}
