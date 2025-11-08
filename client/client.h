#ifndef CLIENT_H
#define CLIENT_H

#include <pthread.h>


#define BUFFER_SIZE 1024
#define NAME_SIZE 50

typedef struct
{
    int server_connection_fd;
    pthread_t send_thread; // thread ID for send
    pthread_t recv_thread; // thread ID for recv (optional)
} connection_info;

int connect_to_server(const char *ip, int port);
void read_hidden_input(char *buf, size_t size);
void *recv_from_server(void *arg);
void *send_to_server(void *arg);

#endif
