#define _DEFAULT_SOURCE
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "server.h"
#include "utils.h"
#define VIP_PASSWORD "vip123"

volatile int server_running = 1;

client_info clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rooms_mutex = PTHREAD_MUTEX_INITIALIZER;
room_info rooms[MAX_ROOMS];

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

// Get local IP address (non-localhost)
static void get_local_ip(char *ip_str, size_t ip_str_size)
{
    struct ifaddrs *ifaddrs_ptr, *ifa;
    getifaddrs(&ifaddrs_ptr);

    for (ifa = ifaddrs_ptr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
            continue;

        // Check for IPv4 address
        if (ifa->ifa_addr->sa_family == AF_INET)
        {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            const char *ip = inet_ntoa(sin->sin_addr);

            // Skip localhost and loopback
            if (strcmp(ip, "127.0.0.1") != 0 && strncmp(ip, "127.", 4) != 0)
            {
                strncpy(ip_str, ip, ip_str_size - 1);
                ip_str[ip_str_size - 1] = '\0';
                freeifaddrs(ifaddrs_ptr);
                return;
            }
        }
    }

    // If no network IP found, use localhost as fallback
    strncpy(ip_str, "127.0.0.1", ip_str_size - 1);
    ip_str[ip_str_size - 1] = '\0';
    freeifaddrs(ifaddrs_ptr);
}

// Create server socket
int create_server_socket(int port)
{
    ignore_signals();
    int server_socket;
    struct sockaddr_in server_addr;
    char local_ip[INET_ADDRSTRLEN];

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
        error_exit("Socket creation failed");

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        error_exit("setsockopt failed");

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        error_exit("Bind failed");

    if (listen(server_socket, MAX_CLIENTS) < 0)
        error_exit("Listen failed");

    // Get and print local IP address
    get_local_ip(local_ip, sizeof(local_ip));
    printf("\n\033[1;95mServer listening on IP: %s, Port: %d\033[0m\n", local_ip, port);
    fflush(stdout);
    return server_socket;
}

// Initialize chat rooms
void initialize_rooms()
{
    strcpy(rooms[0].name, "General");
    strcpy(rooms[1].name, "Gaming");
    strcpy(rooms[2].name, "Music");
    strcpy(rooms[3].name, "Study");
    strcpy(rooms[4].name, "VIP");

    for (int i = 0; i < MAX_ROOMS; i++)
    {
        rooms[i].client_count = 0;
    }
}

// Accept new client
int accept_client(int server_socket)
{
    int client_socket;
    struct sockaddr_in client_addr;
    socklen_t addr_size = sizeof(client_addr);

    client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_size);
    if (client_socket < 0)
        error_exit("Accept failed");

    myPrint("\n\033[1;92mClient connected! 🤝\033[0m\n\n");
    return client_socket;
}

