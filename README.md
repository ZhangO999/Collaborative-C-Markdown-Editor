# Collaborative Markdown Document Editor
A robust C-based client–server Markdown editor that supports real-time collaborative editing, deterministic versioning, including merge-conflict handling and role-based access control. 

## Overview

This system enables multiple CLI clients to concurrently edit a shared Markdown document. Clients communicate atomic edit commands to a central server via POSIX named pipes (FIFOs) and receive batched updates through real-time signals to maintain a consistent view. A custom parallel linkedList-like data structure is utilised to simulate and track committed changes. 

## Features
- **Client-server architecture** using POSIX FIFOs and signals
- **Collaborative editing** of a shared markdown document
- **Role-based permissions** (read/write) loaded from `roles.txt`
- **Command protocol** for editing, formatting, and querying the document
- **Batch processing and broadcast** of edits to all clients
- **Document versioning** and logging
- **Graceful client disconnect and server shutdown**

## Key Behaviors

- **Concurrent Edit Batching**: Clients send individual commands (e.g., `INSERT`, `DEL`, formatting) which the server aggregates over a configurable interval (e.g., 500 ms). All commands are applied in arrival order and broadcast as a versioned delta.
- **Role-Based Access Control**: User roles (`write` or `read`) defined in `roles.txt` govern permissions. Write-enabled users modify content; read-only users only receive updates.
- **Deterministic Versioning & Auditing**: Each broadcast cycle increments the global version counter. Clients can query specific versions or retrieve the full, timestamped command log for rollback and audit purposes.
- **Fault Tolerance & Cleanup**: The server detects client disconnects via signal handlers, persists the latest `doc.md` snapshot, and removes FIFOs to prevent resource leaks.
- **Rich Markdown Formatting**: Native support for:
  - Headings (H1–H3)
  - Bold, Italic
  - Blockquotes, Ordered/Unordered Lists
  - Inline Code, Horizontal Rules
  - Hyperlinks
 

## Building
This project uses a standard C compiler (e.g., `gcc`). To build both the server and client:

```sh
gcc -o server source/server.c source/markdown.c -lpthread
gcc -o client source/client.c source/markdown.c
```

- Ensure all source files and headers are in the correct directories.
- The `-lpthread` flag is required for the server.

## Usage
### 1. Prepare roles file
Edit `roles.txt` to specify usernames and their roles (either `write` or `read`):

```
alice write
bob read
charlie write
dave read
eve write
```

### 2. Start the server
   ```bash
   ./client <server_pid> <username>
````
Run the server with the desired broadcast interval (in milliseconds):
   ```bash
   ./server <broadcast-interval-ms>
   ```

```sh
./server 1000
```

- The server will print its PID, which clients need to connect.

### 3. Start a client
Run the client, providing the server PID and your username:

```sh
./client <server_pid> <username>
```

- Example: `./client 12345 alice`
- The client will authenticate and allow you to enter commands.

### 4. Commands
- **Editing commands:** `INSERT`, `DEL`, `NEWLINE`, `HEADING`, `BOLD`, `ITALIC`, `BLOCKQUOTE`, `ORDERED_LIST`, `UNORDERED_LIST`, `CODE`, `HORIZONTAL_RULE`, `LINK`
- **Query commands:** `DOC?`, `PERM?`, `LOG?`
- **Disconnect:** `DISCONNECT`

### 5. Server shutdown
- Type `QUIT` in the server terminal to shut down (only when no clients are connected).
- The document is saved to `doc.md` on shutdown and on client disconnects, and relevant cleanup operations are executed (i.e. remove all FIFOs) 

## Cleaning Up
To remove build artifacts and FIFOs:

```sh
rm -f server client *.o FIFO_C2S_* FIFO_S2C_* doc.md
```

## Notes
- Ensure you have the correct permissions to create FIFOs in the working directory.
- The project is designed for Linux systems with POSIX support.

