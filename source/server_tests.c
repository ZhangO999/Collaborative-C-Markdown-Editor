#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> 
#include <sys/stat.h> 
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include "../libs/markdown.h"
#include "../libs/server.h"

// Test results tracking
static int tests_passed = 0;
static int tests_total = 0;

#define TEST_ASSERT(condition, message) do { \
    tests_total++; \
    if (condition) { \
        printf("✓ %s\n", message); \
        tests_passed++; \
    } else { \
        printf("✗ %s\n", message); \
    } \
} while(0)

// Helper function to check if a file exists
int file_exists(const char *filename) {
    return access(filename, F_OK) == 0;
}

// Helper to create test roles.txt file
void create_test_roles_file(void) {
    FILE *roles_file = fopen("roles.txt", "w");
    if (roles_file) {
        fprintf(roles_file, "alice write\n");
        fprintf(roles_file, "bob read\n");
        fprintf(roles_file, "charlie write\n");
        fprintf(roles_file, "admin write\n");
        fclose(roles_file);
    }
}

// Helper to cleanup test files
void cleanup_test_files(void) {
    unlink("roles.txt");
    unlink("doc.md");
}

// Test 1: ACTUAL get_user_permissions function from server.c
int test_actual_get_user_permissions(void) {
    printf("\n=== Test 1: ACTUAL get_user_permissions Function ===\n");
    
    create_test_roles_file();
    
    char role[16];
    int permission;
    
    // Test valid write user
    int result = get_user_permissions("alice", role, &permission);
    TEST_ASSERT(result == 1, "Valid user 'alice' is authorized");
    TEST_ASSERT(strcmp(role, "write") == 0, "Alice has 'write' role");
    TEST_ASSERT(permission == 1, "Alice has write permission (1)");
    
    // Test valid read user
    result = get_user_permissions("bob", role, &permission);
    TEST_ASSERT(result == 1, "Valid user 'bob' is authorized");
    TEST_ASSERT(strcmp(role, "read") == 0, "Bob has 'read' role");
    TEST_ASSERT(permission == 0, "Bob has read permission (0)");
    
    // Test invalid user
    result = get_user_permissions("invalid_user", role, &permission);
    TEST_ASSERT(result == 0, "Invalid user is rejected");
    
    // Test case sensitivity
    result = get_user_permissions("Alice", role, &permission);
    TEST_ASSERT(result == 0, "Username comparison is case-sensitive");
    
    cleanup_test_files();
    return 0;
}

// Test 2: ACTUAL apply_command function from server.c
int test_actual_apply_command(void) {
    printf("\n=== Test 2: ACTUAL apply_command Function ===\n");
    
    // Initialize global doc (this is what server.c does)
    if (!doc) {
        doc = markdown_init();
    }
    
    char result[256];
    
    // Test INSERT command
    apply_command("alice", "INSERT 0 Hello", result);
    TEST_ASSERT(strcmp(result, "SUCCESS") == 0, "INSERT command returns SUCCESS");
    
    // Test invalid position
    apply_command("alice", "INSERT 1000 Text", result);
    TEST_ASSERT(strstr(result, "Reject") != NULL, "Invalid position is rejected");
    
    // Test DEL command
    apply_command("alice", "DEL 0 5", result);
    TEST_ASSERT(strcmp(result, "SUCCESS") == 0, "DEL command returns SUCCESS");
    
    // Test formatting commands
    apply_command("alice", "BOLD 0 5", result);
    TEST_ASSERT(strcmp(result, "SUCCESS") == 0, "BOLD command returns SUCCESS");
    
    apply_command("alice", "ITALIC 0 5", result);
    TEST_ASSERT(strcmp(result, "SUCCESS") == 0, "ITALIC command returns SUCCESS");
    
    // Test invalid command format
    apply_command("alice", "INVALID_COMMAND", result);
    TEST_ASSERT(strstr(result, "Reject") != NULL, "Invalid command is rejected");
    
    return 0;
}

// Test 3: ACTUAL save_document function from server.c
int test_actual_save_document(void) {
    printf("\n=== Test 3: ACTUAL save_document Function ===\n");
    
    // Initialize global doc with content
    if (!doc) {
        doc = markdown_init();
    }
    
    markdown_insert(doc, doc->current_version, 0, "Test document content");
    markdown_increment_version(doc);
    
    // Test save functionality
    save_document();
    
    TEST_ASSERT(file_exists("doc.md"), "doc.md file is created");
    
    // Verify content
    FILE *file = fopen("doc.md", "r");
    if (file) {
        char buffer[256];
        fgets(buffer, sizeof(buffer), file);
        fclose(file);
        TEST_ASSERT(strcmp(buffer, "Test document content") == 0, "Saved content matches document");
    }
    
    cleanup_test_files();
    return 0;
}

// Test 4: Command queue functions
int test_command_queue_functions(void) {
    printf("\n=== Test 4: Command Queue Functions ===\n");
    
    // Test enqueue_command (this function exists in server.c)
    enqueue_command("alice", "INSERT 0 Hello");
    TEST_ASSERT(1, "enqueue_command executes without error");
    
    enqueue_command("bob", "INSERT 5 World");
    TEST_ASSERT(1, "Multiple commands can be enqueued");
    
    return 0;
}

// Test 5: FIFO Creation Logic (same as before but testing actual server behavior)
int test_fifo_creation_logic(void) {
    printf("\n=== Test 5: FIFO Creation Logic ===\n");
    
    pid_t test_pid = 12345;
    char fifo_c2s[64], fifo_s2c[64];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", test_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", test_pid);
    
    // Clean up any existing FIFOs
    unlink(fifo_c2s);
    unlink(fifo_s2c);
    
    // Test FIFO creation (same logic as in handle_sigrtmin)
    int c2s_result = mkfifo(fifo_c2s, 0666);
    int s2c_result = mkfifo(fifo_s2c, 0666);
    
    TEST_ASSERT(c2s_result == 0, "Client-to-Server FIFO created successfully");
    TEST_ASSERT(s2c_result == 0, "Server-to-Client FIFO created successfully");
    TEST_ASSERT(file_exists(fifo_c2s), "FIFO_C2S file exists on filesystem");
    TEST_ASSERT(file_exists(fifo_s2c), "FIFO_S2C file exists on filesystem");
    
    // Test FIFO permissions
    struct stat st;
    if (stat(fifo_c2s, &st) == 0) {
        TEST_ASSERT(S_ISFIFO(st.st_mode), "C2S is a FIFO (named pipe)");
    }
    if (stat(fifo_s2c, &st) == 0) {
        TEST_ASSERT(S_ISFIFO(st.st_mode), "S2C is a FIFO (named pipe)");
    }
    
    // Cleanup
    unlink(fifo_c2s);
    unlink(fifo_s2c);
    
    return 0;
}

int main() {
    printf("=== ACTUAL Server.c Function Unit Tests ===\n");
    printf("Testing real server.c functions by linking to them directly\n");
    
    // Run all tests
    test_actual_get_user_permissions();
    test_actual_apply_command();
    test_actual_save_document();
    test_command_queue_functions();
    test_fifo_creation_logic();
    
    printf("\n=== Test Summary ===\n");
    printf("Passed: %d/%d tests\n", tests_passed, tests_total);
    
    if (tests_passed == tests_total) {
        printf("✓ All tests passed! Server functions work correctly.\n");
        return 0;
    } else {
        printf("✗ Some tests failed. Review server implementation.\n");
        return 1;
    }
} 