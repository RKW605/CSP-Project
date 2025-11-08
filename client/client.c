#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include "client.h"
#include "utils.h"

// Shared buffer to store last received message
char last_server_msg[BUFFER_SIZE] = {0};
pthread_mutex_t msg_mutex = PTHREAD_MUTEX_INITIALIZER;

// Track if user is joining room 5 (for password input)
static int joining_room5 = 0;

// Save/restore terminal settings safely
static struct termios orig_termios;
static int orig_termios_saved = 0;

// Synchronization for password phase
static pthread_mutex_t password_done_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t password_done_cond = PTHREAD_COND_INITIALIZER;
static int waiting_for_password_done = 0;

// Ignore external signals; rely on /disconnect for clean shutdown
static void ignore_signals(void)
{
    signal(SIGINT, SIG_IGN);
#ifdef SIGTERM
    signal(SIGTERM, SIG_IGN);
#endif
#ifdef SIGQUIT
    signal(SIGQUIT, SIG_IGN);
#endif
#ifdef SIGTSTP
    signal(SIGTSTP, SIG_IGN);
#endif
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
}

static void save_original_termios(void)
{
    if (!orig_termios_saved)
    {
        if (tcgetattr(STDIN_FILENO, &orig_termios) == 0)
            orig_termios_saved = 1;
    }
}

