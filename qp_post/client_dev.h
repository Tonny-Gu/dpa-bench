#ifndef QP_POST_CLIENT_DEV_H
#define QP_POST_CLIENT_DEV_H

#include <stdint.h>
#include <stdbool.h>

#include "defs.h"

struct qp_post_dpa_thread_stats {
	uint64_t server_a_writes;
	uint64_t server_b_writes;
	uint32_t status;
	uint32_t failed_qp;
	uint32_t finished;
	uint32_t reserved;
} __attribute__((__packed__, aligned(8)));

struct qp_post_dpa_args {
	uint64_t rdma_dpa_handle;
	uint64_t stats_dev_ptr;
	uint64_t rdma_handles_dev_ptr;
	uint64_t completion_handles_dev_ptr;
	uint64_t remote_addrs_dev_ptr;
	uint64_t remote_mmap_handles_dev_ptr;
	uint64_t local_addrs_dev_ptr;
	uint64_t local_mmap_handles_dev_ptr;
	uint64_t run_duration_us;
	uint64_t drain_timeout_us;
	uint32_t thread_index;
	uint32_t thread_count;
	uint32_t num_qps;
	uint32_t payload_size;
} __attribute__((__packed__, aligned(8)));

struct qp_post_dpa_runtime {
	struct qp_post_dpa_args args;
	uint64_t rdma_handles[QP_POST_TOTAL_QPS];
	uint64_t completion_handles[QP_POST_TOTAL_QPS];
	uint64_t remote_addrs[QP_POST_TOTAL_QPS];
	uint32_t remote_mmap_handles[QP_POST_TOTAL_QPS];
	uint64_t local_addrs[QP_POST_TOTAL_QPS];
	uint32_t local_mmap_handles[QP_POST_TOTAL_QPS];
	struct qp_post_dpa_thread_stats stats[QP_POST_TOTAL_QPS];
} __attribute__((__packed__, aligned(8)));

#endif
