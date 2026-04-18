CC = gcc
CFLAGS = -Wall -Wextra -g
THREAD_FLAGS = -pthread
INCLUDE = -Iinclude

CORE_SRC = src/exec.c src/pipes.c src/parser.c src/shell_core.c
CORE_OBJ = $(CORE_SRC:.c=.o)

SCHEDULER_SRC = src/scheduler.c
SCHEDULER_OBJ = $(SCHEDULER_SRC:.c=.o)

SHELL_SRC = src/main.c
SHELL_OBJ = $(SHELL_SRC:.c=.o)

CLIENT_SRC = src/client.c
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

SERVER_SRC = src/server.c
SERVER_OBJ = $(SERVER_SRC:.c=.o)

SCHEDULER_TEST_SRC = src/scheduler_test.c
SCHEDULER_TEST_OBJ = $(SCHEDULER_TEST_SRC:.c=.o)

TARGETS = myshell client server scheduler_test

.PHONY: all clean

all: $(TARGETS)

myshell: $(CORE_OBJ) $(SHELL_OBJ)
	$(CC) $(CFLAGS) $(CORE_OBJ) $(SHELL_OBJ) -o myshell

client: $(CLIENT_OBJ)
	$(CC) $(CFLAGS) $(CLIENT_OBJ) -o client

server: $(CORE_OBJ) $(SCHEDULER_OBJ) $(SERVER_OBJ)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CORE_OBJ) $(SCHEDULER_OBJ) $(SERVER_OBJ) -o server

scheduler_test: $(SCHEDULER_OBJ) $(SCHEDULER_TEST_OBJ)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(SCHEDULER_OBJ) $(SCHEDULER_TEST_OBJ) -o scheduler_test

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

clean:
	rm -f src/*.o $(TARGETS)
