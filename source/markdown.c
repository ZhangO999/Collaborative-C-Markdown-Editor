#include "../libs/markdown.h"
#include "../libs/document.h"  // path depends on your folder structure
#include <stdlib.h> 
#include <string.h> 
#include <ctype.h>

#define SUCCESS 0

// === Forward Declarations for Internal Helper Functions ===
static int validate_version_op(document *doc, uint64_t version);
static int validate_range_op(document *doc, uint64_t version, 
                            size_t start, size_t end);
static char get_char_at_pos(const char *flat, size_t flat_len, size_t pos);
static int needs_newline_before(const char *flat, size_t pos);
static int insert_block_element(document *doc, size_t pos, 
                               const char *marker);
static int apply_range_format(document *doc, size_t start, size_t end, 
                             const char *marker);
static void free_segment_list(text_segment *head);

// Document manipulation functions (internal)
int add_text(document *doc, size_t pos, const char *text);
int put_text(document *doc, size_t pos, const char *text);
int remove_text(document *doc, size_t pos, size_t len);
int find_cursor(document *doc, size_t pos, text_segment **seg, 
               size_t *offset);
void sync_working(document *doc);

// === Helper Functions ===

/**
 * Standard validation for version-based operations
 */
static int validate_version_op(document *doc, uint64_t version) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->current_version) {
        return OUTDATED_VERSION;
    }
    return SUCCESS;
}

/**
 * Standard validation for range operations
 */
static int validate_range_op(document *doc, uint64_t version, 
                            size_t start, size_t end) {
    int result = validate_version_op(doc, version);
    if (result != SUCCESS) {
        return result;
    }
    if (end <= start) {
        return INVALID_CURSOR_POS;
    }
    return SUCCESS;
}

/**
 * Get character at position in flattened document, returns 0 if out of bounds
 */
static char get_char_at_pos(const char *flat, size_t flat_len, size_t pos) {
    return (pos < flat_len) ? flat[pos] : 0;
}

/**
 * Check if position needs newline before block element
 */
static int needs_newline_before(const char *flat, size_t pos) {
    if (pos == 0) {
        return 0;  // At start of document
    }
    char prev = get_char_at_pos(flat, pos, pos - 1);
    return prev != '\n';
}

/**
 * Insert block element with automatic newline handling
 */
static int insert_block_element(document *doc, size_t pos, 
                               const char *marker) {
    char *flat = markdown_flatten(doc);
    if (!flat) {
        return INVALID_CURSOR_POS;
    }
    
    size_t flat_len = strlen(flat);
    if (pos > flat_len) {
        free(flat);
        return INVALID_CURSOR_POS;
    }
    
    int result = 0;
    if (needs_newline_before(flat, pos)) {
        // Need newline before marker
        char *with_newline = (char *)malloc(strlen(marker) + 2);
        sprintf(with_newline, "\n%s", marker);
        result = add_text(doc, pos, with_newline);
        free(with_newline);
    } else {
        // Just insert marker
        result = add_text(doc, pos, marker);
    }
    
    free(flat);
    return result;
}

/**
 * Apply range formatting (bold, italic, code) by inserting markers
 */
static int apply_range_format(document *doc, size_t start, size_t end, 
                             const char *marker) {
    // Insert closing marker first to avoid position shifting
    int result = add_text(doc, end, marker);
    if (result != SUCCESS) {
        return result;
    }
    
    // Insert opening marker at start
    return add_text(doc, start, marker);
}

/**
 * Free a segment list
 */
static void free_segment_list(text_segment *head) {
    text_segment *cur = head;
    text_segment *tmp = NULL;
    while (cur) {
        tmp = cur->next_segment;
        free(cur->content);
        free(cur);
        cur = tmp;
    }
}

// === Init and Free ===

/**
 * Initialize a new markdown document structure
 * Sets up empty committed and working lists, version 0
 */
document *markdown_init(void) {
    document *doc = (document *)calloc(1, sizeof(document));
    doc->committed_head = NULL;    // No committed content initially
    doc->working_head = NULL;      // No working changes initially
    doc->total_length = 0;         // Document starts empty
    doc->current_version = 0;      // Start at version 0
    return doc;
}

/**
 * Free all memory associated with a markdown document
 * Cleans up both committed and working linked lists
 */
void markdown_free(document *doc) {
    if (!doc) {
        return;
    }
    
    free_segment_list(doc->committed_head);
    free_segment_list(doc->working_head);
    free(doc);                   // Free document structure itself
}

// === Edit Commands ===

/**
 * Insert text content at specified position in document
 * Validates version and delegates to put_text helper
 */
