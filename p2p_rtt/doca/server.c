#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "server_dev.h"

#include <doca_dpa.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum server_mode {
	SERVER_MODE_HOST,
	SERVER_MODE_DPA,
};

struct server_config {
	const char *device_name;
	const char *pf_device_name;
	const char *rdma_device_name;
	uint16_t port;
	bool has_gid_index;
	uint32_t gid_index;
	enum server_mode mode;
};

struct dpa_server {
	struct doca_dev *pf_dev;
	struct doca_dev *rdma_dev;
	struct doca_dpa *pf_dpa;
	struct doca_dpa *rdma_dpa;
	doca_dpa_dev_t rdma_dpa_handle;
	struct doca_dpa_thread *thread;
	doca_dpa_dev_uintptr_t thread_arg_dev_ptr;
	struct doca_dpa_completion *completion;
	doca_dpa_dev_completion_t completion_handle;
	struct doca_rdma *rdma;
	struct doca_ctx *ctx;
	doca_dpa_dev_rdma_t rdma_handle;
	struct doca_rdma_connection *connection;
	const void *connection_desc;
	size_t connection_desc_len;
	void *remote_connection_desc;
	size_t remote_connection_desc_len;
	struct doca_mmap *local_mmap;
	doca_dpa_dev_mmap_t local_mmap_handle;
	const void *local_mmap_export;
	size_t local_mmap_export_len;
	struct doca_mmap *remote_mmap;
	doca_dpa_dev_mmap_t remote_mmap_handle;
	void *remote_addr;
	size_t remote_len;
	void *remote_mmap_export;
	size_t remote_mmap_export_len;
	void *local_buf;
	doca_dpa_dev_uintptr_t local_buf_addr;
};

extern struct doca_dpa_app *dpa_sample_app;

doca_dpa_func_t latency_server_kernel;
doca_dpa_func_t latency_post_initial_receive_rpc;

static doca_error_t dpa_server_destroy(struct dpa_server *server);

static void set_first_error(doca_error_t *result, doca_error_t err)
{
	if (*result == DOCA_SUCCESS && err != DOCA_SUCCESS)
		*result = err;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --mode <host|dpa>      Server datapath mode (default: host)\n"
		"  --device <ibdev>       Host device, or PF device in DPA mode\n"
		"  --pf-device <ibdev>    DPA PF device name\n"
		"  --rdma-device <ibdev>  RDMA device name (required on DPU)\n"
		"  --port <port>          TCP exchange port (default: %u)\n"
		"  --gid-index <index>    RoCE GID index\n",
		prog,
		LAT_DEFAULT_PORT);
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

