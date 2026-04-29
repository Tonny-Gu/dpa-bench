#define _POSIX_C_SOURCE 200809L

#include "client.h"

#include <doca_log.h>

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DOCA_LOG_REGISTER(QP_POST::CLIENT);

static void usage(const char *prog)
{
	DOCA_LOG_INFO("Usage: %s [options]", prog);
	DOCA_LOG_INFO("  --mode <host|dpa>       Client datapath mode (default: host)");
	DOCA_LOG_INFO("  --device <ibdev>        RDMA device name");
	DOCA_LOG_INFO("  --pf-device <ibdev>     DPA PF device name");
	DOCA_LOG_INFO("  --rdma-device <ibdev>   RDMA device name for DPA split-device mode");
	DOCA_LOG_INFO("  --server-a-ip <addr>    First remote server IP");
	DOCA_LOG_INFO("  --server-b-ip <addr>    Second remote server IP");
	DOCA_LOG_INFO("  --port <port>           TCP exchange port (default: %u)", QP_POST_DEFAULT_PORT);
	DOCA_LOG_INFO("  --server-a-port <port>  TCP exchange port for server A");
	DOCA_LOG_INFO("  --server-b-port <port>  TCP exchange port for server B");
	DOCA_LOG_INFO("  --gid-index <index>     RoCE GID index");
	DOCA_LOG_INFO("  --sq-depth <count>      Outstanding writes per QP (default: %u, max: %u)",
		      QP_POST_DEFAULT_DEPTH,
		      QP_POST_MAX_DEPTH);
	DOCA_LOG_INFO("  --cq-depth <n>          DPA completion queue depth per QP (default: %u)",
		      QP_POST_DEFAULT_DPA_COMP_QUEUE_DEPTH);
	DOCA_LOG_INFO("  --payload-size <bytes>  RDMA write payload size, 0..%u", QP_POST_MAX_PAYLOAD);
	DOCA_LOG_INFO("  --duration <seconds>    Benchmark duration in seconds");
	DOCA_LOG_INFO("DPA mode is built with %u threads (%u QPs per thread).",
		      QP_POST_DPA_THREAD_COUNT,
		      QP_POST_DPA_QPS_PER_THREAD);
}

static int32_t parse_args(int argc, char **argv, struct client_config *cfg)
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

void qp_post_client_destroy_endpoints(struct qp_post_endpoint *eps, uint32_t num_eps)
{
	while (num_eps-- != 0)
		(void)qp_post_endpoint_destroy(&eps[num_eps]);
}

doca_error_t qp_post_client_init_endpoints(struct qp_post_endpoint *eps,
						  uint32_t num_eps,
						  struct doca_dev *rdma_dev,
						  struct doca_dpa *rdma_dpa,
						  bool has_gid_index,
						  uint32_t gid_index,
						  uint32_t depth,
						  uint32_t completion_depth,
						  size_t payload_size,
						  enum qp_post_endpoint_mode mode,
						  struct doca_pe *shared_pe,
						  struct doca_dpa_completion **thread_comps,
						  doca_dpa_dev_completion_t *thread_comp_handles)
{
	uint32_t i;

	if (mode == QP_POST_ENDPOINT_DPA_CLIENT) {
		for (i = 0; i < QP_POST_DPA_THREAD_COUNT; ++i) {
			uint32_t qp_index;
			uint32_t slot;

			DOCA_CHECK(qp_post_endpoint_init_dpa(&eps[i],
						   rdma_dev,
						   rdma_dpa,
						   has_gid_index,
						   gid_index,
						   QP_POST_MAX_PAYLOAD,
						   QP_POST_DPA_QPS_PER_THREAD,
						   depth,
						   completion_depth,
						   payload_size,
						   shared_pe,
						   thread_comps == NULL ? NULL : thread_comps[i],
						   thread_comp_handles == NULL ? 0 : thread_comp_handles[i]));

			memset(eps[i].local_buf, (int)('A' + (i % 26U)), QP_POST_MAX_PAYLOAD);

			for (slot = 1; slot < QP_POST_DPA_QPS_PER_THREAD; ++slot) {
				qp_index = i + (slot * QP_POST_DPA_THREAD_COUNT);
				DOCA_CHECK(qp_post_endpoint_init_shared_connection(&eps[qp_index], &eps[i]));
			}
		}

		return DOCA_SUCCESS;
	}

	if (mode == QP_POST_ENDPOINT_HOST_CLIENT) {
		DOCA_CHECK(qp_post_endpoint_init_host(&eps[0],
						rdma_dev,
						has_gid_index,
						gid_index,
						QP_POST_MAX_PAYLOAD,
						num_eps,
						depth,
						payload_size,
						shared_pe));

		memset(eps[0].local_buf, 'A', QP_POST_MAX_PAYLOAD);

		for (i = 1; i < num_eps; ++i)
			DOCA_CHECK(qp_post_endpoint_init_shared_connection(&eps[i], &eps[0]));

		return DOCA_SUCCESS;
	}

	for (i = 0; i < num_eps; ++i) {
		DOCA_CHECK(qp_post_endpoint_init_passive(&eps[i],
						   rdma_dev,
						   has_gid_index,
						   gid_index,
						   QP_POST_MAX_PAYLOAD,
						   1,
						   shared_pe));

		memset(eps[i].local_buf, (int)('A' + (i % 26U)), QP_POST_MAX_PAYLOAD);
	}

	return DOCA_SUCCESS;
}

