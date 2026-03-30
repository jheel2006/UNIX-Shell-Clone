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
 * TCP port used by both the client and the server for the
 * Phase 2 handshake milestone. This mirrors the Lab 7 style,
 * where both programs share one fixed port constant.
 */
#define PORT 9002

/*
 * BUFFER_SIZE
 * -----------
 * Fixed-size buffer used for the simple hello / hello back
 * message exchange in this branch.
 */
#define BUFFER_SIZE 1024

/*
 * main
 * ----
 * Phase 2 client handshake program.
 *
 * Responsibilities in this branch:
 *   1. Create a TCP socket
 *   2. Connect to the server socket
 *   3. Send one fixed "hello" message
 *   4. Receive one fixed reply from the server
 *   5. Print the received response
 *   6. Close the socket cleanly
 *
 * This intentionally follows the same progression used by the
 * provided Lab 7 reference before we move to real shell commands.
 */
int main(void) {
    int network_socket;
    int connection_status;
    ssize_t bytes_received;
    char hello_msg[] = "hello";
    char hello_back_msg[BUFFER_SIZE];
    struct sockaddr_in server_address;

    /*
     * Create one IPv4 TCP socket.
     * AF_INET     -> IPv4
     * SOCK_STREAM -> TCP byte stream
     */
    network_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (network_socket == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * Describe the remote server we want to connect to.
     * INADDR_ANY resolves to the local host in this simple
     * single-machine test setup used in Lab 7 style exercises.
     */
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;

    /*
     * Establish the client-to-server TCP connection.
     * If the server is not running or not listening on this port,
     * connect() fails and we report that error immediately.
     */
    connection_status = connect(network_socket,
                                (struct sockaddr *) &server_address,
                                sizeof(server_address));
    if (connection_status == -1) {
        perror("connect");
        close(network_socket);
        return EXIT_FAILURE;
    }

    printf("[CLIENT] Connected to server on port %d.\n", PORT);

    /*
     * Send one fixed greeting message to the server.
     * The terminating '\0' is included so the server can print
     * the received text directly as a C string in this simple phase.
     */
    if (send(network_socket, hello_msg, sizeof(hello_msg), 0) == -1) {
        perror("send");
        close(network_socket);
        return EXIT_FAILURE;
    }

    printf("[CLIENT] Sent message: %s\n", hello_msg);

    /*
     * Clear the receive buffer first so partial fills still leave
     * a null-terminated string for safe printf("%s") usage.
     */
    memset(hello_back_msg, 0, sizeof(hello_back_msg));
    bytes_received = recv(network_socket,
                          hello_back_msg,
                          sizeof(hello_back_msg) - 1,
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

    printf("[CLIENT] Received reply: %s\n", hello_back_msg);

    /*
     * Close the client socket after the single handshake is done.
     * Later branches will keep the connection open for repeated
     * command exchanges, but this branch stops after one round trip.
     */
    close(network_socket);
    return EXIT_SUCCESS;
}
