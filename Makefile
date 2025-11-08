# Makefile for Chat Rooms Application

# Compiler
CC = gcc

# Flags
CFLAGS = -Wall -Wextra -std=c99 -pthread

# Directories
SERVER_DIR = server
CLIENT_DIR = client

# Server files
SERVER_SOURCES = $(SERVER_DIR)/main.c $(SERVER_DIR)/server.c $(SERVER_DIR)/utils.c
SERVER_TARGET = $(SERVER_DIR)/server

# Client files  
CLIENT_SOURCES = $(CLIENT_DIR)/main.c $(CLIENT_DIR)/client.c $(CLIENT_DIR)/utils.c
CLIENT_TARGET = $(CLIENT_DIR)/client

# Default target
all: server client

# Build server
server:
	$(CC) $(CFLAGS) -o $(SERVER_TARGET) $(SERVER_SOURCES)

# Build client
client:
	$(CC) $(CFLAGS) -o $(CLIENT_TARGET) $(CLIENT_SOURCES)

# Clean build artifacts
clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET)

# Run server (for testing)
run-server: server
	cd $(SERVER_DIR) && ./server

# Run client (for testing)
run-client: client
	cd $(CLIENT_DIR) && ./client

# Help
help:
	@echo "Available targets:"
	@echo "  all        - Build both server and client"
	@echo "  server     - Build server only"
	@echo "  client     - Build client only"
	@echo "  clean      - Remove build artifacts"
	@echo "  run-server - Build and run server"
	@echo "  run-client - Build and run client"
	@echo "  help       - Show this help message"

.PHONY: all server client clean run-server run-client help
