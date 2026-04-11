#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
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
 * Number of pending client connections the listening socket may queue.
 */
#define BACKLOG 10

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
 * ClientContext
 * -------------
 * Thread-owned session state for one connected client.
 */
typedef struct {
    int socket_fd;
    int client_number;
    int thread_number;
    struct sockaddr_in address;
} ClientContext;

/*
 * Global state used only for synchronized server-side logging and for
 * assigning deterministic client/thread numbers to new connections.
 */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int next_client_number = 1;
static int next_thread_number = 1;

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
           strncmp(payload, "open:", 5) == 0 ||
           strncmp(payload, "Unmatched quote", 15) == 0 ||
           strncmp(payload, "fork:", 5) == 0 ||
           strncmp(payload, "pipe:", 5) == 0;
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
 * print_payload_inline
 * --------------------
 * Prints one payload inline after a log prefix while trimming a final
 * newline if present. This is useful for single-line error messages,
 * where the sample server output keeps the message on the same line.
 */
static void print_payload_inline(const char *payload, size_t payload_size) {
    size_t printable_size = payload_size;

    if (payload == NULL || payload_size == 0) {
        return;
    }

    if (payload[printable_size - 1] == '\n') {
        printable_size--;
    }

    fwrite(payload, 1, printable_size, stdout);
    printf("\n");
}

/*
 * log_client_prefix
 * -----------------
 * Writes the shared client identity block used in the required output.
 */
static void log_client_prefix(const ClientContext *client) {
    char ip_address[INET_ADDRSTRLEN];

    inet_ntop(AF_INET,
              &client->address.sin_addr,
              ip_address,
              sizeof(ip_address));

    printf("[Client #%d - %s:%d]",
           client->client_number,
           ip_address,
           ntohs(client->address.sin_port));
}

/*
 * log_connection_open
 * -------------------
 * Prints the connection assignment message for a newly accepted client.
 */
static void log_connection_open(const ClientContext *client) {
    char ip_address[INET_ADDRSTRLEN];

    inet_ntop(AF_INET,
              &client->address.sin_addr,
              ip_address,
              sizeof(ip_address));

    pthread_mutex_lock(&log_mutex);
    printf("[INFO] Client #%d connected from %s:%d. Assigned to Thread-%d.\n",
           client->client_number,
           ip_address,
           ntohs(client->address.sin_port),
           client->thread_number);
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
}

/*
 * log_disconnect
 * --------------
 * Prints a client disconnect event in the same synchronized log stream.
 */
static void log_disconnect(const ClientContext *client, const char *reason) {
    pthread_mutex_lock(&log_mutex);

    if (reason != NULL) {
        printf("[INFO] ");
        log_client_prefix(client);
        printf(" %s\n", reason);
    }

    printf("[INFO] Client #%d disconnected.\n", client->client_number);
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
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
 * handle_client
 * -------------
 * Detached worker thread that owns one client connection for the full
 * duration of that remote session.
 */
static void *handle_client(void *arg) {
    ClientContext *client = arg;

    while (1) {
        ssize_t bytes_received;
        char command[BUFFER_SIZE];
        char *captured_output = NULL;
        size_t output_size = 0;
        int exit_requested;

        memset(command, 0, sizeof(command));
        bytes_received = recv(client->socket_fd, command, sizeof(command) - 1, 0);
        if (bytes_received < 0) {
            pthread_mutex_lock(&log_mutex);
            printf("[ERROR] ");
            log_client_prefix(client);
            printf(" recv failed: %s\n", strerror(errno));
            fflush(stdout);
            pthread_mutex_unlock(&log_mutex);
            break;
        }

        if (bytes_received == 0) {
            log_disconnect(client, "Client closed the connection.");
            break;
        }

        pthread_mutex_lock(&log_mutex);
        printf("[RECEIVED] ");
        log_client_prefix(client);
        printf(" Received command: \"%s\"\n", command);
        printf("[EXECUTING] ");
        log_client_prefix(client);
        printf(" Executing command: \"%s\"\n", command);
        fflush(stdout);
        pthread_mutex_unlock(&log_mutex);

        exit_requested = shell_execute_line_capture(command,
                                                    &captured_output,
                                                    &output_size);

        pthread_mutex_lock(&log_mutex);
        if (is_error_output(captured_output)) {
            printf("[ERROR] ");
            log_client_prefix(client);
            printf(" ");
            print_payload_inline(captured_output, output_size);
            printf("[OUTPUT] ");
            log_client_prefix(client);
            printf(" Sending error message to client:\n");
            print_payload_block(captured_output, output_size);
        } else if (output_size > 0) {
            printf("[OUTPUT] ");
            log_client_prefix(client);
            printf(" Sending output to client:\n");
            print_payload_block(captured_output, output_size);
        } else {
            printf("[OUTPUT] ");
            log_client_prefix(client);
            printf(" Command produced no visible output.\n");
        }
        fflush(stdout);
        pthread_mutex_unlock(&log_mutex);

        if (send_response(client->socket_fd,
                          exit_requested ? SERVER_EXIT : SERVER_CONTINUE,
                          captured_output,
                          output_size) == -1) {
            pthread_mutex_lock(&log_mutex);
            printf("[ERROR] ");
            log_client_prefix(client);
            printf(" send failed: %s\n", strerror(errno));
            fflush(stdout);
            pthread_mutex_unlock(&log_mutex);
            free(captured_output);
            break;
        }

        free(captured_output);

        if (exit_requested) {
            log_disconnect(client, "Client requested disconnect. Closing connection.");
            break;
        }
    }

    close(client->socket_fd);
    free(client);
    return NULL;
}

/*
 * main
 * ----
 * Phase 3 server entry point.
 *
 * Responsibilities in this branch:
 *   1. Create, bind, and listen on a TCP socket
 *   2. Accept clients continuously
 *   3. Spawn one detached thread per client session
 *   4. Reuse the shared shell execution path for each command
 *   5. Log every incoming and outgoing event with client identity
 */
int main(void) {
    int opt = 1;
    int server_socket;
    struct sockaddr_in server_address;

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (setsockopt(server_socket,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &opt,
                   sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_socket);
        return EXIT_FAILURE;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket,
             (struct sockaddr *) &server_address,
             sizeof(server_address)) < 0) {
        perror("bind");
        close(server_socket);
        return EXIT_FAILURE;
    }

    if (listen(server_socket, BACKLOG) < 0) {
        perror("listen");
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("[INFO] Server started, waiting for client connections...\n");
    fflush(stdout);

    while (1) {
        int client_socket;
        socklen_t client_length;
        pthread_t thread_id;
        ClientContext *client;

        client = malloc(sizeof(*client));
        if (client == NULL) {
            perror("malloc");
            close(server_socket);
            return EXIT_FAILURE;
        }

        client_length = sizeof(client->address);
        client_socket = accept(server_socket,
                               (struct sockaddr *) &client->address,
                               &client_length);
        if (client_socket < 0) {
            free(client);
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&counter_mutex);
        client->socket_fd = client_socket;
        client->client_number = next_client_number++;
        client->thread_number = next_thread_number++;
        pthread_mutex_unlock(&counter_mutex);

        log_connection_open(client);

        if (pthread_create(&thread_id, NULL, handle_client, client) != 0) {
            perror("pthread_create");
            close(client_socket);
            free(client);
            continue;
        }

        if (pthread_detach(thread_id) != 0) {
            perror("pthread_detach");
            close(client_socket);
            free(client);
            continue;
        }
    }

    close(server_socket);
    return EXIT_SUCCESS;
}
