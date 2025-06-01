# Makefile for COMP2017 P2

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -Ilibs -fsanitize=address
LDFLAGS := -pthread

.PHONY: all clean
all: server client

# Compile markdown.o
markdown.o: source/markdown.c libs/markdown.h libs/document.h
	$(CC) $(CFLAGS) -c source/markdown.c -o markdown.o

# Compile server.o
server.o: source/server.c libs/markdown.h libs/document.h libs/server.h
	$(CC) $(CFLAGS) -c source/server.c -o server.o

# Compile server_lib.o (server functions without main for testing)
server_lib.o: source/server_lib.c libs/markdown.h libs/document.h libs/server.h
	$(CC) $(CFLAGS) -c source/server_lib.c -o server_lib.o

# Compile client.o
client.o: source/client.c libs/markdown.h
	$(CC) $(CFLAGS) -c source/client.c -o client.o

# Link server (needs pthreads)
server: server.o markdown.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o server server.o markdown.o

# Link client
client: client.o markdown.o
	$(CC) $(CFLAGS) -o client client.o markdown.o

# Cleanup
clean:
	rm -f *.o server client