// Broadcast message to all other clients
void broadcast_message(const char *msg, int sender_socket)
{
    // Mutex ensures the clients array isn't modified by another thread
    // (e.g., a client joining or leaving) while we are iterating over it
    pthread_mutex_lock(&clients_mutex);
    
    // Find sender's name
    char sender_name[NAME_SIZE] = {0};
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].client_socket == sender_socket)
        {
            strncpy(sender_name, clients[i].name, NAME_SIZE - 1);
            sender_name[NAME_SIZE - 1] = '\0';
            break;
        }
    }
    
    size_t msg_length = strlen(msg);
    for (int i = 0; i < client_count; i++)
    {
        int sock = clients[i].client_socket;
        if (sock != sender_socket)
        {
            // Check if this recipient has muted the sender
            int is_muted = 0;
            for (int j = 0; j < clients[i].muted_count; j++)
            {
                if (clients[i].muted_users[j][0] != '\0' && 
                    strcasecmp(clients[i].muted_users[j], sender_name) == 0)
                {
                    is_muted = 1;
                    break;
                }
            }
            
            if (!is_muted)
            {
                send(sock, msg, msg_length, 0);
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Receive the client name
void receive_name(client_info *ci)
{
    char name_buffer[NAME_SIZE];
    int name_ok = 0;

    while (!name_ok)
    {
        memset(name_buffer, 0, NAME_SIZE);
        int bytes = recv(ci->client_socket, name_buffer, NAME_SIZE, 0);
        if (bytes <= 0)
        {
            close(ci->client_socket);
            free(ci);
            pthread_exit(NULL);
        }

        name_buffer[strcspn(name_buffer, "\r\n")] = 0; // remove newline if any

        // Check if name already exists
        name_ok = 1;
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < client_count; i++)
        {
            if (strcasecmp(clients[i].name, name_buffer) == 0)
            {
                name_ok = 0;
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);

        if (!name_ok)
        {
            char msg[] = "\033[1;91m❌ Name already taken. Please choose another name:\033[0m ";
            send(ci->client_socket, msg, strlen(msg), 0);
        }
        else
        {
            strncpy(ci->name, name_buffer, NAME_SIZE);
            ci->name[NAME_SIZE - 1] = '\0';
            char welcome_msg[100];
            snprintf(welcome_msg, sizeof(welcome_msg),
                     "\n\033[1;32m✅ Welcome, %s!\033[0m\n\n", ci->name);
            send(ci->client_socket, welcome_msg, strlen(welcome_msg), 0);
        }
    }
}

// Announce all the other clients about joining
void announce_join(client_info *ci)
{
    char join_msg[BUFFER_SIZE];
    snprintf(join_msg, BUFFER_SIZE, "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m %s has joined the chat.\n\n", ci->name);
    broadcast_message(join_msg, ci->client_socket);
    myPrint(join_msg);
}

// Announce all the other clients about leaving
void announce_leave(client_info *ci)
{
    char leave_msg[BUFFER_SIZE];
    snprintf(leave_msg, BUFFER_SIZE, "\n\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m %s has left the chat.\n\n", ci->name);
    broadcast_message(leave_msg, ci->client_socket);
    myPrint(leave_msg);
}

// Add client to the list
void add_client(client_info *ci)
{
    pthread_mutex_lock(&clients_mutex);
    if (client_count < MAX_CLIENTS)
    {
        ci->current_room = -1; // Initialize with no room
        clients[client_count++] = *ci;
    }
    else
    {
        char *msg = "\033[1;91mChat room full. Try again later.🔄\033[0m\n";
        send(ci->client_socket, msg, strlen(msg), 0);
        close(ci->client_socket);
        pthread_mutex_unlock(&clients_mutex);
        free(ci);
        pthread_exit(NULL);
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Remove client from the list
void remove_client(client_info *ci)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].client_socket == ci->client_socket)
        {
            // Update room count if client was in a room
            if (clients[i].current_room != -1)
            {
                pthread_mutex_lock(&rooms_mutex);
                rooms[clients[i].current_room].client_count--;
                pthread_mutex_unlock(&rooms_mutex);

                myPrint("Client %s left room %d (%s), room %d now has %d users",
                        clients[i].name, clients[i].current_room + 1,
                        rooms[clients[i].current_room].name,
                        clients[i].current_room + 1,
                        rooms[clients[i].current_room].client_count);
            }

            // Beautiful array removal, has very good explanation
            clients[i] = clients[client_count - 1];
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Leave current room
void leave_room(client_info *ci)
{
    myPrint("\nClient %s trying to leave room", ci->name);

    if (ci->current_room == -1)
    {
        char error_msg[] = "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m You are not in any room\n";
        send(ci->client_socket, error_msg, strlen(error_msg), 0);
        myPrint("Client %s not in any room\n", ci->name);
        return;
    }

    int room_index = ci->current_room;
    pthread_mutex_lock(&rooms_mutex);
    rooms[room_index].client_count--;
    pthread_mutex_unlock(&rooms_mutex);

    // Update the global clients array
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].client_socket == ci->client_socket)
        {
            clients[i].current_room = -1;
            myPrint("\nUpdated global clients array: %s left room %d", clients[i].name, room_index);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    myPrint("\nClient %s left room %d (%s), room now has %d users",
            ci->name, room_index + 1, rooms[room_index].name,
            rooms[room_index].client_count);

    char leave_msg[BUFFER_SIZE];
    snprintf(leave_msg, BUFFER_SIZE, "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m %s has left room %d (%s)\n",
             ci->name, room_index + 1, rooms[room_index].name);

    // Announce to room members
    broadcast_to_room(leave_msg, ci->client_socket, room_index);

    // Send confirmation to client
    char confirm_msg[BUFFER_SIZE];
    snprintf(confirm_msg, BUFFER_SIZE, "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m You left room %d (%s)\n",
             room_index + 1, rooms[room_index].name);
    send(ci->client_socket, confirm_msg, strlen(confirm_msg), 0);

    ci->current_room = -1;
}

// Join a specific room
void join_room(client_info *ci, int room_number)
{
    myPrint("\nClient %s trying to join room %d", ci->name, room_number);

    if (room_number < 1 || room_number > MAX_ROOMS)
    {
        char error_msg[BUFFER_SIZE];
        snprintf(error_msg, BUFFER_SIZE, "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m Invalid room number.❌ Please choose 1-%d\n", MAX_ROOMS);
        send(ci->client_socket, error_msg, strlen(error_msg), 0);
        myPrint("Invalid room number %d from %s\n", room_number, ci->name);
        return;
    }

    int room_index = room_number - 1; // Convert to 0-based index

    // 🏰 Check VIP password before entering room 5
    if (room_index == 4) // room 5 (VIP)
    {
        char password_prompt[] = "\033[1;93m🔐 Enter VIP room password:\033[0m ";
        send(ci->client_socket, password_prompt, strlen(password_prompt), 0);

        char recv_buffer[BUFFER_SIZE];
        int attempts = 0;

        while (1)
        {
            memset(recv_buffer, 0, BUFFER_SIZE);
            int bytes = recv(ci->client_socket, recv_buffer, BUFFER_SIZE - 1, 0);
            if (bytes <= 0)
            {
                myPrint("Client %s disconnected while entering password\n", ci->name);
                return;
            }

            recv_buffer[strcspn(recv_buffer, "\r\n")] = 0; // Trim newline
            attempts++;

            if (strcmp(recv_buffer, VIP_PASSWORD) == 0)
            {
                char success_msg[] = "\033[1;92m✅ Correct password! Access granted to VIP room.\033[0m\n";
                send(ci->client_socket, success_msg, strlen(success_msg), 0);
                break;
            }
            else
            {
                char error_msg[] = "\033[1;91m❌ Incorrect password. Try again:\033[0m ";
                send(ci->client_socket, error_msg, strlen(error_msg), 0);
            }

            if (attempts >= 5)
            {
                char deny_msg[] = "\n\033[1;91mToo many failed attempts. Access denied.\033[0m\n";
                send(ci->client_socket, deny_msg, strlen(deny_msg), 0);
                myPrint("Client %s denied VIP room after 5 failed attempts\n", ci->name);
                return;
            }
        }
    }
    // Can't join the same room
    if (ci->current_room == room_index)
    {
        char msg[] = "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m You are already in this room!\n";
        send(ci->client_socket, msg, strlen(msg), 0);
        return;
    }
    // Leave current room if in one
    if (ci->current_room != -1)
    {
        myPrint("\nClient %s leaving room %d to join room %d", ci->name, ci->current_room + 1, room_number);
        leave_room(ci);
    }

    // Join new room - update both local copy and global array
    ci->current_room = room_index;
    pthread_mutex_lock(&rooms_mutex);
    rooms[room_index].client_count++;
    pthread_mutex_unlock(&rooms_mutex);

    // Update the global clients array
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].client_socket == ci->client_socket)
        {
            clients[i].current_room = room_index;
            myPrint("\nUpdated global clients array: %s now in room %d", clients[i].name, room_index + 1);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    myPrint("\nClient %s joined room %d (%s), room now has %d users",
            ci->name, room_number, rooms[room_index].name, rooms[room_index].client_count);

    char join_msg[BUFFER_SIZE];
    snprintf(join_msg, BUFFER_SIZE, "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m %s has joined room %d (%s)\n",
             ci->name, room_number, rooms[room_index].name);

    // Announce to room members
    broadcast_to_room(join_msg, ci->client_socket, room_index);

    // Send confirmation to client
    char confirm_msg[BUFFER_SIZE];
    snprintf(confirm_msg, BUFFER_SIZE, "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m You joined room %d (%s)\n",
             room_number, rooms[room_index].name);
    send(ci->client_socket, confirm_msg, strlen(confirm_msg), 0);
}

// Broadcast message to specific room
void broadcast_to_room(const char *msg, int sender_socket, int room_number)
{
    pthread_mutex_lock(&clients_mutex);

    myPrint("\nBroadcasting to room %d: %s", room_number + 1, msg);

    // Find sender's name
    char sender_name[NAME_SIZE] = {0};
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].client_socket == sender_socket)
        {
            strncpy(sender_name, clients[i].name, NAME_SIZE - 1);
            sender_name[NAME_SIZE - 1] = '\0';
            myPrint("\nSender found: %s (socket %d)\n", sender_name, sender_socket);
            break;
        }
    }

    int sent_count = 0;
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].client_socket != sender_socket && clients[i].current_room == room_number)
        {
            myPrint("\nChecking recipient %s (muted_count=%d)\n", clients[i].name, clients[i].muted_count);
            
            // Check if this recipient has muted the sender
            int is_muted = 0;
            for (int j = 0; j < clients[i].muted_count; j++)
            {
                myPrint("  Muted user[%d]: '%s' vs sender '%s'\n", j, clients[i].muted_users[j], sender_name);
                if (clients[i].muted_users[j][0] != '\0' && 
                    strcasecmp(clients[i].muted_users[j], sender_name) == 0)
                {
                    is_muted = 1;
                    myPrint("  -> MATCH! User is muted!\n");
                    break;
                }
            }
            
            if (is_muted)
            {
                myPrint("Message not sent to %s (muted)\n", clients[i].name);
                continue;
            }
            
            int bytes_sent = send(clients[i].client_socket, msg, strlen(msg), 0);
            if (bytes_sent > 0)
            {
                sent_count++;
                myPrint("Message sent to %s\n", clients[i].name);
            }
            else
            {
                myPrint("Failed to send message to %s\n", clients[i].name);
            }
        }
    }

    myPrint("Message sent to %d clients in room %d\n", sent_count, room_number + 1);
    pthread_mutex_unlock(&clients_mutex);
}