// Connect to server with timeout
int connect_to_server(const char *ip, int port)
{
    int server_connection_fd;
    struct sockaddr_in server_addr;
    int flags;
    fd_set writefds;
    struct timeval timeout;
    int result;
    int so_error;
    socklen_t len = sizeof(so_error);

    ignore_signals();

    server_connection_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_connection_fd < 0)
        return -1;

    // Non-blocking connect with timeout
    flags = fcntl(server_connection_fd, F_GETFL, 0);
    if (flags < 0)
    {
        close(server_connection_fd);
        return -1;
    }
    if (fcntl(server_connection_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        close(server_connection_fd);
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    result = connect(server_connection_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (result < 0 && errno != EINPROGRESS)
    {
        close(server_connection_fd);
        return -1;
    }

    if (result < 0 && errno == EINPROGRESS)
    {
        FD_ZERO(&writefds);
        FD_SET(server_connection_fd, &writefds);
        timeout.tv_sec = 7;
        timeout.tv_usec = 0;
        result = select(server_connection_fd + 1, NULL, &writefds, NULL, &timeout);

        if (result <= 0)
        {
            close(server_connection_fd);
            return -1;
        }

        if (getsockopt(server_connection_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0)
        {
            close(server_connection_fd);
            return -1;
        }
    }

    // Back to blocking
    flags = fcntl(server_connection_fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(server_connection_fd, F_SETFL, flags & ~O_NONBLOCK);

    printf("\n\033[1;33m🛜   Connected to server!   🛜\033[0m\n\n");
    fflush(stdout);

    save_original_termios();
    return server_connection_fd;
}

// Read hidden input (password)
void read_hidden_input(char *buf, size_t size)
{
    struct termios t;
    if (!orig_termios_saved)
        save_original_termios();

    tcgetattr(STDIN_FILENO, &t);
    struct termios hidden = t;
    hidden.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &hidden);

    if (fgets(buf, size, stdin) == NULL)
        buf[0] = '\0';
    else
        buf[strcspn(buf, "\n")] = 0;

    tcflush(STDIN_FILENO, TCIFLUSH);
}

// Receive messages from server
void *recv_from_server(void *arg)
{
    connection_info *ci = (connection_info *)arg;
    char buffer[BUFFER_SIZE];

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    while (1)
    {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(ci->server_connection_fd, buffer, BUFFER_SIZE, 0);
        if (bytes <= 0)
        {
            myPrint("\n\033[1;91mServer disconnected. Exiting...❌\033[0m\n");
            pthread_cancel(ci->send_thread);
            close(ci->server_connection_fd);
            break;
        }

        buffer[bytes] = '\0';

        pthread_mutex_lock(&msg_mutex);
        strncpy(last_server_msg, buffer, BUFFER_SIZE - 1);

        // Case 1: Password phase completed (success or permanent denial)
        if ((strstr(buffer, "Correct password! Access granted to VIP room.") != NULL ||
             strstr(buffer, "Too many failed attempts. Access denied.") != NULL ||
             (strstr(buffer, "You joined room") != NULL && strstr(buffer, "VIP") == NULL)) &&
            (strchr(buffer, ':') == NULL))
        {
            joining_room5 = 0;

            if (orig_termios_saved)
            {
                tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
                tcflush(STDIN_FILENO, TCIFLUSH);
                fflush(stdout);
            }

            pthread_mutex_lock(&password_done_mutex);
            waiting_for_password_done = 0;
            pthread_cond_signal(&password_done_cond);
            pthread_mutex_unlock(&password_done_mutex);
        }

        // Case 2: Incorrect password → signal send thread to prompt again
        else if (strstr(buffer, "Incorrect password") != NULL)
        {
            pthread_mutex_lock(&password_done_mutex);
            waiting_for_password_done = 0; // allow send thread to retry
            pthread_cond_signal(&password_done_cond);
            pthread_mutex_unlock(&password_done_mutex);
        }

        pthread_mutex_unlock(&msg_mutex);

        myPrint("%s", buffer);
    }

    pthread_exit(NULL);
}

// Send messages to server
void *send_to_server(void *arg)
{
    connection_info *ci = (connection_info *)arg;
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    char buffer[BUFFER_SIZE];
    char name[NAME_SIZE];

    myPrint("\033[1;38;2;0;255;102mEnter your name: \033[0m");
    fgets(name, NAME_SIZE, stdin);
    name[strcspn(name, "\n")] = 0;
    send(ci->server_connection_fd, name, strlen(name), 0);

    while (1)
    {
        usleep(150000);
        memset(buffer, 0, BUFFER_SIZE);

        int is_password_prompt = 0;
        pthread_mutex_lock(&msg_mutex);
        if (joining_room5)
            is_password_prompt = 1;
        pthread_mutex_unlock(&msg_mutex);

        if (is_password_prompt)
        {
            read_hidden_input(buffer, BUFFER_SIZE);
            printf("\n");
            send(ci->server_connection_fd, buffer, strlen(buffer), 0);

            pthread_mutex_lock(&password_done_mutex);
            waiting_for_password_done = 1;
            while (waiting_for_password_done)
                pthread_cond_wait(&password_done_cond, &password_done_mutex);
            pthread_mutex_unlock(&password_done_mutex);

            pthread_mutex_lock(&msg_mutex);
            memset(last_server_msg, 0, BUFFER_SIZE);
            pthread_mutex_unlock(&msg_mutex);
            continue;
        }

        if (!fgets(buffer, BUFFER_SIZE, stdin))
            continue;

        buffer[strcspn(buffer, "\n")] = 0;

        if (strncmp(buffer, "/join5", 6) == 0)
            joining_room5 = 1;

        if (strcmp(buffer, "/disconnect") == 0)
        {
            send(ci->server_connection_fd, buffer, strlen(buffer), 0);
            pthread_cancel(ci->recv_thread);
            close(ci->server_connection_fd);
            break;
        }

        if (strcmp(buffer, "/help") == 0)
        {
            pthread_mutex_lock(&print_mutex);
            printf("\n\033[1;38;2;0;0;255mAvailable commands:\033[0m\n");
            printf("  \033[38;2;255;165;0m/join<number>      - Join a room (1-5)\n");
            printf("  /exit              - Leave current room\n");
            printf("  /rooms             - List all rooms\n");
            printf("  /room              - Show current room\n");
            printf("  /clear             - Clear your screen\n");
            printf("  /clear -hard       - Hard clear\n");
            printf("  /disconnect        - Disconnect\n");
            printf("  /help              - Show help\n");
            printf("  /ls -all           - Show all clients\n");
            printf("  /ls -<room-number> - Show specific room clients\033[0m\n\n");
            fflush(stdout);
            pthread_mutex_unlock(&print_mutex);
            continue;
        }

        if (strcmp(buffer, "/clear") == 0)
        {
            myPrint("\033[2J\033[H");
            continue;
        }

        if (strcmp(buffer, "/clear -hard") == 0)
        {
            clear_screen();
            continue;
        }

        send(ci->server_connection_fd, buffer, strlen(buffer), 0);
    }

    pthread_exit(NULL);
}
