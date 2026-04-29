#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <doca_log.h>

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DOCA_LOG_REGISTER(QP_POST::SERVER);

struct server_config {
	const char *device_name;
	uint16_t port;
	bool has_gid_index;
	uint32_t gid_index;
};

static void usage(const char *prog)
{
	DOCA_LOG_INFO("Usage: %s [options]", prog);
	DOCA_LOG_INFO("  --device <ibdev>      RDMA device name");
	DOCA_LOG_INFO("  --port <port>         TCP exchange port (default: %u)", QP_POST_DEFAULT_PORT);
	DOCA_LOG_INFO("  --gid-index <index>   RoCE GID index");
}

static int32_t parse_args(int argc, char **argv, struct server_config *cfg)
{
	static const struct option long_opts[] = {
		{"device", required_argument, NULL, 'd'},
		{"port", required_argument, NULL, 'p'},
		{"gid-index", required_argument, NULL, 'g'},
		{0, 0, 0, 0},
	};
	int opt;

	memset(cfg, 0, sizeof(*cfg));
	cfg->device_name = "";
	cfg->port = QP_POST_DEFAULT_PORT;

	while ((opt = getopt_long(argc, argv, "d:p:g:", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			cfg->device_name = optarg;
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

	if (optind != argc || cfg->device_name[0] == '\0')
		return -1;
	return 0;
}

static doca_error_t server_caps(const struct doca_devinfo *devinfo)
{
	return doca_rdma_cap_task_write_is_supported(devinfo);
}

static void destroy_endpoints(struct qp_post_endpoint *eps, uint32_t num_eps)
{
	for (uint32_t i = 0; i < num_eps; ++i)
		(void)qp_post_endpoint_destroy(&eps[i]);
}

int main(int argc, char **argv)
{
	struct server_config cfg;
	struct qp_post_endpoint eps[QP_POST_QPS_PER_SERVER];
	struct doca_dev *dev = NULL;
	doca_error_t result;
	doca_error_t cleanup_result;
	int32_t exit_code = 1;

	memset(eps, 0, sizeof(eps));

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return 1;

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(argv[0]);
		return 1;
	}

	install_signal_handlers();

	result = open_doca_device_with_caps(cfg.device_name, server_caps, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("open_doca_device_with_caps failed: %s", doca_strerror(result));
		goto out;
	}

	for (uint32_t i = 0; i < QP_POST_QPS_PER_SERVER; ++i) {
		result = qp_post_endpoint_init(&eps[i],
				      dev,
				      NULL,
				      cfg.has_gid_index,
				      cfg.gid_index,
				      QP_POST_MAX_PAYLOAD,
				      1,
				      1,
				      0,
				      0,
				      QP_POST_ENDPOINT_PASSIVE,
				      NULL,
				      NULL,
				      0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("qp_post_endpoint_init[%u] failed: %s", i, doca_strerror(result));
			goto out;
		}
	}

	DOCA_LOG_INFO("Waiting for client control connection on port %u", cfg.port);

	result = qp_post_exchange_server(eps, QP_POST_QPS_PER_SERVER, cfg.port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("qp_post_exchange_server failed: %s", doca_strerror(result));
		goto out;
	}

	for (uint32_t i = 0; i < QP_POST_QPS_PER_SERVER; ++i) {
		result = qp_post_endpoint_connect_remote(&eps[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("qp_post_endpoint_connect_remote[%u] failed: %s", i, doca_strerror(result));
			goto out;
		}
	}

	DOCA_LOG_INFO("Server ready: 64 QPs exported, 1KB MR per QP");

	while (!g_stop)
		sleep(1);

	exit_code = 0;

out:
	destroy_endpoints(eps, QP_POST_QPS_PER_SERVER);
	if (dev != NULL) {
		cleanup_result = doca_dev_close(dev);
		if (cleanup_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("doca_dev_close failed: %s", doca_strerror(cleanup_result));
			exit_code = 1;
		}
	}
	return exit_code;
}
