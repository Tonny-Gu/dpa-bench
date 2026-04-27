#include <doca_dpa_dev.h>
#include <doca_dpa_dev_rdma.h>
#include <doca_pcc_dev_utils.h>

#include <dpaintrin.h>

#include "client_dev.h"

static inline void set_device(uint64_t raw_dpa_handle)
{
	if (raw_dpa_handle != 0)
		doca_dpa_dev_device_set((doca_dpa_dev_t)raw_dpa_handle);
}

__dpa_rpc__ uint64_t qp_post_notify_threads_rpc(uint64_t dpa_handle_raw,
					       uint64_t notify_handles_dev_ptr,
					       uint64_t num_threads_raw)
{
	doca_dpa_dev_notification_completion_t *notify_handles =
		(doca_dpa_dev_notification_completion_t *)(uintptr_t)notify_handles_dev_ptr;
	unsigned int num_threads = (unsigned int)num_threads_raw;
	unsigned int i;

	set_device(dpa_handle_raw);
	for (i = 0; i < num_threads; ++i)
		doca_dpa_dev_thread_notify(notify_handles[i]);

	return 0;
}

__dpa_global__ void qp_post_client_kernel(uint64_t raw_arg)
{
	struct qp_post_dpa_args *arg = (struct qp_post_dpa_args *)raw_arg;
	struct qp_post_dpa_thread_stats *stats =
		(struct qp_post_dpa_thread_stats *)(uintptr_t)arg->stats_dev_ptr;
	doca_dpa_dev_rdma_t *rdma_handles = (doca_dpa_dev_rdma_t *)(uintptr_t)arg->rdma_handles_dev_ptr;
	doca_dpa_dev_completion_t *completion_handles =
		(doca_dpa_dev_completion_t *)(uintptr_t)arg->completion_handles_dev_ptr;
	uint64_t *remote_addrs = (uint64_t *)(uintptr_t)arg->remote_addrs_dev_ptr;
	doca_dpa_dev_mmap_t *remote_mmap_handles =
		(doca_dpa_dev_mmap_t *)(uintptr_t)arg->remote_mmap_handles_dev_ptr;
	uint64_t *local_addrs = (uint64_t *)(uintptr_t)arg->local_addrs_dev_ptr;
	doca_dpa_dev_mmap_t *local_mmap_handles =
		(doca_dpa_dev_mmap_t *)(uintptr_t)arg->local_mmap_handles_dev_ptr;
	struct qp_post_dpa_thread_stats *thread_stats;
	doca_dpa_dev_completion_element_t comp_element;
	unsigned int thread_rank = arg->thread_index;
	unsigned int num_threads = arg->thread_count;
	uint64_t start_time_us;
	uint64_t now_us;
	uint8_t pending[QP_POST_TOTAL_QPS] = {0};
	bool stop_requested = false;
	bool made_progress;
	unsigned int i;

	set_device(arg->rdma_dpa_handle);
	thread_stats = &stats[thread_rank];

	start_time_us = doca_pcc_dev_get_timer();
	DOCA_DPA_DEV_LOG_INFO("qp_post thread %u started, qps=%u, payload=%u, duration_us=%llu\n",
			      thread_rank,
			      arg->num_qps,
			      arg->payload_size,
			      (unsigned long long)arg->run_duration_us);

	for (i = thread_rank; i < arg->num_qps; i += num_threads)
		doca_dpa_dev_completion_request_notification(completion_handles[i]);

	while (1) {
		made_progress = false;

		for (i = thread_rank; i < arg->num_qps; i += num_threads) {
			if (!pending[i] && !stop_requested) {
				doca_dpa_dev_rdma_post_write(rdma_handles[i],
						     0,
						     remote_mmap_handles[i],
						     remote_addrs[i],
						     local_mmap_handles[i],
						     local_addrs[i],
						     arg->payload_size,
						     DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
				pending[i] = 1;
				made_progress = true;
			}

			if (pending[i] && doca_dpa_dev_get_completion(completion_handles[i], &comp_element)) {
				doca_dpa_dev_completion_type_t comp_type = doca_dpa_dev_get_completion_type(comp_element);

				doca_dpa_dev_completion_ack(completion_handles[i], 1);
				doca_dpa_dev_completion_request_notification(completion_handles[i]);
				pending[i] = 0;
				made_progress = true;

				if (comp_type != DOCA_DPA_DEV_COMP_SEND) {
					thread_stats->status = QP_POST_DPA_STATUS_BAD_COMPLETION;
					thread_stats->failed_qp = i;
					DOCA_DPA_DEV_LOG_ERR("qp_post thread %u got bad completion type=%u on qp=%u\n",
						     thread_rank,
						     comp_type,
						     i);
					stop_requested = true;
					continue;
				}

				if (i < QP_POST_QPS_PER_SERVER)
					thread_stats->server_a_writes++;
				else
					thread_stats->server_b_writes++;
			}
		}

		if (!stop_requested) {
			now_us = doca_pcc_dev_get_timer();
			stop_requested = (now_us - start_time_us) >= arg->run_duration_us;
		}

		if (stop_requested) {
			bool any_pending = false;
			uint32_t first_pending = UINT32_MAX;

			for (i = thread_rank; i < arg->num_qps; i += num_threads) {
				if (pending[i] != 0) {
					any_pending = true;
					first_pending = i;
					break;
				}
			}

			if (!any_pending)
				break;

			if (!made_progress) {
				now_us = doca_pcc_dev_get_timer();
				if ((now_us - start_time_us) >= (arg->run_duration_us + arg->drain_timeout_us)) {
					thread_stats->status = QP_POST_DPA_STATUS_DRAIN_TIMEOUT;
					thread_stats->failed_qp = first_pending;
					DOCA_DPA_DEV_LOG_ERR("qp_post thread %u drain timeout, pending qp=%u, elapsed_us=%llu\n",
						     thread_rank,
						     first_pending,
						     (unsigned long long)(now_us - start_time_us));
					break;
				}
			}
		}
	}

	DOCA_DPA_DEV_LOG_INFO("qp_post thread %u finished: a=%llu b=%llu status=%u failed_qp=%u\n",
			      thread_rank,
			      (unsigned long long)thread_stats->server_a_writes,
			      (unsigned long long)thread_stats->server_b_writes,
			      thread_stats->status,
			      thread_stats->failed_qp);
	thread_stats->finished = 1;
	__dpa_thread_fence(__DPA_HEAP, __DPA_W, __DPA_W);
	doca_dpa_dev_thread_finish();
}
