#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ERR(source) (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     perror(source), kill(0, SIGKILL),               \
                     exit(EXIT_FAILURE))

volatile sig_atomic_t last_signal = 0;

void usage(char *name)
{
    fprintf(stderr, "USAGE: %s t n r b\n", name);
    fprintf(stderr, "t in [50,500]\n");
    fprintf(stderr, "n in [3,30]\n");
    fprintf(stderr, "r in [0,100]\n");
    fprintf(stderr, "b in [1,PIPE_BUF-6]\n");
    exit(EXIT_FAILURE);
}

void set_handler(void (*f)(int), int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = f;
    if (sigaction(sig, &act, NULL) < 0)
        ERR("sigaction()");
}

void sig_handler(int sig)
{
    last_signal = sig;
}

void third_generation(int wrend, int t, int n, int b)
{
    set_handler(sig_handler, SIGINT);
    srand(getpid());
    struct timespec st = {0, t * 1000000};
    int size;
    size_t offset = sizeof(size);
    char buf[PIPE_BUF];
    for (int i = 0; i < n; ++i)
    {
        if (last_signal == SIGINT)
            // interruption with C-c
            break;
        size = b + rand() % (PIPE_BUF - b - 5);
        for (int j = 0; j < size; ++j)
            buf[j + offset] = 'a' + rand() % ('z' - 'a' + 1);
        memcpy(buf, &size, offset);
        if (write(wrend, buf, size + offset) < 0)
            ERR("write()");
        nanosleep(&st, NULL);
    }
    if (close(wrend))
        ERR("close()");
}

void second_generation(int wrend, int t, int n, int r, int b)
{
    srand(getpid());
    int pipedes[2];
    if (pipe(pipedes))
        ERR("pipe()");
    switch (fork())
    {
    case -1:
        ERR("fork()");
    case 0:
        if (close(pipedes[0]))
            ERR("close()");
        third_generation(pipedes[1], t, n, b);
        exit(EXIT_SUCCESS);
    }
    if (close(pipedes[1]))
        ERR("close()");
    ssize_t status;
    int size;
    size_t offset = sizeof(size);
    char buf[PIPE_BUF];
    const char *message = "injected";
    const size_t length = sizeof(message);
    do
    {
        if ((status = read(pipedes[0], &size, offset)) < 0)
            ERR("read()");
        if (!status)
            // EOF - broken pipe
            break;
        if ((status = read(pipedes[0], buf + offset, size)) < size)
            ERR("read()");
        if (r > rand() % 100)
        {
            memcpy(buf + offset + size, message, length);
            size += length;
        }
        memcpy(buf, &size, offset);
        if (write(wrend, buf, size + offset) < 0)
            ERR("write()");
    } while (status > 0);
    if (close(pipedes[0]) || close(wrend))
        ERR("close()");
}

void first_generation(int t, int n, int r, int b)
{
    int pipedes[2];
    if (pipe(pipedes))
        ERR("pipe()");
    for (int i = 0; i < 2; ++i)
    {
        switch (fork())
        {
        case -1:
            ERR("fork()");
        case 0:
            if (close(pipedes[0]))
                ERR("close()");
            second_generation(pipedes[1], t, n, r, b);
            exit(EXIT_SUCCESS);
        }
    }
    if (close(pipedes[1]))
        ERR("close()");
    ssize_t status;
    int size;
    size_t offset = sizeof(size);
    char buf[PIPE_BUF];
    int count = 0;
    do
    {
        if ((status = read(pipedes[0], &size, offset)) < 0)
            ERR("read()");
        if (!status)
            // EOF - broken pipe
            break;
        if ((status = read(pipedes[0], buf, size)) < size)
            ERR("read()");
        buf[size] = 0;
        printf("[%d]: [%d]: [%s]\n", ++count, size, buf + offset);
    } while (status > 0);
    if (close(pipedes[0]))
        ERR("close()");
}

int main(int argc, char **argv)
{
    set_handler(SIG_IGN, SIGINT);
    if (argc != 5)
        usage(argv[0]);
    int t = atoi(argv[1]), n = atoi(argv[2]), r = atoi(argv[3]), b = atoi(argv[4]);
    if (!(50 <= t && t <= 500 && 3 <= n && n <= 30 && 0 <= r && r <= 100 && 1 <= b && b <= PIPE_BUF - 6))
        usage(argv[0]);
    first_generation(t, n, r, b);
    return EXIT_SUCCESS;
}