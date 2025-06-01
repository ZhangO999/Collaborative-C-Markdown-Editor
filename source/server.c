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

// Background thread that processes command queue and broadcasts updates
void *broadcast_thread(void *arg) {
    (void)arg;
    
    while (server_running) {
        // Convert ms to microseconds
        usleep(broadcast_interval_ms * BROADCAST_INTERVAL_MULTIPLIER); 
        
        // Check if there are commands to process
        pthread_mutex_lock(&command_queue_mutex);
        if (!command_head) {
            pthread_mutex_unlock(&command_queue_mutex);
            continue;
        }

        // Collect all commands from queue first
        command_node_t *commands_to_process = command_head;
        command_head = command_tail = NULL;
        pthread_mutex_unlock(&command_queue_mutex);

        // Now process all commands while holding doc mutex
        pthread_mutex_lock(&doc_mutex);
        uint64_t old_version = doc->current_version;
        char version_message[MAX_LOG_LEN];
        char temp_buffer[1024];
        
        snprintf(version_message, sizeof(version_message), 
                "VERSION %lu\n", old_version + 1);

        command_node_t *cmd = commands_to_process;
        int commands_processed = 0;
        while (cmd != NULL) {
            char result[256];
            execute_queued_command(cmd->username, cmd->command, result);
            
            snprintf(temp_buffer, sizeof(temp_buffer), 
                    "EDIT %s %s %s\n", cmd->username, cmd->command, result);
            strcat(version_message, temp_buffer);
            
            commands_processed++;
            
            command_node_t *next = cmd->next;
            free(cmd);
            cmd = next;
        }
        
        strcat(version_message, "END\n");

        // Only increment version and broadcast if commands were processed
        if (commands_processed > 0) {
            markdown_increment_version(doc);
            
            // Update broadcast log
            pthread_mutex_lock(&log_mutex);
            strcat(broadcast_log, version_message);
            pthread_mutex_unlock(&log_mutex);
            
            // Broadcast to all clients
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active) {
                    write(clients[i].write_fd, version_message, 
                          strlen(version_message));
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        }
        
        pthread_mutex_unlock(&doc_mutex);
    }
    
    return NULL;
}

// Thread to handle server stdin commands
void *stdin_command_thread(void *arg) {
    (void)arg;
    char command[128];
    
    while (fgets(command, sizeof(command), stdin)) {
        command[strcspn(command, "\n")] = '\0';
        
        if (strcmp(command, "QUIT") == 0) {
            pthread_mutex_lock(&clients_mutex);
            int active_clients = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active) {
                    active_clients++;
                }
            }
            
            if (active_clients == 0) {
                printf("Shutting down server...\n");
                save_document_to_file();
                server_running = 0;
                exit(0);
            } else {
                printf("QUIT rejected, %d clients still connected.\n", 
                       active_clients);
            }
            pthread_mutex_unlock(&clients_mutex);
        } 
        else if (strcmp(command, "DOC?") == 0) {
            pthread_mutex_lock(&doc_mutex);
            char *content = markdown_flatten(doc);
            printf("DOC?\n%s\n", content ? content : "");
            free(content);
            pthread_mutex_unlock(&doc_mutex);
        } 
        else if (strcmp(command, "LOG?") == 0) {
            pthread_mutex_lock(&log_mutex);
            printf("LOG?\n%s", broadcast_log);
            pthread_mutex_unlock(&log_mutex);
        }
    }
    return NULL;
}

// Authenticate client against roles.txt
int authenticate_client(const char *username, char *role, int *permission) {
    FILE *roles_file = fopen("roles.txt", "r");
    if (!roles_file) {
        return 0;
    }

    char file_username[MAX_USERNAME_LEN];
    char file_role[MAX_ROLE_LEN];
    while (fscanf(roles_file, "%127s %15s", file_username, file_role) == 2) {
        if (strcmp(file_username, username) == 0) {
            strcpy(role, file_role);
            *permission = (strcmp(file_role, "write") == 0) ? 1 : 0;
            fclose(roles_file);
            return 1;
        }
    }
    
    fclose(roles_file);
    return 0;
}

