#include <doca_dpa_dev.h>
#include <doca_dpa_dev_rdma.h>
#include <doca_dpa_dev_sync_event.h>
#include <doca_pcc_dev_utils.h>

#include <dpaintrin.h>

#include "client_dev.h"

static inline void set_device(uint64_t raw_dpa_handle)
{
	if (raw_dpa_handle != 0)
		doca_dpa_dev_device_set((doca_dpa_dev_t)raw_dpa_handle);
}

/*
 * With the scheme-1 layout each thread owns one RDMA context, and DOCA currently
 * numbers that context's connections densely from 0..QP_POST_DPA_QPS_PER_THREAD-1.
 * We rely on that here so the dense slot can be used as both the post-write
 * connection_id and the completion qp_slot without an extra lookup in the DPA hot path.
 */
static inline uint32_t qp_slot_from_connection_id(uint32_t connection_id)
{
	return connection_id;
}

static inline void atomic_add_u64(uint64_t *value, uint64_t addend)
{
	(void)__atomic_add_fetch(value, addend, __ATOMIC_ACQ_REL);
}

static inline uint64_t atomic_load_u64(uint64_t *value)
{
	return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

__dpa_rpc__ uint64_t qp_post_notify_threads_rpc(uint64_t dpa_handle_raw,
					       uint64_t notify_handles_dev_ptr)
{
	doca_dpa_dev_notification_completion_t *notify_handles =
		(doca_dpa_dev_notification_completion_t *)(uintptr_t)notify_handles_dev_ptr;
	uint32_t i;

	set_device(dpa_handle_raw);
	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i)
		doca_dpa_dev_thread_notify(notify_handles[i]);

	return 0;
}

