#ifndef QP_POST_CLIENT_H
#define QP_POST_CLIENT_H

#include "common.h"

#include <stdint.h>

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

doca_error_t qp_post_client_run_host(const struct client_config *cfg);
doca_error_t qp_post_client_run_dpa(const struct client_config *cfg);

void qp_post_client_destroy_endpoints(struct qp_post_endpoint *eps, uint32_t num_eps);

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
						  doca_dpa_dev_completion_t *thread_comp_handles);

doca_error_t qp_post_client_connect_servers(struct qp_post_endpoint *eps, const struct client_config *cfg);

void qp_post_client_print_results(const struct client_config *cfg,
					  const char *mode_name,
					  uint64_t server_a_writes,
					  uint64_t server_b_writes);

#endif