static int parse_args(int argc, char **argv, struct server_config *cfg)
{
	static const struct option long_opts[] = {
		{"mode", required_argument, NULL, 'm'},
		{"device", required_argument, NULL, 'd'},
		{"pf-device", required_argument, NULL, 'f'},
		{"rdma-device", required_argument, NULL, 'r'},
		{"port", required_argument, NULL, 'p'},
		{"gid-index", required_argument, NULL, 'g'},
		{0, 0, 0, 0},
	};
	int opt;

	memset(cfg, 0, sizeof(*cfg));
	cfg->device_name = "";
	cfg->pf_device_name = "";
	cfg->rdma_device_name = "";
	cfg->port = LAT_DEFAULT_PORT;
	cfg->mode = SERVER_MODE_HOST;

	while ((opt = getopt_long(argc, argv, "m:d:f:r:p:g:", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			if (strcmp(optarg, "host") == 0)
				cfg->mode = SERVER_MODE_HOST;
			else if (strcmp(optarg, "dpa") == 0)
				cfg->mode = SERVER_MODE_DPA;
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
		case 'p':
			if (parse_u16(optarg, &cfg->port) != 0)
				return -1;
			break;
		case 'g':
			if (parse_u32(optarg, &cfg->gid_index) != 0)
				return -1;
			cfg->has_gid_index = true;
			break;
		default:
			return -1;
		}
	}

	if (cfg->mode == SERVER_MODE_DPA) {
		const char *pf_device_name = cfg->pf_device_name[0] != '\0' ? cfg->pf_device_name : cfg->device_name;

		if (cfg->rdma_device_name[0] != '\0' && pf_device_name[0] != '\0' &&
		    strcmp(cfg->rdma_device_name, pf_device_name) == 0)
			return -1;
#ifdef DOCA_ARCH_DPU
		if (cfg->rdma_device_name[0] == '\0')
			return -1;
#endif
	}

	return optind == argc ? 0 : -1;
}

static const char *server_host_device_name(const struct server_config *cfg)
{
	if (cfg->device_name[0] != '\0')
		return cfg->device_name;
	if (cfg->rdma_device_name[0] != '\0')
		return cfg->rdma_device_name;
	return cfg->pf_device_name;
}

static const char *server_pf_device_name(const struct server_config *cfg)
{
	if (cfg->pf_device_name[0] != '\0')
		return cfg->pf_device_name;
	return cfg->device_name;
}

static doca_error_t dpa_pf_caps(const struct doca_devinfo *devinfo)
{
	return doca_dpa_cap_is_supported(devinfo);
}

static doca_error_t dpa_rdma_caps(const struct doca_devinfo *devinfo)
{
	doca_error_t result;

	result = doca_rdma_cap_task_receive_is_supported(devinfo);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_rdma_cap_task_write_imm_is_supported(devinfo);
}

#ifndef DOCA_ARCH_DPU
static doca_error_t dpa_server_caps(const struct doca_devinfo *devinfo)
{
	doca_error_t result;

	result = dpa_pf_caps(devinfo);
	if (result != DOCA_SUCCESS)
		return result;

	return dpa_rdma_caps(devinfo);
}
#endif

static doca_error_t open_dpa_server_devices(struct dpa_server *server, const struct server_config *cfg)
{
	const char *pf_device_name = server_pf_device_name(cfg);
	doca_error_t result;

#ifdef DOCA_ARCH_DPU
	if (cfg->rdma_device_name[0] == '\0')
		return DOCA_ERROR_INVALID_VALUE;

	result = open_doca_device_with_caps(pf_device_name, dpa_pf_caps, &server->pf_dev);
	if (result != DOCA_SUCCESS)
		return result;

	result = open_doca_device_with_caps(cfg->rdma_device_name, dpa_rdma_caps, &server->rdma_dev);
	if (result != DOCA_SUCCESS)
		return result;
#else
	if (cfg->rdma_device_name[0] != '\0' &&
	    (pf_device_name[0] == '\0' || strcmp(cfg->rdma_device_name, pf_device_name) != 0))
		return DOCA_ERROR_INVALID_VALUE;

	result = open_doca_device_with_caps(pf_device_name, dpa_server_caps, &server->pf_dev);
	if (result != DOCA_SUCCESS)
		return result;

	server->rdma_dev = server->pf_dev;
#endif

	return DOCA_SUCCESS;
}

static doca_error_t create_local_host_mmap(struct dpa_server *server)
{
	doca_error_t result;

	result = create_local_cpu_mmap(server->rdma_dev,
					      server->local_buf,
					      LAT_BUF_SIZE,
					      DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE,
					      &server->local_mmap);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_mmap_dev_get_dpa_handle(server->local_mmap, server->rdma_dev, &server->local_mmap_handle);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_mmap_export_rdma(server->local_mmap,
					 server->rdma_dev,
					 &server->local_mmap_export,
					 &server->local_mmap_export_len);
}

static doca_error_t dpa_server_init(struct dpa_server *server, const struct server_config *cfg)
{
	#define CHECK_STAGE(name, expr) \
		do { \
			stage = (name); \
			result = (expr); \
			if (result != DOCA_SUCCESS) \
				goto fail; \
		} while (0)

	const char *stage = "init";
	doca_error_t result;

	memset(server, 0, sizeof(*server));

	CHECK_STAGE("open devices", open_dpa_server_devices(server, cfg));
	CHECK_STAGE("create pf dpa", doca_dpa_create(server->pf_dev, &server->pf_dpa));
	CHECK_STAGE("set dpa app", doca_dpa_set_app(server->pf_dpa, dpa_sample_app));
	CHECK_STAGE("start pf dpa", doca_dpa_start(server->pf_dpa));

	server->rdma_dpa = server->pf_dpa;
#ifdef DOCA_ARCH_DPU
	if (server->rdma_dev != server->pf_dev) {
		CHECK_STAGE("extend dpa to rdma device",
			   doca_dpa_device_extend(server->pf_dpa, server->rdma_dev, &server->rdma_dpa));
	}
#endif

	CHECK_STAGE("get rdma dpa handle", doca_dpa_get_dpa_handle(server->rdma_dpa, &server->rdma_dpa_handle));
	server->local_buf = calloc(1, LAT_BUF_SIZE);
	if (server->local_buf == NULL) {
		stage = "allocate host buffer";
		result = DOCA_ERROR_NO_MEMORY;
		goto fail;
	}
	server->local_buf_addr = (doca_dpa_dev_uintptr_t)(uintptr_t)server->local_buf;
	CHECK_STAGE("create local host mmap", create_local_host_mmap(server));
	CHECK_STAGE("allocate thread arg",
		   doca_dpa_mem_alloc(server->rdma_dpa, sizeof(struct latency_dpa_thread_arg), &server->thread_arg_dev_ptr));
	CHECK_STAGE("create dpa thread", doca_dpa_thread_create(server->rdma_dpa, &server->thread));
	CHECK_STAGE("set dpa thread arg",
		   doca_dpa_thread_set_func_arg(server->thread, &latency_server_kernel, server->thread_arg_dev_ptr));
	CHECK_STAGE("start dpa thread", doca_dpa_thread_start(server->thread));
	CHECK_STAGE("create dpa completion", doca_dpa_completion_create(server->rdma_dpa, LAT_TASK_DEPTH, &server->completion));
	CHECK_STAGE("attach completion thread", doca_dpa_completion_set_thread(server->completion, server->thread));
	CHECK_STAGE("start dpa completion", doca_dpa_completion_start(server->completion));
	CHECK_STAGE("get completion handle", doca_dpa_completion_get_dpa_handle(server->completion, &server->completion_handle));
	CHECK_STAGE("create rdma", doca_rdma_create(server->rdma_dev, &server->rdma));

	server->ctx = doca_rdma_as_ctx(server->rdma);
	if (server->ctx == NULL) {
		stage = "get rdma context";
		result = DOCA_ERROR_INVALID_VALUE;
		goto fail;
	}

	CHECK_STAGE("set rdma permissions",
		   doca_rdma_set_permissions(server->rdma,
					   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE));
	CHECK_STAGE("enable grh", doca_rdma_set_grh_enabled(server->rdma, 1));
	CHECK_STAGE("set datapath on dpa", doca_ctx_set_datapath_on_dpa(server->ctx, server->rdma_dpa));
	CHECK_STAGE("set recv queue size", doca_rdma_set_recv_queue_size(server->rdma, 1));
	CHECK_STAGE("set receive buf list len", doca_rdma_task_receive_set_dst_buf_list_len(server->rdma, 1));

	if (cfg->has_gid_index) {
		CHECK_STAGE("set gid index", doca_rdma_set_gid_index(server->rdma, cfg->gid_index));
	}

	CHECK_STAGE("set max connections", doca_rdma_set_max_num_connections(server->rdma, 1));
	CHECK_STAGE("attach dpa completion", doca_rdma_dpa_completion_attach(server->rdma, server->completion));
	CHECK_STAGE("start rdma context", doca_ctx_start(server->ctx));
	CHECK_STAGE("get rdma handle", doca_rdma_get_dpa_handle(server->rdma, &server->rdma_handle));

	stage = "export rdma";
	return doca_rdma_export(server->rdma,
				&server->connection_desc,
				&server->connection_desc_len,
				&server->connection);

fail:
	fprintf(stderr, "dpa_server_init[%s] failed: %s\n", stage, doca_strerror(result));
	(void)dpa_server_destroy(server);
	#undef CHECK_STAGE
	return result;
}

static doca_error_t dpa_server_connect(struct dpa_server *server, uint16_t port)
{
	struct latency_dpa_thread_arg thread_arg = {0};
	uint64_t retval = 0;
	doca_error_t result;

	result = exchange_descriptors_server(port,
					    server->connection_desc,
					    server->connection_desc_len,
					    server->local_mmap_export,
					    server->local_mmap_export_len,
					    &server->remote_connection_desc,
					    &server->remote_connection_desc_len,
					    &server->remote_mmap_export,
					    &server->remote_mmap_export_len);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_mmap_create_from_export(NULL,
					 server->remote_mmap_export,
					 server->remote_mmap_export_len,
					 server->rdma_dev,
					 &server->remote_mmap);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_mmap_dev_get_dpa_handle(server->remote_mmap, server->rdma_dev, &server->remote_mmap_handle);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_mmap_get_memrange(server->remote_mmap, &server->remote_addr, &server->remote_len);
	if (result != DOCA_SUCCESS)
		return result;
	if (server->remote_len < LAT_BUF_SIZE)
		return DOCA_ERROR_INVALID_VALUE;

	result = doca_rdma_connect(server->rdma,
				 server->remote_connection_desc,
				 server->remote_connection_desc_len,
				 server->connection);
	if (result != DOCA_SUCCESS)
		return result;

	thread_arg.rdma_dpa_handle = server->rdma_dpa_handle;
	thread_arg.completion_handle = server->completion_handle;
	thread_arg.rdma_handle = server->rdma_handle;
	thread_arg.local_buf_addr = server->local_buf_addr;
	thread_arg.remote_buf_addr = (uint64_t)(uintptr_t)server->remote_addr;
	thread_arg.local_mmap_handle = server->local_mmap_handle;
	thread_arg.remote_mmap_handle = server->remote_mmap_handle;
	thread_arg.length = LAT_PAYLOAD_SIZE;

	result = doca_dpa_h2d_memcpy(server->rdma_dpa,
				     server->thread_arg_dev_ptr,
				     &thread_arg,
				     sizeof(thread_arg));
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_thread_run(server->thread);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_dpa_rpc(server->rdma_dpa,
			   &latency_post_initial_receive_rpc,
			   &retval,
			   server->rdma_dpa_handle,
			   server->rdma_handle,
			   server->local_mmap_handle,
			   (uint64_t)server->local_buf_addr,
			   (size_t)LAT_PAYLOAD_SIZE);
}

static doca_error_t dpa_server_destroy(struct dpa_server *server)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp;
	enum doca_ctx_states state = DOCA_CTX_STATE_IDLE;

	if (server->thread != NULL) {
		tmp = doca_dpa_thread_stop(server->thread);
		set_first_error(&result, tmp);
	}

	if (server->completion != NULL) {
		tmp = doca_dpa_completion_stop(server->completion);
		set_first_error(&result, tmp);
	}

	if (server->ctx != NULL) {
		tmp = doca_ctx_get_state(server->ctx, &state);
		set_first_error(&result, tmp);
		if (tmp == DOCA_SUCCESS && state != DOCA_CTX_STATE_IDLE) {
			tmp = doca_ctx_stop(server->ctx);
			set_first_error(&result, tmp);
		}
	}

	if (server->remote_mmap != NULL) {
		tmp = doca_mmap_destroy(server->remote_mmap);
		set_first_error(&result, tmp);
	}

	if (server->rdma != NULL) {
		tmp = doca_rdma_destroy(server->rdma);
		set_first_error(&result, tmp);
	}

	if (server->completion != NULL) {
		tmp = doca_dpa_completion_destroy(server->completion);
		set_first_error(&result, tmp);
	}

	if (server->thread != NULL) {
		tmp = doca_dpa_thread_destroy(server->thread);
		set_first_error(&result, tmp);
	}

	if (server->local_mmap != NULL) {
		tmp = doca_mmap_destroy(server->local_mmap);
		set_first_error(&result, tmp);
	}

	if (server->thread_arg_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(server->rdma_dpa, server->thread_arg_dev_ptr);
		set_first_error(&result, tmp);
	}

	if (server->local_buf != NULL) {
		free(server->local_buf);
	}

	free(server->remote_connection_desc);
	free(server->remote_mmap_export);

	if (server->rdma_dpa != NULL && server->rdma_dpa != server->pf_dpa) {
		tmp = doca_dpa_destroy(server->rdma_dpa);
		set_first_error(&result, tmp);
	}

	if (server->pf_dpa != NULL) {
		tmp = doca_dpa_destroy(server->pf_dpa);
		set_first_error(&result, tmp);
	}

	if (server->rdma_dev != NULL && server->rdma_dev != server->pf_dev) {
		tmp = doca_dev_close(server->rdma_dev);
		set_first_error(&result, tmp);
	}

	if (server->pf_dev != NULL) {
		tmp = doca_dev_close(server->pf_dev);
		set_first_error(&result, tmp);
	}

	memset(server, 0, sizeof(*server));
	return result;
}

