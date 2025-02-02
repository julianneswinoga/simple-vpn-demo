#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common.h"

#define MTU 1400

#define SOCKET_PATH       "/tmp/vm_port3"
#define TUN_INTERFACE     "tun_client"
#define INTERFACE_ADDRESS "10.8.0.2/16"

/*
 * Create VPN interface /dev/tun0 and return a fd
 */
static int tun_alloc(void)
{
    printf("Creating tun interface %s\n", TUN_INTERFACE);

    const int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("Cannot open /dev/net/tun");
        return fd;
    }

    struct ifreq ifr = {
        .ifr_flags = IFF_TUN | IFF_NO_PI,
    };
    strncpy(ifr.ifr_name, TUN_INTERFACE, IFNAMSIZ);

    const int e = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (e < 0) {
        perror("ioctl[TUNSETIFF]");
        close(fd);
        return e;
    }

    return fd;
}

/*
 * Execute commands
 */
static void run(char *cmd, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, cmd);
    vsnprintf(buf, sizeof(buf), cmd, args);
    va_end(args);

    printf("Execute `%s`\n", buf);
    if (system(buf)) {
        perror(buf);
        exit(1);
    }
}

/*
 * Setup route table via `iptables` & `ip route`
 */
static void setup_route_table(void)
{
    printf("Adding routing tables\n");

    run("ip route add 192.168.1.191/32 dev %s proto static", TUN_INTERFACE);
    run("echo 0 > /proc/sys/net/ipv4/conf/%s/rp_filter", TUN_INTERFACE);
    run("echo 1 > /proc/sys/net/ipv4/conf/%s/accept_local", TUN_INTERFACE);
}

/*
 * Cleanup route table
 */
static void cleanup_route_table(void)
{
    // Routes automatically get deleted when interface is deleted
}

/*
 * Catch Ctrl-C and `kill`s, make sure route table gets cleaned before this process exit
 */
static void cleanup(int signo)
{
    printf("Exiting....\n");
    if (signo == SIGHUP || signo == SIGINT || signo == SIGTERM) {
        cleanup_route_table();
        exit(0);
    }
}

static void cleanup_when_sig_exit(void)
{
    struct sigaction sa = {
        .sa_handler = &cleanup,
        .sa_flags   = SA_RESTART,
    };
    sigfillset(&sa.sa_mask);

    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        perror("Cannot handle SIGHUP");
    }
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("Cannot handle SIGINT");
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("Cannot handle SIGTERM");
    }
}

static bool tunToSkt(fd_set readSet, int tunFd, char *tunBuf, int sktFd, char *sktBuf)
{
    if (!FD_ISSET(tunFd, &readSet)) {
        printf("tunToSkt: FD not set: %i\n", tunFd);
        return true;
    }
    const ssize_t tunBytesRead = read(tunFd, tunBuf, MTU);
    if (tunBytesRead < 0) {
        // TODO: ignore some errno
        perror("read from tunFd error");
        return false;
    }
    const uint8_t ipVersion = tunBuf[0] >> 4;
    if (ipVersion != 4) {
        printf("Ignoring IPv%u packet\n", ipVersion);
        return true;
    }

    memcpy(sktBuf, tunBuf, tunBytesRead);
    printf("%zu>", tunBytesRead);
    fflush(stdout);

    const ssize_t bytesWritten = write(sktFd, sktBuf, tunBytesRead);
    if (bytesWritten < 0) {
        // TODO: ignore some errno
        perror("write sktFd error");
        return false;
    }
    for (int i = 0; i < tunBytesRead; i++) {
        printf("%02hhx", sktBuf[i]);
    }
    printf("\n");
    fflush(stdout);

    return true;
}

static bool sktToTun(int sktFd, char *sktBuf, int tunFd, char *tunBuf)
{
    printf("sktToTun: Start read()\n");
    ssize_t sktBytesRead = read(sktFd, sktBuf, 256);
    printf("sktToTun: done read()\n");
    if (sktBytesRead < 0) {
        printf("Error reading: %zi\n", sktBytesRead);
        return 1;
    }

    memcpy(tunBuf, sktBuf, sktBytesRead);
    printf("%zu<", sktBytesRead);
    fflush(stdout);

    printf("sktToTun: Start write()\n");
    const ssize_t bytesWritten = write(tunFd, tunBuf, sktBytesRead);
    printf("sktToTun: done write()\n");
    if (bytesWritten < 0) {
        // TODO: ignore some errno
        perror("write tun_fd error");
        printf("%i %zi\n", tunFd, sktBytesRead);
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    UNUSED(argc, argv);

    printf("Client startup, server=%s\n", SOCKET_PATH);

    const int tun_fd = tun_alloc();
    if (tun_fd < 0) {
        return 1;
    }
    // Set tun as nonblocking
    if (fcntl(tun_fd, F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl(tun_fd) error");
        return 1;
    }

    // Set interface address and MTU
    run("ifconfig %s %s mtu %d up", TUN_INTERFACE, INTERFACE_ADDRESS, MTU);
    setup_route_table();
    cleanup_when_sig_exit();

    const int skt_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (skt_fd < 0) {
        printf("socket() failed\n");
        return 1;
    }
    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
        .sun_path   = SOCKET_PATH,
    };

    // Set socket as nonblocking
    if (fcntl(skt_fd, F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl(skt_fd) error");
        return 1;
    }
    if (connect(skt_fd, (struct sockaddr *) &address, sizeof(struct sockaddr_un)) != 0) {
        printf("connect() failed\n");
        return 1;
    }

    /*
     * tun_buf - memory buffer read from/write to tun dev - is always plain
     * skt_buf - memory buffer read from/write to socket fd - is always encrypted
     */
    char tun_buf[MTU];
    bzero(tun_buf, MTU);
    char skt_buf[MTU];
    bzero(skt_buf, MTU);

    while (true) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(tun_fd, &readSet);
        const int max_fd = tun_fd + 1;

        printf("Start select\n");
        if (-1 == select(max_fd, &readSet, NULL, NULL, NULL)) {
            perror("select error");
            break;
        }

        printf("Start tunToSkt\n");
        if (!tunToSkt(readSet, tun_fd, tun_buf, skt_fd, skt_buf)) {
            printf("tunToSkt error\n");
            break;
        }
        printf("Start sktToTun\n");
        if (!sktToTun(skt_fd, skt_buf, tun_fd, tun_buf)) {
            printf("sktToTun error\n");
            break;
        }
        printf("Done\n");
    }

    close(tun_fd);
    close(skt_fd);

    cleanup_route_table();

    return 0;
}
