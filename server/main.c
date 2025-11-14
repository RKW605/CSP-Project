#include <stdio.h>
#include <stdlib.h>     // for malloc, free
#include <unistd.h>     // for close()
#include <pthread.h>
#include <sys/select.h>  // for select()
#include <sys/time.h>   // for timeval
#include <string.h>     // for memset()
#include "server.h"
#include "utils.h"

extern volatile int server_running;
extern void *server_console_thread(void *arg);

#define PORT 12345

int main()
{
    clear_screen();

    if (is_running_in_windows())
    {
        fprintf(stderr, "\n\033[1;91m ⚠️ Error: Detected Windows.\033[0m ⚠️ \n");
        fprintf(stderr, "\033[1;96m 👉 The application can only run in UNIX like systems(Linux, MacOS, WSL).\033[0m\n");
        fprintf(stderr, "\033[1;96m 👉 Please run this server on a UNIX like systems.\033[0m\n\n");
        exit(EXIT_FAILURE);
    }

    // If you will just use locl host, you can comment the following if block
    // If machine has WSL, immediately exit and advise user to use native Linux for better networking
    /* if (is_running_in_wsl())
    {
        fprintf(stderr, "\n\033[1;91m ⚠️ Error: Detected Windows Subsystem for Linux (WSL).\033[0m ⚠️ \n");
        fprintf(stderr, "\033[1;96m 👉 Networking in WSL can be unreliable for server applications.\033[0m\n");
        fprintf(stderr, "\033[1;96m 👉 Please run this server on a native Linux installation for best results.\033[0m\n\n");
        exit(EXIT_FAILURE);
    } */

    // Initialize chat rooms
    initialize_rooms();

    int server_socket = create_server_socket(PORT);

    // Start console thread for /disconnect
    pthread_t console_tid;
    pthread_create(&console_tid, NULL, server_console_thread, NULL);

    while (server_running)
    {
        // Use select() to make accept non-blocking
        fd_set readfds;
        struct timeval timeout;
        
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);
        
        timeout.tv_sec = 1;  // 1 second timeout
        timeout.tv_usec = 0;
        
        int result = select(server_socket + 1, &readfds, NULL, NULL, &timeout);
        
        if (result > 0 && FD_ISSET(server_socket, &readfds))
        {
            int client_socket = accept_client(server_socket);

            pthread_t tid;
            client_info *ci = malloc(sizeof(client_info)); // allocate for thread
            ci->client_socket = client_socket;
            ci->muted_count = 0; // Initialize muted_count
            memset(ci->muted_users, 0, sizeof(ci->muted_users)); // Initialize muted_users array

            pthread_create(&tid, NULL, handle_client, ci);
            pthread_detach(tid);
        }
        // If result == 0, timeout occurred, check server_running and continue
        // If result < 0, error occurred, but we'll continue anyway
    }

    close(server_socket);
    printf("\033[1;38;2;255;0;0mServer shut down. Bye👋\033[0m\n");
    fflush(stdout);
    return 0;
}
