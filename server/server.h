#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>

#define MAX_CLIENTS 10
#define MAX_ROOMS 5
#define ROOM_NAME_LENGTH 20
#define BUFFER_SIZE 1024
#define NAME_SIZE 50

typedef struct
{
    int client_socket;
    char name[NAME_SIZE]; // client name
    int current_room; // -1 means not in any room
    char muted_users[MAX_CLIENTS][NAME_SIZE]; // list of muted users
    int muted_count;
} client_info;

typedef struct
{
    char name[ROOM_NAME_LENGTH];
    int client_count;
} room_info;

int create_server_socket(int port);
int accept_client(int server_socket);
void broadcast_message(const char *msg, int sender_socket);
void receive_name(client_info *ci);
void announce_join(client_info *ci);
void announce_leave(client_info *ci);
void add_client(client_info *ci);
void remove_client(client_info *ci);
void handle_mute_command(client_info *ci, const char *command);
void handle_unmute_command(client_info *ci, const char *command);
void *handle_client(void *arg);

// Room management functions
void initialize_rooms();
void join_room(client_info *ci, int room_number);
void leave_room(client_info *ci);
void broadcast_to_room(const char *msg, int sender_socket, int room_number);
void send_room_list(int socket);
void send_room_info(int socket);

// Server console thread
void *server_console_thread(void *arg);

// extern client_info clients[MAX_CLIENTS];
// extern int client_count;
// extern pthread_mutex_t clients_mutex;
// extern room_info rooms[MAX_ROOMS];

#endif
