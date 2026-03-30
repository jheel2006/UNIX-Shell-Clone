#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <netinet/in.h>

/*
 * PORT
 * ----
 * Shared TCP port used by the client and server.
 */
#define PORT 9002

/*
 * BACKLOG
 * -------
 * Number of queued pending client connection requests.
 */
#define BACKLOG 5

/*
 * BUFFER_SIZE
 * -----------
 * Maximum size of one received command and one placeholder reply.
 */
#define BUFFER_SIZE 1024

/*
 * main
 * ----
 * Phase 2 server for the command-send milestone.
 *
 * Responsibilities in this branch:
 *   1. Create, bind, and listen on a TCP socket
 *   2. Accept one client connection
 *   3. Receive one raw shell command string
 *   4. Log the exact command text on the server side
 *   5. Send back a placeholder acknowledgement
 *   6. Close sockets cleanly
 *
 * The server still does not execute the command here. This branch is
 * only about proving that command strings travel correctly over the socket.
 */
int main(void) {
    int opt = 1;
    int server_socket;
    int client_socket;
    int addrlen;
    ssize_t bytes_received;
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];
    struct sockaddr_in server_address;

    /*
     * Create the listening TCP socket.
     */
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * Allow quick server restarts while developing and testing.
     * Without this, the port may remain temporarily unavailable
     * after a recent close because of TCP TIME_WAIT behavior.
     */
    if (setsockopt(server_socket,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &opt,
                   sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_socket);
        return EXIT_FAILURE;
    }

    /*
     * Fill in the local address used by bind().
     */
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;

    printf("[SERVER] Socket created successfully.\n");

    /*
     * Bind the server socket to the chosen local port.
     */
    if (bind(server_socket,
             (struct sockaddr *) &server_address,
             sizeof(server_address)) < 0) {
        perror("bind");
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Bind successful on port %d.\n", PORT);

    /*
     * Start listening for one or more incoming connection attempts.
     */
    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen");
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Listening for client connections...\n");

    /*
     * Wait for exactly one client for this milestone.
     */
    addrlen = sizeof(server_address);
    client_socket = accept(server_socket,
                           (struct sockaddr *) &server_address,
                           (socklen_t *) &addrlen);
    if (client_socket < 0) {
        perror("accept");
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Client connected successfully.\n");

    /*
     * Receive one raw command string from the client.
     * The client sends the trailing '\0', so the server can
     * print the command exactly as a normal C string.
     */
    memset(command, 0, sizeof(command));
    bytes_received = recv(client_socket, command, sizeof(command) - 1, 0);
    if (bytes_received == -1) {
        perror("recv");
        close(client_socket);
        close(server_socket);
        return EXIT_FAILURE;
    }

    if (bytes_received == 0) {
        fprintf(stderr, "[SERVER] Client disconnected before sending a command.\n");
        close(client_socket);
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Received command from client: \"%s\"\n", command);
    printf("[SERVER] Executing command: \"%s\"\n", command);

    /*
     * This milestone stops at verified transport, so the server sends
     * back a placeholder acknowledgement instead of real command output.
     */
    snprintf(response,
             sizeof(response),
             "[CLIENT] Server received command: \"%s\"",
             command);

    if (send(client_socket, response, strlen(response) + 1, 0) == -1) {
        perror("send");
        close(client_socket);
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Sent placeholder response to client.\n");

    close(client_socket);
    close(server_socket);

    printf("[SERVER] Command transfer complete. Server shutting down.\n");
    return EXIT_SUCCESS;
}
