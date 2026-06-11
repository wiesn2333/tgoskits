#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 49152

int main(void)
{
    struct sockaddr_in addr;
    char buf[16];
    long ret;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        printf("server: socket failed (errno=%d)\n", errno);
        return 1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("server: bind failed (errno=%d)\n", errno);
        return 1;
    }
    if (listen(srv, 1) < 0) {
        printf("server: listen failed (errno=%d)\n", errno);
        return 1;
    }
    printf("server: ready on port %d\n", PORT);

    socklen_t alen = sizeof(addr);
    int cli = accept(srv, (struct sockaddr *)&addr, &alen);
    if (cli < 0) {
        printf("server: accept failed (errno=%d)\n", errno);
        return 1;
    }
    printf("server: accepted\n");

    memset(buf, 0, sizeof(buf));
    ret = read(cli, buf, sizeof(buf));
    if (ret < 0) {
        printf("server: read failed (errno=%d)\n", errno);
    } else {
        printf("server: read %ld bytes\n", ret);
    }

    ret = write(cli, buf, (size_t)(ret > 0 ? ret : 0));
    if (ret < 0) {
        printf("server: write failed (errno=%d)\n", errno);
    } else {
        printf("server: wrote %ld bytes\n", ret);
    }

    close(cli);
    close(srv);
    printf("server: done\n");
    return 0;
}
