#define _GNU_SOURCE

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERR(source) (perror(source),                                 \
                     fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), \
                     exit(EXIT_FAILURE))

#define MAX_PLAYERS 5
#define MAX_SIZE 100

volatile int do_work = 1;

typedef struct
{
    int *board;
    int board_size;
    int num_players;
    int player_number;
    int *positions;
    sem_t *semaphores;
    int *socketfds;
    pthread_t *threads;
} linear;

void usage(char *name)
{
    fprintf(stderr, "USAGE: %s port_number num_players board_size\n", name);
    exit(EXIT_FAILURE);
}

void verify_args(int num_players, int board_size)
{
    if (num_players < 2 || num_players > 5 ||
        board_size < num_players || board_size > 5 * num_players)
        exit(EXIT_FAILURE);
}

void sigint_handler(int sig)
{
    do_work = 0;
}

void set_handler(void (*f)(int), int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = f;
    if (sigaction(sig, &act, NULL))
        ERR("sigaction");
}

int make_socket(int domain, int type)
{
    int socketfd = socket(domain, type, 0);
    if (socketfd < 0)
        ERR("socket");
    return socketfd;
}

int bind_tcp_socket(uint16_t port)
{
    int socketfd = make_socket(PF_INET, SOCK_STREAM);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int t = 1;
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(t)))
        ERR("setsockopt");
    if (bind(socketfd, (struct sockaddr *)&addr, sizeof(addr)))
        ERR("bind");
    if (listen(socketfd, MAX_PLAYERS))
        ERR("listen");
    return socketfd;
}

