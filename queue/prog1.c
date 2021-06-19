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

#define ERR(source) (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     perror(source), kill(0, SIGKILL),               \
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

void child_work(int pid, int t)
{
    char name[MSGSIZE];
    snprintf(name, sizeof(name), "/q%d", pid);

    struct mq_attr attr;
    attr.mq_maxmsg = 1;
    attr.mq_msgsize = MSGSIZE;

    mqd_t mqdes = mq_open(name, O_WRONLY | O_CREAT | O_EXCL, 0600, &attr);
    if (-1 == mqdes)
        ERR("mq_open");

    struct timespec st = {0, 0};
    if (t >= 1000)
        st.tv_sec += t / 1000;
    else
        st.tv_nsec = t * 1000000;
    for (int i = 0; last_signal != SIGINT; ++i)
    {
        char buf[MSGSIZE];
        snprintf(buf, sizeof(buf), "check status [%d]", i);
        TEMP_FAILURE_RETRY(mq_send(mqdes, buf, sizeof(buf), STATUS));
        nanosleep(&st, NULL);
    }

    mq_close(mqdes);
    if (mq_unlink(name))
        ERR("mq_unlink");
}

void parent_work(mqd_t mqdes, int t)
{
    for (; last_signal != SIGINT;)
    {
        char buf[MSGSIZE];
        unsigned msg_prio;
        if (TEMP_FAILURE_RETRY(mq_receive(mqdes, buf, sizeof(buf), &msg_prio)) == -1)
        {
            if (EAGAIN == errno)
                continue;
            ERR("mq_receive");
        }

        char msg[MSGSIZE];
        strcpy(msg, buf);
        char *token = strtok(buf, " ");

        if (strcmp(token, "status") == 0 && STATUS == msg_prio)
        {
            printf("%s\n", msg);
            fflush(stdout);
        }

        else if (strcmp(token, "register") == 0 && REGISTER == msg_prio)
        {
            printf("%s\n", msg);
            fflush(stdout);
            token = strtok(NULL, " ");
            int pid = strtol(token, NULL, 10);

            switch (fork())
            {
            case -1:
                ERR("fork()");
            case 0:
                child_work(pid, t);
                exit(EXIT_SUCCESS);
            }
        }
    }

    while (wait(NULL) > 0)
        ;
}

int main(int argc, char **argv)
{
    set_handler(sig_handler, SIGINT);

    if (argc != 3)
        usage(argv[0]);

    int t = strtol(argv[2], NULL, 10);
    if (t < 100 || t > 2000)
        usage(argv[0]);

    struct mq_attr attr;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = MSGSIZE;

    mqd_t mqdes = mq_open(argv[1], O_RDONLY | O_CREAT | O_EXCL | O_NONBLOCK, 0600, &attr);
    if (-1 == mqdes)
        ERR("mq_open");

    parent_work(mqdes, t);

    mq_close(mqdes);
    if (mq_unlink(argv[1]))
        ERR("mq_unlink");
    return EXIT_SUCCESS;
}