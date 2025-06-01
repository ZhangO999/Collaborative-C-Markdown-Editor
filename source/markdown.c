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
    size_t flat_len = strlen(flat);
    char prev = get_char_at_pos(flat, flat_len, pos - 1);
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

/**
 * Insert blockquote formatting at specified position
 * Handles newline insertion for block-level element requirements
 */
int markdown_blockquote(document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    if (doc->current_version != version) {
        return OUTDATED_VERSION;
    }

    return insert_block_element(doc, pos, "> ");
}

/**
 * Insert ordered list item with automatic numbering
 * Handles renumbering of subsequent list items
 */
int markdown_ordered_list(document *doc, uint64_t version, size_t pos) {
    if (doc->current_version != version) {
        return OUTDATED_VERSION;
    }
    
    char *flat = markdown_flatten(doc);
    if (!flat) {
        return INVALID_CURSOR_POS;
    }
    
    size_t flat_len = strlen(flat);
    if (pos > flat_len) {
        free(flat);
        return INVALID_CURSOR_POS;
    }

    // Check if at line start
    int at_line_start = (pos == 0 || flat[pos - 1] == '\n');

    // Find previous list number
    int prev_num = 0;
    if (pos > 0) {
        // Find start of previous line
        int i = (int)pos - 2;
        while (i >= 0 && flat[i] != '\n') {
            i--;
        }
        size_t prev_line_start = (i >= 0) ? (size_t)(i + 1) : 0;

        // Check if previous line is numbered list
        if (isdigit(flat[prev_line_start])) {
            size_t n = 0;
            while (isdigit(flat[prev_line_start + n])) {
                n++;
            }
            if (flat[prev_line_start + n] == '.' && 
                flat[prev_line_start + n + 1] == ' ') {
                prev_num = atoi(flat + prev_line_start);
            }
        }
    }
    
    int new_num = prev_num + 1;

    // Create list item prefix
    char prefix[20];
    if (at_line_start) {
        snprintf(prefix, sizeof(prefix), "%d. ", new_num);
    } else {
        snprintf(prefix, sizeof(prefix), "\n%d. ", new_num);
    }

    // Insert new list item
    int res = add_text(doc, pos, prefix);
    if (res != SUCCESS) {
        free(flat);
        return res;
    }

    // Renumber subsequent list items
    size_t scan = pos + strlen(prefix);
    int next_num = new_num + 1;
    
    while (scan < flat_len) {
        // Find next line
        size_t next_line = scan;
        while (next_line < flat_len && flat[next_line] != '\n') {
            next_line++;
        }
        if (next_line >= flat_len) {
            break;
        }
        next_line++; // skip the '\n'

        // Check if it's a numbered list item
        if (isdigit(flat[next_line])) {
            size_t n = 0;
            while (isdigit(flat[next_line + n])) {
                n++;
            }
            
            if (flat[next_line + n] == '.' && 
                flat[next_line + n + 1] == ' ') {
                // Renumber this item
                size_t old_len = n + 2;
                char new_prefix[20];
                snprintf(new_prefix, sizeof(new_prefix), "%d. ", next_num++);
                
                remove_text(doc, next_line, old_len);
                add_text(doc, next_line, new_prefix);
                scan = next_line + strlen(new_prefix);
                continue;
            }
        }
        break; // Not a numbered line, stop renumbering
    }
    free(flat);
    return SUCCESS;
}

/**
 * Insert unordered list item marker at specified position
 * Handles newline insertion for block-level element requirements
 */
int markdown_unordered_list(document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    if (doc->current_version != version) {
        return OUTDATED_VERSION;
    }
    
    return insert_block_element(doc, pos, "- ");
}

/**
 * Apply inline code formatting to text range
 */
int markdown_code(document *doc, uint64_t version, size_t start, size_t end) {
    int result = validate_range_op(doc, version, start, end);
    if (result != SUCCESS) {
        return result;
    }
    
    return apply_range_format(doc, start, end, "`");
}

/**
 * Insert horizontal rule with proper newline handling
 * Ensures rule is on its own line with newlines before and after
 */
int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->current_version) {
        return OUTDATED_VERSION;
    }

    // Insert horizontal rule as a complete block element including trailing newline
    return insert_block_element(doc, pos, "---\n");
}

/**
 * Create markdown link by wrapping text in brackets and adding URL
 */
int markdown_link(document *doc, uint64_t version, size_t start, size_t end, 
                 const char *url) {
    if (!url) {
        return INVALID_CURSOR_POS;
    }
    int result = validate_range_op(doc, version, start, end);
    if (result != SUCCESS) {
        return result;
    }

    // Insert closing bracket and URL first
    size_t url_len = strlen(url);
    char *suffix = (char *)malloc(url_len + 4); // "]" + "()" + null
    sprintf(suffix, "](%s)", url);
    
    result = add_text(doc, end, suffix);
    free(suffix);
    if (result != SUCCESS) {
        return result;
    }

    // Insert opening bracket
    return add_text(doc, start, "[");
}


// === Utilities ===

