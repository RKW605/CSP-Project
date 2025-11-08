#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"

#define SERVER_PORT 12345

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

    char server_ip[16]; // IPv4 address max length is 15
    
    printf("\n\033[1;38;2;0;255;102mEnter server IP address: \033[0m");
    if (fgets(server_ip, sizeof(server_ip), stdin) == NULL)
    {
        printf("Error reading IP address\n");
        return 1;
    }
    
    // Remove newline character
    server_ip[strcspn(server_ip, "\n")] = 0;
    
    // If empty, use localhost as default
    if (strlen(server_ip) == 0)
    {
        strcpy(server_ip, "127.0.0.1");
    }
    
    int sock = connect_to_server(server_ip, SERVER_PORT);
    
    if (sock < 0)
    {
        printf("\033[1;38;2;255;0;0mNo server found!\033[0m\n");
        printf("\033[1;38;2;255;0;0mMake sure You are connected to the internet!\033[0m\n");
        return 1;
    }

    connection_info ci;
    ci.server_connection_fd = sock;

    pthread_create(&ci.send_thread, NULL, send_to_server, &ci);
    pthread_create(&ci.recv_thread, NULL, recv_from_server, &ci);

    pthread_join(ci.send_thread, NULL);
    pthread_join(ci.recv_thread, NULL);

    return 0;
}
