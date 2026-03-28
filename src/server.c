#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <netinet/in.h>

#include "shell_core.h"

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
 * Maximum size of one received command and one acknowledgement reply.
 */
#define BUFFER_SIZE 1024

/*
 * send_all
 * --------
 * Sends an entire buffer over the connected socket, retrying until all
 * bytes are written or a hard error occurs.
 */
static int send_all(int socket_fd, const void *buffer, size_t length) {
    const char *cursor = buffer;
    size_t total_sent = 0;
    ssize_t sent_now;

    while (total_sent < length) {
        sent_now = send(socket_fd, cursor + total_sent, length - total_sent, 0);
        if (sent_now <= 0) {
            return -1;
        }
        total_sent += (size_t) sent_now;
    }

    return 0;
}

/*
 * main
 * ----
 * Phase 2 server for the command-send milestone.
 *
 * Responsibilities in this branch:
 *   1. Create, bind, and listen on a TCP socket
 *   2. Accept one client connection
 *   3. Receive one raw shell command string
 *   4. Pass that command into the shared Phase 1 shell engine
 *   5. Capture the resulting stdout/stderr text
 *   6. Send that captured output back to the client
 *   6. Close sockets cleanly
 *
 * This branch intentionally reuses shell_execute_line() from shell_core
 * so the server does not duplicate parser/executor logic locally.
 */
int main(void) {
    int opt = 1;
    int server_socket;
    int client_socket;
    int addrlen;
    ssize_t bytes_received;
    size_t output_size;
    char command[BUFFER_SIZE];
    char *captured_output;
    uint32_t network_output_size;
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
     * Reuse the shared Phase 1 execution path directly, but this time
     * capture both stdout and stderr so they can be sent back over the
     * socket and displayed on the client side.
     */
    shell_execute_line_capture(command, &captured_output, &output_size);

    /*
     * Send a fixed-size length header first so the client knows how many
     * bytes of command output to read next. This supports both long output
     * and empty-output commands without relying on connection close timing.
     */
    network_output_size = htonl((uint32_t) output_size);
    if (send_all(client_socket,
                 &network_output_size,
                 sizeof(network_output_size)) == -1) {
        perror("send");
        free(captured_output);
        close(client_socket);
        close(server_socket);
        return EXIT_FAILURE;
    }

    if (output_size > 0 &&
        send_all(client_socket, captured_output, output_size) == -1) {
        perror("send");
        free(captured_output);
        close(client_socket);
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Sent %zu bytes of command output to client.\n", output_size);

    free(captured_output);
    close(client_socket);
    close(server_socket);

    printf("[SERVER] Command transfer complete. Server shutting down.\n");
    return EXIT_SUCCESS;
}
