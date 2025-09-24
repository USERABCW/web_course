# Makefile for  File Transfer System

CXX = g++
CXXFLAGS = -std=c++11 -pthread -Wall -O2
TARGETS = server client

all: $(TARGETS)

server: server.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

client: client.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(TARGETS) *.o

run-server: server
	./server 8888

run-client: client
	./client 127.0.0.1 8888

test-file:
	dd if=/dev/urandom of=test10mb.bin bs=1M count=10
	@echo "Created test10mb.bin (10MB)"

.PHONY: all clean run-server run-client test-file