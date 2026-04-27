#include <doca_dpa_dev.h>
#include <doca_dpa_dev_sync_event.h>

#include <dpaintrin.h>

#include "thread_comm_common.h"

static inline void set_device(uint64_t dpa_handle_raw)
{
	if (dpa_handle_raw != 0)
		doca_dpa_dev_device_set((doca_dpa_dev_t)dpa_handle_raw);
}

__dpa_global__ void thread_a_kernel(uint64_t raw_arg)
{
	struct thread_a_arg *arg = (struct thread_a_arg *)raw_arg;
	struct thread_comm_shared_state *shared = (struct thread_comm_shared_state *)arg->shared_state_dev_ptr;

	set_device(arg->dpa_handle);

	shared->stage = THREAD_COMM_STAGE_THREAD_A;
	shared->message = THREAD_COMM_MESSAGE;
	shared->reply = 0;
	__dpa_thread_fence(__DPA_HEAP, __DPA_W, __DPA_W);

	doca_dpa_dev_thread_notify((doca_dpa_dev_notification_completion_t)arg->thread_b_notify_handle);
	doca_dpa_dev_thread_finish();
}

__dpa_global__ void thread_b_kernel(uint64_t raw_arg)
{
	struct thread_b_arg *arg = (struct thread_b_arg *)raw_arg;
	struct thread_comm_shared_state *shared = (struct thread_comm_shared_state *)arg->shared_state_dev_ptr;
	uint64_t event_value;

	set_device(arg->dpa_handle);

	if (shared->stage == THREAD_COMM_STAGE_THREAD_A && shared->message == THREAD_COMM_MESSAGE) {
		shared->reply = THREAD_COMM_REPLY;
		shared->stage = THREAD_COMM_STAGE_THREAD_B;
		event_value = THREAD_COMM_STAGE_THREAD_B;
	} else {
		shared->reply = THREAD_COMM_REPLY_ERROR;
		shared->stage = THREAD_COMM_STAGE_ERROR;
		event_value = THREAD_COMM_STAGE_ERROR;
	}
	__dpa_thread_fence(__DPA_HEAP, __DPA_W, __DPA_W);

	doca_dpa_dev_sync_event_update_set((doca_dpa_dev_sync_event_t)arg->host_sync_event_handle, event_value);
	doca_dpa_dev_thread_finish();
}

__dpa_rpc__ uint64_t kick_thread_a_rpc(uint64_t dpa_handle_raw, uint64_t thread_a_notify_handle)
{
	set_device(dpa_handle_raw);
	doca_dpa_dev_thread_notify((doca_dpa_dev_notification_completion_t)thread_a_notify_handle);
	return 0;
}
