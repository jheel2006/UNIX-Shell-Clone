CC = gcc
CFLAGS = -Wall -Wextra -g
SRC = src/main.c src/exec.c src/pipes.c src/parser.c
OBJ = $(SRC:.c=.o)
INCLUDE = -Iinclude

myshell: $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o myshell

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

clean:
	rm -f src/*.o myshell
