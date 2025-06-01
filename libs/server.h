#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include <pthread.h>
#include "document.h"

// Server function declarations for testing
int authenticate_client(const char *username, char *role, int *permission);
void execute_queued_command(const char *username, const char *command, 
                           char *result);
void save_document_to_file(void);
void enqueue_edit_command(const char *username, const char *command);
void cleanup_client_connection(int client_index);

// Additional functions from server_lib.c for testing
int get_user_permissions(const char *username, char *role, int *permission);
void apply_command(const char *username, const char *command, char *result);
void save_document(void);
void enqueue_command(const char *username, const char *command);

// For testing access to global state
extern document *doc;

#endif // SERVER_H 