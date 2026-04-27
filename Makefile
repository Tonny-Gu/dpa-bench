CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -libverbs

VERBS_DIR = p2p_rtt/verbs
DOCA_DIR = p2p_rtt/doca
VERBS_BINS = $(VERBS_DIR)/server $(VERBS_DIR)/client

.PHONY: all verbs dpa clean

all: verbs

verbs: $(VERBS_BINS)

$(VERBS_DIR)/server: $(VERBS_DIR)/server.c $(VERBS_DIR)/rdma_common.h
	$(CC) $(CFLAGS) -o $@ $(VERBS_DIR)/server.c $(LDFLAGS)

$(VERBS_DIR)/client: $(VERBS_DIR)/client.c $(VERBS_DIR)/rdma_common.h
	$(CC) $(CFLAGS) -o $@ $(VERBS_DIR)/client.c $(LDFLAGS)

dpa:
	rm -rf $(DOCA_DIR)/build
	meson setup $(DOCA_DIR)/build $(DOCA_DIR)
	meson compile -C $(DOCA_DIR)/build

clean:
	rm -f server client $(VERBS_BINS)
	rm -rf dpa/build $(DOCA_DIR)/build
