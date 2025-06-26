# Makefile for COMP2017 P2

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -Ilibs -fsanitize=address
LDFLAGS := -pthread

# Source files
SERVER_SOURCES = source/server.c source/markdown.c
CLIENT_SOURCES = source/client.c source/markdown.c
TEST_SOURCES = test_debug_complex.c source/markdown.c

# Object files
SERVER_OBJECTS = $(SERVER_SOURCES:.c=.o)
CLIENT_OBJECTS = $(CLIENT_SOURCES:.c=.o)

.PHONY: all clean debug_test
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
server: $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o server $(SERVER_OBJECTS)

# Link client
client: $(CLIENT_OBJECTS)
	$(CC) $(CFLAGS) -o client $(CLIENT_OBJECTS)

# Debug test
debug_test: test_debug_complex.o source/markdown.o
	$(CC) $(CFLAGS) -o debug_test test_debug_complex.o source/markdown.o
	./debug_test

test_debug_complex.o: test_debug_complex.c
	$(CC) $(CFLAGS) -c test_debug_complex.c -o test_debug_complex.o

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Cleanup
clean:
	rm -f server client debug_test *.o source/*.o test_debug_complex.o
