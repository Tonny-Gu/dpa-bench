#include <doca_dpa_dev.h>
#include <doca_dpa_dev_rdma.h>

#include "server_dev.h"

__dpa_global__ void latency_server_kernel(uint64_t arg)
{
	struct latency_dpa_thread_arg *thread_arg = (struct latency_dpa_thread_arg *)arg;
	doca_dpa_dev_completion_element_t comp_element;
	doca_dpa_dev_completion_type_t comp_type;
	uint32_t connection_id;
	uint32_t imm;

	if (thread_arg->rdma_dpa_handle)
		doca_dpa_dev_device_set(thread_arg->rdma_dpa_handle);

	while (!doca_dpa_dev_get_completion(thread_arg->completion_handle, &comp_element))
		;

	while (1) {
		comp_type = doca_dpa_dev_get_completion_type(comp_element);
		if (comp_type == DOCA_DPA_DEV_COMP_RECV_RDMA_WRITE_IMM) {
			connection_id = doca_dpa_dev_get_completion_user_data(comp_element);
			imm = doca_dpa_dev_get_completion_immediate(comp_element);
			doca_dpa_dev_rdma_post_write_imm(thread_arg->rdma_handle,
						 connection_id,
						 thread_arg->remote_mmap_handle,
						 thread_arg->remote_buf_addr,
						 thread_arg->local_mmap_handle,
						 thread_arg->local_buf_addr,
						 thread_arg->length,
						 imm,
						 DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH |
						 DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS);
			doca_dpa_dev_rdma_post_receive(thread_arg->rdma_handle,
						       thread_arg->local_mmap_handle,
						       thread_arg->local_buf_addr,
						       thread_arg->length);
		}

		doca_dpa_dev_completion_ack(thread_arg->completion_handle, 1);

		while (!doca_dpa_dev_get_completion(thread_arg->completion_handle, &comp_element))
			;
	}
}

__dpa_rpc__ uint64_t latency_post_initial_receive_rpc(doca_dpa_dev_t rdma_dpa_handle,
					      doca_dpa_dev_rdma_t rdma_handle,
					      doca_dpa_dev_mmap_t local_mmap_handle,
					      uint64_t local_buf_addr,
					      size_t length)
{
	if (rdma_dpa_handle)
		doca_dpa_dev_device_set(rdma_dpa_handle);

	doca_dpa_dev_rdma_post_receive(rdma_handle, local_mmap_handle, local_buf_addr, length);
	return 0;
}
