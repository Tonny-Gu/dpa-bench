#ifndef QP_POST_COMMON_H
#define QP_POST_COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#include "defs.h"

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
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

enum qp_post_endpoint_mode {
	QP_POST_ENDPOINT_PASSIVE = 0,
	QP_POST_ENDPOINT_HOST_CLIENT,
	QP_POST_ENDPOINT_DPA_CLIENT,
};

struct qp_post_write_wait {
	bool done;
	doca_error_t status;
};

struct qp_post_endpoint {
	struct doca_dev *rdma_dev;
	struct doca_dpa *rdma_dpa;
	struct doca_pe *pe;
	struct doca_rdma *rdma;
	struct doca_ctx *ctx;
	struct doca_rdma_connection *connection;
	const void *connection_desc;
	size_t connection_desc_len;
	void *remote_connection_desc;
	size_t remote_connection_desc_len;
	struct doca_mmap *local_mmap;
	const void *local_mmap_export;
	size_t local_mmap_export_len;
	struct doca_mmap *remote_mmap;
	void *remote_mmap_base;
	size_t remote_mmap_len;
	void *remote_mmap_export;
	size_t remote_mmap_export_len;
	char *local_buf;
	size_t local_buf_len;
	size_t payload_size;
	uint64_t remote_buf_addr;
	size_t remote_buf_len;
	struct doca_buf_inventory *buf_inventory;
	struct doca_buf *local_doca_buf;
	struct doca_buf *remote_doca_buf;
	struct doca_rdma_task_write *write_task;
	struct qp_post_write_wait write_wait;
	bool write_inflight;
	struct doca_dpa_completion *dpa_completion;
	doca_dpa_dev_completion_t dpa_completion_handle;
	doca_dpa_dev_rdma_t dpa_rdma_handle;
	doca_dpa_dev_mmap_t local_mmap_handle;
	doca_dpa_dev_mmap_t remote_mmap_handle;
	enum qp_post_endpoint_mode mode;
};

extern volatile sig_atomic_t g_stop;

void install_signal_handlers(void);
double get_time_us(void);
void sleep_poll_interval(void);
const char *doca_strerror(doca_error_t err);
bool qp_post_is_power_of_two_u32(uint32_t value);

doca_error_t open_doca_device_with_caps(const char *device_name,
				       doca_error_t (*cap_check)(const struct doca_devinfo *),
				       struct doca_dev **dev);

doca_error_t create_local_cpu_mmap(struct doca_dev *dev,
				      void *addr,
				      size_t len,
				      uint32_t permissions,
				      struct doca_mmap **mmap);

void qp_post_reset_write_wait(struct qp_post_write_wait *wait);

doca_error_t qp_post_endpoint_init(struct qp_post_endpoint *ep,
				   struct doca_dev *rdma_dev,
				   struct doca_dpa *rdma_dpa,
				   bool has_gid_index,
				   uint32_t gid_index,
				   size_t local_buf_len,
				   size_t payload_size,
				   enum qp_post_endpoint_mode mode);

doca_error_t qp_post_endpoint_connect_remote(struct qp_post_endpoint *ep);
doca_error_t qp_post_endpoint_post_write(struct qp_post_endpoint *ep);
doca_error_t qp_post_endpoint_poll_write(struct qp_post_endpoint *ep, bool *completed);
doca_error_t qp_post_endpoint_destroy(struct qp_post_endpoint *ep);

doca_error_t qp_post_exchange_client(struct qp_post_endpoint *eps,
				    unsigned int num_eps,
				    const char *server_ip,
				    uint16_t port);

doca_error_t qp_post_exchange_server(struct qp_post_endpoint *eps,
				    unsigned int num_eps,
				    uint16_t port);

#endif
