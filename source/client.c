#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include "markdown.h"

#define MAX_COMMAND_LENGTH 256
#define MAX_USERNAME_LENGTH 128
#define MAX_RESPONSE_LENGTH 4096
#define HANDSHAKE_TIMEOUT_SEC 1

// Global state
static int server_write_fd = -1;  // FIFO_C2S for writing commands to server
static int server_read_fd = -1;   // FIFO_S2C for reading responses from server
static char client_username[MAX_USERNAME_LENGTH];
static char user_role[16];
static document *local_document = NULL;
static volatile sig_atomic_t handshake_complete = 0;

// Signal handler for server acknowledgment
void handshake_signal_handler(int signal_number) {
    if (signal_number == SIGRTMIN + 1) {
        handshake_complete = 1;
    }
}

// Setup signal handling for handshake
void setup_signal_handling(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handshake_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGRTMIN + 1, &sa, NULL) < 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

// Perform initial handshake with server
int perform_handshake(pid_t server_pid) {
    printf("Connecting to server (PID: %d)...\n", server_pid);
    
    // Send SIGRTMIN to server
    if (kill(server_pid, SIGRTMIN) < 0) {
        perror("Failed to signal server");
        return -1;
    }

    // Wait for SIGRTMIN+1 response with timeout
    alarm(HANDSHAKE_TIMEOUT_SEC);  // 1 second timeout
    while (!handshake_complete) {
        pause();
    }
    alarm(0);

    if (!handshake_complete) {
        fprintf(stderr, "Server did not respond to connection request\n");
        return -1;
    }

    return 0;
}

// Open communication FIFOs
int open_communication_channels(void) {
    pid_t my_pid = getpid();
    char fifo_c2s[64];
    char fifo_s2c[64];
    
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", my_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", my_pid);

    // Open FIFOs
    server_write_fd = open(fifo_c2s, O_WRONLY);
    if (server_write_fd < 0) {
        perror("Failed to open client-to-server FIFO");
        return -1;
    }

    server_read_fd = open(fifo_s2c, O_RDONLY);
    if (server_read_fd < 0) {
        perror("Failed to open server-to-client FIFO");
        close(server_write_fd);
        return -1;
    }

    return 0;
}

// Authenticate and download initial document
int authenticate_and_download(void) {
    // Send username
    dprintf(server_write_fd, "%s\n", client_username);

    // Read role response
    char response[256];
    ssize_t bytes_read = read(server_read_fd, response, sizeof(response) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read authentication response");
        return -1;
    }
    response[bytes_read] = '\0';
    
    // Check for rejection
    if (strncmp(response, "Reject", 6) == 0) {
        printf("Authentication failed: %s", response);
        return -1;
    }

    // Extract role
    sscanf(response, "%15s", user_role);

    // Read version
    bytes_read = read(server_read_fd, response, sizeof(response) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read document version");
        return -1;
    }
    response[bytes_read] = '\0';
    uint64_t version = strtoull(response, NULL, 10);

    // Read document length
    bytes_read = read(server_read_fd, response, sizeof(response) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read document length");
        return -1;
    }
    response[bytes_read] = '\0';
    size_t doc_length = strtoull(response, NULL, 10);

    // Initialize local document - but DON'T populate it yet
    local_document = markdown_init();
    
    // Read and discard initial document content - client should wait for 
    // broadcasts
    if (doc_length > 0) {
        char *content = (char *)malloc(doc_length + 1);
        if (!content) {
            perror("malloc");
            return -1;
        }
        
        size_t total_read = 0;
        while (total_read < doc_length) {
            ssize_t chunk = read(server_read_fd, content + total_read, 
                                doc_length - total_read);
            if (chunk <= 0) {
                free(content);
                perror("Failed to read document content");
                return -1;
            }
            total_read += chunk;
        }
        // Don't insert content into local document - wait for server 
        // broadcasts
        free(content);
    }
    
    // Don't set document version - wait for server broadcasts
    (void)version; // Suppress unused variable warning

    printf("Connected as '%s' with '%s' permissions\n", 
           client_username, user_role);
    return 0;
}