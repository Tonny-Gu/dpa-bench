#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "client_dev.h"

#include <doca_sync_event.h>

#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum client_mode {
	CLIENT_MODE_HOST = 0,
	CLIENT_MODE_DPA,
};

struct client_config {
	const char *server_a_ip;
	const char *server_b_ip;
	const char *device_name;
	const char *pf_device_name;
	const char *rdma_device_name;
	uint16_t port;
	uint16_t server_a_port;
	uint16_t server_b_port;
	bool has_gid_index;
	uint32_t gid_index;
	enum client_mode mode;
	uint32_t depth;
	uint32_t completion_depth;
	uint32_t payload_size;
	uint32_t duration_s;
};

struct dpa_client_resources {
	struct doca_dev *pf_dev;
	struct doca_dev *rdma_dev;
	struct doca_dpa *pf_dpa;
	struct doca_dpa *rdma_dpa;
	doca_dpa_dev_t rdma_dpa_handle;
	struct doca_sync_event *start_sync_event;
	struct doca_dpa_thread *threads[QP_POST_DPA_THREAD_COUNT];
	bool thread_started[QP_POST_DPA_THREAD_COUNT];
	struct doca_dpa_notification_completion *notify_comps[QP_POST_DPA_THREAD_COUNT];
	bool notify_comp_started[QP_POST_DPA_THREAD_COUNT];
	bool start_sync_event_started;
	doca_dpa_dev_notification_completion_t notify_handles[QP_POST_DPA_THREAD_COUNT];
	doca_dpa_dev_sync_event_t start_sync_event_handle;
	doca_dpa_dev_uintptr_t thread_data_dev_ptr;
	doca_dpa_dev_uintptr_t thread_args_dev_ptr;
	doca_dpa_dev_uintptr_t notify_handles_dev_ptr;
	struct qp_post_dpa_thread_data thread_data_host[QP_POST_DPA_THREAD_COUNT];
	struct qp_post_dpa_args thread_args_host[QP_POST_DPA_THREAD_COUNT];
};

extern struct doca_dpa_app *dpa_sample_app;

doca_dpa_func_t qp_post_client_kernel;
doca_dpa_func_t qp_post_notify_threads_rpc;

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --mode <host|dpa>       Client datapath mode (default: host)\n"
		"  --device <ibdev>        RDMA device name\n"
		"  --pf-device <ibdev>     DPA PF device name\n"
		"  --rdma-device <ibdev>   RDMA device name for DPA split-device mode\n"
		"  --server-a-ip <addr>    First remote server IP\n"
		"  --server-b-ip <addr>    Second remote server IP\n"
		"  --port <port>           TCP exchange port (default: %u)\n"
		"  --server-a-port <port>  TCP exchange port for server A\n"
		"  --server-b-port <port>  TCP exchange port for server B\n"
		"  --gid-index <index>     RoCE GID index\n"
		"  --sq-depth <count>      Outstanding writes per QP (default: %u, max: %u)\n"
		"  --cq-depth <n>          DPA completion queue depth per QP (default: %u)\n"
		"  --payload-size <bytes>  RDMA write payload size, 0..%u\n"
		"  --duration <seconds>    Benchmark duration in seconds\n"
		"\n"
		"DPA mode is built with %u threads (%u QPs per thread).\n",
		prog,
		QP_POST_DEFAULT_PORT,
		QP_POST_DEFAULT_DEPTH,
		QP_POST_MAX_DEPTH,
		QP_POST_DEFAULT_DPA_COMP_QUEUE_DEPTH,
		QP_POST_MAX_PAYLOAD,
		QP_POST_DPA_THREAD_COUNT,
		QP_POST_DPA_QPS_PER_THREAD);
}

static void set_first_error(doca_error_t *result, doca_error_t err)
{
	if (*result == DOCA_SUCCESS && err != DOCA_SUCCESS)
		*result = err;
}

static int parse_u16(const char *text, uint16_t *value)
{
	char *end = NULL;
	unsigned long tmp;

	tmp = strtoul(text, &end, 0);
	if (text[0] == '\0' || *end != '\0' || tmp > UINT16_MAX)
		return -1;
	*value = (uint16_t)tmp;
	return 0;
}