static int run_host_server(const struct server_config *cfg)
{
	struct host_endpoint ep;
	struct task_wait recv_wait;
	struct task_wait write_wait;
	doca_error_t result;
	doca_error_t cleanup_result;
	int exit_code = 1;

	memset(&ep, 0, sizeof(ep));
	result = host_endpoint_init(&ep,
				     server_host_device_name(cfg),
				     cfg->has_gid_index,
				     cfg->gid_index,
				     DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE,
				     DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "host_endpoint_init failed: %s\n", doca_strerror(result));
		goto out;
	}

	result = host_endpoint_start(&ep);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "host_endpoint_start failed: %s\n", doca_strerror(result));
		goto out;
	}

	printf("Waiting for client...\n");
	fflush(stdout);
	result = host_endpoint_exchange_server(&ep, cfg->port);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "descriptor exchange failed: %s\n", doca_strerror(result));
		goto out;
	}

	result = host_endpoint_connect_remote(&ep);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "host_endpoint_connect_remote failed: %s\n", doca_strerror(result));
		goto out;
	}

	printf("Connected. Echoing packets on CPU host...\n");
	fflush(stdout);

	while (!g_stop) {
		result = host_endpoint_post_receive(&ep, &recv_wait);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "post receive failed: %s\n", doca_strerror(result));
			goto out;
		}

		result = host_endpoint_wait_task(&ep, &recv_wait);
		if (result != DOCA_SUCCESS) {
			if (g_stop)
				break;
			fprintf(stderr, "receive completion failed: %s\n", doca_strerror(result));
			goto out;
		}

		if (recv_wait.opcode != DOCA_RDMA_OPCODE_RECV_WRITE_WITH_IMM) {
			fprintf(stderr, "unexpected opcode: %d\n", recv_wait.opcode);
			goto out;
		}

		result = host_endpoint_post_write_imm(&ep, &write_wait, recv_wait.imm);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "post write failed: %s\n", doca_strerror(result));
			goto out;
		}

		result = host_endpoint_wait_task(&ep, &write_wait);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "write completion failed: %s\n", doca_strerror(result));
			goto out;
		}
	}

	exit_code = 0;

