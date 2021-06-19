#define _GNU_SOURCE
#include <errno.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MSGSIZE 50
#define REGISTER 0
#define STATUS 1

#define ERR(source) (perror(source),                                 \
                     fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     exit(EXIT_FAILURE))

volatile sig_atomic_t last_signal = 0;

void usage(char *name)
{
    fprintf(stderr, "USAGE: %s q0_name t\n", name);
    fprintf(stderr, "USAGE: q0_name matches \"/[A-Za-z0-9._-]+\"\n");
    fprintf(stderr, "USAGE: t belongs to [100, 2000]\n");
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

void set_timeout(struct timespec *st, int t)
{
    clock_gettime(CLOCK_REALTIME, st);
    if (t >= 1000)
        st->tv_sec += t / 1000;
    else
    {
        int total = st->tv_nsec + t * 1000000;
        int nsec = 1000000000 - total;
        if (nsec > 0)
            st->tv_nsec = total;
        else
        {
            st->tv_sec += 1;
            st->tv_nsec = -nsec;
        }
    }
}

void process_messages(mqd_t mqdes1, mqd_t mqdes2, int t)
{
    int pid = getpid();
    srand(pid);

    for (int i = 0;; ++i)
    {
        int value = rand() % 2;

        char buf[MSGSIZE];
        struct timespec st;
        set_timeout(&st, t);

        if (mq_timedreceive(mqdes2, buf, sizeof(buf), NULL, &st) == -1)
        {
            if (ETIMEDOUT == errno)
                continue;
            if (EINTR == errno && SIGINT == last_signal)
                break;
            ERR("mq_timedreceive");
        }

        printf("%s\n", buf);
        fflush(stdout);

        snprintf(buf, sizeof(buf), "status %d %d [%d]", pid, value, i);
        TEMP_FAILURE_RETRY(mq_send(mqdes1, buf, sizeof(buf), STATUS));
    }
}

int main(int argc, char **argv)
{
    set_handler(sig_handler, SIGINT);

    if (argc != 3)
        usage(argv[0]);

    int t = strtol(argv[2], NULL, 10);
    if (t < 100 || t > 2000)
        usage(argv[0]);

    sleep(1);
    mqd_t mqdes1 = mq_open(argv[1], O_WRONLY);
    if (-1 == mqdes1)
        ERR("mq_open");

    struct mq_attr attr;
    if (mq_getattr(mqdes1, &attr))
        ERR("mq_getattr");
    if (attr.mq_msgsize != MSGSIZE)
        exit(EXIT_FAILURE);

    int pid = getpid();
    char buf[MSGSIZE];
    snprintf(buf, sizeof(buf), "register %d", pid);
    TEMP_FAILURE_RETRY(mq_send(mqdes1, buf, sizeof(buf), REGISTER));

    char name[MSGSIZE];
    snprintf(name, sizeof(name), "/q%d", pid);

    sleep(1);
    mqd_t mqdes2 = mq_open(name, O_RDONLY);
    if (-1 == mqdes2)
        ERR("mq_open");

    if (mq_getattr(mqdes2, &attr))
        ERR("mq_getattr");
    if (attr.mq_msgsize != MSGSIZE)
        exit(EXIT_FAILURE);

    process_messages(mqdes1, mqdes2, t);

    mq_close(mqdes1);
    mq_close(mqdes2);
    return EXIT_SUCCESS;
}