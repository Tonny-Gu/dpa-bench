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
					       uint64_t notify_handles_dev_ptr)
{
	doca_dpa_dev_notification_completion_t *notify_handles =
		(doca_dpa_dev_notification_completion_t *)(uintptr_t)notify_handles_dev_ptr;
	unsigned int i;

	set_device(dpa_handle_raw);
	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i)
		doca_dpa_dev_thread_notify(notify_handles[i]);

	return 0;
}

__dpa_global__ void qp_post_client_kernel(uint64_t raw_arg)
{
	struct qp_post_dpa_args *arg = (struct qp_post_dpa_args *)raw_arg;
	struct qp_post_dpa_thread_data *thread_data =
		(struct qp_post_dpa_thread_data *)(uintptr_t)arg->thread_data_dev_ptr;
	struct qp_post_dpa_thread_stats *thread_stats;
	doca_dpa_dev_completion_element_t comp_element;
	unsigned int thread_rank = arg->thread_index;
	uint64_t start_time_us;
	uint64_t now_us;
	bool stop_requested = false;
	bool made_progress;
	unsigned int i;

	set_device(arg->rdma_dpa_handle);
	thread_stats = &thread_data->stats;

	start_time_us = doca_pcc_dev_get_timer();
	DOCA_DPA_DEV_LOG_INFO("qp_post thread %u started, qps=%u, sq_depth=%u, payload=%u, duration_us=%llu\n",
			      thread_rank,
			      QP_POST_DPA_QPS_PER_THREAD,
			      arg->depth,
			      arg->payload_size,
			      (unsigned long long)arg->run_duration_us);

	for (i = 0; i < QP_POST_DPA_QPS_PER_THREAD; ++i)
		doca_dpa_dev_completion_request_notification(thread_data->qps[i].completion_handle);

	while (1) {
		made_progress = false;

		for (i = 0; i < QP_POST_DPA_QPS_PER_THREAD; ++i) {
			unsigned int qp_index = thread_rank + (i * QP_POST_DPA_THREAD_COUNT);

			while (thread_data->qps[i].outstanding != 0 &&
			       doca_dpa_dev_get_completion(thread_data->qps[i].completion_handle, &comp_element)) {
				doca_dpa_dev_completion_type_t comp_type = doca_dpa_dev_get_completion_type(comp_element);

				doca_dpa_dev_completion_ack(thread_data->qps[i].completion_handle, 1);
				doca_dpa_dev_completion_request_notification(thread_data->qps[i].completion_handle);
				thread_data->qps[i].outstanding--;
				made_progress = true;

				if (comp_type != DOCA_DPA_DEV_COMP_SEND) {
					thread_stats->status = QP_POST_DPA_STATUS_BAD_COMPLETION;
					thread_stats->failed_qp = qp_index;
					DOCA_DPA_DEV_LOG_ERR("qp_post thread %u got bad completion type=%u on qp=%u\n",
						     thread_rank,
						     comp_type,
						     qp_index);
					stop_requested = true;
					continue;
				}

				if (qp_index < QP_POST_QPS_PER_SERVER)
					thread_stats->server_a_writes++;
				else
					thread_stats->server_b_writes++;
			}

			while (!stop_requested && thread_data->qps[i].outstanding < arg->depth) {
				doca_dpa_dev_rdma_post_write(thread_data->qps[i].rdma_handle,
						     0,
						     thread_data->qps[i].remote_mmap_handle,
						     thread_data->qps[i].remote_addr,
						     thread_data->qps[i].local_mmap_handle,
						     thread_data->qps[i].local_addr,
						     arg->payload_size,
						     DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
				thread_data->qps[i].outstanding++;
				made_progress = true;
			}
		}

		if (!stop_requested) {
			now_us = doca_pcc_dev_get_timer();
			stop_requested = (now_us - start_time_us) >= arg->run_duration_us;
		}

		if (stop_requested) {
			bool any_pending = false;
			uint32_t first_pending = UINT32_MAX;

			for (i = 0; i < QP_POST_DPA_QPS_PER_THREAD; ++i) {
				if (thread_data->qps[i].outstanding != 0) {
					any_pending = true;
					first_pending = thread_rank + (i * QP_POST_DPA_THREAD_COUNT);
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
