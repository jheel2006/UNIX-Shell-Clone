CC = gcc
CFLAGS = -Wall -Wextra -g
INCLUDE = -Iinclude

CORE_SRC = src/exec.c src/pipes.c src/parser.c src/shell_core.c
CORE_OBJ = $(CORE_SRC:.c=.o)

SHELL_SRC = src/main.c
SHELL_OBJ = $(SHELL_SRC:.c=.o)

CLIENT_SRC = src/client.c
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

SERVER_SRC = src/server.c
SERVER_OBJ = $(SERVER_SRC:.c=.o)

TARGETS = myshell client server

all: $(TARGETS)

myshell: $(CORE_OBJ) $(SHELL_OBJ)
	$(CC) $(CFLAGS) $(CORE_OBJ) $(SHELL_OBJ) -o myshell

client: $(CLIENT_OBJ)
	$(CC) $(CFLAGS) $(CLIENT_OBJ) -o client

server: $(SERVER_OBJ)
	$(CC) $(CFLAGS) $(SERVER_OBJ) -o server

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

clean:
	rm -f src/*.o $(TARGETS)