static int parse_u32(const char *text, uint32_t *value)
{
	char *end = NULL;
	unsigned long tmp;

	tmp = strtoul(text, &end, 0);
	if (text[0] == '\0' || *end != '\0' || tmp > UINT32_MAX)
		return -1;
	*value = (uint32_t)tmp;
	return 0;
}

static int parse_args(int argc, char **argv, struct client_config *cfg)
{
	static const struct option long_opts[] = {
		{"mode", required_argument, NULL, 'm'},
		{"device", required_argument, NULL, 'd'},
		{"pf-device", required_argument, NULL, 'f'},
		{"rdma-device", required_argument, NULL, 'r'},
		{"server-a-ip", required_argument, NULL, 'a'},
		{"server-b-ip", required_argument, NULL, 'b'},
		{"port", required_argument, NULL, 'p'},
		{"server-a-port", required_argument, NULL, 'A'},
		{"server-b-port", required_argument, NULL, 'B'},
		{"gid-index", required_argument, NULL, 'g'},
		{"sq-depth", required_argument, NULL, 'q'},
		{"cq-depth", required_argument, NULL, 'c'},
		{"payload-size", required_argument, NULL, 's'},
		{"duration", required_argument, NULL, 't'},
		{0, 0, 0, 0},
	};
	int opt;

	memset(cfg, 0, sizeof(*cfg));
	cfg->device_name = "";
	cfg->pf_device_name = "";
	cfg->rdma_device_name = "";
	cfg->port = QP_POST_DEFAULT_PORT;
	cfg->server_a_port = QP_POST_DEFAULT_PORT;
	cfg->server_b_port = QP_POST_DEFAULT_PORT;
	cfg->mode = CLIENT_MODE_HOST;
	cfg->depth = QP_POST_DEFAULT_DEPTH;
	cfg->completion_depth = QP_POST_DEFAULT_DPA_COMP_QUEUE_DEPTH;
	cfg->payload_size = QP_POST_MAX_PAYLOAD;
	cfg->duration_s = 10;

	while ((opt = getopt_long(argc, argv, "m:d:f:r:a:b:p:A:B:g:q:c:s:t:", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			if (strcmp(optarg, "host") == 0)
				cfg->mode = CLIENT_MODE_HOST;
			else if (strcmp(optarg, "dpa") == 0)
				cfg->mode = CLIENT_MODE_DPA;
			else
				return -1;
			break;
		case 'd':
			cfg->device_name = optarg;
			break;
		case 'f':
			cfg->pf_device_name = optarg;
			break;
		case 'r':
			cfg->rdma_device_name = optarg;
			break;
		case 'a':
			cfg->server_a_ip = optarg;
			break;
		case 'b':
			cfg->server_b_ip = optarg;
			break;
		case 'p':
			if (parse_u16(optarg, &cfg->port) != 0)
				return -1;
			cfg->server_a_port = cfg->port;
			cfg->server_b_port = cfg->port;
			break;
		case 'A':
			if (parse_u16(optarg, &cfg->server_a_port) != 0)
				return -1;
			break;
		case 'B':
			if (parse_u16(optarg, &cfg->server_b_port) != 0)
				return -1;
			break;
		case 'g':
			if (parse_u32(optarg, &cfg->gid_index) != 0)
				return -1;
			cfg->has_gid_index = true;
			break;
		case 'q':
			if (parse_u32(optarg, &cfg->depth) != 0 || cfg->depth == 0 || cfg->depth > QP_POST_MAX_DEPTH)
				return -1;
			break;
		case 'c':
			if (parse_u32(optarg, &cfg->completion_depth) != 0 || cfg->completion_depth == 0)
				return -1;
			break;
		case 's':
			if (parse_u32(optarg, &cfg->payload_size) != 0 || cfg->payload_size > QP_POST_MAX_PAYLOAD)
				return -1;
			break;
		case 't':
			if (parse_u32(optarg, &cfg->duration_s) != 0 || cfg->duration_s == 0)
				return -1;
			break;
		default:
			return -1;
		}
	}

	if (optind != argc)
		return -1;
	if (cfg->server_a_ip == NULL || cfg->server_b_ip == NULL)
		return -1;
	if (cfg->mode == CLIENT_MODE_HOST) {
		if (cfg->device_name[0] == '\0')
			return -1;
	} else if (cfg->completion_depth < cfg->depth) {
		return -1;
	}

	return 0;
}

static doca_error_t host_client_caps(const struct doca_devinfo *devinfo)
{
	return doca_rdma_cap_task_write_is_supported(devinfo);
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
	doca_error_t result;

#ifdef DOCA_ARCH_DPU
	if (pf_device_name[0] == '\0' || rdma_device_name[0] == '\0')
		return DOCA_ERROR_INVALID_VALUE;

	result = open_doca_device_with_caps(pf_device_name, dpa_pf_caps, &res->pf_dev);
	if (result != DOCA_SUCCESS)
		return result;

	result = open_doca_device_with_caps(rdma_device_name, dpa_rdma_caps, &res->rdma_dev);
	if (result != DOCA_SUCCESS)
		return result;
#else
	if (pf_device_name[0] == '\0')
		return DOCA_ERROR_INVALID_VALUE;
	if (rdma_device_name[0] != '\0' && strcmp(rdma_device_name, pf_device_name) != 0)
		return DOCA_ERROR_INVALID_VALUE;

	result = open_doca_device_with_caps(pf_device_name, dpa_client_caps, &res->pf_dev);
	if (result != DOCA_SUCCESS)
		return result;

	res->rdma_dev = res->pf_dev;
#endif

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_create_start_sync_event(struct dpa_client_resources *res)
{
	doca_error_t result;

	result = doca_sync_event_create(&res->start_sync_event);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sync_event_add_publisher_location_cpu(res->start_sync_event, res->rdma_dev);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sync_event_add_publisher_location_dpa(res->start_sync_event, res->rdma_dpa);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sync_event_add_subscriber_location_dpa(res->start_sync_event, res->rdma_dpa);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sync_event_start(res->start_sync_event);
	if (result != DOCA_SUCCESS)
		return result;
	res->start_sync_event_started = true;

	result = doca_sync_event_update_set(res->start_sync_event, 0);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_sync_event_get_dpa_handle(res->start_sync_event,
					      res->rdma_dpa,
					      &res->start_sync_event_handle);
}

static doca_error_t dpa_client_resources_init(struct dpa_client_resources *res, const struct client_config *cfg)
{
	doca_error_t result;

	memset(res, 0, sizeof(*res));

	result = open_dpa_client_devices(res, cfg);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_create(res->pf_dev, &res->pf_dpa);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_set_log_level(res->pf_dpa, DOCA_DPA_DEV_LOG_LEVEL_INFO);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_set_app(res->pf_dpa, dpa_sample_app);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_start(res->pf_dpa);
	if (result != DOCA_SUCCESS)
		return result;

	res->rdma_dpa = res->pf_dpa;
#ifdef DOCA_ARCH_DPU
	if (res->rdma_dev != res->pf_dev) {
		result = doca_dpa_device_extend(res->pf_dpa, res->rdma_dev, &res->rdma_dpa);
		if (result != DOCA_SUCCESS)
			return result;
	}
#endif

	result = doca_dpa_get_dpa_handle(res->rdma_dpa, &res->rdma_dpa_handle);
	if (result != DOCA_SUCCESS)
		return result;

	result = dpa_client_create_start_sync_event(res);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_mem_alloc(res->rdma_dpa, sizeof(res->thread_data_host), &res->thread_data_dev_ptr);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_mem_alloc(res->rdma_dpa,
				    sizeof(res->thread_args_host),
				    &res->thread_args_dev_ptr);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_dpa_mem_alloc(res->rdma_dpa,
				  sizeof(res->notify_handles),
				  &res->notify_handles_dev_ptr);
}

static doca_error_t dpa_client_prepare_runtime(struct dpa_client_resources *res,
					      struct qp_post_endpoint *eps,
					      uint32_t payload_size,
					      uint32_t duration_s,
					      uint32_t depth)
{
	struct qp_post_dpa_args *thread_arg;
	struct qp_post_dpa_thread_data *thread_data;
	doca_error_t result;
	unsigned int i;
	unsigned int slot;
	unsigned int qp_index;

	memset(res->thread_data_host, 0, sizeof(res->thread_data_host));

	memset(res->thread_args_host, 0, sizeof(res->thread_args_host));

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		thread_data = &res->thread_data_host[i];
		thread_arg = &res->thread_args_host[i];
		thread_arg->rdma_dpa_handle = res->rdma_dpa_handle;
		thread_arg->thread_data_dev_ptr = res->thread_data_dev_ptr + ((uint64_t)i * sizeof(res->thread_data_host[0]));
		thread_arg->start_sync_event_handle = res->start_sync_event_handle;
		thread_arg->run_duration_us = (uint64_t)duration_s * 1000000ULL;
		thread_arg->drain_timeout_us = QP_POST_DPA_DRAIN_TIMEOUT_US;
		thread_arg->thread_index = i;
		thread_arg->depth = depth;
		thread_arg->payload_size = payload_size;

		for (slot = 0; slot < QP_POST_DPA_QPS_PER_THREAD; ++slot) {
			qp_index = i + (slot * QP_POST_DPA_THREAD_COUNT);
			thread_data->qps[slot].rdma_handle = eps[qp_index].dpa_rdma_handle;
			thread_data->qps[slot].completion_handle = eps[qp_index].dpa_completion_handle;
			thread_data->qps[slot].remote_addr = eps[qp_index].remote_buf_addr;
			thread_data->qps[slot].local_addr = (uint64_t)(uintptr_t)eps[qp_index].local_buf;
			thread_data->qps[slot].remote_mmap_handle = eps[qp_index].remote_mmap_handle;
			thread_data->qps[slot].local_mmap_handle = eps[qp_index].local_mmap_handle;
		}
	}

	result = doca_dpa_h2d_memcpy(res->rdma_dpa,
				    res->thread_data_dev_ptr,
				    res->thread_data_host,
				    sizeof(res->thread_data_host));
	if (result != DOCA_SUCCESS)
		return result;

	return doca_dpa_h2d_memcpy(res->rdma_dpa,
				   res->thread_args_dev_ptr,
				   res->thread_args_host,
				   sizeof(res->thread_args_host));
}

static doca_error_t dpa_client_wait_done(struct dpa_client_resources *res, uint32_t duration_s)
{
	const double deadline_us = get_time_us() + ((double)duration_s * 1000000.0) + 5000000.0;
	double next_log_us = get_time_us() + 1000000.0;
	doca_error_t result;
	uint32_t finished_threads;
	uint32_t i;

	while (!g_stop) {
		result = doca_dpa_d2h_memcpy(res->rdma_dpa,
					&res->thread_data_host,
					res->thread_data_dev_ptr,
					sizeof(res->thread_data_host));
		if (result != DOCA_SUCCESS)
			return result;

		finished_threads = 0;
		for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
			if (res->thread_data_host[i].stats.finished != 0)
				finished_threads++;
		}

		if (finished_threads == QP_POST_DPA_THREAD_COUNT)
			return DOCA_SUCCESS;
		if (get_time_us() >= next_log_us) {
			fprintf(stderr,
				"dpa: waiting for finished threads, current=%u target=%u\n",
				finished_threads,
				QP_POST_DPA_THREAD_COUNT);
			next_log_us += 1000000.0;
		}
		if (get_time_us() >= deadline_us)
			return DOCA_ERROR_TIME_OUT;
		sleep_poll_interval();
	}

	return DOCA_ERROR_AGAIN;
}

