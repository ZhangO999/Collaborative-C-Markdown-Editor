# Makefile for COMP2017 P2

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -Ilibs -fsanitize=address
LDFLAGS := -pthread

.PHONY: all clean run-tests run-server-tests
all: server client tests server_tests

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

# Compile tests.o
tests.o: source/tests.c libs/markdown.h libs/document.h
	$(CC) $(CFLAGS) -c source/tests.c -o tests.o

# Compile server_tests.o
server_tests.o: source/server_tests.c libs/markdown.h libs/document.h libs/server.h
	$(CC) $(CFLAGS) -c source/server_tests.c -o server_tests.o

# Link server (needs pthreads)
server: server.o markdown.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o server server.o markdown.o

# Link client
client: client.o markdown.o
	$(CC) $(CFLAGS) -o client client.o markdown.o

# Link tests
tests: tests.o markdown.o
	$(CC) $(CFLAGS) -o tests tests.o markdown.o

# Link server_tests (uses server_lib.o instead of server.o to avoid main conflict)
server_tests: server_tests.o server_lib.o markdown.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o server_tests server_tests.o server_lib.o markdown.o

# Run tests
run-tests: tests
	./tests

# Run server tests
run-server-tests: server_tests
	./server_tests

# Cleanup
clean:
	rm -f *.o server client tests server_tests