// Send room list to client
void send_room_list(int client_socket)
{
    char room_list[500];
    strcpy(room_list, "\033[1;38;2;0;0;255mAvailable chat rooms:\033[0m 🏡\n\n");

    pthread_mutex_lock(&rooms_mutex);
    for (int i = 0; i < MAX_ROOMS; i++)
    {
        char room_info[BUFFER_SIZE];
        snprintf(room_info, BUFFER_SIZE, "\033[38;2;255;255;0m     %d. %s (%d users)\n\033[0m",
                 i + 1, rooms[i].name, rooms[i].client_count);
        strcat(room_list, room_info);
    }
    pthread_mutex_unlock(&rooms_mutex);

    strcat(room_list, "\n\033[1;38;2;255;105;180mUse /join<number> to join a room (e.g., /join1 for General)\033[0m\n");
    strcat(room_list, "\033[1;38;2;255;105;180mUse /help to know about all the commands\033[0m\n\n");
    send(client_socket, room_list, strlen(room_list), 0);
}

// Send current room info to client
void send_room_info(int socket)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].client_socket == socket)
        {
            if (clients[i].current_room != -1)
            {
                char room_info[BUFFER_SIZE];
                snprintf(room_info, BUFFER_SIZE, "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m You are in room %d (%s) with %d other users\n",
                         clients[i].current_room + 1, rooms[clients[i].current_room].name,
                         rooms[clients[i].current_room].client_count - 1);
                send(socket, room_info, strlen(room_info), 0);
                myPrint("Sent room info to %s: room %d (%s)\n",
                        clients[i].name, clients[i].current_room + 1, rooms[clients[i].current_room].name);
            }
            else
            {
                char msg[] = "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m You are not in any room. Use /join<number> to join a room.\n";
                send(socket, msg, strlen(msg), 0);
                myPrint("Sent room info to %s: not in any room\n", clients[i].name);
            }
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// List clients in a specific room
void send_room_client_list(int room_number, int client_socket)
{
    char msg[BUFFER_SIZE];
    if (room_number > 0)
    {
        snprintf(msg, BUFFER_SIZE, "\n\033[1;36m[ Room %d: %s ]\033[0m\n", 
            room_number, rooms[room_number - 1].name);
    }
    else
    {
        snprintf(msg, BUFFER_SIZE, "\n\033[1;36m[ Not in Any Room ]\033[0m\n");
    }
    send(client_socket, msg, strlen(msg), 0);

    int found = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].current_room == room_number - 1)
        {
            snprintf(msg, BUFFER_SIZE, "  • %s\n", clients[i].name);
            send(client_socket, msg, strlen(msg), 0);
            found = 1;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (!found)
    {
        snprintf(msg, BUFFER_SIZE, "  (No clients in this room)\n");
        send(client_socket, msg, strlen(msg), 0);
    }
}

