#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
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
 * Maximum size of one command line read from the terminal.
 * This stays aligned with the shell's local input size.
 */
#define BUFFER_SIZE 1024

/*
 * SERVER_CONTINUE
 * ---------------
 * Status flag sent by the server when the client should stay in the
 * current session and prompt for another command.
 */
#define SERVER_CONTINUE 0U

/*
 * SERVER_EXIT
 * -----------
 * Status flag sent by the server when the remote shell session should
 * terminate cleanly. This is used for the "exit" command.
 */
#define SERVER_EXIT 1U

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
 * receive_response
 * ----------------
 * Reads one server response packet for the current session command.
 * The packet consists of:
 *   1. a 32-bit status flag
 *   2. a 32-bit payload length
 *   3. payload bytes (command output / error text)
 *
 * The caller owns *server_reply and must free it.
 */
static int receive_response(int socket_fd,
                            uint32_t *server_status,
                            char **server_reply,
                            size_t *reply_size) {
    uint32_t network_status;
    uint32_t network_reply_size;

    if (server_status == NULL || server_reply == NULL || reply_size == NULL) {
        return -1;
    }

    *server_reply = NULL;
    *reply_size = 0;

    if (recv_all(socket_fd, &network_status, sizeof(network_status)) == -1) {
        return -1;
    }

    if (recv_all(socket_fd,
                 &network_reply_size,
                 sizeof(network_reply_size)) == -1) {
        return -1;
    }

    *server_status = ntohl(network_status);
    *reply_size = (size_t) ntohl(network_reply_size);
    *server_reply = malloc(*reply_size + 1);
    if (*server_reply == NULL) {
        return -1;
    }

    if (*reply_size > 0 &&
        recv_all(socket_fd, *server_reply, *reply_size) == -1) {
        free(*server_reply);
        *server_reply = NULL;
        return -1;
    }

    (*server_reply)[*reply_size] = '\0';
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
 * Phase 2 client for the persistent-session milestone.
 *
 * Responsibilities in this branch:
 *   1. Create a TCP socket
 *   2. Connect to the server
 *   3. Repeatedly show a shell-style "$ " prompt
 *   4. Read one command at a time and send it to the server
 *   5. Receive one response packet per command
 *   6. Print returned output/error text exactly
 *   7. Stop only when the server signals session exit or input ends
 */
int main(void) {
    int network_socket;
    int connection_status;
    char command[BUFFER_SIZE];
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
    if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) != 1) {
        fprintf(stderr, "[CLIENT] Failed to parse server address.\n");
        close(network_socket);
        return EXIT_FAILURE;
    }

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

    while (1) {
        char *server_reply;
        size_t reply_size;
        uint32_t server_status;

        /*
         * Re-prompt after every completed round trip so the remote
         * client behaves like an interactive shell session.
         */
        printf("$ ");
        fflush(stdout);

        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }

        trim_newline(command);

        /*
         * Send the raw command string, including the null terminator.
         * The single persistent connection stays open for later commands.
         */
        if (send_all(network_socket, command, strlen(command) + 1) == -1) {
            perror("send");
            close(network_socket);
            return EXIT_FAILURE;
        }

        /*
         * Receive one complete response packet for this command.
         * The status flag tells the client whether to continue the
         * session or exit after printing the returned payload.
         */
        if (receive_response(network_socket,
                             &server_status,
                             &server_reply,
                             &reply_size) == -1) {
            fprintf(stderr, "[CLIENT] Failed to receive a complete server response.\n");
            close(network_socket);
            return EXIT_FAILURE;
        }

        if (reply_size > 0) {
            fwrite(server_reply, 1, reply_size, stdout);
            if (server_reply[reply_size - 1] != '\n') {
                printf("\n");
            }
        }

        free(server_reply);

        if (server_status == SERVER_EXIT) {
            printf("Disconnected from server.\n");
            break;
        }
    }

    close(network_socket);
    return EXIT_SUCCESS;
}
