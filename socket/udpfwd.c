#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERR(source) (perror(source),                                 \
                     fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     exit(EXIT_FAILURE))

#define MAX_CLIENTS 3
#define MAX_RULES 10
#define MAX_SIZE 100

struct udp_table
{
    int udp_socket;
    struct sockaddr_in *addr;
    int size;
};

volatile sig_atomic_t do_work = 1;

void my_close(uint16_t port, struct udp_table *udp_rules)
{
    port = htons(port);
    for (int i = 0; i < MAX_RULES; ++i)
    {
        if (-1 == udp_rules[i].udp_socket)
            continue;
        struct sockaddr_in addr;
        socklen_t size = sizeof addr;
        if (getsockname(udp_rules[i].udp_socket, &addr, &size))
            ERR("getsockname()");
        if (addr.sin_port == port)
        {
            if (TEMP_FAILURE_RETRY(close(udp_rules[i].udp_socket)))
                ERR("close");
            udp_rules[i].udp_socket = -1;
            free(udp_rules[i].addr);
            udp_rules[i].addr = NULL;
            udp_rules[i].size = 0;
            return;
        }
    }
}

struct sockaddr_in make_address(char *address, char *port)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    struct addrinfo *result;
    int ret = getaddrinfo(address, port, &hints, &result);
    if (ret)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in addr = *(struct sockaddr_in *)(result->ai_addr);
    freeaddrinfo(result);
    return addr;
}

char *trim_whitespace(char *str)
{
    char *end;
    while (isspace((unsigned char)*str))
        ++str;
    if (!*str)
        return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        --end;
    end[1] = '\0';
    return str;
}

int make_socket(int domain, int type)
{
    int socketfd = socket(domain, type, 0);
    if (socketfd < 0)
        ERR("socket");
    return socketfd;
}

int bind_inet_socket(uint16_t port, int type)
{
    int socketfd = make_socket(PF_INET, type);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int t = 1;
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof t))
        ERR("setsockopt");
    if (bind(socketfd, (struct sockaddr *)&addr, sizeof addr))
        ERR("bind");
    if (SOCK_STREAM == type)
        if (listen(socketfd, MAX_CLIENTS))
            ERR("listen");
    return socketfd;
}

void fwd(uint16_t port, struct udp_table *udp_rules)
{
    bool exists = false;
    int j = -1;
    uint16_t lport = htons(port);
    for (int i = 0; i < MAX_RULES; ++i)
    {
        struct udp_table udp_rule = udp_rules[i];
        if (-1 == udp_rule.udp_socket)
        {
            if (-1 == j)
                j = i;
        }
        else
        {
            struct sockaddr_in addr;
            socklen_t size = sizeof addr;
            if (getsockname(udp_rule.udp_socket, &addr, &size))
                ERR("getsockname()");
            if (addr.sin_port == lport)
            {
                exists = true;
                j = i;
                break;
            }
        }
    }

    if (-1 == j || MAX_RULES == j)
        return;

    if (exists)
    {
        free(udp_rules[j].addr);
        udp_rules[j].addr = NULL;
        udp_rules[j].size = 0;
    }
    else
        udp_rules[j].udp_socket = bind_inet_socket(port, SOCK_DGRAM);

    for (;;)
    {
        char *address = strtok(NULL, ":");
        if (!address)
            break;
        address = trim_whitespace(address);
        char *udp_port = strtok(NULL, " ");
        if (!udp_port)
            break;
        udp_port = trim_whitespace(udp_port);

        if (!udp_rules[j].size)
        {
            udp_rules[j].addr = malloc(sizeof *udp_rules[j].addr);
            if (!udp_rules[j].addr)
                ERR("malloc");
        }
        else
        {
            udp_rules[j].addr = realloc(udp_rules[j].addr, (udp_rules[j].size + 1) * sizeof *udp_rules[j].addr);
            if (!udp_rules[j].addr)
                ERR("realloc");
        }
        udp_rules[j].addr[udp_rules[j].size++] = make_address(address, udp_port);
    }
}

void parse(char *buf, struct udp_table *udp_rules)
{
    char *cmd = strtok(buf, " ");
    if (!cmd)
        return;
    char *lport = strtok(NULL, " ");
    if (!lport)
        return;
    uint16_t port = atoi(lport);
    if (!strcmp(cmd, "fwd"))
        return fwd(port, udp_rules);
    if (!strcmp(cmd, "close"))
        return my_close(port, udp_rules);
}

ssize_t bulk_read(int fd, char *buf, size_t count)
{
    int c;
    size_t len = 0;
    do
    {
        c = TEMP_FAILURE_RETRY(read(fd, buf, count));
        if (c < 0)
            return c;
        if (0 == c)
            return len;
        buf += c;
        len += c;
        count -= c;
    } while (count > 0);
    return len;
}

ssize_t bulk_write(int fd, char *buf, size_t count)
{
    int c;
    size_t len = 0;
    do
    {
        c = TEMP_FAILURE_RETRY(write(fd, buf, count));
        if (c < 0)
            return c;
        buf += c;
        len += c;
        count -= c;
    } while (count > 0);
    return len;
}

void communicate(int socketfd, bool deny)
{
    char buf[MAX_SIZE];
    if (deny)
        snprintf(buf, sizeof buf, "At most %d connections are allowed!\n", MAX_CLIENTS);
    else
        snprintf(buf, sizeof buf, "Hello\n");
    if (bulk_write(socketfd, buf, strlen(buf)) < 0 && errno != EPIPE)
        ERR("write");
}

