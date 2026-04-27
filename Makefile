CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -libverbs

.PHONY: all clean dpa

all: server client

server: server.c rdma_common.h
	$(CC) $(CFLAGS) -o $@ server.c $(LDFLAGS)

client: client.c rdma_common.h
	$(CC) $(CFLAGS) -o $@ client.c $(LDFLAGS)

dpa:
	meson setup dpa/build dpa --wipe
	meson compile -C dpa/build

clean:
	rm -f server client
	rm -rf dpa/build
