#ifndef THREAD_COMM_COMMON_H_
#define THREAD_COMM_COMMON_H_

#include <stdint.h>

#define THREAD_COMM_MESSAGE 0x544852454144554cULL
#define THREAD_COMM_REPLY 0x425f41434bULL
#define THREAD_COMM_REPLY_ERROR 0x455252ULL

#define THREAD_COMM_STAGE_INIT 0ULL
#define THREAD_COMM_STAGE_THREAD_A 1ULL
#define THREAD_COMM_STAGE_THREAD_B 2ULL
#define THREAD_COMM_STAGE_ERROR 255ULL

struct thread_comm_shared_state {
	uint64_t stage;
	uint64_t message;
	uint64_t reply;
};

struct thread_a_arg {
	uint64_t dpa_handle;
	uint64_t shared_state_dev_ptr;
	uint64_t thread_b_notify_handle;
};

struct thread_b_arg {
	uint64_t dpa_handle;
	uint64_t shared_state_dev_ptr;
	uint64_t host_sync_event_handle;
};

#endif /* THREAD_COMM_COMMON_H_ */