int add_new_client(int socketfd)
{
    int fd = TEMP_FAILURE_RETRY(accept(socketfd, NULL, NULL));
    if (fd < 0)
    {
        if (EAGAIN == errno || EWOULDBLOCK == errno)
            return -1;
        ERR("accept");
    }
    return fd;
}

void do_server(int tcp_socket)
{
    int tcp_clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; ++i)
        tcp_clients[i] = -1;

    struct udp_table udp_rules[MAX_RULES];
    for (int i = 0; i < MAX_RULES; ++i)
    {
        udp_rules[i].udp_socket = -1;
        udp_rules[i].addr = NULL;
        udp_rules[i].size = 0;
    }

    fd_set readfds;
    sigset_t old_mask, mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);
    char buf[MAX_SIZE];

    while (do_work)
    {
        FD_ZERO(&readfds);
        FD_SET(tcp_socket, &readfds);
        int maxfd = tcp_socket;
        for (int i = 0; i < MAX_CLIENTS + MAX_RULES; ++i)
        {
            int fd = -1;
            if (i < MAX_CLIENTS)
                fd = tcp_clients[i];
            else
                fd = udp_rules[i - MAX_CLIENTS].udp_socket;
            if (fd != -1)
            {
                FD_SET(fd, &readfds);
                if (fd > maxfd)
                    maxfd = fd;
            }
        }

        if (pselect(maxfd + 1, &readfds, NULL, NULL, NULL, &old_mask) > 0)
        {
            if (FD_ISSET(tcp_socket, &readfds))
            {
                int fd = add_new_client(tcp_socket);
                if (fd != -1)
                {
                    int i = 0;
                    for (; i < MAX_CLIENTS; ++i)
                        if (-1 == tcp_clients[i])
                        {
                            communicate(fd, false);
                            tcp_clients[i] = fd;
                            break;
                        }
                    if (MAX_CLIENTS == i)
                    {
                        communicate(fd, true);
                        if (TEMP_FAILURE_RETRY(close(fd)))
                            ERR("close");
                    }
                }
            }

            else
            {
                for (int i = 0; i < MAX_CLIENTS; ++i)
                {
                    int fd = tcp_clients[i];
                    if (FD_ISSET(fd, &readfds))
                    {
                        int count = TEMP_FAILURE_RETRY(read(fd, buf, sizeof buf));
                        if (count > 0)
                        {
                            int len = strchr(buf, '\n') - buf + 2;
                            buf[len - 1] = '\0';
                            parse(buf, udp_rules);
                        }
                        else if (!count)
                        {
                            if (TEMP_FAILURE_RETRY(close(fd)))
                                ERR("close");
                            tcp_clients[i] = -1;
                        }
                        else
                            ERR("read");
                    }
                }

                for (int i = 0; i < MAX_RULES; ++i)
                {
                    int fd = udp_rules[i].udp_socket;
                    if (FD_ISSET(fd, &readfds))
                    {
                        int count = TEMP_FAILURE_RETRY(recv(fd, buf, sizeof buf, 0));
                        if (count > 0)
                        {
                            int len = strchr(buf, '\n') - buf + 2;
                            buf[len - 1] = '\0';
                            for (int j = 0; j < udp_rules[i].size; ++j)
                                if (TEMP_FAILURE_RETRY(sendto(fd, buf, len, 0,
                                                              (struct sockaddr *)&udp_rules[i].addr[j], sizeof udp_rules[i].addr[j])) < 0)
                                    ERR("sendto");
                        }
                        else if (!count)
                        {
                            if (TEMP_FAILURE_RETRY(close(fd)))
                                ERR("close");
                            udp_rules[i].udp_socket = -1;
                            free(udp_rules[i].addr);
                            udp_rules[i].addr = NULL;
                            udp_rules[i].size = 0;
                        }
                        else
                            ERR("recv");
                    }
                }
            }
        }
        else if (errno != EINTR)
            ERR("pselect");
    }
    sigprocmask(SIG_UNBLOCK, &mask, NULL);

    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        int fd = tcp_clients[i];
        if (fd != -1)
            if (TEMP_FAILURE_RETRY(close(fd)))
                ERR("close");
    }

    for (int i = 0; i < MAX_RULES; ++i)
    {
        int fd = udp_rules[i].udp_socket;
        if (fd != -1)
        {
            if (TEMP_FAILURE_RETRY(close(fd)))
                ERR("close");
            free(udp_rules[i].addr);
        }
    }
}

void sigint_handler(int sig)
{
    do_work = 0;
}

void set_handler(void (*f)(int), int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof act);
    act.sa_handler = f;
    if (sigaction(sig, &act, NULL))
        ERR("sigaction");
}

void usage(char *name)
{
    fprintf(stderr, "USAGE: %s port\n", name);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
        usage(argv[0]);
    set_handler(SIG_IGN, SIGPIPE);
    set_handler(sigint_handler, SIGINT);
    int tcp_socket = bind_inet_socket(atoi(argv[1]), SOCK_STREAM);
    do_server(tcp_socket);
    if (TEMP_FAILURE_RETRY(close(tcp_socket)))
        ERR("close");
    return EXIT_SUCCESS;
}