int markdown_insert(document *doc, uint64_t version, size_t pos, 
                   const char *content) {
    if (!doc || !content) {
        return INVALID_CURSOR_POS;
    }
    
    // Only accept edits on current version
    int result = validate_version_op(doc, version);
    if (result != SUCCESS) {
        return result;
    }
    
    return put_text(doc, pos, content);
}

/**
 * Delete specified number of characters starting from position
 * Validates version and delegates to remove_text helper
 */
int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // Only accept edits on current version
    int result = validate_version_op(doc, version);
    if (result != SUCCESS) {
        return result;
    }
    
    return remove_text(doc, pos, len);
}

// === Formatting Commands ===

/**
 * Insert a newline character at the specified position
 * Simple wrapper around add_text for newline insertion
 */
int markdown_newline(document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    
    // Only accept edits on current version
    int result = validate_version_op(doc, version);
    if (result != SUCCESS) {
        return result;
    }
    
    // Insert newline at the specified position
    return add_text(doc, pos, "\n");
}

/**
 * Insert markdown heading at specified position
 * Handles automatic newline insertion for block-level elements
 * Creates heading markers (# ## ###) with required space
 */
int markdown_heading(document *doc, uint64_t version, size_t level, 
                    size_t pos) {
    int result = validate_version_op(doc, version);
    if (result != SUCCESS) {
        return result;
    }
    if (level < 1 || level > 3) {
        return INVALID_CURSOR_POS;
    }

    // Ensure working list exists
    if (!doc->working_head) {
        sync_working(doc);
    }

    // Check if we need newline before heading
    int needs_newline = 0;
    if (pos > 0) {
        size_t count = 0;
        text_segment *cur = doc->working_head;
        char prev_char = 0;
        
        // Find character before insertion point
        while (cur && count + cur->length < pos) {
            count += cur->length;
            cur = cur->next_segment;
        }
        
        if (cur && pos > count && cur->length >= (pos - count)) {
            prev_char = cur->content[pos - count - 1];
        } else if (!cur && count > 0) {
            // At end - find last character
            cur = doc->working_head;
            size_t total = 0;
            while (cur) {
                if (total + cur->length >= count) {
                    break;
                }
                total += cur->length;
                cur = cur->next_segment;
            }
            if (cur && cur->length > 0) {
                prev_char = cur->content[cur->length - 1];
            }
        }
        
        needs_newline = (prev_char != '\n');
    }

    // Insert newline if needed
    if (needs_newline) {
        result = add_text(doc, pos, "\n");
        if (result != SUCCESS) {
            return result;
        }
        pos++;
    }

    // Build and insert heading marker
    char heading_str[5]; // max "### " + '\0'
    for (size_t i = 0; i < level; i++) {
        heading_str[i] = '#';
    }
    heading_str[level] = ' ';
    heading_str[level + 1] = '\0';

    return add_text(doc, pos, heading_str);
}

/**
 * Apply bold formatting to text range
 */
int markdown_bold(document *doc, uint64_t version, size_t start, size_t end) {
    int result = validate_range_op(doc, version, start, end);
    if (result != SUCCESS) {
        return result;
    }
    
    return apply_range_format(doc, start, end, "**");
}

/**
 * Apply italic formatting to text range
 */
int markdown_italic(document *doc, uint64_t version, size_t start, 
                   size_t end) {
    int result = validate_range_op(doc, version, start, end);
    if (result != SUCCESS) {
        return result;
    }
    
    return apply_range_format(doc, start, end, "*");
}

int markdown_blockquote(document *doc, uint64_t version, size_t pos) {
    (void)doc; (void)version; (void)pos;
    return SUCCESS;
}

int markdown_ordered_list(document *doc, uint64_t version, size_t pos) {
    (void)doc; (void)version; (void)pos;
    return SUCCESS;
}

int markdown_unordered_list(document *doc, uint64_t version, size_t pos) {
    (void)doc; (void)version; (void)pos;
    return SUCCESS;
}

int markdown_code(document *doc, uint64_t version, size_t start, size_t end) {
    (void)doc; (void)version; (void)start; (void)end;
    return SUCCESS;
}

int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos) {
    (void)doc; (void)version; (void)pos;
    return SUCCESS;
}

int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url) {
    (void)doc; (void)version; (void)start; (void)end; (void)url;
    return SUCCESS;
}

// === Utilities ===
void markdown_print(const document *doc, FILE *stream) {
    (void)doc; (void)stream;
}

char *markdown_flatten(const document *doc) {
    (void)doc;
    return NULL;
}

// === Versioning ===
void markdown_increment_version(document *doc) {
    (void)doc;
}

