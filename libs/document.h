#ifndef DOCUMENT_H
#define DOCUMENT_H
#include <stddef.h> 
#include <stdint.h> 

/**
 * This file is the header file for all the document functions. You will be
 * tested on the functions inside markdown.h
 * You are allowed to and encouraged multiple helper functions and data 
 * structures, and make your code as modular as possible. 
 * Ensure you DO NOT change the name of document struct.
 */

enum seg_state {
    COMMITTED_ORIGINAL,    // Segment exists in the committed document version
    PENDING_INS,           // Segment is a new insertion awaiting commit
    PENDING_DEL            // Segment is marked for deletion in next commit
};

typedef struct text_segment {
    char* content;                     // Text content of this segment
    size_t length;                     // Length of the text content
    enum seg_state state;              // Current state of this segment
    struct text_segment *next_segment; // Pointer to next segment in the list
} text_segment;

typedef struct {
    text_segment *committed_head;      // Starting point of the committed 
                                      // document version
    text_segment *working_head;        // Starting point of the working 
                                      // document version
    size_t total_length;               // Total length of the document 
    uint64_t current_version;          // Current version number
} document; 

#define SUCCESS 0
#define INVALID_CURSOR_POS -1 
#define DELETED_POSITION -2 
#define OUTDATED_VERSION -3 

// Functions from here onwards.
#endif