out:
	cleanup_result = host_endpoint_destroy(&ep);
	if (cleanup_result != DOCA_SUCCESS) {
		fprintf(stderr, "host_endpoint_destroy failed: %s\n", doca_strerror(cleanup_result));
		exit_code = 1;
	}
	return exit_code;
}

static int run_dpa_server(const struct server_config *cfg)
{
	struct dpa_server server;
	doca_error_t result;
	doca_error_t cleanup_result;
	int exit_code = 1;

	memset(&server, 0, sizeof(server));
	result = dpa_server_init(&server, cfg);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "dpa_server_init failed: %s\n", doca_strerror(result));
		goto out;
	}

	printf("Waiting for client...\n");
	fflush(stdout);
	result = dpa_server_connect(&server, cfg->port);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "dpa_server_connect failed: %s\n", doca_strerror(result));
		goto out;
	}

	printf("Connected. Echoing packets on DPA...\n");
	fflush(stdout);
	while (!g_stop)
		sleep_poll_interval();

	exit_code = 0;

out:
	cleanup_result = dpa_server_destroy(&server);
	if (cleanup_result != DOCA_SUCCESS) {
		fprintf(stderr, "dpa_server_destroy failed: %s\n", doca_strerror(cleanup_result));
		exit_code = 1;
	}
	return exit_code;
}

int main(int argc, char **argv)
{
	struct server_config cfg;

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(argv[0]);
		return 1;
	}

	install_signal_handlers();
	if (cfg.mode == SERVER_MODE_DPA)
		return run_dpa_server(&cfg);
	return run_host_server(&cfg);
}
