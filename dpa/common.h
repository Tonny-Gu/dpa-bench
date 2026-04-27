#ifndef DPA_LATENCY_COMMON_H
#define DPA_LATENCY_COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_rdma.h>

#define LAT_BUF_SIZE 8192U
#define LAT_PAYLOAD_SIZE 8192U
#define LAT_DEFAULT_PORT 18515U
#define LAT_POLL_NS 10000L
#define LAT_TASK_DEPTH 8U

struct descriptor_header {
	uint32_t connection_len;
	uint32_t mmap_len;
} __attribute__((packed));

struct host_endpoint {
	struct doca_dev *dev;
	struct doca_pe *pe;
	struct doca_mmap *local_mmap;
	struct doca_mmap *remote_mmap;
	struct doca_buf_inventory *buf_inventory;
	struct doca_buf *local_buf;
	struct doca_buf *remote_buf;
	struct doca_rdma *rdma;
	struct doca_ctx *ctx;
	struct doca_rdma_task_receive *recv_task;
	struct doca_rdma_task_write_imm *write_task;
	struct doca_rdma_connection *connection;
	const void *connection_desc;
	size_t connection_desc_len;
	const void *local_mmap_export;
	size_t local_mmap_export_len;
	void *remote_connection_desc;
	size_t remote_connection_desc_len;
	void *remote_mmap_export;
	size_t remote_mmap_export_len;
	char *buffer;
	void *remote_addr;
	size_t remote_len;
};

struct task_wait {
	bool done;
	doca_error_t status;
	enum doca_rdma_opcode opcode;
	uint32_t imm;
};

extern volatile sig_atomic_t g_stop;

void install_signal_handlers(void);
double get_time_us(void);
void sleep_poll_interval(void);
void reset_task_wait(struct task_wait *wait);

const char *doca_strerror(doca_error_t err);

doca_error_t open_doca_device_with_caps(const char *device_name,
					       doca_error_t (*cap_check)(const struct doca_devinfo *),
					       struct doca_dev **dev);

doca_error_t create_local_cpu_mmap(struct doca_dev *dev,
				      void *addr,
				      size_t len,
				      uint32_t permissions,
				      struct doca_mmap **mmap);

doca_error_t host_endpoint_init(struct host_endpoint *ep,
			       const char *device_name,
			       bool has_gid_index,
			       uint32_t gid_index,
			       uint32_t mmap_permissions,
			       uint32_t rdma_permissions);

doca_error_t host_endpoint_start(struct host_endpoint *ep);
doca_error_t exchange_descriptors_client(const char *server_ip,
					 uint16_t port,
					 const void *local_connection_desc,
					 size_t local_connection_desc_len,
					 const void *local_mmap_export,
					 size_t local_mmap_export_len,
					 void **remote_connection_desc,
					 size_t *remote_connection_desc_len,
					 void **remote_mmap_export,
					 size_t *remote_mmap_export_len);
doca_error_t exchange_descriptors_server(uint16_t port,
					 const void *local_connection_desc,
					 size_t local_connection_desc_len,
					 const void *local_mmap_export,
					 size_t local_mmap_export_len,
					 void **remote_connection_desc,
					 size_t *remote_connection_desc_len,
					 void **remote_mmap_export,
					 size_t *remote_mmap_export_len);
doca_error_t host_endpoint_exchange_client(struct host_endpoint *ep, const char *server_ip, uint16_t port);
doca_error_t host_endpoint_exchange_server(struct host_endpoint *ep, uint16_t port);
doca_error_t host_endpoint_connect_remote(struct host_endpoint *ep);
doca_error_t host_endpoint_post_receive(struct host_endpoint *ep, struct task_wait *wait);
doca_error_t host_endpoint_post_write_imm(struct host_endpoint *ep, struct task_wait *wait, uint32_t imm);
doca_error_t host_endpoint_wait_task(struct host_endpoint *ep, struct task_wait *wait);
doca_error_t host_endpoint_destroy(struct host_endpoint *ep);

void task_wait_receive_done(struct doca_rdma_task_receive *task,
			    union doca_data task_user_data,
			    union doca_data ctx_user_data);

void task_wait_receive_error(struct doca_rdma_task_receive *task,
			     union doca_data task_user_data,
			     union doca_data ctx_user_data);

void task_wait_write_done(struct doca_rdma_task_write_imm *task,
			  union doca_data task_user_data,
			  union doca_data ctx_user_data);

void task_wait_write_error(struct doca_rdma_task_write_imm *task,
			   union doca_data task_user_data,
			   union doca_data ctx_user_data);

#endif
