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
#include "server.h"

#define MAX_CLIENTS 100
#define MAX_CMD_LEN 256
#define MAX_USERNAME_LEN 128
#define MAX_ROLE_LEN 16
#define MAX_LOG_LEN 10000

typedef struct {
    pid_t client_pid;
    char username[MAX_USERNAME_LEN];
    int write_fd;
    int read_fd;
    int permission;  // 0 = read, 1 = write
    int active;      // 1 = connected, 0 = free slot
    pthread_t thread;
} client_t;

typedef struct command_node {
    char command[MAX_CMD_LEN];
    char username[MAX_USERNAME_LEN];
    struct timespec timestamp;
    struct command_node *next;
} command_node_t;

// Global variables (for testing access)
document *doc = NULL;
static client_t clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static command_node_t *command_head = NULL;
static command_node_t *command_tail = NULL;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

// Function implementations (copied from server.c, excluding main)

int get_user_permissions(const char *username, char *role, int *permission) {
    FILE *roles_file = fopen("roles.txt", "r");
    if (!roles_file) return 0;

    char file_username[MAX_USERNAME_LEN], file_role[MAX_ROLE_LEN];
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

void enqueue_command(const char *username, const char *command) {
    command_node_t *node = malloc(sizeof(command_node_t));
    if (!node) return;

    strncpy(node->command, command, MAX_CMD_LEN - 1);
    strncpy(node->username, username, MAX_USERNAME_LEN - 1);
    clock_gettime(CLOCK_REALTIME, &node->timestamp);
    node->next = NULL;

    pthread_mutex_lock(&queue_lock);
    if (!command_tail) {
        command_head = command_tail = node;
    } else {
        command_tail->next = node;
        command_tail = node;
    }
    pthread_mutex_unlock(&queue_lock);
}

command_node_t *dequeue_command(void) {
    if (!command_head) return NULL;
    
    command_node_t *node = command_head;
    command_head = command_head->next;
    if (!command_head) command_tail = NULL;
    
    return node;
}

void apply_command(const char *username, const char *command, char *result) {
    // Initialize doc if not already done
    if (!doc) {
        doc = markdown_init();
    }

    // Check permissions for write commands
    int user_permission = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].username, username) == 0) {
            user_permission = clients[i].permission;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    // For testing purposes, if user not found in clients array, check roles.txt directly
    if (user_permission == 0) {
        char role[MAX_ROLE_LEN];
        if (get_user_permissions(username, role, &user_permission)) {
            // User found in roles.txt, use that permission
        }
    }

    char cmd_type[32];
    sscanf(command, "%31s", cmd_type);
    
    // Check if command requires write permission
    if (strcmp(cmd_type, "INSERT") == 0 || strcmp(cmd_type, "DEL") == 0 ||
        strcmp(cmd_type, "NEWLINE") == 0 || strcmp(cmd_type, "HEADING") == 0 ||
        strcmp(cmd_type, "BOLD") == 0 || strcmp(cmd_type, "ITALIC") == 0 ||
        strcmp(cmd_type, "BLOCKQUOTE") == 0 || strcmp(cmd_type, "ORDERED_LIST") == 0 ||
        strcmp(cmd_type, "UNORDERED_LIST") == 0 || strcmp(cmd_type, "CODE") == 0 ||
        strcmp(cmd_type, "HORIZONTAL_RULE") == 0 || strcmp(cmd_type, "LINK") == 0) {
        
        if (!user_permission) {
            strcpy(result, "Reject UNAUTHORISED");
            return;
        }
    }

    // Parse and execute command
    int ret = 0;
    if (strcmp(cmd_type, "INSERT") == 0) {
        size_t pos;
        char content[256];
        if (sscanf(command, "INSERT %zu %255[^\n]", &pos, content) == 2) {
            ret = markdown_insert(doc, doc->current_version, pos, content);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "DEL") == 0) {
        size_t pos, len;
        if (sscanf(command, "DEL %zu %zu", &pos, &len) == 2) {
            ret = markdown_delete(doc, doc->current_version, pos, len);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "NEWLINE") == 0) {
        size_t pos;
        if (sscanf(command, "NEWLINE %zu", &pos) == 1) {
            ret = markdown_newline(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "HEADING") == 0) {
        size_t level, pos;
        if (sscanf(command, "HEADING %zu %zu", &level, &pos) == 2) {
            ret = markdown_heading(doc, doc->current_version, level, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "BOLD") == 0) {
        size_t start, end;
        if (sscanf(command, "BOLD %zu %zu", &start, &end) == 2) {
            ret = markdown_bold(doc, doc->current_version, start, end);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "ITALIC") == 0) {
        size_t start, end;
        if (sscanf(command, "ITALIC %zu %zu", &start, &end) == 2) {
            ret = markdown_italic(doc, doc->current_version, start, end);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "BLOCKQUOTE") == 0) {
        size_t pos;
        if (sscanf(command, "BLOCKQUOTE %zu", &pos) == 1) {
            ret = markdown_blockquote(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "ORDERED_LIST") == 0) {
        size_t pos;
        if (sscanf(command, "ORDERED_LIST %zu", &pos) == 1) {
            ret = markdown_ordered_list(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "UNORDERED_LIST") == 0) {
        size_t pos;
        if (sscanf(command, "UNORDERED_LIST %zu", &pos) == 1) {
            ret = markdown_unordered_list(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "CODE") == 0) {
        size_t start, end;
        if (sscanf(command, "CODE %zu %zu", &start, &end) == 2) {
            ret = markdown_code(doc, doc->current_version, start, end);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "HORIZONTAL_RULE") == 0) {
        size_t pos;
        if (sscanf(command, "HORIZONTAL_RULE %zu", &pos) == 1) {
            ret = markdown_horizontal_rule(doc, doc->current_version, pos);
        } else {
            strcpy(result, "Reject INVALID_POSITION");
            return;
        }
    } else if (strcmp(cmd_type, "LINK") == 0) {
        size_t start, end;
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

void cleanup_client(int client_index) {
    pthread_mutex_lock(&clients_mutex);
    clients[client_index].active = 0;
    memset(&clients[client_index], 0, sizeof(client_t));
    pthread_mutex_unlock(&clients_mutex);
}

void save_document(void) {
    if (!doc) {
        doc = markdown_init();
    }
    
    FILE *file = fopen("doc.md", "w");
    if (file) {
        char *content = markdown_flatten(doc);
        if (content) {
            fprintf(file, "%s", content);
            free(content);
        }
        fclose(file);
    }
} 