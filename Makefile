CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -libverbs

VERBS_DIR = p2p_rtt/verbs
DOCA_DIR = p2p_rtt/doca
QP_POST_DIR = qp_post
QP_POST_DPA_THREAD_COUNT ?= 1
VERBS_BINS = $(VERBS_DIR)/server $(VERBS_DIR)/client

.PHONY: all verbs dpa qp_post clean

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

qp_post:
	rm -rf $(QP_POST_DIR)/build
	meson setup $(QP_POST_DIR)/build $(QP_POST_DIR) -Ddpa_thread_count=$(QP_POST_DPA_THREAD_COUNT)
	meson compile -C $(QP_POST_DIR)/build

clean:
	rm -f server client $(VERBS_BINS)
	rm -rf dpa/build $(DOCA_DIR)/build $(QP_POST_DIR)/build