// Execute a queued edit command
void execute_queued_command(const char *username, const char *command, 
                           char *result) {
    // Check user permissions
    int user_permission = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].username, username) == 0) {
            user_permission = clients[i].permission;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    // Parse command type
    char cmd_type[32];
    sscanf(command, "%31s", cmd_type);
    
    // Check if command requires write permission
    const char *write_commands[] = {"INSERT", "DEL", "NEWLINE", "HEADING", 
                                   "BOLD", "ITALIC", "BLOCKQUOTE", 
                                   "ORDERED_LIST", "UNORDERED_LIST", "CODE", 
                                   "HORIZONTAL_RULE", "LINK"};
    int requires_write = 0;
    size_t num_write_commands = sizeof(write_commands) / 
                               sizeof(write_commands[0]);
    for (size_t i = 0; i < num_write_commands; i++) {
        if (strcmp(cmd_type, write_commands[i]) == 0) {
            requires_write = 1;
            break;
        }
    }
    
    if (requires_write && !user_permission) {
        strcpy(result, "Reject UNAUTHORISED");
        return;
    }

    // Execute command
    int ret = 0;
    if (strcmp(cmd_type, "INSERT") == 0) {
        size_t pos = 0;
        char content[256];
        if (sscanf(command, "INSERT %zu %255[^\n]", &pos, content) == 2) {
            ret = markdown_insert(doc, doc->current_version, pos, content);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "DEL") == 0) {
        size_t pos = 0;
        size_t len = 0;
        if (sscanf(command, "DEL %zu %zu", &pos, &len) == 2) {
            ret = markdown_delete(doc, doc->current_version, pos, len);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "NEWLINE") == 0) {
        size_t pos = 0;
        if (sscanf(command, "NEWLINE %zu", &pos) == 1) {
            ret = markdown_newline(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "HEADING") == 0) {
        size_t level = 0;
        size_t pos = 0;
        if (sscanf(command, "HEADING %zu %zu", &level, &pos) == 2) {
            ret = markdown_heading(doc, doc->current_version, level, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "BOLD") == 0) {
        size_t start = 0;
        size_t end = 0;
        if (sscanf(command, "BOLD %zu %zu", &start, &end) == 2) {
            ret = markdown_bold(doc, doc->current_version, start, end);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "ITALIC") == 0) {
        size_t start = 0;
        size_t end = 0;
        if (sscanf(command, "ITALIC %zu %zu", &start, &end) == 2) {
            ret = markdown_italic(doc, doc->current_version, start, end);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "BLOCKQUOTE") == 0) {
        size_t pos = 0;
        if (sscanf(command, "BLOCKQUOTE %zu", &pos) == 1) {
            ret = markdown_blockquote(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "ORDERED_LIST") == 0) {
        size_t pos = 0;
        if (sscanf(command, "ORDERED_LIST %zu", &pos) == 1) {
            ret = markdown_ordered_list(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "UNORDERED_LIST") == 0) {
        size_t pos = 0;
        if (sscanf(command, "UNORDERED_LIST %zu", &pos) == 1) {
            ret = markdown_unordered_list(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "CODE") == 0) {
        size_t start = 0;
        size_t end = 0;
        if (sscanf(command, "CODE %zu %zu", &start, &end) == 2) {
            ret = markdown_code(doc, doc->current_version, start, end);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "HORIZONTAL_RULE") == 0) {
        size_t pos = 0;
        if (sscanf(command, "HORIZONTAL_RULE %zu", &pos) == 1) {
            ret = markdown_horizontal_rule(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "LINK") == 0) {
        size_t start = 0;
        size_t end = 0;
        char url[256];
        if (sscanf(command, "LINK %zu %zu %255s", &start, &end, url) == 3) {
            ret = markdown_link(doc, doc->current_version, start, end, url);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else {
        strcpy(result, "Reject INVALID_POSITION");
        return;
    }

    // Convert return code to result string
    switch (ret) {
        case SUCCESS:
            strcpy(result, "SUCCESS");
            break;
        case INVALID_CURSOR_POS:
            strcpy(result, "Reject INVALID_POSITION");
            break;
        case DELETED_POSITION:
            strcpy(result, "Reject DELETED_POSITION");
            break;
        case OUTDATED_VERSION:
            strcpy(result, "Reject OUTDATED_VERSION");
            break;
        default:
            strcpy(result, "Reject INVALID_POSITION");
            break;
    }
}

// Clean up client connection
void cleanup_client_connection(int client_index) {
    pthread_mutex_lock(&clients_mutex);
    clients[client_index].active = 0;
    memset(&clients[client_index], 0, sizeof(client_t));
    pthread_mutex_unlock(&clients_mutex);
}

// Save document to file
void save_document_to_file(void) {
    FILE *file = fopen("doc.md", "w");
    if (file) {
        char *content = markdown_flatten(doc);
        if (content) {
            fprintf(file, "%s", content);
            free(content);
        }
        fclose(file);
        printf("Document saved to doc.md\n");
    }
}
