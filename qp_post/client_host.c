#define _POSIX_C_SOURCE 200809L

#include "client.h"

#include <doca_log.h>

#include <string.h>

DOCA_LOG_REGISTER(QP_POST::CLIENT::HOST);

struct host_client_resources {
	struct doca_pe *shared_pe;
};

static doca_error_t host_client_caps(const struct doca_devinfo *devinfo)
{
	return doca_rdma_cap_task_write_is_supported(devinfo);
}

static doca_error_t host_client_create_shared_pe(struct host_client_resources *res)
{
	memset(res, 0, sizeof(*res));
	DOCA_CHECK(doca_pe_create(&res->shared_pe));

	return DOCA_SUCCESS;
}

static void host_client_destroy_shared_pe(struct host_client_resources *res)
{
	if (res->shared_pe != NULL) {
		(void)doca_pe_destroy(res->shared_pe);
		res->shared_pe = NULL;
	}
}

static doca_error_t run_host_datapath(struct qp_post_endpoint *eps,
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
			DOCA_CHECK(qp_post_endpoint_poll_write(&eps[i], &completed_count));

			if (completed_count != 0) {
				if (i < QP_POST_QPS_PER_SERVER)
					*server_a_writes += completed_count;
				else
					*server_b_writes += completed_count;
			}

			if (eps[i].write_outstanding != 0)
				inflight = true;

			while (should_post && eps[i].write_outstanding < cfg->depth) {
				DOCA_CHECK(qp_post_endpoint_post_write(&eps[i]));
				inflight = true;
			}
		}

		if (!should_post && !inflight)
			break;
	}

	return g_stop ? DOCA_ERROR_AGAIN : DOCA_SUCCESS;
}

static doca_error_t host_client_cleanup(struct qp_post_endpoint *eps,
					      struct host_client_resources *host_res,
					      struct doca_dev **host_dev)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t cleanup_result;

	qp_post_client_destroy_endpoints(eps, QP_POST_TOTAL_QPS);
	host_client_destroy_shared_pe(host_res);
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

static doca_error_t host_client_execute_steps(const struct client_config *cfg,
						    struct qp_post_endpoint *eps,
						    struct host_client_resources *host_res,
						    struct doca_dev **host_dev)
{
	uint64_t server_a_writes = 0;
	uint64_t server_b_writes = 0;
	doca_error_t result;

	DOCA_CHECK(open_doca_device_with_caps(cfg->device_name, host_client_caps, host_dev));
	DOCA_CHECK(host_client_create_shared_pe(host_res));
	DOCA_CHECK(qp_post_client_init_endpoints(eps,
					      QP_POST_TOTAL_QPS,
					      *host_dev,
					      NULL,
					      cfg->has_gid_index,
					      cfg->gid_index,
					      cfg->depth,
					      0,
					      cfg->payload_size,
					      QP_POST_ENDPOINT_HOST_CLIENT,
					      host_res->shared_pe,
					      NULL,
					      NULL));
	DOCA_CHECK(qp_post_client_connect_servers(eps, cfg));

	result = run_host_datapath(eps, cfg, &server_a_writes, &server_b_writes);
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
	struct host_client_resources host_res;
	struct doca_dev *host_dev = NULL;
	doca_error_t result;
	doca_error_t cleanup_result;

	memset(eps, 0, sizeof(eps));
	memset(&host_res, 0, sizeof(host_res));

	result = host_client_execute_steps(cfg, eps, &host_res, &host_dev);
	cleanup_result = host_client_cleanup(eps, &host_res, &host_dev);
	if (result == DOCA_SUCCESS)
		result = cleanup_result;

	return result;
}
