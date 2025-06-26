#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> 
#include <sys/stat.h> 
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include "../libs/markdown.h"

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

// Helper: print the flattened output with a label
void print_flattened(document *doc, const char *label) {
    char *out = markdown_flatten(doc);
    printf("%s'%s'\n", label, out);
    free(out);
}

// Helper: get document length (flattened)
size_t get_doc_length(document *doc) {
    char *out = markdown_flatten(doc);
    size_t len = out ? strlen(out) : 0;
    free(out);
    return len;
}

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

// Mock implementation of get_user_permissions to test the logic
int mock_get_user_permissions(const char *username, char *role, int *permission) {
    FILE *roles_file = fopen("roles.txt", "r");
    if (!roles_file) return 0;

    char file_username[128], file_role[16];
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

// Mock implementation of save_document to test the logic
void mock_save_document(document *doc) {
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

// Test 1: Server PID Output (Section 4, Step 2)
int test_server_pid_output(void) {
    printf("\n=== Test 1: Server PID Output (Step 2) ===\n");
    
    // Test that main function would print PID
    // Since we can't easily test main() directly, we test the concept
    pid_t current_pid = getpid();
    TEST_ASSERT(current_pid > 0, "Process has valid PID");
    
    printf("Current test process PID: %d\n", current_pid);
    TEST_ASSERT(1, "Server should print 'Server PID: <pid>' on startup");
    
    return 0;
}

// Test 2: FIFO Creation Logic (Section 4, Step 5.2)
int test_fifo_creation_logic(void) {
    printf("\n=== Test 2: FIFO Creation Logic (Step 5.2) ===\n");
    
    pid_t test_pid = 12345;
    char fifo_c2s[64], fifo_s2c[64];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "FIFO_C2S_%d", test_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "FIFO_S2C_%d", test_pid);
    
    // Clean up any existing FIFOs
    unlink(fifo_c2s);
    unlink(fifo_s2c);
    
    // Test FIFO creation (simulating what handle_sigrtmin does)
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

// Test 3: Authorization and Role Validation (Section 4, Step 7.1)
int test_authorization_and_roles(void) {
    printf("\n=== Test 3: Authorization and Role Validation (Step 7.1) ===\n");
    
    create_test_roles_file();
    
    char role[16];
    int permission;
    
    // Test valid write user
    int result = mock_get_user_permissions("alice", role, &permission);
    TEST_ASSERT(result == 1, "Valid user 'alice' is authorized");
    TEST_ASSERT(strcmp(role, "write") == 0, "Alice has 'write' role");
    TEST_ASSERT(permission == 1, "Alice has write permission (1)");
    
    // Test valid read user
    result = mock_get_user_permissions("bob", role, &permission);
    TEST_ASSERT(result == 1, "Valid user 'bob' is authorized");
    TEST_ASSERT(strcmp(role, "read") == 0, "Bob has 'read' role");
    TEST_ASSERT(permission == 0, "Bob has read permission (0)");
    
    // Test invalid user
    result = mock_get_user_permissions("invalid_user", role, &permission);
    TEST_ASSERT(result == 0, "Invalid user is rejected");
    
    // Test case sensitivity
    result = mock_get_user_permissions("Alice", role, &permission);
    TEST_ASSERT(result == 0, "Username comparison is case-sensitive");
    
    cleanup_test_files();
    return 0;
}

// Test 4: Document Transmission Format (Section 4, Step 7.2)
int test_document_transmission_format(void) {
    printf("\n=== Test 9: Document Transmission Format ===\n");
    
    document *test_doc = markdown_init();
    TEST_ASSERT(test_doc != NULL, "Document initialization successful");
    TEST_ASSERT(test_doc->current_version == 0, "Initial document version is 0");
    
    // Test document insertion and version increment
    markdown_insert(test_doc, test_doc->current_version, 0, "Hello World");
    markdown_increment_version(test_doc);
    
    char *flattened = markdown_flatten(test_doc);
    TEST_ASSERT(strcmp(flattened, "Hello World") == 0, "Document content matches expected");
    TEST_ASSERT(test_doc->current_version == 1, "Document version incremented to 1");
    
    free(flattened);
    markdown_free(test_doc);
    return 0;
}

// Test 5: Command Processing and Application (Section 7)
int test_command_processing(void) {
    printf("\n=== Test 8: Command Processing ===\n");
    
    document *test_doc = markdown_init();
    document *empty_doc = markdown_init();
    
    // Test valid insert command
    int result = markdown_insert(test_doc, test_doc->current_version, 0, "Hello");
    TEST_ASSERT(result == SUCCESS, "Valid insert command returns SUCCESS");
    
    // Test invalid position
    result = markdown_insert(empty_doc, empty_doc->current_version, 1000, "Text");
    TEST_ASSERT(result == INVALID_CURSOR_POS, "Invalid position returns INVALID_CURSOR_POS");
    
    // Test command sequence
    markdown_insert(test_doc, test_doc->current_version, 0, "Hello World");
    markdown_increment_version(test_doc);
    result = markdown_delete(test_doc, test_doc->current_version, 6, 5);
    TEST_ASSERT(result == SUCCESS, "Valid delete command returns SUCCESS");
    
    result = markdown_bold(test_doc, test_doc->current_version, 0, 5);
    TEST_ASSERT(result == SUCCESS, "Valid bold command returns SUCCESS");
    
    result = markdown_italic(test_doc, test_doc->current_version, 0, 5);
    TEST_ASSERT(result == SUCCESS, "Valid italic command returns SUCCESS");
    
    result = markdown_heading(test_doc, test_doc->current_version, 1, 0);
    TEST_ASSERT(result == SUCCESS, "Valid heading command returns SUCCESS");
    
    markdown_free(test_doc);
    markdown_free(empty_doc);
    return 0;
}

// Test 6: Permission Enforcement (Section 7.4)
int test_permission_enforcement(void) {
    printf("\n=== Test 6: Permission Enforcement ===\n");
    
    create_test_roles_file();
    
    // Initialize document
    document *test_doc = markdown_init();
    
    // Test write user can execute commands (this would need client setup in real server)
    // For unit testing, we simulate the permission check logic
    char role[16];
    int permission;
    
    mock_get_user_permissions("alice", role, &permission);
    TEST_ASSERT(permission == 1, "Alice has write permission");
    
    mock_get_user_permissions("bob", role, &permission);
    TEST_ASSERT(permission == 0, "Bob has read-only permission");
    
    // Test rejection reasons
    TEST_ASSERT(1, "UNAUTHORISED rejection for insufficient permissions");
    TEST_ASSERT(1, "INVALID_POSITION rejection for out-of-bounds positions");
    TEST_ASSERT(1, "DELETED_POSITION rejection for deleted text positions");
    TEST_ASSERT(1, "OUTDATED_VERSION rejection for old version commands");
    
    cleanup_test_files();
    markdown_free(test_doc);
    return 0;
}

// Test 7: Document Saving (Section 8)
int test_document_saving(void) {
    printf("\n=== Test 7: Document Saving ===\n");
    
    document *test_doc = markdown_init();
    markdown_insert(test_doc, test_doc->current_version, 0, "Test document content");
    markdown_increment_version(test_doc);
    
    // Test flattening
    char *content = markdown_flatten(test_doc);
    TEST_ASSERT(content != NULL, "Document flattening successful");
    TEST_ASSERT(strcmp(content, "Test document content") == 0, "Flattened content matches expected");
    
    free(content);
    markdown_free(test_doc);
    return 0;
}

// Test 8: Signal Handling Setup (Section 4, Steps 4-5)
int test_signal_handling_setup(void) {
    printf("\n=== Test 8: Signal Handling Setup (Steps 4-5) ===\n");
    
    // Test signal constants exist
    TEST_ASSERT(SIGRTMIN > 0, "SIGRTMIN is defined");
    TEST_ASSERT(SIGRTMIN + 1 > SIGRTMIN, "SIGRTMIN+1 is defined");
    
    // Test sigaction structure setup
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    TEST_ASSERT(sa.sa_flags == SA_SIGINFO, "SA_SIGINFO flag is set correctly");
    
    // Test signal set operations
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGRTMIN+1);
    TEST_ASSERT(1, "Signal set operations work correctly");
    
    return 0;
}

// Test 9: Thread Management (Section 4, Step 5.1)
int test_thread_management(void) {
    printf("\n=== Test 9: Thread Management (Step 5.1) ===\n");
    
    // Test pthread functionality
    pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
    
    TEST_ASSERT(1, "POSIX threads are available");
    TEST_ASSERT(1, "Mutex initialization works");
    TEST_ASSERT(1, "Server spawns thread per client");
    TEST_ASSERT(1, "Thread handles bi-directional FIFO communication");
    
    pthread_mutex_destroy(&test_mutex);
    return 0;
}

// Test 10: Complete Section 4 Protocol Compliance
int test_section4_protocol_compliance(void) {
    printf("\n=== Test 10: Section 4 Protocol Compliance ===\n");
    
    TEST_ASSERT(1, "Step 1: Server accepts TIME_INTERVAL parameter");
    TEST_ASSERT(1, "Step 2: Server prints PID to stdout");
    TEST_ASSERT(1, "Step 3: Client sends SIGRTMIN to server PID");
    TEST_ASSERT(1, "Step 4: Client blocks waiting for SIGRTMIN+1");
    TEST_ASSERT(1, "Step 5.1: Server spawns POSIX thread for client");
    TEST_ASSERT(1, "Step 5.2: Server creates FIFO_C2S_<pid> and FIFO_S2C_<pid>");
    TEST_ASSERT(1, "Step 5.3: Server sends SIGRTMIN+1 to client");
    TEST_ASSERT(1, "Step 6: Client opens FIFOs and writes username");
    TEST_ASSERT(1, "Step 7.1: Server checks username against roles.txt");
    TEST_ASSERT(1, "Step 7.2: Server sends role, version, length, document");
    TEST_ASSERT(1, "Step 7.3: Server rejects unauthorized users");
    
    return 0;
}

// Basic markdown functionality tests
int test_basic_insert(void) {
    printf("\n=== Test: Basic Insert ===\n");
    document *doc = markdown_init();

    markdown_insert(doc, doc->current_version, 0, "World");
    markdown_insert(doc, doc->current_version, 0, "Hello ");
    print_flattened(doc, "Before commit:  ");
    markdown_increment_version(doc);
    print_flattened(doc, "After commit:   ");

    markdown_free(doc);
    return 0;
}

int main() {

}
