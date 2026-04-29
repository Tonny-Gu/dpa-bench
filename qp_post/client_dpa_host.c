#define _POSIX_C_SOURCE 200809L

#include "client.h"
#include "client_dpa_dev.h"

#include <doca_log.h>
#include <doca_sync_event.h>

#include <string.h>

struct dpa_client_resources {
	struct doca_dev *pf_dev;
	struct doca_dev *rdma_dev;
	struct doca_dpa *pf_dpa;
	struct doca_dpa *rdma_dpa;
	doca_dpa_dev_t rdma_dpa_handle;
	struct doca_sync_event *done_sync_event;
	struct doca_dpa_completion *thread_comps[QP_POST_DPA_THREAD_COUNT];
	struct doca_dpa_thread *threads[QP_POST_DPA_THREAD_COUNT];
	bool thread_comp_started[QP_POST_DPA_THREAD_COUNT];
	bool thread_started[QP_POST_DPA_THREAD_COUNT];
	struct doca_dpa_notification_completion *notify_comps[QP_POST_DPA_THREAD_COUNT];
	bool notify_comp_started[QP_POST_DPA_THREAD_COUNT];
	bool done_sync_event_started;
	doca_dpa_dev_completion_t thread_comp_handles[QP_POST_DPA_THREAD_COUNT];
	doca_dpa_dev_notification_completion_t notify_handles[QP_POST_DPA_THREAD_COUNT];
	doca_dpa_dev_sync_event_t done_sync_event_handle;
	doca_dpa_dev_uintptr_t thread_results_dev_ptr;
	doca_dpa_dev_uintptr_t shared_state_dev_ptr;
	doca_dpa_dev_uintptr_t thread_args_dev_ptr;
	doca_dpa_dev_uintptr_t notify_handles_dev_ptr;
	struct qp_post_dpa_thread_result thread_results_host[QP_POST_DPA_THREAD_COUNT];
	struct qp_post_dpa_shared_state shared_state_host;
	struct qp_post_dpa_args thread_args_host[QP_POST_DPA_THREAD_COUNT];
};

extern struct doca_dpa_app *dpa_sample_app;

doca_dpa_func_t qp_post_client_kernel;
doca_dpa_func_t qp_post_notify_threads_rpc;

DOCA_LOG_REGISTER(QP_POST::CLIENT::DPA_HOST);

static void set_first_error(doca_error_t *result, doca_error_t err)
{
	if (*result == DOCA_SUCCESS && err != DOCA_SUCCESS)
		*result = err;
}

static doca_error_t dpa_pf_caps(const struct doca_devinfo *devinfo)
{
	return doca_dpa_cap_is_supported(devinfo);
}

static doca_error_t dpa_rdma_caps(const struct doca_devinfo *devinfo)
{
	return doca_rdma_cap_task_write_is_supported(devinfo);
}

#ifndef DOCA_ARCH_DPU
static doca_error_t dpa_client_caps(const struct doca_devinfo *devinfo)
{
	doca_error_t result;

	result = dpa_pf_caps(devinfo);
	if (result != DOCA_SUCCESS)
		return result;

	return dpa_rdma_caps(devinfo);
}
#endif

static const char *client_pf_device_name(const struct client_config *cfg)
{
	if (cfg->pf_device_name[0] != '\0')
		return cfg->pf_device_name;
	return cfg->device_name;
}

static const char *client_rdma_device_name(const struct client_config *cfg)
{
	if (cfg->rdma_device_name[0] != '\0')
		return cfg->rdma_device_name;
	return cfg->device_name;
}

