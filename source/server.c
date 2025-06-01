#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  // For usleep function
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include "markdown.h"
#include "document.h"

#define MAX_CLIENTS 100
#define MAX_CMD_LEN 256
#define MAX_USERNAME_LEN 128
#define MAX_ROLE_LEN 16
#define MAX_LOG_LEN 10000
#define FIFO_PERMISSIONS 0666
#define SLEEP_INTERVAL_SEC 1
#define AUTH_DELAY_SEC 1
#define BROADCAST_INTERVAL_MULTIPLIER 1000

// Client connection structure
typedef struct {
    pid_t client_pid;
    char username[MAX_USERNAME_LEN];
    int write_fd;  // Server writes to client
    int read_fd;   // Server reads from client
    char role[MAX_ROLE_LEN];
    int permission;  // 0 = read, 1 = write
    int active;      // 1 = connected, 0 = free slot
    pthread_t thread;
} client_t;

// Command queue node
typedef struct command_node {
    char command[MAX_CMD_LEN];
    char username[MAX_USERNAME_LEN];
    struct timespec timestamp;
    struct command_node *next;
} command_node_t;

// Global state
static document *doc = NULL;
static client_t clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;
static command_node_t *command_head = NULL;
static command_node_t *command_tail = NULL;
static pthread_mutex_t command_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t server_running = 1;
static int broadcast_interval_ms = 1000;
static char broadcast_log[MAX_LOG_LEN] = "";
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function declarations
void handle_client_connection(int sig, siginfo_t *info, void *ctx);
void *client_handler_thread(void *arg);
void *stdin_command_thread(void *arg);
void *broadcast_thread(void *arg);
int authenticate_client(const char *username, char *role, int *permission);
void handle_immediate_command(int client_index, const char *command);
void enqueue_edit_command(const char *username, const char *command);
command_node_t *dequeue_command(void);
void execute_queued_command(const char *username, const char *command, 
                           char *result);
void cleanup_client_connection(int client_index);
void save_document_to_file(void);

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <TIME_INTERVAL_MS>\n", argv[0]);
        return EXIT_FAILURE;
    }

    broadcast_interval_ms = atoi(argv[1]);
    printf("Server PID: %d\n", getpid());
    fflush(stdout);

    // Initialize document and client array
    doc = markdown_init();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = 0;
    }

    // Block SIGRTMIN+1 for all threads
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGRTMIN + 1);
    pthread_sigmask(SIG_BLOCK, &block_set, NULL);

    // Setup signal handler for client connections
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = handle_client_connection;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGRTMIN, &sa, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    // Start background threads
    pthread_t stdin_thread;
    pthread_t broadcast_worker;
    pthread_create(&stdin_thread, NULL, stdin_command_thread, NULL);
    pthread_create(&broadcast_worker, NULL, broadcast_thread, NULL);

    // Main server loop - just wait for termination
    while (server_running) {
        sleep(SLEEP_INTERVAL_SEC);
    }

    // Cleanup and save document before exit
    pthread_mutex_lock(&doc_mutex);
    save_document_to_file();
    pthread_mutex_unlock(&doc_mutex);
    
    markdown_free(doc);
    return EXIT_SUCCESS;
}

// Handle new client connection signals
void handle_client_connection(int sig, siginfo_t *info, void *ctx) {
    (void)sig; 
    (void)ctx;
    pid_t client_pid = info->si_pid;
    
    // Find available client slot
    pthread_mutex_lock(&clients_mutex);
    int client_index = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            client_index = i;
            clients[i].active = 1;
            clients[i].client_pid = client_pid;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (client_index == -1) {
        // No free slots - reject client
        kill(client_pid, SIGRTMIN + 1);
        return;
    }

    // Create FIFOs for this client
    char fifo_c2s[64];
    char fifo_s2c[64];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", client_pid);

    // Clean up any existing FIFOs
    unlink(fifo_c2s);
    unlink(fifo_s2c);

    // Create new FIFOs
    if (mkfifo(fifo_c2s, FIFO_PERMISSIONS) < 0 && errno != EEXIST) {
        perror("mkfifo C2S");
        cleanup_client_connection(client_index);
        return;
    }
    if (mkfifo(fifo_s2c, FIFO_PERMISSIONS) < 0 && errno != EEXIST) {
        perror("mkfifo S2C");
        cleanup_client_connection(client_index);
        return;
    }

    // Start client handler thread
    pthread_create(&clients[client_index].thread, NULL, 
                   client_handler_thread, (void *)(intptr_t)client_index);
    
    // Send acknowledgment
    kill(client_pid, SIGRTMIN + 1);
}