static doca_error_t dpa_client_start_threads(struct dpa_client_resources *res)
{
	doca_error_t result;
	uint64_t rpc_retval = 0;
	uint32_t i;

	result = doca_sync_event_update_set(res->start_sync_event, 0);
	if (result != DOCA_SUCCESS)
		return result;

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		result = doca_dpa_thread_create(res->rdma_dpa, &res->threads[i]);
		if (result != DOCA_SUCCESS)
			return result;

		result = doca_dpa_thread_set_func_arg(res->threads[i],
					     &qp_post_client_kernel,
					     res->thread_args_dev_ptr + (uint64_t)i * sizeof(res->thread_args_host[0]));
		if (result != DOCA_SUCCESS)
			return result;

		result = doca_dpa_thread_start(res->threads[i]);
		if (result != DOCA_SUCCESS)
			return result;
		res->thread_started[i] = true;

		result = doca_dpa_notification_completion_create(res->rdma_dpa, res->threads[i], &res->notify_comps[i]);
		if (result != DOCA_SUCCESS)
			return result;

		result = doca_dpa_notification_completion_start(res->notify_comps[i]);
		if (result != DOCA_SUCCESS)
			return result;
		res->notify_comp_started[i] = true;

		result = doca_dpa_notification_completion_get_dpa_handle(res->notify_comps[i], &res->notify_handles[i]);
		if (result != DOCA_SUCCESS)
			return result;

		result = doca_dpa_thread_run(res->threads[i]);
		if (result != DOCA_SUCCESS)
			return result;
	}

	result = doca_dpa_h2d_memcpy(res->rdma_dpa,
				    res->notify_handles_dev_ptr,
				    res->notify_handles,
				    sizeof(res->notify_handles));
	if (result != DOCA_SUCCESS)
		return result;

	return doca_dpa_rpc(res->rdma_dpa,
			    &qp_post_notify_threads_rpc,
			    &rpc_retval,
			    (uint64_t)res->rdma_dpa_handle,
			    (uint64_t)res->notify_handles_dev_ptr,
			    (uint64_t)res->start_sync_event_handle);
}

