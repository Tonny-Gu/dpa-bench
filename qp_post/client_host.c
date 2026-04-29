#define _POSIX_C_SOURCE 200809L

#include "client.h"

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_log.h>

#include <stdlib.h>
#include <string.h>

DOCA_LOG_REGISTER(QP_POST::CLIENT::HOST);

struct host_client_resources {
	struct doca_pe *shared_pe;
	struct doca_buf_inventory *buf_inventory;
};

struct host_write_wait {
	bool done;
	doca_error_t status;
};

struct host_write_slot {
	struct doca_buf *local_doca_buf;
	struct doca_buf *remote_doca_buf;
	struct doca_rdma_task_write *write_task;
	struct host_write_wait wait;
	bool inflight;
};

struct host_client_endpoint {
	struct qp_post_endpoint *base;
	struct host_write_slot *write_slots;
	uint32_t write_depth;
	uint32_t write_outstanding;
};

static void host_write_wait_reset(struct host_write_wait *wait)
{
	memset(wait, 0, sizeof(*wait));
	wait->status = DOCA_SUCCESS;
}

static void host_write_task_done(struct doca_rdma_task_write *task,
					 union doca_data task_user_data,
					 union doca_data ctx_user_data)
{
	struct host_write_wait *wait = task_user_data.ptr;

	(void)task;
	(void)ctx_user_data;
	if (wait == NULL)
		return;

	wait->status = DOCA_SUCCESS;
	wait->done = true;
}

static void host_write_task_error(struct doca_rdma_task_write *task,
					  union doca_data task_user_data,
					  union doca_data ctx_user_data)
{
	struct host_write_wait *wait = task_user_data.ptr;
	struct doca_task *base_task = doca_rdma_task_write_as_task(task);

	(void)ctx_user_data;
	if (wait == NULL)
		return;

	wait->status = doca_task_get_status(base_task);
	wait->done = true;
}

static doca_error_t host_client_caps(const struct doca_devinfo *devinfo)
{
	return doca_rdma_cap_task_write_is_supported(devinfo);
}

static doca_error_t host_client_resources_init(struct host_client_resources *res, uint32_t depth)
{
	memset(res, 0, sizeof(*res));
	DOCA_CHECK(doca_pe_create(&res->shared_pe));
	DOCA_CHECK(doca_buf_inventory_create(2 * depth * QP_POST_TOTAL_QPS, &res->buf_inventory));
	DOCA_CHECK(doca_buf_inventory_start(res->buf_inventory));

	return DOCA_SUCCESS;
}

static void host_client_resources_destroy(struct host_client_resources *res)
{
	if (res->buf_inventory != NULL) {
		(void)doca_buf_inventory_stop(res->buf_inventory);
		(void)doca_buf_inventory_destroy(res->buf_inventory);
		res->buf_inventory = NULL;
	}

	if (res->shared_pe != NULL) {
		(void)doca_pe_destroy(res->shared_pe);
		res->shared_pe = NULL;
	}
}

static doca_error_t host_client_prepare_write(struct host_client_endpoint *host_ep,
					     struct qp_post_endpoint *base,
					     struct doca_buf_inventory *buf_inventory,
					     uint32_t depth,
					     size_t payload_size)
{
	union doca_data user_data = {0};
	const struct doca_buf *src_buf;
	struct doca_buf *dst_buf;
	uint32_t i;

	if (payload_size > base->remote_buf_len)
		return DOCA_ERROR_INVALID_VALUE;

	host_ep->base = base;
	host_ep->write_depth = depth;
	host_ep->write_slots = calloc(depth, sizeof(*host_ep->write_slots));
	if (host_ep->write_slots == NULL)
		return DOCA_ERROR_NO_MEMORY;

	for (i = 0; i < depth; ++i) {
		if (payload_size != 0) {
			DOCA_CHECK(doca_buf_inventory_buf_get_by_addr(buf_inventory,
							    base->local_mmap,
							    base->local_buf,
							    payload_size,
							    &host_ep->write_slots[i].local_doca_buf));

			DOCA_CHECK(doca_buf_inventory_buf_get_by_addr(buf_inventory,
							    base->remote_mmap,
							    (void *)(uintptr_t)base->remote_buf_addr,
							    payload_size,
							    &host_ep->write_slots[i].remote_doca_buf));
		}

		src_buf = payload_size == 0 ? NULL : host_ep->write_slots[i].local_doca_buf;
		dst_buf = payload_size == 0 ? NULL : host_ep->write_slots[i].remote_doca_buf;

		DOCA_CHECK(doca_rdma_task_write_allocate_init(base->rdma,
						     base->connection,
						     src_buf,
						     dst_buf,
						     user_data,
						     &host_ep->write_slots[i].write_task));
	}

	return DOCA_SUCCESS;
}