static doca_error_t open_dpa_client_devices(struct dpa_client_resources *res, const struct client_config *cfg)
{
	const char *pf_device_name = client_pf_device_name(cfg);
	const char *rdma_device_name = client_rdma_device_name(cfg);

#ifdef DOCA_ARCH_DPU
	if (pf_device_name[0] == '\0' || rdma_device_name[0] == '\0')
		return DOCA_ERROR_INVALID_VALUE;

	DOCA_CHECK(open_doca_device_with_caps(pf_device_name, dpa_pf_caps, &res->pf_dev));
	DOCA_CHECK(open_doca_device_with_caps(rdma_device_name, dpa_rdma_caps, &res->rdma_dev));
#else
	if (pf_device_name[0] == '\0')
		return DOCA_ERROR_INVALID_VALUE;
	if (rdma_device_name[0] != '\0' && strcmp(rdma_device_name, pf_device_name) != 0)
		return DOCA_ERROR_INVALID_VALUE;

	DOCA_CHECK(open_doca_device_with_caps(pf_device_name, dpa_client_caps, &res->pf_dev));

	res->rdma_dev = res->pf_dev;
#endif

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_create_done_sync_event(struct dpa_client_resources *res)
{
	DOCA_CHECK(doca_sync_event_create(&res->done_sync_event));
	DOCA_CHECK(doca_sync_event_add_publisher_location_dpa(res->done_sync_event, res->rdma_dpa));
	DOCA_CHECK(doca_sync_event_add_subscriber_location_cpu(res->done_sync_event, res->rdma_dev));
	DOCA_CHECK(doca_sync_event_start(res->done_sync_event));
	res->done_sync_event_started = true;

	DOCA_CHECK(doca_sync_event_get_dpa_handle(res->done_sync_event,
					       res->rdma_dpa,
					       &res->done_sync_event_handle));

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_create_thread_completions(struct dpa_client_resources *res, uint32_t completion_depth)
{
	uint32_t i;

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		DOCA_CHECK(doca_dpa_completion_create(res->rdma_dpa, completion_depth, &res->thread_comps[i]));

		DOCA_CHECK(doca_dpa_completion_start(res->thread_comps[i]));
		res->thread_comp_started[i] = true;

		DOCA_CHECK(doca_dpa_completion_get_dpa_handle(res->thread_comps[i], &res->thread_comp_handles[i]));
	}

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_resources_init(struct dpa_client_resources *res, const struct client_config *cfg)
{
	memset(res, 0, sizeof(*res));

	DOCA_CHECK(open_dpa_client_devices(res, cfg));

	DOCA_CHECK(doca_dpa_create(res->pf_dev, &res->pf_dpa));

	DOCA_CHECK(doca_dpa_set_log_level(res->pf_dpa, DOCA_DPA_DEV_LOG_LEVEL_INFO));

	DOCA_CHECK(doca_dpa_set_app(res->pf_dpa, dpa_sample_app));

	DOCA_CHECK(doca_dpa_start(res->pf_dpa));

	res->rdma_dpa = res->pf_dpa;
#ifdef DOCA_ARCH_DPU
	if (res->rdma_dev != res->pf_dev)
		DOCA_CHECK(doca_dpa_device_extend(res->pf_dpa, res->rdma_dev, &res->rdma_dpa));
#endif

	DOCA_CHECK(doca_dpa_get_dpa_handle(res->rdma_dpa, &res->rdma_dpa_handle));

	DOCA_CHECK(dpa_client_create_done_sync_event(res));

	DOCA_CHECK(dpa_client_create_thread_completions(res, cfg->completion_depth));

	DOCA_CHECK(doca_dpa_mem_alloc(res->rdma_dpa,
				    sizeof(res->thread_results_host),
				    &res->thread_results_dev_ptr));

	DOCA_CHECK(doca_dpa_mem_alloc(res->rdma_dpa,
				    sizeof(res->shared_state_host),
				    &res->shared_state_dev_ptr));

	DOCA_CHECK(doca_dpa_mem_alloc(res->rdma_dpa,
				    sizeof(res->thread_args_host),
				    &res->thread_args_dev_ptr));

	DOCA_CHECK(doca_dpa_mem_alloc(res->rdma_dpa,
				    sizeof(res->notify_handles),
				    &res->notify_handles_dev_ptr));

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_prepare_runtime(struct dpa_client_resources *res,
					      struct qp_post_endpoint *eps,
					      uint32_t payload_size,
					      uint32_t duration_s,
					      uint32_t depth)
{
	struct qp_post_dpa_args *thread_arg;
	uint32_t i;
	uint32_t slot;
	uint32_t qp_index;

	memset(res->thread_results_host, 0, sizeof(res->thread_results_host));
	memset(&res->shared_state_host, 0, sizeof(res->shared_state_host));

	memset(res->thread_args_host, 0, sizeof(res->thread_args_host));

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		thread_arg = &res->thread_args_host[i];
		thread_arg->rdma_dpa_handle = res->rdma_dpa_handle;
		thread_arg->completion_handle = res->thread_comp_handles[i];
		thread_arg->shared_state_dev_ptr = res->shared_state_dev_ptr;
		thread_arg->thread_result_dev_ptr =
			res->thread_results_dev_ptr + ((uint64_t)i * sizeof(res->thread_results_host[0]));
		thread_arg->done_sync_event_handle = res->done_sync_event_handle;
		thread_arg->run_duration_us = (uint64_t)duration_s * 1000000ULL;
		thread_arg->drain_timeout_us = QP_POST_DPA_DRAIN_TIMEOUT_US;
		thread_arg->depth = depth;
		thread_arg->payload_size = payload_size;
		thread_arg->thread_index = i;

		for (slot = 0; slot < QP_POST_DPA_QPS_PER_THREAD; ++slot) {
			qp_index = i + (slot * QP_POST_DPA_THREAD_COUNT);
			thread_arg->qps[slot].rdma_handle = eps[qp_index].dpa_rdma_handle;
			thread_arg->qps[slot].remote_addr = eps[qp_index].remote_buf_addr;
			thread_arg->qps[slot].local_addr = (uint64_t)(uintptr_t)eps[qp_index].local_buf;
			thread_arg->qps[slot].remote_mmap_handle = eps[qp_index].remote_mmap_handle;
			thread_arg->qps[slot].local_mmap_handle = eps[qp_index].local_mmap_handle;
		}
	}

	DOCA_CHECK(doca_dpa_h2d_memcpy(res->rdma_dpa,
				    res->thread_results_dev_ptr,
				    res->thread_results_host,
				    sizeof(res->thread_results_host)));

	DOCA_CHECK(doca_dpa_h2d_memcpy(res->rdma_dpa,
				    res->shared_state_dev_ptr,
				    &res->shared_state_host,
				    sizeof(res->shared_state_host)));

	DOCA_CHECK(doca_dpa_h2d_memcpy(res->rdma_dpa,
				    res->thread_args_dev_ptr,
				    res->thread_args_host,
				    sizeof(res->thread_args_host)));

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_wait_done(struct dpa_client_resources *res, uint32_t duration_s)
{
	(void)duration_s;
	DOCA_CHECK(doca_sync_event_wait_gt(res->done_sync_event, 0, UINT64_MAX));

	DOCA_CHECK(doca_dpa_d2h_memcpy(res->rdma_dpa,
				    &res->thread_results_host,
				    res->thread_results_dev_ptr,
				    sizeof(res->thread_results_host)));

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_start_threads(struct dpa_client_resources *res)
{
	uint64_t rpc_retval = 0;
	uint32_t i;

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		DOCA_CHECK(doca_dpa_thread_create(res->rdma_dpa, &res->threads[i]));

		DOCA_CHECK(doca_dpa_thread_set_func_arg(res->threads[i],
					     &qp_post_client_kernel,
					     res->thread_args_dev_ptr + (uint64_t)i * sizeof(res->thread_args_host[0])));

		DOCA_CHECK(doca_dpa_thread_start(res->threads[i]));
		res->thread_started[i] = true;

		DOCA_CHECK(doca_dpa_notification_completion_create(res->rdma_dpa,
							       res->threads[i],
							       &res->notify_comps[i]));

		DOCA_CHECK(doca_dpa_notification_completion_start(res->notify_comps[i]));
		res->notify_comp_started[i] = true;

		DOCA_CHECK(doca_dpa_notification_completion_get_dpa_handle(res->notify_comps[i],
								       &res->notify_handles[i]));

		DOCA_CHECK(doca_dpa_thread_run(res->threads[i]));
	}

	DOCA_CHECK(doca_dpa_h2d_memcpy(res->rdma_dpa,
				    res->notify_handles_dev_ptr,
				    res->notify_handles,
				    sizeof(res->notify_handles)));

	DOCA_CHECK(doca_dpa_rpc(res->rdma_dpa,
			      &qp_post_notify_threads_rpc,
			      &rpc_retval,
			      (uint64_t)res->rdma_dpa_handle,
			      (uint64_t)res->notify_handles_dev_ptr));

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_run(struct dpa_client_resources *res, const struct client_config *cfg)
{
	DOCA_CHECK(dpa_client_start_threads(res));
	DOCA_LOG_INFO("dpa: started %u independent threads", QP_POST_DPA_THREAD_COUNT);
	DOCA_LOG_INFO("dpa: notify rpc kicked all threads");

	DOCA_CHECK(dpa_client_wait_done(res, cfg->duration_s));
	DOCA_LOG_INFO("dpa: all thread stats observed");

	return DOCA_SUCCESS;
}

static void dpa_client_stop_threads(struct dpa_client_resources *res, doca_error_t *result)
{
	doca_error_t tmp;
	uint32_t i;

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		if (res->notify_comp_started[i]) {
			tmp = doca_dpa_notification_completion_stop(res->notify_comps[i]);
			if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
				set_first_error(result, tmp);
			res->notify_comp_started[i] = false;
		}

		if (res->thread_started[i]) {
			tmp = doca_dpa_thread_stop(res->threads[i]);
			if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
				set_first_error(result, tmp);
			res->thread_started[i] = false;
		}
	}
}

static doca_error_t dpa_client_resources_destroy(struct dpa_client_resources *res)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp;
	uint32_t i;

	dpa_client_stop_threads(res, &result);

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		if (res->thread_comp_started[i]) {
			tmp = doca_dpa_completion_stop(res->thread_comps[i]);
			if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
				set_first_error(&result, tmp);
			res->thread_comp_started[i] = false;
		}

		if (res->thread_comps[i] != NULL) {
			tmp = doca_dpa_completion_destroy(res->thread_comps[i]);
			set_first_error(&result, tmp);
			res->thread_comps[i] = NULL;
		}

		if (res->notify_comps[i] != NULL) {
			tmp = doca_dpa_notification_completion_destroy(res->notify_comps[i]);
			set_first_error(&result, tmp);
			res->notify_comps[i] = NULL;
		}

		if (res->threads[i] != NULL) {
			tmp = doca_dpa_thread_destroy(res->threads[i]);
			set_first_error(&result, tmp);
			res->threads[i] = NULL;
		}
	}

	if (res->thread_results_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(res->rdma_dpa, res->thread_results_dev_ptr);
		set_first_error(&result, tmp);
		res->thread_results_dev_ptr = 0;
	}

	if (res->thread_args_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(res->rdma_dpa, res->thread_args_dev_ptr);
		set_first_error(&result, tmp);
		res->thread_args_dev_ptr = 0;
	}

	if (res->shared_state_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(res->rdma_dpa, res->shared_state_dev_ptr);
		set_first_error(&result, tmp);
		res->shared_state_dev_ptr = 0;
	}

	if (res->notify_handles_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(res->rdma_dpa, res->notify_handles_dev_ptr);
		set_first_error(&result, tmp);
		res->notify_handles_dev_ptr = 0;
	}

	if (res->done_sync_event_started) {
		tmp = doca_sync_event_stop(res->done_sync_event);
		set_first_error(&result, tmp);
		res->done_sync_event_started = false;
	}

	if (res->done_sync_event != NULL) {
		tmp = doca_sync_event_destroy(res->done_sync_event);
		set_first_error(&result, tmp);
		res->done_sync_event = NULL;
	}

	if (res->rdma_dpa != NULL && res->rdma_dpa != res->pf_dpa) {
		tmp = doca_dpa_destroy(res->rdma_dpa);
		set_first_error(&result, tmp);
		res->rdma_dpa = NULL;
	}

	if (res->pf_dpa != NULL) {
		tmp = doca_dpa_destroy(res->pf_dpa);
		set_first_error(&result, tmp);
		res->pf_dpa = NULL;
	}

	if (res->rdma_dev != NULL && res->rdma_dev != res->pf_dev) {
		tmp = doca_dev_close(res->rdma_dev);
		set_first_error(&result, tmp);
		res->rdma_dev = NULL;
	}

	if (res->pf_dev != NULL) {
		tmp = doca_dev_close(res->pf_dev);
		set_first_error(&result, tmp);
		res->pf_dev = NULL;
	}

	return result;
}

static doca_error_t dpa_client_cleanup(struct qp_post_endpoint *eps, struct dpa_client_resources *res)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t cleanup_result = DOCA_SUCCESS;

	dpa_client_stop_threads(res, &cleanup_result);
	if (cleanup_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dpa_client_stop_threads failed: %s", doca_strerror(cleanup_result));
		set_first_error(&result, cleanup_result);
	}
	qp_post_client_destroy_endpoints(eps, QP_POST_TOTAL_QPS);
	cleanup_result = dpa_client_resources_destroy(res);
	if (cleanup_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dpa_client_resources_destroy failed: %s", doca_strerror(cleanup_result));
		set_first_error(&result, cleanup_result);
	}

	return result;
}