int add_new_client(int socketfd)
{
    int sock = TEMP_FAILURE_RETRY(accept(socketfd, NULL, NULL));
    if (sock < 0)
    {
        if (EAGAIN == errno || EWOULDBLOCK == errno)
            return -1;
        ERR("accept");
    }
    return sock;
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

int my_random(int board_size)
{
    static int count = 0;
    static int *positions = NULL;
    if (board_size > 0)
    {
        if (positions)
            free(positions);
        positions = malloc(board_size * sizeof(*positions));
        if (!positions)
            ERR("malloc");
        for (int i = 0; i < board_size; ++i)
            positions[i] = i;
        count = board_size;
    }
    if (!count)
        return -1;
    unsigned seed = time(NULL);
    int n = rand_r(&seed) % count;
    int i = positions[n];
    positions[n] = positions[count - 1];
    --count;
    if (!count)
    {
        free(positions);
        positions = NULL;
    }
    return i;
}

void print_board(char *buf, int length, int *board, int board_size)
{
    strncpy(buf, "|", length);
    char field[3];
    memset(field, 0, sizeof(field));
    for (int i = 0; i < board_size; ++i)
    {
        if (-1 == board[i])
            strncpy(field, " |", sizeof(field));
        else
            snprintf(field, sizeof(field), "%d|", board[i]);
        strncat(buf, field, length - strlen(buf) - 1);
    }
    strncat(buf, "\n", length - strlen(buf) - 1);
}

void move_player(char *buf, int length, linear *data)
{
    int step = strtol(buf, NULL, 10);
    if (step < -2 || step > 2)
        return;
    if (!step)
    {
        print_board(buf, length, data->board, data->board_size);
        if (bulk_write(data->socketfds[data->player_number], buf, strlen(buf)) < 0 && errno != EPIPE)
            ERR("write");
        return;
    }
    int position = data->positions[data->player_number] + step;
    data->board[data->positions[data->player_number]] = -1;
    if (sem_post(&data->semaphores[data->positions[data->player_number]]))
        ERR("sem_post");
    if (position < 0 || position >= data->board_size)
    {
        strncpy(buf, "You lost: you stepped out of the board!\n", length);
        if (bulk_write(data->socketfds[data->player_number], buf, strlen(buf)) < 0 && errno != EPIPE)
            ERR("write");
        if (TEMP_FAILURE_RETRY(close(data->socketfds[data->player_number])))
            ERR("close");
        pthread_exit(NULL);
    }
    if (sem_trywait(&data->semaphores[position]))
    {
        if (EAGAIN == errno)
        {
            snprintf(buf, length, "You lost: player#%d stepped on you!\n", data->player_number);
            if (bulk_write(data->socketfds[data->board[position]], buf, strlen(buf)) < 0 && errno != EPIPE)
                ERR("write");
            if (TEMP_FAILURE_RETRY(close(data->socketfds[data->board[position]])))
                ERR("close");
            if (pthread_cancel(data->threads[data->board[position]]))
                ERR("pthread_cancel");
        }
        else
            ERR("sem_trywait");
    }
    data->positions[data->player_number] = position;
    data->board[data->positions[data->player_number]] = data->player_number;
    int count = 0;
    for (int i = 0; i < data->board_size; ++i)
        if (data->board[i] != -1)
            ++count;
    if (count == 1)
    {
        strncpy(buf, "You have won!\n", length);
        if (bulk_write(data->socketfds[data->player_number], buf, strlen(buf)) < 0 && errno != EPIPE)
            ERR("write");
        if (TEMP_FAILURE_RETRY(close(data->socketfds[data->player_number])))
            ERR("close");
        pthread_exit(NULL);
    }
}

void *interact_with_player(void *ptr)
{
    linear *data = (linear *)ptr;
    char buf[MAX_SIZE];
    memset(buf, 0, sizeof(buf));
    strncpy(buf, "The game has started.\n", sizeof(buf));
    if (bulk_write(data->socketfds[data->player_number], buf, strlen(buf)) < 0 && errno != EPIPE)
        ERR("write");
    print_board(buf, sizeof(buf), data->board, data->board_size);
    if (bulk_write(data->socketfds[data->player_number], buf, strlen(buf)) < 0 && errno != EPIPE)
        ERR("write");
    while (do_work)
    {
        int count = TEMP_FAILURE_RETRY(read(data->socketfds[data->player_number], buf, sizeof(buf)));
        if (count > 0)
            move_player(buf, sizeof(buf), data);
        else if (count < 0)
            ERR("read");
        else
            break;
    }
    if (TEMP_FAILURE_RETRY(close(data->socketfds[data->player_number])))
        ERR("close");
    return NULL;
}

void do_server(int socketfd, int num_players, int board_size)
{
    linear *data = malloc(num_players * sizeof(*data));
    int *board = malloc(board_size * sizeof(*board));
    int *positions = malloc(num_players * sizeof(*positions));
    sem_t *semaphores = malloc(board_size * sizeof(*semaphores));
    int *socketfds = malloc(num_players * sizeof(*socketfds));
    pthread_t *threads = malloc(num_players * sizeof(*threads));
    if (!(data && board && positions && semaphores && socketfds && threads))
        ERR("malloc");

    char buf[MAX_SIZE];
    memset(buf, 0, sizeof(buf));
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigset_t old_mask;
    sigprocmask(SIG_BLOCK, &mask, &old_mask);
    fd_set readfds;

    while (do_work)
    {
        int index = 0;
        while (do_work && index < num_players)
        {
            FD_ZERO(&readfds);
            FD_SET(socketfd, &readfds);
            if (pselect(socketfd + 1, &readfds, NULL, NULL, NULL, &old_mask) > 0)
            {
                int sock = add_new_client(socketfd);
                if (sock >= 0)
                {
                    socketfds[index] = sock;
                    snprintf(buf, sizeof(buf), "You are player#%d. Please wait...\n", index);
                    if (bulk_write(socketfds[index], buf, strlen(buf)) < 0 && errno != EPIPE)
                        ERR("write");
                    ++index;
                }
            }
            else
            {
                if (EINTR == errno)
                    continue;
                ERR("pselect");
            }
        }
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        if (do_work)
        {
            for (int i = 0; i < board_size; ++i)
            {
                board[i] = -1;
                if (sem_init(&semaphores[i], 0, 1))
                    ERR("sem_init");
            }
            for (int i = 0; i < num_players; ++i)
            {
                if (!i)
                    positions[i] = my_random(board_size);
                else
                    positions[i] = my_random(-1);
                if (sem_wait(&semaphores[positions[i]]))
                    ERR("sem_wait");
                board[positions[i]] = i;
            }
            for (int i = 0; i < num_players; ++i)
            {
                data[i].board = board;
                data[i].board_size = board_size;
                data[i].num_players = num_players;
                data[i].player_number = i;
                data[i].positions = positions;
                data[i].semaphores = semaphores;
                data[i].socketfds = socketfds;
                data[i].threads = threads;
            }
            for (int i = 0; i < num_players; ++i)
                if (pthread_create(&threads[i], NULL, interact_with_player, &data[i]))
                    ERR("pthread_create");
            for (int i = 0; i < num_players; ++i)
                if (pthread_join(threads[i], NULL))
                    ERR("phtread_join");
            for (int i = 0; i < board_size; ++i)
                if (sem_destroy(&semaphores[i]))
                    ERR("sem_destroy");
        }
    }

    free(data);
    free(board);
    free(positions);
    free(semaphores);
    free(socketfds);
    free(threads);
}

int main(int argc, char *argv[])
{
    if (argc != 4)
        usage(argv[0]);
    int port_number = strtol(argv[1], NULL, 10);
    int num_players = strtol(argv[2], NULL, 10);
    int board_size = strtol(argv[3], NULL, 10);
    verify_args(num_players, board_size);
    set_handler(SIG_IGN, SIGPIPE);
    set_handler(sigint_handler, SIGINT);
    int socketfd = bind_tcp_socket(port_number);
    do_server(socketfd, num_players, board_size);
    if (TEMP_FAILURE_RETRY(close(socketfd)))
        ERR("close");
    return EXIT_SUCCESS;
}