/**
 * Print document to specified stream (currently not implemented)
 */
void markdown_print(const document *doc, FILE *stream) {
    (void)doc; 
    (void)stream;
}

/**
 * Flatten the committed document into a single string
 * Only includes committed content, not working changes
 */
char *markdown_flatten(const document *doc) {
    const text_segment *lst = doc->committed_head;
    
    // Calculate total length needed
    size_t total = 0;
    for (const text_segment *n = lst; n; n = n->next_segment) {
        total += n->length;
    }
    
    // Allocate buffer and copy all text segments
    char *buf = (char *)malloc(total + 1);
    char *p = buf;
    for (const text_segment *n = lst; n; n = n->next_segment) {
        memcpy(p, n->content, n->length);
        p += n->length;
    }
    buf[total] = 0; // Null terminate
    return buf;
}



// === Versioning ===

/**
 * Commit working changes to create new document version
 * Promotes working list to committed, removes deleted segments
 */
void markdown_increment_version(document *doc) {
    if (!doc->working_head) {
        return;
    }
    
    // Free old committed list
    free_segment_list(doc->committed_head);
    doc->committed_head = NULL;

    // Promote working list to committed, filtering out deleted segments
    text_segment **tail = &doc->committed_head;
    text_segment *cur = doc->working_head;
    text_segment *tmp = NULL;
    
    while (cur) {
        tmp = cur->next_segment;
        
        if (cur->state != PENDING_DEL) {
            // Keep this segment - convert inserted segments to original
            if (cur->state == PENDING_INS) {
                cur->state = COMMITTED_ORIGINAL;
            }
            cur->next_segment = NULL;
            *tail = cur;
            tail = &(cur->next_segment);
        } else {
            // Remove deleted segment
            free(cur->content);
            free(cur);
        }
        cur = tmp;
    }
    
    doc->working_head = NULL;       // Clear working list
    doc->current_version += 1;      // Increment version number
}


// Helper functions: 

/**
 * Copy committed list into working list for editing
 * All segments become COMMITTED_ORIGINAL state
 */
void sync_working(document *doc) {
    // Free any existing working list
    free_segment_list(doc->working_head);
    doc->working_head = NULL;

    // Clone committed list into working list
    text_segment **tail = &(doc->working_head);
    for (text_segment *n = doc->committed_head; n; n = n->next_segment) {
        text_segment *copy = (text_segment *)malloc(sizeof(text_segment));
        copy->length = n->length;
        copy->content = (char *)malloc(n->length + 1);
        memcpy(copy->content, n->content, n->length);
        copy->content[n->length] = 0;           // Null terminate
        copy->state = COMMITTED_ORIGINAL;       // All segments start as 
                                               // original
        copy->next_segment = NULL;
        *tail = copy;
        tail = &(copy->next_segment);
    }
}

/**
 * Find the segment and offset for a logical position in the working list
 * Used for cursor positioning in document operations
 */
int find_cursor(document *doc, size_t pos, text_segment **out_line, 
               size_t *out_offset) {
    size_t seen = 0;
    text_segment *cur = doc->working_head;

    while (cur) {
        // Only count non-inserted segments for position calculation
        if (cur->state != PENDING_INS && pos <= seen + cur->length) {
            *out_line = cur;
            *out_offset = pos - seen;
            return SUCCESS;
        }
        if (cur->state != PENDING_INS) {
            seen += cur->length;
        }
        cur = cur->next_segment;
    }
    
    // Handle insertion at end of document
    if (pos == seen) {
        *out_line = NULL;
        *out_offset = 0;
        return SUCCESS;
    }
    
    return INVALID_CURSOR_POS;
}

/**
 * Insert text at specified position in working list
 * Handles position finding, node splitting, and insertion ordering
 */
int add_text(document *doc, size_t pos, const char *str) {
    if (!doc->working_head) {
        sync_working(doc);
    }

    text_segment *cur = doc->working_head;
    text_segment *prev = NULL;
    size_t seen = 0;
    
    // Step 1: Find the insertion position, counting only visible segments 
    // (non-PENDING_INS)
    while (cur) {
        if (cur->state != PENDING_INS) {
            if (seen + cur->length <= pos) {
                // Position is after this segment
                seen += cur->length;
                prev = cur;
                cur = cur->next_segment;
            } else {
                // Position is within this segment
                break;
            }
        } else {
            // Skip inserted segments for position calculation
            prev = cur;
            cur = cur->next_segment;
        }
    }
    
    // Step 2: If inserting in the middle of a visible node, split it
    if (cur && cur->state != PENDING_INS && pos > seen && 
        pos < seen + cur->length) {
        size_t l1 = pos - seen;
        size_t l2 = cur->length - l1;
        
        // Create second half of split node
        text_segment *mid = (text_segment *)malloc(sizeof(text_segment));
        mid->length = l2;
        mid->content = (char *)malloc(l2 + 1);
        memcpy(mid->content, cur->content + l1, l2);
        mid->content[l2] = 0;
        mid->state = cur->state;
        mid->next_segment = cur->next_segment;

        // Truncate first half
        cur->length = l1;
        cur->content[l1] = 0;
        cur->next_segment = mid;
        prev = cur;
        cur = mid;
    }

    // Step 3: Find the end of any existing insertions at this position
    // New insertions go after existing ones at the same logical position
    while (cur && cur->state == PENDING_INS) {
        prev = cur;
        cur = cur->next_segment;
    }

    // Step 4: Insert new segment after existing insertions at same position
    text_segment *ins = (text_segment *)malloc(sizeof(text_segment));
    ins->length = strlen(str);
    ins->content = (char *)malloc(ins->length + 1);
    strcpy(ins->content, str);
    ins->state = PENDING_INS;
    ins->next_segment = cur;

    // Link into list
    if (prev) {
        prev->next_segment = ins;
    } else {
        doc->working_head = ins;
    }

    return SUCCESS;
}

