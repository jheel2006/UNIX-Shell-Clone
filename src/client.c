#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <netinet/in.h>

/*
 * PORT
 * ----
 * TCP port shared with the server.
 */
#define PORT 9002

/*
 * BUFFER_SIZE
 * -----------
 * Maximum size of one command line and one placeholder server reply
 * for this branch. The project already uses 1024-byte command input
 * locally, so keeping the same scale here is consistent.
 */
#define BUFFER_SIZE 1024

/*
 * send_all
 * --------
 * Retries send() until the full buffer has been transmitted or an
 * error occurs. This keeps the client/server message exchange correct
 * even if the kernel accepts only part of the data in one send() call.
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
 * recv_all
 * --------
 * Reads exactly length bytes unless the peer disconnects or recv()
 * fails. This is required once we begin sending an explicit length
 * header before the command output payload.
 */
static int recv_all(int socket_fd, void *buffer, size_t length) {
    char *cursor = buffer;
    size_t total_received = 0;
    ssize_t received_now;

    while (total_received < length) {
        received_now = recv(socket_fd,
                            cursor + total_received,
                            length - total_received,
                            0);
        if (received_now <= 0) {
            return -1;
        }
        total_received += (size_t) received_now;
    }

    return 0;
}

/*
 * trim_newline
 * ------------
 * Removes the trailing newline added by fgets() so the exact command
 * text sent to the server matches what a shell parser would expect.
 */
static void trim_newline(char *text) {
    if (text == NULL) {
        return;
    }

    text[strcspn(text, "\n")] = '\0';
}

/*
 * main
 * ----
 * Phase 2 client for the command-send milestone.
 *
 * Responsibilities in this branch:
 *   1. Create a TCP socket
 *   2. Connect to the server
 *   3. Show a shell-style "$ " prompt
 *   4. Read one full command line from the user
 *   5. Send that raw command string to the server
 *   6. Receive the server's captured output/error payload
 *   7. Print that payload exactly and close
 *
 * We intentionally handle only one command in this milestone.
 * The repeated command loop comes in the next branch.
 */
int main(void) {
    int network_socket;
    int connection_status;
    char command[BUFFER_SIZE];
    char *server_reply;
    uint32_t network_reply_size;
    size_t reply_size;
    struct sockaddr_in server_address;

    /*
     * Create the client TCP socket.
     */
    network_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (network_socket == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * Prepare the server address information used by connect().
     */
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;

    /*
     * Connect the client socket to the listening server.
     */
    connection_status = connect(network_socket,
                                (struct sockaddr *) &server_address,
                                sizeof(server_address));
    if (connection_status == -1) {
        perror("connect");
        close(network_socket);
        return EXIT_FAILURE;
    }

    /*
     * Read one command line in the same style as the local shell:
     * display "$ " and wait for one full line of input.
     */
    printf("$ ");
    fflush(stdout);

    if (fgets(command, sizeof(command), stdin) == NULL) {
        fprintf(stderr, "[CLIENT] Failed to read a command from standard input.\n");
        close(network_socket);
        return EXIT_FAILURE;
    }

    trim_newline(command);

    /*
     * Send the raw command string, including its terminating null byte.
     * The server still logs the command as a plain C string, so sending
     * the terminator keeps that side simple.
     */
    if (send_all(network_socket, command, strlen(command) + 1) == -1) {
        perror("send");
        close(network_socket);
        return EXIT_FAILURE;
    }

    /*
     * Receive the exact byte length of the captured output first.
     * This lets the client handle outputs longer than one recv() call
     * and also supports empty-output commands cleanly.
     */
    if (recv_all(network_socket,
                 &network_reply_size,
                 sizeof(network_reply_size)) == -1) {
        perror("recv");
        close(network_socket);
        return EXIT_FAILURE;
    }

    reply_size = (size_t) ntohl(network_reply_size);
    server_reply = malloc(reply_size + 1);
    if (server_reply == NULL) {
        fprintf(stderr, "[CLIENT] Failed to allocate memory for server output.\n");
        close(network_socket);
        return EXIT_FAILURE;
    }

    if (reply_size > 0 &&
        recv_all(network_socket, server_reply, reply_size) == -1) {
        perror("recv");
        free(server_reply);
        close(network_socket);
        return EXIT_FAILURE;
    }

    server_reply[reply_size] = '\0';

    /*
     * Print the returned command output exactly as received.
     * We avoid adding our own formatting so shell output and error
     * text remain unchanged from the server-side execution path.
     */
    if (reply_size > 0) {
        fwrite(server_reply, 1, reply_size, stdout);
        if (server_reply[reply_size - 1] != '\n') {
            printf("\n");
        }
    }

    free(server_reply);
    close(network_socket);
    return EXIT_SUCCESS;
}
