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
};

struct qp_post_dpa_qp_state {
	uint64_t rdma_handle;
	uint64_t remote_addr;
	uint64_t local_addr;
	uint32_t remote_mmap_handle;
	uint32_t local_mmap_handle;
	uint32_t connection_id;
	uint32_t server_index;
};

struct qp_post_dpa_thread_data {
	struct qp_post_dpa_thread_stats stats;
	uint64_t completion_handle;
	struct qp_post_dpa_qp_state qps[QP_POST_DPA_QPS_PER_THREAD];
} __attribute__((aligned(QP_POST_DPA_THREAD_DATA_ALIGNMENT)));

struct qp_post_dpa_shared_state {
	uint64_t start_count;
	uint64_t done_count;
} __attribute__((aligned(QP_POST_DPA_THREAD_DATA_ALIGNMENT)));

struct qp_post_dpa_args {
	uint64_t rdma_dpa_handle;
	uint64_t thread_data_dev_ptr;
	uint64_t shared_state_dev_ptr;
	uint64_t done_sync_event_handle;
	uint64_t run_duration_us;
	uint64_t drain_timeout_us;
	uint32_t thread_index;
	uint32_t depth;
	uint32_t payload_size;
} __attribute__((__packed__, aligned(8)));

_Static_assert(sizeof(struct qp_post_dpa_thread_data) % QP_POST_DPA_THREAD_DATA_ALIGNMENT == 0,
	       "qp_post_dpa_thread_data must be cacheline aligned");

#endif