static void host_client_destroy_write_endpoint(struct host_client_endpoint *host_ep)
{
	if (host_ep->write_slots == NULL)
		return;

	for (uint32_t i = 0; i < host_ep->write_depth; ++i) {
		if (host_ep->write_slots[i].write_task != NULL)
			doca_task_free(doca_rdma_task_write_as_task(host_ep->write_slots[i].write_task));

		if (host_ep->write_slots[i].remote_doca_buf != NULL)
			(void)doca_buf_dec_refcount(host_ep->write_slots[i].remote_doca_buf, NULL);

		if (host_ep->write_slots[i].local_doca_buf != NULL)
			(void)doca_buf_dec_refcount(host_ep->write_slots[i].local_doca_buf, NULL);
	}

	free(host_ep->write_slots);
	memset(host_ep, 0, sizeof(*host_ep));
}

static void host_client_destroy_write_endpoints(struct host_client_endpoint *host_eps, uint32_t num_eps)
{
	while (num_eps-- != 0)
		host_client_destroy_write_endpoint(&host_eps[num_eps]);
}

static doca_error_t host_client_post_write(struct host_client_endpoint *host_ep)
{
	union doca_data user_data = {0};
	doca_error_t result;
	uint32_t i;

	if (host_ep->write_slots == NULL)
		return DOCA_ERROR_BAD_STATE;

	for (i = 0; i < host_ep->write_depth; ++i) {
		if (!host_ep->write_slots[i].inflight)
			break;
	}
	if (i == host_ep->write_depth)
		return DOCA_ERROR_AGAIN;

	host_write_wait_reset(&host_ep->write_slots[i].wait);
	user_data.ptr = &host_ep->write_slots[i].wait;
	doca_task_set_user_data(doca_rdma_task_write_as_task(host_ep->write_slots[i].write_task), user_data);
	host_ep->write_slots[i].inflight = true;
	host_ep->write_outstanding++;

	result = doca_task_submit(doca_rdma_task_write_as_task(host_ep->write_slots[i].write_task));
	if (result != DOCA_SUCCESS) {
		host_ep->write_slots[i].inflight = false;
		host_ep->write_outstanding--;
	}

	return result;
}

static doca_error_t host_client_poll_write(struct host_client_endpoint *host_ep, uint32_t *completed_count)
{
	uint32_t i;
	doca_error_t result = DOCA_SUCCESS;

	if (completed_count != NULL)
		*completed_count = 0;

	if (host_ep->write_outstanding == 0)
		return DOCA_SUCCESS;

	while (doca_pe_progress(host_ep->base->pe) != 0)
		;

	for (i = 0; i < host_ep->write_depth; ++i) {
		if (!host_ep->write_slots[i].inflight || !host_ep->write_slots[i].wait.done)
			continue;

		host_ep->write_slots[i].inflight = false;
		host_ep->write_outstanding--;
		if (completed_count != NULL)
			(*completed_count)++;
		if (result == DOCA_SUCCESS)
			result = host_ep->write_slots[i].wait.status;
	}

	return result;
}

static doca_error_t run_host_datapath(struct host_client_endpoint *host_eps,
					    const struct client_config *cfg,
					    uint64_t *server_a_writes,
					    uint64_t *server_b_writes)
{
	const double duration_us = (double)cfg->duration_s * 1000000.0;
	double start_us = get_time_us();
	double now_us = start_us;
	bool should_post;
	bool inflight;
	uint32_t completed_count;
	uint32_t i;

	*server_a_writes = 0;
	*server_b_writes = 0;

	while (!g_stop) {
		now_us = get_time_us();
		should_post = now_us - start_us < duration_us;
		inflight = false;

		for (i = 0; i < QP_POST_TOTAL_QPS; ++i) {
			DOCA_CHECK(host_client_poll_write(&host_eps[i], &completed_count));

			if (completed_count != 0) {
				if (i < QP_POST_QPS_PER_SERVER)
					*server_a_writes += completed_count;
				else
					*server_b_writes += completed_count;
			}

			if (host_eps[i].write_outstanding != 0)
				inflight = true;

			while (should_post && host_eps[i].write_outstanding < cfg->depth) {
				DOCA_CHECK(host_client_post_write(&host_eps[i]));
				inflight = true;
			}
		}

		if (!should_post && !inflight)
			break;
	}

	return g_stop ? DOCA_ERROR_AGAIN : DOCA_SUCCESS;
}

