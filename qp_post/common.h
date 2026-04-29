#ifndef QP_POST_COMMON_H
#define QP_POST_COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#include "defs.h"

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

struct qp_post_desc_header {
	uint32_t connection_len;
	uint32_t mmap_len;
	uint64_t remote_addr;
	uint32_t remote_len;
	uint32_t reserved;
} __attribute__((packed));

struct qp_post_endpoint {
	struct doca_dev *rdma_dev;
	struct doca_pe *pe;
	bool owns_pe;
	struct doca_rdma *rdma;
	bool owns_rdma;
	struct doca_ctx *ctx;
	struct doca_rdma_connection *connection;
	uint32_t connection_id;
	const void *connection_desc;
	size_t connection_desc_len;
	void *remote_connection_desc;
	size_t remote_connection_desc_len;
	struct doca_mmap *local_mmap;
	bool owns_local_mmap;
	const void *local_mmap_export;
	size_t local_mmap_export_len;
	struct doca_mmap *remote_mmap;
	void *remote_mmap_base;
	size_t remote_mmap_len;
	void *remote_mmap_export;
	size_t remote_mmap_export_len;
	char *local_buf;
	bool owns_local_buf;
	size_t local_buf_len;
	uint64_t remote_buf_addr;
	size_t remote_buf_len;
};

extern volatile sig_atomic_t g_stop;

void install_signal_handlers(void);
double get_time_us(void);
void sleep_poll_interval(void);
const char *doca_strerror(doca_error_t err);

#define DOCA_CHECK(func) \
	do { \
		doca_error_t doca_check_result = (func); \
		if (DOCA_IS_ERROR(doca_check_result)) { \
			DOCA_LOG_ERR("%s failed: %s", #func, doca_strerror(doca_check_result)); \
			return doca_check_result; \
		} \
	} while (0)

bool qp_post_is_power_of_two_u32(uint32_t value);
int32_t parse_u16(const char *text, uint16_t *value);
int32_t parse_u32(const char *text, uint32_t *value);

doca_error_t open_doca_device_with_caps(const char *device_name,
				       doca_error_t (*cap_check)(const struct doca_devinfo *),
				       struct doca_dev **dev);

doca_error_t create_local_cpu_mmap(struct doca_dev *dev,
				      void *addr,
				      size_t len,
				      uint32_t permissions,
				      struct doca_mmap **mmap);

doca_error_t qp_post_endpoint_create(struct qp_post_endpoint *ep,
				     struct doca_dev *rdma_dev,
				     bool has_gid_index,
				     uint32_t gid_index,
				     size_t local_buf_len,
				     uint16_t max_connections,
				     struct doca_pe *shared_pe);

doca_error_t qp_post_endpoint_start(struct qp_post_endpoint *ep);

doca_error_t qp_post_endpoint_init_shared_connection(struct qp_post_endpoint *ep,
					     const struct qp_post_endpoint *shared_ep);

doca_error_t qp_post_endpoint_connect_remote(struct qp_post_endpoint *ep);
doca_error_t qp_post_endpoint_destroy(struct qp_post_endpoint *ep);

doca_error_t qp_post_exchange_client(struct qp_post_endpoint *eps,
				    uint32_t num_eps,
				    const char *server_ip,
				    uint16_t port);

doca_error_t qp_post_exchange_server(struct qp_post_endpoint *eps,
				    uint32_t num_eps,
				    uint16_t port);

#endif