static doca_error_t dpa_client_execute_steps(const struct client_config *cfg,
						   struct qp_post_endpoint *eps,
						   struct dpa_client_resources *res)
{
	uint64_t server_a_writes = 0;
	uint64_t server_b_writes = 0;
	doca_error_t result;
	doca_error_t dpa_result;

	DOCA_CHECK(dpa_client_resources_init(res, cfg));
	DOCA_CHECK(qp_post_client_init_endpoints(eps,
					      QP_POST_TOTAL_QPS,
					      res->rdma_dev,
					      res->rdma_dpa,
					      cfg->has_gid_index,
					      cfg->gid_index,
					      cfg->depth,
					      cfg->completion_depth,
					      cfg->payload_size,
					      QP_POST_ENDPOINT_DPA_CLIENT,
					      NULL,
					      res->thread_comps,
					      res->thread_comp_handles));
	DOCA_CHECK(qp_post_client_connect_servers(eps, cfg));
	DOCA_CHECK(dpa_client_prepare_runtime(res, eps, cfg->payload_size, cfg->duration_s, cfg->depth));

	result = dpa_client_run(res, cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dpa_client_run failed: %s", doca_strerror(result));
		dpa_result = doca_dpa_peek_at_last_error(res->rdma_dpa);
		if (dpa_result != DOCA_SUCCESS)
			DOCA_LOG_ERR("dpa runtime last error: %s", doca_strerror(dpa_result));
		return result;
	}

	for (uint32_t i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		server_a_writes += res->thread_results_host[i].server_a_writes;
		server_b_writes += res->thread_results_host[i].server_b_writes;
		if (res->thread_results_host[i].status != QP_POST_DPA_STATUS_OK) {
			DOCA_LOG_ERR("DPA thread %u reported status=%u failed_qp=%u",
				     i,
				     res->thread_results_host[i].status,
				     res->thread_results_host[i].failed_qp);
			return DOCA_ERROR_BAD_STATE;
		}
	}

	qp_post_client_print_results(cfg, "dpa", server_a_writes, server_b_writes);
	return DOCA_SUCCESS;
}

doca_error_t qp_post_client_run_dpa(const struct client_config *cfg)
{
	struct qp_post_endpoint eps[QP_POST_TOTAL_QPS];
	struct dpa_client_resources res;
	doca_error_t result;
	doca_error_t cleanup_result;

	memset(eps, 0, sizeof(eps));
	memset(&res, 0, sizeof(res));

	result = dpa_client_execute_steps(cfg, eps, &res);
	cleanup_result = dpa_client_cleanup(eps, &res);
	if (result == DOCA_SUCCESS)
		result = cleanup_result;

	return result;
}