static doca_error_t connect_server_slice(struct qp_post_endpoint *eps,
					 uint32_t base,
					 const char *server_ip,
					 uint16_t port)
{
	uint32_t i;

	DOCA_CHECK(qp_post_exchange_client(&eps[base], QP_POST_QPS_PER_SERVER, server_ip, port));

	for (i = 0; i < QP_POST_QPS_PER_SERVER; ++i)
		DOCA_CHECK(qp_post_endpoint_connect_remote(&eps[base + i]));

	return DOCA_SUCCESS;
}

doca_error_t qp_post_client_connect_servers(struct qp_post_endpoint *eps, const struct client_config *cfg)
{
	DOCA_CHECK(connect_server_slice(eps, 0, cfg->server_a_ip, cfg->server_a_port));
	DOCA_CHECK(connect_server_slice(eps, QP_POST_QPS_PER_SERVER, cfg->server_b_ip, cfg->server_b_port));

	return DOCA_SUCCESS;
}

void qp_post_client_print_results(const struct client_config *cfg,
					  const char *mode_name,
					  uint64_t server_a_writes,
					  uint64_t server_b_writes)
{
	uint64_t total_writes = server_a_writes + server_b_writes;
	double duration_s = (double)cfg->duration_s;

	DOCA_LOG_INFO("mode=%s payload=%u duration=%u threads=%u",
		      mode_name,
		      cfg->payload_size,
		      cfg->duration_s,
		      cfg->mode == CLIENT_MODE_DPA ? QP_POST_DPA_THREAD_COUNT : 1U);
	DOCA_LOG_INFO("sq_depth=%u", cfg->depth);
	if (cfg->mode == CLIENT_MODE_DPA)
		DOCA_LOG_INFO("cq_depth=%u", cfg->completion_depth);
	DOCA_LOG_INFO("server_a_writes=%lu", server_a_writes);
	DOCA_LOG_INFO("server_b_writes=%lu", server_b_writes);
	DOCA_LOG_INFO("total_writes=%lu", total_writes);
	DOCA_LOG_INFO("writes_per_sec=%.2f", duration_s == 0.0 ? 0.0 : (double)total_writes / duration_s);
}

static doca_error_t client_main(int argc, char **argv)
{
	struct client_config cfg;

	DOCA_CHECK(doca_log_backend_create_standard());

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(argv[0]);
		return DOCA_ERROR_INVALID_VALUE;
	}

	install_signal_handlers();
	if (cfg.mode == CLIENT_MODE_HOST)
		return qp_post_client_run_host(&cfg);

	return qp_post_client_run_dpa(&cfg);
}

int main(int argc, char **argv)
{
	return client_main(argc, argv) == DOCA_SUCCESS ? 0 : 1;
}
