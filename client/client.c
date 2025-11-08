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

// Connect to server with 5 second timeout
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
    
    // Ignore external termination signals; rely on /disconnect
    ignore_signals();
    
    server_connection_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_connection_fd < 0)
        return -1;
    
    // Set socket to non-blocking mode
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
    
    // Attempt connection (will return immediately with non-blocking socket)
    result = connect(server_connection_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    
    if (result < 0 && errno != EINPROGRESS)
    {
        close(server_connection_fd);
        return -1;
    }
    
    // If connection didn't complete immediately, wait with timeout
    if (result < 0 && errno == EINPROGRESS)
    {
        FD_ZERO(&writefds);
        FD_SET(server_connection_fd, &writefds);

        timeout.tv_sec = 7;  // 7 second timeout
        timeout.tv_usec = 0;
        
        result = select(server_connection_fd + 1, NULL, &writefds, NULL, &timeout);
        
        if (result <= 0)
        {
            // Timeout or error
            close(server_connection_fd);
            return -1;
        }
        
        // Check if connection was successful
        if (getsockopt(server_connection_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0)
        {
            close(server_connection_fd);
            return -1;
        }
    }
    
    // Set socket back to blocking mode
    flags = fcntl(server_connection_fd, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(server_connection_fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    printf("\n\033[1;33m🛜   Connected to server!   🛜\033[0m\n\n");
    fflush(stdout);
    return server_connection_fd;
}

// Helper function to read hidden input (no echo)
void read_hidden_input(char *buf, size_t size)
{
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    fgets(buf, size, stdin);
    buf[strcspn(buf, "\n")] = 0;

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

// Receive messages
void *recv_from_server(void *arg)
{
    connection_info *ci = (connection_info *)arg;
    char buffer[BUFFER_SIZE];
    
    // Allow this thread to be canceled asynchronously
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    
    while (1)
    {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(ci->server_connection_fd, buffer, BUFFER_SIZE, 0);
        if (bytes <= 0)
        {
            myPrint("\n\033[1;91mServer disconnected. Exiting...❌\033[0m\n");
            // Calmly cancel the send thread
            pthread_cancel(ci->send_thread);
            close(ci->server_connection_fd); // close socket
            break;
        }
        buffer[bytes] = '\0';

        // Store last message for password detection
        pthread_mutex_lock(&msg_mutex);
        strncpy(last_server_msg, buffer, BUFFER_SIZE - 1);
        
        // If server indicates success or final denial, stop password mode
        if ((strstr(buffer, "Correct password! Access granted to VIP room.") != NULL ||
        strstr(buffer, "You joined room") != NULL ||
        strstr(buffer, "Too many failed attempts. Access denied.") != NULL) && (strchr(buffer, ':') == NULL))
        {
            joining_room5 = 0;
        }
        pthread_mutex_unlock(&msg_mutex);

        myPrint("%s", buffer);
    }
    
    pthread_exit(NULL);
}

// Send messages
void *send_to_server(void *arg)
{
    connection_info *ci = (connection_info *)arg;
    
    // Allow this thread to be canceled asynchronously
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    
    char buffer[BUFFER_SIZE];
    // Ask for name
    char name[NAME_SIZE];
    // Very edge case mutex, like if we just connected and server immediately disconnects
    myPrint("\033[1;38;2;0;255;102mEnter your name: \033[0m");
    fgets(name, NAME_SIZE, stdin);
    name[strcspn(name, "\n")] = 0;
    send(ci->server_connection_fd, name, strlen(name), 0);
    
    while (1)
    {
        // Small delay to let recv thread update last_server_msg
        usleep(100000); // 100ms
        
        memset(buffer, 0, BUFFER_SIZE);

        // Decide if server just asked for the VIP password.
        // Only treat it as a password prompt when joining_room5 == 1 and the last server message matches the VIP prompt strings.
        int is_password_prompt = 0;
        pthread_mutex_lock(&msg_mutex);
        if (joining_room5)
        {
            is_password_prompt = 1;
        }
        pthread_mutex_unlock(&msg_mutex);

        if (is_password_prompt)
        {
            // Use hidden input for password
            read_hidden_input(buffer, BUFFER_SIZE);
            printf("\n");
            send(ci->server_connection_fd, buffer, strlen(buffer), 0);
            
            // Leave joining_room5 set — recv thread will clear it on success or final denial.
            // Clear the last message so we don't re-trigger unnecessarily
            pthread_mutex_lock(&msg_mutex);
            memset(last_server_msg, 0, BUFFER_SIZE);
            pthread_mutex_unlock(&msg_mutex);
            continue;
        }

        // Normal input
        fgets(buffer, BUFFER_SIZE, stdin);
        buffer[strcspn(buffer, "\n")] = 0;

        // If user typed "/join5" locally, enable VIP-password-mode
        // (we set this flag before sending so the next server prompt will be handled as hidden)
        if (strncmp(buffer, "/join5", 6) == 0)
        {
            joining_room5 = 1;
            // fall through to send "/join5" to server
        }

        if (strcmp(buffer, "/disconnect") == 0)
        {
            send(ci->server_connection_fd, buffer, strlen(buffer), 0);
            pthread_cancel(ci->recv_thread);
            close(ci->server_connection_fd);
            break;
        }
        
        // Add help command for better UX
        if (strcmp(buffer, "/help") == 0)
        {
            pthread_mutex_lock(&print_mutex);
            printf("\n\033[1;38;2;0;0;255mAvailable commands:\033[0m\n");
            printf("  \033[38;2;255;165;0m/join<number>      - Join a room (1-5)\n");
            printf("  /exit              - Leave current room\n");
            printf("  /rooms             - List all rooms\n");
            printf("  /room              - Show current room\n");
            printf("  /clear             - Clear your screen, Messages will not be removed\n");
            printf("  /clear -hard       - Empty your screen, Messages will be removed\n");
            printf("  /disconnect        - Disconnect from server\n");
            printf("  /help              - Show this help\n");
            printf("  /ls -all           - Show all the clients\n");
            printf("  /ls -<room-number> - Show clients of the specific room\033[0m\n\n");
            fflush(stdout);
            pthread_mutex_unlock(&print_mutex);
            continue;
        }
        
        // Handle local-only clear command
        if (strcmp(buffer, "/clear") == 0)
        {
            // ANSI escape sequence to clear screen and move cursor to home
            myPrint("\033[2J\033[H");
            continue; // do not send to server
        }

        // Handle local-only hard clear command
        if (strcmp(buffer, "/clear -hard") == 0)
        {
            clear_screen();
            continue; // do not send to server
        }
        
        send(ci->server_connection_fd, buffer, strlen(buffer), 0);
    }
    
    pthread_exit(NULL);
}
