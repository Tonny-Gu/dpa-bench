#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct server_config {
	const char *device_name;
	uint16_t port;
	bool has_gid_index;
	uint32_t gid_index;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --device <ibdev>      RDMA device name\n"
		"  --port <port>         TCP exchange port (default: %u)\n"
		"  --gid-index <index>   RoCE GID index\n",
		prog,
		QP_POST_DEFAULT_PORT);
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

static void destroy_endpoints(struct qp_post_endpoint *eps, unsigned int num_eps)
{
	for (unsigned int i = 0; i < num_eps; ++i)
		(void)qp_post_endpoint_destroy(&eps[i]);
}

int main(int argc, char **argv)
{
	struct server_config cfg;
	struct qp_post_endpoint eps[QP_POST_QPS_PER_SERVER];
	struct doca_dev *dev = NULL;
	doca_error_t result;
	doca_error_t cleanup_result;
	int exit_code = 1;

	memset(eps, 0, sizeof(eps));

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(argv[0]);
		return 1;
	}

	install_signal_handlers();

	result = open_doca_device_with_caps(cfg.device_name, server_caps, &dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "open_doca_device_with_caps failed: %s\n", doca_strerror(result));
		goto out;
	}

	for (unsigned int i = 0; i < QP_POST_QPS_PER_SERVER; ++i) {
		result = qp_post_endpoint_init(&eps[i],
				      dev,
				      NULL,
				      cfg.has_gid_index,
				      cfg.gid_index,
				      QP_POST_MAX_PAYLOAD,
				      1,
				      0,
				      0,
				      QP_POST_ENDPOINT_PASSIVE);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "qp_post_endpoint_init[%u] failed: %s\n", i, doca_strerror(result));
			goto out;
		}
	}

	printf("Waiting for client control connection on port %u\n", cfg.port);
	fflush(stdout);

	result = qp_post_exchange_server(eps, QP_POST_QPS_PER_SERVER, cfg.port);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "qp_post_exchange_server failed: %s\n", doca_strerror(result));
		goto out;
	}

	for (unsigned int i = 0; i < QP_POST_QPS_PER_SERVER; ++i) {
		result = qp_post_endpoint_connect_remote(&eps[i]);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "qp_post_endpoint_connect_remote[%u] failed: %s\n", i, doca_strerror(result));
			goto out;
		}
	}

	printf("Server ready: 64 QPs exported, 1KB MR per QP\n");
	fflush(stdout);

	while (!g_stop)
		sleep(1);

	exit_code = 0;

out:
	destroy_endpoints(eps, QP_POST_QPS_PER_SERVER);
	if (dev != NULL) {
		cleanup_result = doca_dev_close(dev);
		if (cleanup_result != DOCA_SUCCESS) {
			fprintf(stderr, "doca_dev_close failed: %s\n", doca_strerror(cleanup_result));
			exit_code = 1;
		}
	}
	return exit_code;
}
