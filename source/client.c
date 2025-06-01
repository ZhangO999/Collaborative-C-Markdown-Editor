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