/**
 * Alternative insertion function that places new insertions first
 * Used by markdown_insert to maintain insertion order
 */
int put_text(document *doc, size_t pos, const char *str) {
    if (!doc->working_head) {
        sync_working(doc);
    }

    // Validate position by counting total visible length
    size_t total_length = 0;
    text_segment *cur = doc->working_head;
    while (cur) {
        if (cur->state != PENDING_INS) {
            total_length += cur->length;
        }
        cur = cur->next_segment;
    }
    
    // Position must be within [0, total_length]
    if (pos > total_length) {
        return INVALID_CURSOR_POS;
    }

    cur = doc->working_head;
    text_segment *prev = NULL;
    size_t seen = 0;
    
    // Find the insertion position, counting only visible segments
    while (cur && seen + cur->length <= pos) {
        if (cur->state != PENDING_INS) {
            seen += cur->length;
        }
        prev = cur;
        cur = cur->next_segment;
    }
    
    // If inserting in the middle of a visible node, split it
    if (cur && cur->state != PENDING_INS && pos > seen && 
        pos < seen + cur->length) {
        size_t l1 = pos - seen;
        size_t l2 = cur->length - l1;
        
        text_segment *mid = (text_segment *)malloc(sizeof(text_segment));
        mid->length = l2;
        mid->content = (char *)malloc(l2 + 1);
        memcpy(mid->content, cur->content + l1, l2);
        mid->content[l2] = 0;
        mid->state = cur->state;
        mid->next_segment = cur->next_segment;

        cur->length = l1;
        cur->content[l1] = 0;
        cur->next_segment = mid;
        prev = cur;
        cur = mid;
    }

    // Create and insert new segment
    text_segment *ins = (text_segment *)malloc(sizeof(text_segment));
    ins->length = strlen(str);
    ins->content = (char *)malloc(ins->length + 1);
    strcpy(ins->content, str);
    ins->state = PENDING_INS;
    ins->next_segment = cur;

    if (prev) {
        prev->next_segment = ins;
    } else {
        doc->working_head = ins;
    }

    return SUCCESS;
}

/**
 * Delete text starting at position for specified length
 * Marks segments as PENDING_DEL and handles partial deletions
 */
int remove_text(document *doc, size_t pos, size_t len) {
    if (!doc->working_head) {
        sync_working(doc);
    }
    
    size_t seen = 0;
    size_t remain = len;
    text_segment *cur = doc->working_head;

    // Find starting node - count all visible segments (COMMITTED_ORIGINAL 
    // and PENDING_DEL)
    while (cur && (cur->state == PENDING_INS || seen + cur->length <= pos)) {
        if (cur->state != PENDING_INS) {
            seen += cur->length;
        }
        cur = cur->next_segment;
    }

    // Process deletion across multiple segments
    while (cur && remain > 0) {
        // Skip inserted segments (they don't count for position)
        if (cur->state == PENDING_INS) {
            cur = cur->next_segment;
            continue;
        }
        
        // Calculate deletion range within this segment
        size_t off = (pos > seen) ? (pos - seen) : 0;  // offset within seg
        size_t dellen = (cur->length - off < remain) ? 
                       cur->length - off : remain;  // length to delete

        // If partial delete at end, split the segment after deletion point
        if (off + dellen < cur->length) {
            text_segment *aft = (text_segment *)malloc(sizeof(text_segment));
            aft->length = cur->length - (off + dellen);
            aft->content = (char *)malloc(aft->length + 1);
            memcpy(aft->content, cur->content + off + dellen, aft->length);
            aft->content[aft->length] = 0;
            aft->state = cur->state;
            aft->next_segment = cur->next_segment;
            cur->next_segment = aft;
        }
        
        // If partial delete at beginning, split the segment before deletion 
        // point
        if (off > 0) {
            text_segment *aft = cur->next_segment;
            cur->length = off;
            cur->content[off] = 0;
            cur = aft;
            seen += off + dellen;
            remain -= dellen;
            continue;
        }
        
        // Full node is in delete range - mark for deletion
        cur->state = PENDING_DEL;
        remain -= dellen;
        seen += dellen;
        cur = cur->next_segment;
    }
    return SUCCESS;
}