static doca_error_t host_client_cleanup(struct qp_post_endpoint *eps,
					      struct host_client_endpoint *host_eps,
					      struct host_client_resources *host_res,
					      struct doca_dev **host_dev)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t cleanup_result;

	host_client_destroy_write_endpoints(host_eps, QP_POST_TOTAL_QPS);
	qp_post_client_destroy_endpoints(eps, QP_POST_TOTAL_QPS);
	host_client_resources_destroy(host_res);
	if (*host_dev != NULL) {
		cleanup_result = doca_dev_close(*host_dev);
		if (cleanup_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("doca_dev_close failed: %s", doca_strerror(cleanup_result));
			result = cleanup_result;
		}
		*host_dev = NULL;
	}

	return result;
}

static doca_error_t host_client_init_endpoints(const struct client_config *cfg,
					      struct qp_post_endpoint *eps,
					      struct doca_dev *host_dev,
					      struct host_client_resources *host_res)
{
	uint32_t i;

	DOCA_CHECK(qp_post_endpoint_create(&eps[0],
					    host_dev,
					    cfg->has_gid_index,
					    cfg->gid_index,
					    QP_POST_MAX_PAYLOAD,
					    QP_POST_TOTAL_QPS,
					    host_res->shared_pe));

	DOCA_CHECK(doca_rdma_task_write_set_conf(eps[0].rdma,
					       host_write_task_done,
					       host_write_task_error,
					       cfg->depth * QP_POST_TOTAL_QPS));

	DOCA_CHECK(qp_post_endpoint_start(&eps[0]));
	memset(eps[0].local_buf, 'A', QP_POST_MAX_PAYLOAD);

	for (i = 1; i < QP_POST_TOTAL_QPS; ++i)
		DOCA_CHECK(qp_post_endpoint_init_shared_connection(&eps[i], &eps[0]));

	return DOCA_SUCCESS;
}

static doca_error_t host_client_prepare_writes(const struct client_config *cfg,
					      struct qp_post_endpoint *eps,
					      struct host_client_endpoint *host_eps,
					      struct host_client_resources *host_res)
{
	uint32_t i;

	for (i = 0; i < QP_POST_TOTAL_QPS; ++i) {
		DOCA_CHECK(host_client_prepare_write(&host_eps[i],
						     &eps[i],
						     host_res->buf_inventory,
						     cfg->depth,
						     cfg->payload_size));
	}

	return DOCA_SUCCESS;
}

static doca_error_t host_client_execute_steps(const struct client_config *cfg,
						    struct qp_post_endpoint *eps,
						    struct host_client_endpoint *host_eps,
						    struct host_client_resources *host_res,
						    struct doca_dev **host_dev)
{
	uint64_t server_a_writes = 0;
	uint64_t server_b_writes = 0;
	doca_error_t result;

	DOCA_CHECK(open_doca_device_with_caps(cfg->device_name, host_client_caps, host_dev));
	DOCA_CHECK(host_client_resources_init(host_res, cfg->depth));
	DOCA_CHECK(host_client_init_endpoints(cfg, eps, *host_dev, host_res));
	DOCA_CHECK(qp_post_client_connect_servers(eps, cfg));
	DOCA_CHECK(host_client_prepare_writes(cfg, eps, host_eps, host_res));

	result = run_host_datapath(host_eps, cfg, &server_a_writes, &server_b_writes);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_AGAIN) {
		DOCA_LOG_ERR("run_host_datapath failed: %s", doca_strerror(result));
		return result;
	}
	qp_post_client_print_results(cfg, "host", server_a_writes, server_b_writes);

	return DOCA_SUCCESS;
}

doca_error_t qp_post_client_run_host(const struct client_config *cfg)
{
	struct qp_post_endpoint eps[QP_POST_TOTAL_QPS];
	struct host_client_endpoint host_eps[QP_POST_TOTAL_QPS];
	struct host_client_resources host_res;
	struct doca_dev *host_dev = NULL;
	doca_error_t result;
	doca_error_t cleanup_result;

	memset(eps, 0, sizeof(eps));
	memset(host_eps, 0, sizeof(host_eps));
	memset(&host_res, 0, sizeof(host_res));

	result = host_client_execute_steps(cfg, eps, host_eps, &host_res, &host_dev);
	cleanup_result = host_client_cleanup(eps, host_eps, &host_res, &host_dev);
	if (result == DOCA_SUCCESS)
		result = cleanup_result;

	return result;
}
