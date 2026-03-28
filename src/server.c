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
 * Maximum size of one received command string.
 */
#define BUFFER_SIZE 1024

/*
 * SERVER_CONTINUE
 * ---------------
 * Response status sent after normal command processing when the server
 * should remain connected and wait for another command from the client.
 */
#define SERVER_CONTINUE 0U

/*
 * SERVER_EXIT
 * -----------
 * Response status sent when the received command requests a clean
 * end to the remote shell session.
 */
#define SERVER_EXIT 1U

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
 * is_error_output
 * ---------------
 * Best-effort classifier for the text returned by the shared shell path.
 * The server only receives one combined stdout/stderr payload, so this
 * helper uses known assignment error strings and common perror prefixes
 * to decide whether the payload should be described as normal output or
 * an error message in the server log.
 */
static int is_error_output(const char *payload) {
    if (payload == NULL || *payload == '\0') {
        return 0;
    }

    return strncmp(payload, "Input file not specified.", 25) == 0 ||
           strncmp(payload, "Output file not specified.", 26) == 0 ||
           strncmp(payload, "Error output file not specified.", 32) == 0 ||
           strncmp(payload, "Command missing after pipe.", 27) == 0 ||
           strncmp(payload, "Empty command between pipes.", 28) == 0 ||
           strncmp(payload, "Command not found.", 18) == 0 ||
           strncmp(payload, "Command not found in pipe sequence.", 35) == 0 ||
           strncmp(payload, "open:", 5) == 0;
}

/*
 * print_payload_block
 * -------------------
 * Prints the command payload on the server terminal in a readable way
 * without changing the exact bytes that will be sent back to the client.
 */
static void print_payload_block(const char *payload, size_t payload_size) {
    if (payload == NULL || payload_size == 0) {
        return;
    }

    fwrite(payload, 1, payload_size, stdout);
    if (payload[payload_size - 1] != '\n') {
        printf("\n");
    }
}

/*
 * send_response
 * -------------
 * Sends one complete response packet for a command:
 *   1. 32-bit status code
 *   2. 32-bit payload length
 *   3. payload bytes
 *
 * This packet format lets the server keep the socket open across
 * multiple commands while still allowing the client to determine
 * where each response ends.
 */
static int send_response(int client_socket,
                         uint32_t status,
                         const char *payload,
                         size_t payload_size) {
    uint32_t network_status = htonl(status);
    uint32_t network_payload_size = htonl((uint32_t) payload_size);

    if (send_all(client_socket, &network_status, sizeof(network_status)) == -1) {
        return -1;
    }

    if (send_all(client_socket,
                 &network_payload_size,
                 sizeof(network_payload_size)) == -1) {
        return -1;
    }

    if (payload_size > 0 &&
        send_all(client_socket, payload, payload_size) == -1) {
        return -1;
    }

    return 0;
}

/*
 * main
 * ----
 * Phase 2 server for the polished persistent-session milestone.
 *
 * Responsibilities in this branch:
 *   1. Create, bind, and listen on a TCP socket
 *   2. Accept one client connection
 *   3. Repeatedly receive commands over the same connection
 *   4. Pass each command into the shared Phase 1 shell engine
 *   5. Send the resulting output/error text back to the client
 *   6. Stop only on disconnect or clean "exit"
 *   7. Close sockets cleanly
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
    char command[BUFFER_SIZE];
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

    /*
     * Start listening for one or more incoming connection attempts.
     */
    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen");
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[INFO] Server started, waiting for client connections...\n");

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

    printf("[INFO] Client connected.\n");

    while (1) {
        char *captured_output;
        size_t output_size;
        int exit_requested;

        /*
         * Receive the next raw command string from the same connected client.
         * Each command is still sent as a null-terminated string so the server
         * can log it directly without extra parsing at the transport layer.
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
            printf("[INFO] Client disconnected. Closing session.\n");
            break;
        }

        printf("[RECEIVED] Received command: \"%s\" from client.\n", command);
        printf("[EXECUTING] Executing command: \"%s\"\n", command);

        /*
         * Capture both stdout and stderr for the current command using the
         * same shared Phase 1 shell path already used by the local shell.
         */
        exit_requested = shell_execute_line_capture(command,
                                                    &captured_output,
                                                    &output_size);

        /*
         * Return the command output and session status together so the client
         * knows whether to print another prompt or terminate cleanly.
         */
        if (is_error_output(captured_output)) {
            printf("[ERROR] Command produced an error response.\n");
            printf("[OUTPUT] Sending error message to client:\n");
            print_payload_block(captured_output, output_size);
        } else if (output_size > 0) {
            printf("[OUTPUT] Sending output to client:\n");
            print_payload_block(captured_output, output_size);
        } else {
            printf("[OUTPUT] Command produced no visible output.\n");
        }

        if (send_response(client_socket,
                          exit_requested ? SERVER_EXIT : SERVER_CONTINUE,
                          captured_output,
                          output_size) == -1) {
            perror("send");
            free(captured_output);
            close(client_socket);
            close(server_socket);
            return EXIT_FAILURE;
        }

        free(captured_output);

        if (exit_requested) {
            printf("[INFO] Exit command received. Closing session.\n");
            break;
        }
    }

    close(client_socket);
    close(server_socket);

    printf("[INFO] Session complete. Server shutting down.\n");
    return EXIT_SUCCESS;
}
