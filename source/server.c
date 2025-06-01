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