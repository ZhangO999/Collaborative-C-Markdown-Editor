# Collaborative Markdown Document Editor


## Features
- **Client-server architecture** using POSIX FIFOs and signals
- **Collaborative editing** of a shared markdown document
- **Role-based permissions** (read/write) loaded from `roles.txt`
- **Command protocol** for editing, formatting, and querying the document
- **Batch processing and broadcast** of edits to all clients
- **Document versioning** and logging
- **Graceful client disconnect and server shutdown**

## File Structure
- `source/server.c` — Main server implementation
- `source/client.c` — Main client implementation
- `source/markdown.c` — Markdown document logic and editing functions
- `libs/markdown.h`, `libs/document.h` — Public API and document structures
- `roles.txt` — User roles and permissions
- `.gitignore` — Ignores build artifacts, FIFOs, and temp files

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
Run the server with the desired broadcast interval (in milliseconds):

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

Refer to the assignment spec for command syntax and details.

### 5. Server shutdown
- Type `QUIT` in the server terminal to shut down (only when no clients are connected).
- The document is saved to `doc.md` on shutdown and on client disconnects.

## Cleaning Up
To remove build artifacts and FIFOs:

```sh
rm -f server client *.o FIFO_C2S_* FIFO_S2C_* doc.md
```

## Notes
- Ensure you have the correct permissions to create FIFOs in the working directory.
- The project is designed for Linux systems with POSIX support.
- For more details, see the assignment specification and comments in the source code. 
>>>>>>> 746950da (add supporting files)