static doca_error_t dpa_client_run(struct dpa_client_resources *res, const struct client_config *cfg)
{
	doca_error_t result;

	result = dpa_client_start_threads(res);
	if (result != DOCA_SUCCESS)
		return result;
	fprintf(stderr, "dpa: started %u independent threads\n", QP_POST_DPA_THREAD_COUNT);
	fprintf(stderr, "dpa: notify rpc kicked all threads\n");

	result = dpa_client_wait_done(res, cfg->duration_s);
	if (result != DOCA_SUCCESS)
		return result;
	fprintf(stderr, "dpa: all thread stats observed\n");

	return DOCA_SUCCESS;
}

static doca_error_t dpa_client_resources_destroy(struct dpa_client_resources *res)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp;
	uint32_t i;

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
		res->notify_comp_started[i] = false;
		res->thread_started[i] = false;
	}

	for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
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

	if (res->thread_data_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(res->rdma_dpa, res->thread_data_dev_ptr);
		set_first_error(&result, tmp);
		res->thread_data_dev_ptr = 0;
	}

	if (res->thread_args_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(res->rdma_dpa, res->thread_args_dev_ptr);
		set_first_error(&result, tmp);
		res->thread_args_dev_ptr = 0;
	}

	if (res->notify_handles_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(res->rdma_dpa, res->notify_handles_dev_ptr);
		set_first_error(&result, tmp);
		res->notify_handles_dev_ptr = 0;
	}

	if (res->start_sync_event_started) {
		tmp = doca_sync_event_stop(res->start_sync_event);
		set_first_error(&result, tmp);
		res->start_sync_event_started = false;
	}

	if (res->start_sync_event != NULL) {
		tmp = doca_sync_event_destroy(res->start_sync_event);
		set_first_error(&result, tmp);
		res->start_sync_event = NULL;
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

static void destroy_endpoints(struct qp_post_endpoint *eps, unsigned int num_eps)
{
	for (unsigned int i = 0; i < num_eps; ++i)
		(void)qp_post_endpoint_destroy(&eps[i]);
}

static doca_error_t init_endpoints(struct qp_post_endpoint *eps,
				  unsigned int num_eps,
				  struct doca_dev *rdma_dev,
				  struct doca_dpa *rdma_dpa,
				  bool has_gid_index,
				  uint32_t gid_index,
				  uint32_t depth,
				  uint32_t completion_depth,
				  size_t payload_size,
				  enum qp_post_endpoint_mode mode)
{
	doca_error_t result;
	unsigned int i;

	for (i = 0; i < num_eps; ++i) {
		result = qp_post_endpoint_init(&eps[i],
				       rdma_dev,
				       rdma_dpa,
				       has_gid_index,
				       gid_index,
				       QP_POST_MAX_PAYLOAD,
				       depth,
				       completion_depth,
				       payload_size,
				       mode);
		if (result != DOCA_SUCCESS)
			return result;

		memset(eps[i].local_buf, (int)('A' + (i % 26U)), QP_POST_MAX_PAYLOAD);
	}

	return DOCA_SUCCESS;
}

static doca_error_t connect_server_slice(struct qp_post_endpoint *eps,
					 unsigned int base,
					 const char *server_ip,
					 uint16_t port)
{
	doca_error_t result;
	unsigned int i;

	result = qp_post_exchange_client(&eps[base], QP_POST_QPS_PER_SERVER, server_ip, port);
	if (result != DOCA_SUCCESS)
		return result;

	for (i = 0; i < QP_POST_QPS_PER_SERVER; ++i) {
		result = qp_post_endpoint_connect_remote(&eps[base + i]);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

static doca_error_t run_host_client(struct qp_post_endpoint *eps,
				   const struct client_config *cfg,
				   uint64_t *server_a_writes,
				   uint64_t *server_b_writes)
{
	const double duration_us = (double)cfg->duration_s * 1000000.0;
	doca_error_t result;
	double start_us = get_time_us();
	double now_us = start_us;
	bool should_post;
	bool inflight;
	uint32_t completed_count;
	unsigned int i;

	*server_a_writes = 0;
	*server_b_writes = 0;

	while (!g_stop) {
		now_us = get_time_us();
		should_post = now_us - start_us < duration_us;
		inflight = false;

		for (i = 0; i < QP_POST_TOTAL_QPS; ++i) {
			result = qp_post_endpoint_poll_write(&eps[i], &completed_count);
			if (result != DOCA_SUCCESS)
				return result;

			if (completed_count != 0) {
				if (i < QP_POST_QPS_PER_SERVER)
					*server_a_writes += completed_count;
				else
					*server_b_writes += completed_count;
			}

			if (eps[i].write_outstanding != 0)
				inflight = true;

			while (should_post && eps[i].write_outstanding < cfg->depth) {
				result = qp_post_endpoint_post_write(&eps[i]);
				if (result != DOCA_SUCCESS)
					return result;
				inflight = true;
			}
		}

		if (!should_post && !inflight)
			break;
	}

	return g_stop ? DOCA_ERROR_AGAIN : DOCA_SUCCESS;
}

static void print_results(const struct client_config *cfg,
			 const char *mode_name,
			 uint64_t server_a_writes,
			 uint64_t server_b_writes)
{
	uint64_t total_writes = server_a_writes + server_b_writes;
	double duration_s = (double)cfg->duration_s;

	printf("mode=%s payload=%u duration=%u threads=%u\n",
	       mode_name,
	       cfg->payload_size,
	       cfg->duration_s,
	       cfg->mode == CLIENT_MODE_DPA ? QP_POST_DPA_THREAD_COUNT : 1U);
	printf("sq_depth=%u\n", cfg->depth);
	if (cfg->mode == CLIENT_MODE_DPA)
		printf("cq_depth=%u\n", cfg->completion_depth);
	printf("server_a_writes=%llu\n", (unsigned long long)server_a_writes);
	printf("server_b_writes=%llu\n", (unsigned long long)server_b_writes);
	printf("total_writes=%llu\n", (unsigned long long)total_writes);
	printf("writes_per_sec=%.2f\n", duration_s == 0.0 ? 0.0 : (double)total_writes / duration_s);
}

int main(int argc, char **argv)
{
	struct client_config cfg;
	struct qp_post_endpoint eps[QP_POST_TOTAL_QPS];
	struct dpa_client_resources dpa_res;
	struct doca_dev *host_dev = NULL;
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t cleanup_result;
	uint64_t server_a_writes = 0;
	uint64_t server_b_writes = 0;
	int exit_code = 1;

	memset(eps, 0, sizeof(eps));
	memset(&dpa_res, 0, sizeof(dpa_res));

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(argv[0]);
		return 1;
	}

	install_signal_handlers();

	if (cfg.mode == CLIENT_MODE_HOST) {
		result = open_doca_device_with_caps(cfg.device_name, host_client_caps, &host_dev);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "open_doca_device_with_caps failed: %s\n", doca_strerror(result));
			goto out;
		}

		result = init_endpoints(eps,
					QP_POST_TOTAL_QPS,
					host_dev,
					NULL,
					cfg.has_gid_index,
					cfg.gid_index,
					cfg.depth,
					0,
					cfg.payload_size,
					QP_POST_ENDPOINT_HOST_CLIENT);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "init_endpoints failed: %s\n", doca_strerror(result));
			goto out;
		}
	} else {
		result = dpa_client_resources_init(&dpa_res, &cfg);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "dpa_client_resources_init failed: %s\n", doca_strerror(result));
			goto out;
		}

		result = init_endpoints(eps,
					QP_POST_TOTAL_QPS,
					dpa_res.rdma_dev,
					dpa_res.rdma_dpa,
					cfg.has_gid_index,
					cfg.gid_index,
					cfg.depth,
					cfg.completion_depth,
					cfg.payload_size,
					QP_POST_ENDPOINT_DPA_CLIENT);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "init_endpoints failed: %s\n", doca_strerror(result));
			goto out;
		}
	}

	result = connect_server_slice(eps, 0, cfg.server_a_ip, cfg.server_a_port);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "connect_server_slice(server_a) failed: %s\n", doca_strerror(result));
		goto out;
	}

	result = connect_server_slice(eps, QP_POST_QPS_PER_SERVER, cfg.server_b_ip, cfg.server_b_port);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "connect_server_slice(server_b) failed: %s\n", doca_strerror(result));
		goto out;
	}

	if (cfg.mode == CLIENT_MODE_HOST) {
		result = run_host_client(eps, &cfg, &server_a_writes, &server_b_writes);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_AGAIN) {
			fprintf(stderr, "run_host_client failed: %s\n", doca_strerror(result));
			goto out;
		}
		print_results(&cfg, "host", server_a_writes, server_b_writes);
	} else {
		result = dpa_client_prepare_runtime(&dpa_res, eps, cfg.payload_size, cfg.duration_s, cfg.depth);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "dpa_client_prepare_runtime failed: %s\n", doca_strerror(result));
			goto out;
		}

		result = dpa_client_run(&dpa_res, &cfg);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "dpa_client_run failed: %s\n", doca_strerror(result));
			cleanup_result = doca_dpa_peek_at_last_error(dpa_res.rdma_dpa);
			if (cleanup_result != DOCA_SUCCESS)
				fprintf(stderr, "dpa runtime last error: %s\n", doca_strerror(cleanup_result));
			goto out;
		}

		for (uint32_t i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
			server_a_writes += dpa_res.thread_data_host[i].stats.server_a_writes;
			server_b_writes += dpa_res.thread_data_host[i].stats.server_b_writes;
			if (dpa_res.thread_data_host[i].stats.status != QP_POST_DPA_STATUS_OK) {
				fprintf(stderr,
					"DPA thread %u reported status=%u failed_qp=%u\n",
					i,
					dpa_res.thread_data_host[i].stats.status,
					dpa_res.thread_data_host[i].stats.failed_qp);
				goto out;
			}
		}

		print_results(&cfg, "dpa", server_a_writes, server_b_writes);
		fflush(stdout);
		fflush(stderr);
		return 0;
	}

	exit_code = 0;

out:
	destroy_endpoints(eps, QP_POST_TOTAL_QPS);
	cleanup_result = dpa_client_resources_destroy(&dpa_res);
	if (cleanup_result != DOCA_SUCCESS) {
		fprintf(stderr, "dpa_client_resources_destroy failed: %s\n", doca_strerror(cleanup_result));
		exit_code = 1;
	}
	if (host_dev != NULL) {
		cleanup_result = doca_dev_close(host_dev);
		if (cleanup_result != DOCA_SUCCESS) {
			fprintf(stderr, "doca_dev_close failed: %s\n", doca_strerror(cleanup_result));
			exit_code = 1;
		}
	}
	return exit_code;
}
