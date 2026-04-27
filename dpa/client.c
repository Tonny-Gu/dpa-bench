#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct client_config {
	const char *server_ip;
	const char *device_name;
	uint16_t port;
	bool has_gid_index;
	uint32_t gid_index;
	uint64_t iterations;
	uint32_t interval_us;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] <server_ip>\n"
		"  --device <ibdev>       DOCA IB device name\n"
		"  --port <port>          TCP exchange port (default: %u)\n"
		"  --gid-index <index>    RoCE GID index\n"
		"  --iters <count>        Number of RTT samples, 0 means forever\n"
		"  --interval-us <value>  Delay between samples (default: 1000000)\n",
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

static int parse_u64(const char *text, uint64_t *value)
{
	char *end = NULL;
	unsigned long long tmp;

	tmp = strtoull(text, &end, 0);
	if (text[0] == '\0' || *end != '\0')
		return -1;
	*value = (uint64_t)tmp;
	return 0;
}

static int parse_args(int argc, char **argv, struct client_config *cfg)
{
	static const struct option long_opts[] = {
		{"device", required_argument, NULL, 'd'},
		{"port", required_argument, NULL, 'p'},
		{"gid-index", required_argument, NULL, 'g'},
		{"iters", required_argument, NULL, 'n'},
		{"interval-us", required_argument, NULL, 'i'},
		{0, 0, 0, 0},
	};
	int opt;

	memset(cfg, 0, sizeof(*cfg));
	cfg->device_name = "";
	cfg->port = LAT_DEFAULT_PORT;
	cfg->interval_us = 1000000U;

	while ((opt = getopt_long(argc, argv, "d:p:g:n:i:", long_opts, NULL)) != -1) {
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
		case 'n':
			if (parse_u64(optarg, &cfg->iterations) != 0)
				return -1;
			break;
		case 'i':
			if (parse_u32(optarg, &cfg->interval_us) != 0)
				return -1;
			break;
		default:
			return -1;
		}
	}

	if (optind + 1 != argc)
		return -1;

	cfg->server_ip = argv[optind];
	return 0;
}

static void sleep_interval_us(uint32_t interval_us)
{
	struct timespec ts = {
		.tv_sec = interval_us / 1000000U,
		.tv_nsec = (long)(interval_us % 1000000U) * 1000L,
	};

	(void)nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
	struct client_config cfg;
	struct host_endpoint ep;
	struct task_wait recv_wait;
	struct task_wait write_wait;
	doca_error_t result;
	doca_error_t cleanup_result;
	uint32_t seq = 0;
	double t_start;
	double t_end;
	int exit_code = 1;

	if (parse_args(argc, argv, &cfg) != 0) {
		usage(argv[0]);
		return 1;
	}

	install_signal_handlers();
	memset(&ep, 0, sizeof(ep));

	result = host_endpoint_init(&ep,
				     cfg.device_name,
				     cfg.has_gid_index,
				     cfg.gid_index,
				     DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE,
				     DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "host_endpoint_init failed: %s\n", doca_strerror(result));
		goto out;
	}

	memset(ep.buffer, 'A', LAT_BUF_SIZE);

	result = host_endpoint_start(&ep);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "host_endpoint_start failed: %s\n", doca_strerror(result));
		goto out;
	}

	result = host_endpoint_exchange_client(&ep, cfg.server_ip, cfg.port);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "descriptor exchange failed: %s\n", doca_strerror(result));
		goto out;
	}

	result = host_endpoint_connect_remote(&ep);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "host_endpoint_connect_remote failed: %s\n", doca_strerror(result));
		goto out;
	}

	printf("PING %s: %u bytes\n", cfg.server_ip, LAT_PAYLOAD_SIZE);

	while (!g_stop && (cfg.iterations == 0 || seq < cfg.iterations)) {
		seq++;
		result = host_endpoint_post_receive(&ep, &recv_wait);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "post receive failed at seq=%u: %s\n", seq, doca_strerror(result));
			goto out;
		}

		t_start = get_time_us();
		result = host_endpoint_post_write_imm(&ep, &write_wait, seq);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "post write failed at seq=%u: %s\n", seq, doca_strerror(result));
			goto out;
		}

		result = host_endpoint_wait_task(&ep, &write_wait);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "write completion failed at seq=%u: %s\n", seq, doca_strerror(result));
			goto out;
		}

		result = host_endpoint_wait_task(&ep, &recv_wait);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr, "receive completion failed at seq=%u: %s\n", seq, doca_strerror(result));
			goto out;
		}
		t_end = get_time_us();

		if (recv_wait.opcode != DOCA_RDMA_OPCODE_RECV_WRITE_WITH_IMM) {
			fprintf(stderr, "unexpected opcode at seq=%u: %d\n", seq, recv_wait.opcode);
			goto out;
		}
		if (recv_wait.imm != seq) {
			fprintf(stderr, "unexpected immediate at seq=%u: got=%u\n", seq, recv_wait.imm);
			goto out;
		}

		printf("seq=%u time=%.2f us\n", seq, t_end - t_start);
		fflush(stdout);

		if (cfg.interval_us != 0 && (cfg.iterations == 0 || seq < cfg.iterations))
			sleep_interval_us(cfg.interval_us);
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
