#ifndef DPA_LATENCY_SERVER_DEV_H
#define DPA_LATENCY_SERVER_DEV_H

#include <stddef.h>
#include <stdint.h>

struct latency_dpa_thread_arg {
	uint64_t rdma_dpa_handle;
	uint64_t completion_handle;
	uint64_t rdma_handle;
	uint64_t local_buf_addr;
	uint64_t remote_buf_addr;
	uint32_t local_mmap_handle;
	uint32_t remote_mmap_handle;
	size_t length;
} __attribute__((__packed__, aligned(8)));

#endif
