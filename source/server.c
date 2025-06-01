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