// Thread to handle individual client
void *client_handler_thread(void *arg) {
    int client_index = (int)(intptr_t)arg;
    pid_t client_pid = clients[client_index].client_pid;
    
    // Build FIFO names
    char fifo_c2s[64];
    char fifo_s2c[64];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", client_pid);

    // Open FIFOs
    int fd_read = open(fifo_c2s, O_RDONLY);
    if (fd_read < 0) {
        perror("Failed to open C2S FIFO");
        cleanup_client_connection(client_index);
        return NULL;
    }
    
    int fd_write = open(fifo_s2c, O_WRONLY);
    if (fd_write < 0) {
        perror("Failed to open S2C FIFO");
        close(fd_read);
        cleanup_client_connection(client_index);
        return NULL;
    }

    clients[client_index].read_fd = fd_read;
    clients[client_index].write_fd = fd_write;

    // Read and authenticate client
    char username[MAX_USERNAME_LEN];
    ssize_t bytes_read = read(fd_read, username, sizeof(username) - 1);
    if (bytes_read <= 0) {
        perror("Failed to read username");
        goto cleanup;
    }
    username[bytes_read] = '\0';
    username[strcspn(username, "\n")] = '\0';

    // Authenticate user
    char role[MAX_ROLE_LEN];
    int permission = 0;
    if (!authenticate_client(username, role, &permission)) {
        dprintf(fd_write, "Reject UNAUTHORISED\n");
        sleep(AUTH_DELAY_SEC);  // Brief delay as per spec
        goto cleanup;
    }

    // Store client information
    strncpy(clients[client_index].username, username, 
            sizeof(clients[client_index].username) - 1);
    strncpy(clients[client_index].role, role, 
            sizeof(clients[client_index].role) - 1);
    clients[client_index].permission = permission;

    // Send authentication success and initial document
    dprintf(fd_write, "%s\n", role);
    
    // Send document version and content
    pthread_mutex_lock(&doc_mutex);
    uint64_t version = doc->current_version;
    char *doc_content = markdown_flatten(doc);
    size_t doc_length = doc_content ? strlen(doc_content) : 0;
    
    dprintf(fd_write, "%lu\n%zu\n", version, doc_length);
    if (doc_content && doc_length > 0) {
        write(fd_write, doc_content, doc_length);
    }
    pthread_mutex_unlock(&doc_mutex);
    free(doc_content);

    printf("Client connected: %s (%s)\n", username, role);

    // Command processing loop
    char command[MAX_CMD_LEN];
    while (server_running && clients[client_index].active) {
        bytes_read = read(fd_read, command, sizeof(command) - 1);
        if (bytes_read <= 0) {
            break; // Client disconnected
        }
        
        command[bytes_read] = '\0';
        command[strcspn(command, "\n")] = '\0';

        if (strcmp(command, "DISCONNECT") == 0) {
            printf("Client disconnecting: %s\n", username);
            break;
        }

        // Handle different command types
        if (strcmp(command, "DOC?") == 0 || 
            strcmp(command, "PERM?") == 0 || 
            strcmp(command, "LOG?") == 0) {
            // Immediate response commands
            handle_immediate_command(client_index, command);
        } else {
            // Edit commands - queue for batch processing
            enqueue_edit_command(username, command);
        }
    }

cleanup:
    // Cleanup client connection
    close(fd_read);
    close(fd_write);
    unlink(fifo_c2s);
    unlink(fifo_s2c);
    cleanup_client_connection(client_index);
    
    // Save document when client disconnects (to ensure latest state is saved)
    pthread_mutex_lock(&doc_mutex);
    save_document_to_file();
    pthread_mutex_unlock(&doc_mutex);
    
    return NULL;
}

// Handle commands that require immediate response
void handle_immediate_command(int client_index, const char *command) {
    int fd_write = clients[client_index].write_fd;
    
    if (strcmp(command, "DOC?") == 0) {
        pthread_mutex_lock(&doc_mutex);
        char *content = markdown_flatten(doc);
        dprintf(fd_write, "DOC?\n%s\n", content ? content : "");
        free(content);
        pthread_mutex_unlock(&doc_mutex);
    } 
    else if (strcmp(command, "PERM?") == 0) {
        dprintf(fd_write, "PERM?\n%s\n", clients[client_index].role);
    } 
    else if (strcmp(command, "LOG?") == 0) {
        pthread_mutex_lock(&log_mutex);
        dprintf(fd_write, "LOG?\n%s", broadcast_log);
        pthread_mutex_unlock(&log_mutex);
    }
}

// Add edit command to queue
void enqueue_edit_command(const char *username, const char *command) {
    command_node_t *node = (command_node_t *)malloc(sizeof(command_node_t));
    if (!node) {
        return;
    }

    strncpy(node->command, command, MAX_CMD_LEN - 1);
    node->command[MAX_CMD_LEN - 1] = '\0';
    strncpy(node->username, username, MAX_USERNAME_LEN - 1);
    node->username[MAX_USERNAME_LEN - 1] = '\0';
    clock_gettime(CLOCK_REALTIME, &node->timestamp);
    node->next = NULL;

    pthread_mutex_lock(&command_queue_mutex);
    if (!command_tail) {
        command_head = command_tail = node;
    } else {
        command_tail->next = node;
        command_tail = node;
    }
    pthread_mutex_unlock(&command_queue_mutex);
}

// Remove and return next command from queue
command_node_t *dequeue_command(void) {
    if (!command_head) {
        return NULL;
    }
    
    command_node_t *node = command_head;
    command_head = command_head->next;
    if (!command_head) {
        command_tail = NULL;
    }
    
    return node;
}