// List clients in all rooms
void send_all_clients_list(int client_socket)
{
    for (int r = 0; r <= MAX_ROOMS; r++)
    {
        send_room_client_list(r, client_socket);
    }
}

void handle_mute_command(client_info *ci, const char *command)
{
    char target_name[NAME_SIZE];
    if (sscanf(command, "/mute %49s", target_name) != 1)
    {
        char msg[] = "\033[1;93mUsage: /mute <username> or /mute -all\033[0m\n";
        send(ci->client_socket, msg, strlen(msg), 0);
        return;
    }

    myPrint("\n[DEBUG] %s is trying to mute: %s\n", ci->name, target_name);

    if (strcmp(target_name, "-all") == 0)
    {
        // Mute all connected clients
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < client_count; i++)
        {
            if (clients[i].client_socket != ci->client_socket)
            {
                // Find the matching global client entry and add to mute list
                for (int k = 0; k < client_count; k++)
                {
                    if (clients[k].client_socket == ci->client_socket)
                    {
                        // Check if already muted
                        int already_muted = 0;
                        for (int j = 0; j < clients[k].muted_count; j++)
                        {
                            if (strcmp(clients[k].muted_users[j], clients[i].name) == 0)
                            {
                                already_muted = 1;
                                break;
                            }
                        }
                        
                        if (!already_muted && clients[k].muted_count < MAX_CLIENTS)
                        {
                            strncpy(clients[k].muted_users[clients[k].muted_count], clients[i].name, NAME_SIZE - 1);
                            clients[k].muted_users[clients[k].muted_count][NAME_SIZE - 1] = '\0';
                            myPrint("[DEBUG] Muted %s. Total muted: %d\n", clients[i].name, clients[k].muted_count + 1);
                            clients[k].muted_count++;
                        }
                        break;
                    }
                }
            }
        }
        pthread_mutex_unlock(&clients_mutex);
        char msg[] = "\033[1;92mAll users muted.\033[0m\n";
        send(ci->client_socket, msg, strlen(msg), 0);
        return;
    }

    pthread_mutex_lock(&clients_mutex);
    
    // First, check if target user exists
    int target_exists = 0;
    for (int i = 0; i < client_count; i++)
    {
        if (strcasecmp(clients[i].name, target_name) == 0 && clients[i].client_socket != ci->client_socket)
        {
            target_exists = 1;
            break;
        }
    }
    
    if (!target_exists)
    {
        pthread_mutex_unlock(&clients_mutex);
        char msg[BUFFER_SIZE];
        snprintf(msg, BUFFER_SIZE, "\033[1;91m❌ No client named '%s' found.\033[0m\n", target_name);
        send(ci->client_socket, msg, strlen(msg), 0);
        return;
    }
    
    // Find the matching global client entry
    for (int k = 0; k < client_count; k++)
    {
        if (clients[k].client_socket == ci->client_socket)
        {
            // Check if already muted
            for (int i = 0; i < clients[k].muted_count; i++)
            {
                if (strcasecmp(clients[k].muted_users[i], target_name) == 0)
                {
                    pthread_mutex_unlock(&clients_mutex);
                    char msg[BUFFER_SIZE];
                    snprintf(msg, BUFFER_SIZE, "\033[1;91mUser %s is already muted.\033[0m\n", target_name);
                    send(ci->client_socket, msg, strlen(msg), 0);
                    return;
                }
            }

            // Add to mute list if not full
            if (clients[k].muted_count < MAX_CLIENTS)
            {
                strncpy(clients[k].muted_users[clients[k].muted_count], target_name, NAME_SIZE - 1);
                clients[k].muted_users[clients[k].muted_count][NAME_SIZE - 1] = '\0';
                clients[k].muted_count++;
                myPrint("[DEBUG] %s muted %s. Total muted: %d\n", ci->name, target_name, clients[k].muted_count);
                pthread_mutex_unlock(&clients_mutex);
                char msg[BUFFER_SIZE];
                snprintf(msg, BUFFER_SIZE, "\033[1;92mUser %s muted.\033[0m\n", target_name);
                send(ci->client_socket, msg, strlen(msg), 0);
                return;
            }
            else
            {
                pthread_mutex_unlock(&clients_mutex);
                char msg[] = "\033[1;91mMute list full. Cannot mute more users.\033[0m\n";
                send(ci->client_socket, msg, strlen(msg), 0);
                return;
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void handle_unmute_command(client_info *ci, const char *command)
{
    char target_name[NAME_SIZE];
    if (sscanf(command, "/unmute %49s", target_name) != 1)
    {
        char msg[] = "\033[1;93mUsage: /unmute <username> or /unmute -all\033[0m\n";
        send(ci->client_socket, msg, strlen(msg), 0);
        return;
    }

    pthread_mutex_lock(&clients_mutex);
    // Find the matching global client entry
    for (int k = 0; k < client_count; k++)
    {
        if (clients[k].client_socket == ci->client_socket)
        {
            if (strcmp(target_name, "-all") == 0)
            {
                clients[k].muted_count = 0;
                pthread_mutex_unlock(&clients_mutex);
                char msg[] = "\033[1;92mAll users unmuted.\033[0m\n";
                send(ci->client_socket, msg, strlen(msg), 0);
                return;
            }

            // Find and remove the user from mute list
            for (int i = 0; i < clients[k].muted_count; i++)
            {
                if (strcasecmp(clients[k].muted_users[i], target_name) == 0)
                {
                    // Shift remaining muted users to fill the gap
                    for (int j = i; j < clients[k].muted_count - 1; j++)
                    {
                        strncpy(clients[k].muted_users[j], clients[k].muted_users[j + 1], NAME_SIZE - 1);
                        clients[k].muted_users[j][NAME_SIZE - 1] = '\0';
                    }
                    // Clear the last entry
                    clients[k].muted_users[clients[k].muted_count - 1][0] = '\0';
                    clients[k].muted_count--;
                    
                    pthread_mutex_unlock(&clients_mutex);
                    char msg[BUFFER_SIZE];
                    snprintf(msg, BUFFER_SIZE, "\033[1;92mUser %s unmuted.\033[0m\n", target_name);
                    send(ci->client_socket, msg, strlen(msg), 0);
                    return;
                }
            }
            
            // User not found in mute list
            pthread_mutex_unlock(&clients_mutex);
            char msg[BUFFER_SIZE];
            snprintf(msg, BUFFER_SIZE, "\033[1;91m❌ User '%s' is not in your mute list.\033[0m\n", target_name);
            send(ci->client_socket, msg, strlen(msg), 0);
            return;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Handle a single client
void *handle_client(void *arg)
{
    client_info *ci = (client_info *)arg;

    // Receive client name
    receive_name(ci);

    // Add client to list
    add_client(ci);

    // Announce join
    announce_join(ci);

    // Send room list and welcome message
    send_room_list(ci->client_socket);

    // Chat loop
    while (server_running)
    {
        char buffer[BUFFER_SIZE];
        memset(buffer, 0, BUFFER_SIZE);
        int bytes = recv(ci->client_socket, buffer, BUFFER_SIZE, 0);
        if (bytes > 0)
            buffer[bytes] = '\0';

        if (bytes <= 0)
        {
            remove_client(ci);
            announce_leave(ci);
            close(ci->client_socket);
            free(ci);
            break;
        }

        // Enforce disconnect
        if (strcmp(buffer, "/disconnect") == 0)
        {
            myPrint("\nClient %s requested disconnect\n", ci->name);
            remove_client(ci);
            announce_leave(ci);
            close(ci->client_socket);
            free(ci);
            break;
        }

        // Handle room commands
        if (strncmp(buffer, "/join", 5) == 0)
        {
            int room_number = atoi(buffer + 5);
            join_room(ci, room_number);
        }
        else if (strcmp(buffer, "/exit") == 0)
        {
            leave_room(ci);
        }
        else if (strcmp(buffer, "/rooms") == 0)
        {
            send_room_list(ci->client_socket);
        }
        else if (strcmp(buffer, "/room") == 0)
        {
            send_room_info(ci->client_socket);
        }
        else if(strncmp(buffer, "/mute", 5) == 0)
        {
            handle_mute_command(ci, buffer);
        }
        else if(strncmp(buffer, "/unmute", 7) == 0)
        {
            handle_unmute_command(ci, buffer);
        }
        else if (strncmp(buffer, "/ls", 3) == 0)
        {
            if (strcmp(buffer, "/ls -all") == 0)
            {
                send_all_clients_list(ci->client_socket);
            }
            else if (strncmp(buffer, "/ls -", 5) == 0)
            {
                int room_num = atoi(buffer + 5);
                if (room_num >= 0 && room_num <= MAX_ROOMS)
                {
                  send_room_client_list(room_num, ci->client_socket);
                }
                else
                {
                    char msg[] = "\033[1;91mInvalid room number. Use 1-5 or /ls -all.\033[0m\n";
                    send(ci->client_socket, msg, strlen(msg), 0);
                }
            }
            else
            {
                   char msg[] = "\033[1;93mUsage: /ls -<room_number> or /ls -all\033[0m\n";
                   send(ci->client_socket, msg, strlen(msg), 0);
            }
        }
        else if (strncmp(buffer, "/private-", 9) == 0)
        {
            char *recipient = buffer + 9;
            char *message = strchr(recipient, ' ');
            if (!message)
            {
                char msg[] = "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m Usage: /private-<name> <message>\n";
                send(ci->client_socket, msg, strlen(msg), 0);
                continue;
            }

            *message = '\0';
            message++;

            pthread_mutex_lock(&clients_mutex);
            int recipient_found = 0;
            for (int i = 0; i < client_count; i++)
            {
                if (strcasecmp(clients[i].name, recipient) == 0)
                {
                    recipient_found = 1;
                    // Check if recipient has sender muted
                    int is_muted = 0;
                    for (int j = 0; j < clients[i].muted_count; j++)
                    {
                        if (clients[i].muted_users[j][0] != '\0' && 
                            strcasecmp(clients[i].muted_users[j], ci->name) == 0)
                        {
                            is_muted = 1;
                            break;
                        }
                    }
                    
                    if (is_muted)
                    {
                        char msg[BUFFER_SIZE];
                        snprintf(msg, BUFFER_SIZE, "\033[1;91m%s has muted you. Message not delivered.\033[0m\n", recipient);
                        send(ci->client_socket, msg, strlen(msg), 0);
                    }
                    else
                    {
                        char msg_buffer[BUFFER_SIZE];
                        snprintf(msg_buffer, BUFFER_SIZE,
                                 "\033[1;95m🔒 Private from %s:\033[0m %s\n", ci->name, message);
                        send(clients[i].client_socket, msg_buffer, strlen(msg_buffer), 0);
                    }
                    break;
                }
            }
            
            if (!recipient_found)
            {
                char msg[BUFFER_SIZE];
                snprintf(msg, BUFFER_SIZE, "\033[1;91m❌ No client named '%s' found.\033[0m\n", recipient);
                send(ci->client_socket, msg, strlen(msg), 0);
            }
            
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }
        else
        {
            // Regular message - only send to room members if in a room
            if (ci->current_room != -1)
            {
                myPrint("\nClient %s in room %d sending message: %s", ci->name, ci->current_room + 1, buffer);

                char msg_buffer[BUFFER_SIZE];
                size_t name_len = strnlen(ci->name, NAME_SIZE);
                size_t msg_len = strnlen(buffer, BUFFER_SIZE);

                if (name_len + 2 + msg_len >= BUFFER_SIZE)
                {
                    msg_len = BUFFER_SIZE - name_len - 3; // leave space for ": " and null
                }

                snprintf(msg_buffer, BUFFER_SIZE, "\033[1;95;107m%s:\033[0m %.*s\n", ci->name, (int)msg_len, buffer);
                broadcast_to_room(msg_buffer, ci->client_socket, ci->current_room);
                myPrint("[Room %d] %s", ci->current_room + 1, msg_buffer);
            }
            else
            {
                myPrint("Client %s not in any room, rejecting message: %s\n", ci->name, buffer);
                char error_msg[] = "\033[1;38;2;0;0;0;48;2;255;255;255mServer:\033[0m You must join a room first. Use /join<number>\n";
                send(ci->client_socket, error_msg, strlen(error_msg), 0);
            }
        }
    }

    pthread_exit(NULL);
}

// Console thread to accept /disconnect for server shutdown
void *server_console_thread(void *arg)
{
    (void)arg; // Suppress unused parameter warning
    char cmd[256];

    myPrint("\033[1;95mServer console ready. Type '/disconnect' to shutdown server.\033[0m\n\n");

    while (server_running && fgets(cmd, 256, stdin))
    {
        cmd[strcspn(cmd, "\n")] = 0;
        if (strcmp(cmd, "/disconnect") == 0)
        {
            myPrint("\n\033[1;38;2;255;0;0m 🚨 Shutting down server... 🚨\033[0m\n");
            server_running = 0;
            break;
        }
        else if (strlen(cmd) > 0)
        {
            myPrint("Unknown command: '%s'. Type '/disconnect' to shutdown.\n", cmd);
        }
    }
    return NULL;
}