__dpa_global__ void qp_post_client_kernel(uint64_t raw_arg)
{
	struct qp_post_dpa_args *arg = (struct qp_post_dpa_args *)raw_arg;
	struct qp_post_dpa_qp_state *qps = arg->qps;
	uint64_t rdma_dpa_handle = arg->rdma_dpa_handle;
	doca_dpa_dev_completion_t completion_handle = arg->completion_handle;
	struct qp_post_dpa_shared_state *shared_state =
		(struct qp_post_dpa_shared_state *)(uintptr_t)arg->shared_state_dev_ptr;
	struct qp_post_dpa_thread_result *thread_result =
		(struct qp_post_dpa_thread_result *)(uintptr_t)arg->thread_result_dev_ptr;
	doca_dpa_dev_sync_event_t done_sync_event = (doca_dpa_dev_sync_event_t)arg->done_sync_event_handle;
	doca_dpa_dev_completion_element_t comp_element;
	uint32_t thread_rank = arg->thread_index;
	uint64_t run_duration_us = arg->run_duration_us;
	uint64_t drain_timeout_us = arg->drain_timeout_us;
	uint32_t depth = arg->depth;
	uint32_t payload_size = arg->payload_size;
	uint32_t server_a_slot_count = 0;
	uint64_t server_a_writes = 0;
	uint64_t server_b_writes = 0;
	uint32_t status = QP_POST_DPA_STATUS_OK;
	uint32_t failed_qp = 0;
	uint64_t start_time_us;
	uint64_t now_us;
	uint8_t outstanding[QP_POST_DPA_QPS_PER_THREAD] = {0};
	bool stop_requested = false;
	bool made_progress;
	uint32_t i;

	set_device(rdma_dpa_handle);
	if (thread_rank < QP_POST_QPS_PER_SERVER)
		server_a_slot_count =
			((QP_POST_QPS_PER_SERVER - 1U - thread_rank) / QP_POST_DPA_THREAD_COUNT) + 1U;

	doca_dpa_dev_completion_request_notification(completion_handle);

	atomic_add_u64(&shared_state->start_count, 1);
	while (atomic_load_u64(&shared_state->start_count) < QP_POST_DPA_THREAD_COUNT)
		;

	start_time_us = doca_pcc_dev_get_timer();
	DOCA_DPA_DEV_LOG_INFO("qp_post thread %u started, qps=%u, sq_depth=%u, payload=%u, duration_us=%lu\n",
			      thread_rank,
			      QP_POST_DPA_QPS_PER_THREAD,
			      depth,
			      payload_size,
			      run_duration_us);

	while (1) {
		uint32_t completed_count = 0;

		made_progress = false;
		while (doca_dpa_dev_get_completion(completion_handle, &comp_element)) {
			uint32_t connection_id = doca_dpa_dev_get_completion_user_data(comp_element);
			uint32_t qp_slot = qp_slot_from_connection_id(connection_id);
			doca_dpa_dev_completion_type_t comp_type = doca_dpa_dev_get_completion_type(comp_element);

			completed_count++;
			made_progress = true;

			if (outstanding[qp_slot] == 0) {
				status = QP_POST_DPA_STATUS_BAD_COMPLETION;
				failed_qp = thread_rank + (qp_slot * QP_POST_DPA_THREAD_COUNT);
				DOCA_DPA_DEV_LOG_ERR("qp_post thread %u got completion with empty outstanding on qp=%u\n",
						     thread_rank,
						     failed_qp);
				stop_requested = true;
				continue;
			}

			outstanding[qp_slot]--;

			if (comp_type != DOCA_DPA_DEV_COMP_SEND) {
				status = QP_POST_DPA_STATUS_BAD_COMPLETION;
				failed_qp = thread_rank + (qp_slot * QP_POST_DPA_THREAD_COUNT);
				DOCA_DPA_DEV_LOG_ERR("qp_post thread %u got bad completion type=%u on qp=%u\n",
						     thread_rank,
						     comp_type,
						     failed_qp);
				stop_requested = true;
				continue;
			}

			if (qp_slot < server_a_slot_count)
				server_a_writes++;
			else
				server_b_writes++;
		}

		if (completed_count != 0) {
			doca_dpa_dev_completion_ack(completion_handle, completed_count);
			doca_dpa_dev_completion_request_notification(completion_handle);
		}

		for (i = 0; i < QP_POST_DPA_QPS_PER_THREAD; ++i) {
			while (!stop_requested && outstanding[i] < depth) {
				doca_dpa_dev_rdma_post_write(qps[i].rdma_handle,
						     i,
						     qps[i].remote_mmap_handle,
						     qps[i].remote_addr,
						     qps[i].local_mmap_handle,
						     qps[i].local_addr,
						     payload_size,
						     DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH);
				outstanding[i]++;
				made_progress = true;
			}
		}

		if (!stop_requested) {
			now_us = doca_pcc_dev_get_timer();
			stop_requested = (now_us - start_time_us) >= run_duration_us;
		}

		if (stop_requested) {
			bool any_pending = false;
			uint32_t first_pending = UINT32_MAX;

			for (i = 0; i < QP_POST_DPA_QPS_PER_THREAD; ++i) {
				if (outstanding[i] != 0) {
					any_pending = true;
					first_pending = thread_rank + (i * QP_POST_DPA_THREAD_COUNT);
					break;
				}
			}

			if (!any_pending)
				break;

			if (!made_progress) {
				now_us = doca_pcc_dev_get_timer();
				if ((now_us - start_time_us) >= (run_duration_us + drain_timeout_us)) {
					status = QP_POST_DPA_STATUS_DRAIN_TIMEOUT;
					failed_qp = first_pending;
					DOCA_DPA_DEV_LOG_ERR("qp_post thread %u drain timeout, pending qp=%u, elapsed_us=%lu\n",
						     thread_rank,
						     first_pending,
						     now_us - start_time_us);
					break;
				}
			}
		}
	}

	thread_result->server_a_writes = server_a_writes;
	thread_result->server_b_writes = server_b_writes;
	thread_result->status = status;
	thread_result->failed_qp = failed_qp;
	DOCA_DPA_DEV_LOG_INFO("qp_post thread %u finished: a=%lu b=%lu status=%u failed_qp=%u\n",
			      thread_rank,
			      server_a_writes,
			      server_b_writes,
			      status,
			      failed_qp);
	__dpa_thread_fence(__DPA_HEAP, __DPA_W, __DPA_W);
	atomic_add_u64(&shared_state->done_count, 1);

	if (thread_rank == 0) {
		while (atomic_load_u64(&shared_state->done_count) < QP_POST_DPA_THREAD_COUNT)
			;
		doca_dpa_dev_sync_event_update_set(done_sync_event, 1);
	}
	doca_dpa_dev_thread_finish();
}
