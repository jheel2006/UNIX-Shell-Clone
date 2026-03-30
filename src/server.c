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
 * Shared TCP port used by the client and server during the
 * initial handshake milestone.
 */
#define PORT 9002

/*
 * BACKLOG
 * -------
 * Number of queued pending connections that listen() may hold.
 * One client is enough for this milestone, but keeping a small
 * backlog mirrors common socket setup practice.
 */
#define BACKLOG 5

/*
 * BUFFER_SIZE
 * -----------
 * Receive buffer for the fixed handshake message.
 */
#define BUFFER_SIZE 1024

/*
 * main
 * ----
 * Phase 2 server handshake program.
 *
 * Responsibilities in this branch:
 *   1. Create a TCP socket
 *   2. Bind it to the fixed port
 *   3. Listen for one incoming client
 *   4. Accept the connection
 *   5. Receive one fixed greeting message
 *   6. Reply with one fixed response
 *   7. Close sockets cleanly
 *
 * The detailed print statements are intentional because the
 * project rubric wants visible server-side flow information.
 */
int main(void) {
    int server_socket;
    int client_socket;
    int addrlen;
    ssize_t bytes_received;
    char hello_msg[BUFFER_SIZE];
    char hello_back_msg[] = "hello back";
    struct sockaddr_in server_address;

    /*
     * Create the listening socket.
     * This is the endpoint that will later wait for a remote client.
     */
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * Populate the local address structure for bind().
     * The server listens on all local interfaces using INADDR_ANY.
     */
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;

    printf("[SERVER] Socket created successfully.\n");

    /*
     * Bind the socket to the selected port so clients know
     * where to connect.
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
     * Mark the socket as a listening socket so the kernel
     * can queue incoming client connection requests.
     */
    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen");
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Listening for client connections...\n");

    /*
     * accept() blocks until one client connects.
     * For this branch, handling a single client is enough.
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
     * Receive the client's greeting.
     * We clear the buffer first so printing it as a string remains safe.
     */
    memset(hello_msg, 0, sizeof(hello_msg));
    bytes_received = recv(client_socket, hello_msg, sizeof(hello_msg) - 1, 0);
    if (bytes_received == -1) {
        perror("recv");
        close(client_socket);
        close(server_socket);
        return EXIT_FAILURE;
    }

    if (bytes_received == 0) {
        fprintf(stderr, "[SERVER] Client disconnected before sending data.\n");
        close(client_socket);
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Received message from client: %s\n", hello_msg);

    /*
     * Reply with the fixed acknowledgement message required
     * for this handshake-only milestone.
     */
    if (send(client_socket, hello_back_msg, sizeof(hello_back_msg), 0) == -1) {
        perror("send");
        close(client_socket);
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[SERVER] Sent reply to client: %s\n", hello_back_msg);

    /*
     * Close both the connected socket and the listening socket.
     * Later branches may keep the connection open longer, but this
     * milestone ends after one successful request/response pair.
     */
    close(client_socket);
    close(server_socket);

    printf("[SERVER] Handshake complete. Server shutting down.\n");
    return EXIT_SUCCESS;
}
