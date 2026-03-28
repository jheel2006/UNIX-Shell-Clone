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
 *   6. Receive a placeholder acknowledgement from the server
 *   7. Print the acknowledgement and close
 *
 * We intentionally handle only one command in this milestone.
 * The repeated command loop comes in the next branch.
 */
int main(void) {
    int network_socket;
    int connection_status;
    ssize_t bytes_received;
    char command[BUFFER_SIZE];
    char server_reply[BUFFER_SIZE];
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
     * This keeps the server-side logging simple because it can print the
     * received data directly as a C string for this milestone.
     */
    if (send(network_socket, command, strlen(command) + 1, 0) == -1) {
        perror("send");
        close(network_socket);
        return EXIT_FAILURE;
    }

    /*
     * Receive a placeholder acknowledgement from the server.
     * Later branches replace this with real command output.
     */
    memset(server_reply, 0, sizeof(server_reply));
    bytes_received = recv(network_socket,
                          server_reply,
                          sizeof(server_reply) - 1,
                          0);
    if (bytes_received == -1) {
        perror("recv");
        close(network_socket);
        return EXIT_FAILURE;
    }

    if (bytes_received == 0) {
        fprintf(stderr, "[CLIENT] Server closed the connection unexpectedly.\n");
        close(network_socket);
        return EXIT_FAILURE;
    }

    printf("%s\n", server_reply);

    close(network_socket);
    return EXIT_SUCCESS;
}
