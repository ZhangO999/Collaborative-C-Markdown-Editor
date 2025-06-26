#!/bin/bash

echo "Starting simple server-client test..."

# Clean up any existing processes
pkill -f "./server 100" 2>/dev/null || true
sleep 1

# Start server and capture its PID output
echo "Starting server..."
./server 100 > server_output.txt 2>&1 &
SERVER_BG_PID=$!

# Wait for server to start and print its PID
sleep 2

# Extract server PID from output
if [ -f "server_output.txt" ]; then
    SERVER_PID=$(grep "Server PID:" server_output.txt | awk '{print $3}')
    echo "Server PID from output: $SERVER_PID"
    cat server_output.txt
else
    echo "No server output file found"
    exit 1
fi

if [ -z "$SERVER_PID" ]; then
    echo "Could not determine server PID"
    kill $SERVER_BG_PID 2>/dev/null
    exit 1
fi

# Test client connection
echo "Testing client connection to PID $SERVER_PID..."
(echo "DOC?"; sleep 1; echo "DISCONNECT") | ./client $SERVER_PID alice

echo "Client finished, killing server..."
kill $SERVER_BG_PID 2>/dev/null
wait $SERVER_BG_PID 2>/dev/null

# Check if doc.md was created
if [ -f "doc.md" ]; then
    echo "doc.md created:"
    cat doc.md
else
    echo "No doc.md file found"
fi

# Cleanup
rm -f server_output.txt

echo "Test completed." 