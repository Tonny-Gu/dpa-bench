#define _POSIX_C_SOURCE 200809L

#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_stop = 0;

static uint64_t qp_post_cpu_to_be64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(value);
#else
	return value;
#endif
}

static uint64_t qp_post_be64_to_cpu(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(value);
#else
	return value;
#endif
}

static void set_first_error(doca_error_t *result, doca_error_t err)
{
	if (*result == DOCA_SUCCESS && err != DOCA_SUCCESS)
		*result = err;
}

static void signal_handler(int signo)
{
	(void)signo;
	g_stop = 1;
}

void install_signal_handlers(void)
{
	struct sigaction sa = {0};

	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGTERM, &sa, NULL);
}

double get_time_us(void)
{
	struct timespec ts = {0};

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

void sleep_poll_interval(void)
{
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = QP_POST_POLL_NS,
	};

	(void)nanosleep(&ts, NULL);
}

const char *doca_strerror(doca_error_t err)
{
	return doca_error_get_descr(err);
}

bool qp_post_is_power_of_two_u32(uint32_t value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

void qp_post_reset_write_wait(struct qp_post_write_wait *wait)
{
	memset(wait, 0, sizeof(*wait));
	wait->status = DOCA_SUCCESS;
}

doca_error_t open_doca_device_with_caps(const char *device_name,
				       doca_error_t (*cap_check)(const struct doca_devinfo *),
				       struct doca_dev **dev)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	const char *wanted = device_name == NULL ? "" : device_name;
	doca_error_t result;
	uint32_t i;

	*dev = NULL;
	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS)
		return result;

	for (i = 0; i < nb_devs; ++i) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS)
			continue;
		if (wanted[0] != '\0' && strncmp(wanted, ibdev_name, sizeof(ibdev_name)) != 0)
			continue;
		if (cap_check != NULL && cap_check(dev_list[i]) != DOCA_SUCCESS)
			continue;
		result = doca_dev_open(dev_list[i], dev);
		if (result == DOCA_SUCCESS)
			break;
	}

	doca_devinfo_destroy_list(dev_list);
	if (*dev == NULL)
		return DOCA_ERROR_NOT_FOUND;
	return DOCA_SUCCESS;
}

doca_error_t create_local_cpu_mmap(struct doca_dev *dev,
				      void *addr,
				      size_t len,
				      uint32_t permissions,
				      struct doca_mmap **mmap)
{
	doca_error_t result;

	*mmap = NULL;
	result = doca_mmap_create(mmap);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_mmap_set_permissions(*mmap, permissions);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_set_memrange(*mmap, addr, len);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_add_dev(*mmap, dev);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_start(*mmap);
	if (result != DOCA_SUCCESS)
		goto fail;

	return DOCA_SUCCESS;

fail:
	(void)doca_mmap_destroy(*mmap);
	*mmap = NULL;
	return result;
}

static void write_task_done(struct doca_rdma_task_write *task,
				    union doca_data task_user_data,
				    union doca_data ctx_user_data)
{
	struct qp_post_write_wait *wait = task_user_data.ptr;

	(void)task;
	(void)ctx_user_data;
	if (wait == NULL)
		return;

	wait->status = DOCA_SUCCESS;
	wait->done = true;
}

static void write_task_error(struct doca_rdma_task_write *task,
				     union doca_data task_user_data,
				     union doca_data ctx_user_data)
{
	struct qp_post_write_wait *wait = task_user_data.ptr;
	struct doca_task *base_task = doca_rdma_task_write_as_task(task);

	(void)ctx_user_data;
	if (wait == NULL)
		return;

	wait->status = doca_task_get_status(base_task);
	wait->done = true;
}

static doca_error_t wait_for_ctx_state(struct qp_post_endpoint *ep, enum doca_ctx_states wanted)
{
	enum doca_ctx_states state = DOCA_CTX_STATE_IDLE;
	doca_error_t result;

	for (;;) {
		result = doca_ctx_get_state(ep->ctx, &state);
		if (result != DOCA_SUCCESS)
			return result;
		if (state == wanted)
			return DOCA_SUCCESS;
		if (g_stop)
			return DOCA_ERROR_AGAIN;
		if (ep->pe != NULL && doca_pe_progress(ep->pe) == 0)
			sleep_poll_interval();
	}
}

static doca_error_t qp_post_endpoint_prepare_host_write(struct qp_post_endpoint *ep)
{
	union doca_data user_data = {0};
	const struct doca_buf *src_buf;
	struct doca_buf *dst_buf;
	doca_error_t result;
	uint32_t i;

	if (ep->mode != QP_POST_ENDPOINT_HOST_CLIENT)
		return DOCA_SUCCESS;

	if (ep->payload_size > ep->remote_buf_len)
		return DOCA_ERROR_INVALID_VALUE;

	ep->write_slots = calloc(ep->write_depth, sizeof(*ep->write_slots));
	if (ep->write_slots == NULL)
		return DOCA_ERROR_NO_MEMORY;

	for (i = 0; i < ep->write_depth; ++i) {
		if (ep->payload_size != 0) {
			result = doca_buf_inventory_buf_get_by_addr(ep->buf_inventory,
							    ep->local_mmap,
							    ep->local_buf,
							    ep->payload_size,
							    &ep->write_slots[i].local_doca_buf);
			if (result != DOCA_SUCCESS)
				return result;

			result = doca_buf_inventory_buf_get_by_addr(ep->buf_inventory,
							    ep->remote_mmap,
							    (void *)(uintptr_t)ep->remote_buf_addr,
							    ep->payload_size,
							    &ep->write_slots[i].remote_doca_buf);
			if (result != DOCA_SUCCESS)
				return result;
		}

		src_buf = ep->payload_size == 0 ? NULL : ep->write_slots[i].local_doca_buf;
		dst_buf = ep->payload_size == 0 ? NULL : ep->write_slots[i].remote_doca_buf;

		result = doca_rdma_task_write_allocate_init(ep->rdma,
						     ep->connection,
						     src_buf,
						     dst_buf,
						     user_data,
						     &ep->write_slots[i].write_task);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t qp_post_endpoint_init(struct qp_post_endpoint *ep,
				   struct doca_dev *rdma_dev,
				   struct doca_dpa *rdma_dpa,
				   bool has_gid_index,
				   uint32_t gid_index,
				   size_t local_buf_len,
				   uint32_t write_depth,
				   uint32_t dpa_completion_depth,
				   size_t payload_size,
				   enum qp_post_endpoint_mode mode,
				   struct doca_pe *shared_pe,
				   struct doca_dpa_completion *shared_dpa_completion,
				   doca_dpa_dev_completion_t shared_dpa_completion_handle)
{
	const uint32_t mmap_permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE;
	const uint32_t rdma_permissions = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE;
	doca_error_t result;

	memset(ep, 0, sizeof(*ep));
	ep->rdma_dev = rdma_dev;
	ep->rdma_dpa = rdma_dpa;
	ep->owns_rdma = true;
	ep->owns_local_mmap = true;
	ep->owns_local_buf = true;
	ep->local_buf_len = local_buf_len;
	ep->write_depth = write_depth;
	ep->payload_size = payload_size;
	ep->mode = mode;

	ep->local_buf = calloc(1, local_buf_len == 0 ? 1 : local_buf_len);
	if (ep->local_buf == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto fail;
	}

	result = create_local_cpu_mmap(rdma_dev, ep->local_buf, local_buf_len, mmap_permissions, &ep->local_mmap);
	if (result != DOCA_SUCCESS)
		goto fail;

	if (shared_pe != NULL) {
		ep->pe = shared_pe;
		ep->owns_pe = false;
	} else {
		result = doca_pe_create(&ep->pe);
		if (result != DOCA_SUCCESS)
			goto fail;
		ep->owns_pe = true;
	}

	result = doca_rdma_create(rdma_dev, &ep->rdma);
	if (result != DOCA_SUCCESS)
		goto fail;

	ep->ctx = doca_rdma_as_ctx(ep->rdma);
	if (ep->ctx == NULL) {
		result = DOCA_ERROR_INVALID_VALUE;
		goto fail;
	}

	result = doca_rdma_set_permissions(ep->rdma, rdma_permissions);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_rdma_set_grh_enabled(ep->rdma, 1);
	if (result != DOCA_SUCCESS)
		goto fail;

	if (has_gid_index) {
		result = doca_rdma_set_gid_index(ep->rdma, gid_index);
		if (result != DOCA_SUCCESS)
			goto fail;
	}

	result = doca_rdma_set_transport_type(ep->rdma, DOCA_RDMA_TRANSPORT_TYPE_RC);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_rdma_set_max_num_connections(ep->rdma,
					   mode == QP_POST_ENDPOINT_DPA_CLIENT ? QP_POST_DPA_QPS_PER_THREAD : 1);
	if (result != DOCA_SUCCESS)
		goto fail;

	if (mode == QP_POST_ENDPOINT_HOST_CLIENT) {
		result = doca_buf_inventory_create(2 * write_depth, &ep->buf_inventory);
		if (result != DOCA_SUCCESS)
			goto fail;
		ep->owns_buf_inventory = true;

		result = doca_buf_inventory_start(ep->buf_inventory);
		if (result != DOCA_SUCCESS)
			goto fail;

		result = doca_rdma_task_write_set_conf(ep->rdma, write_task_done, write_task_error, write_depth);
		if (result != DOCA_SUCCESS)
			goto fail;
	}

	if (mode == QP_POST_ENDPOINT_DPA_CLIENT) {
		result = doca_ctx_set_datapath_on_dpa(ep->ctx, rdma_dpa);
		if (result != DOCA_SUCCESS)
			goto fail;

		if (shared_dpa_completion != NULL) {
			ep->dpa_completion = shared_dpa_completion;
			ep->dpa_completion_handle = shared_dpa_completion_handle;
			ep->owns_dpa_completion = false;
		} else {
			result = doca_dpa_completion_create(rdma_dpa,
						   dpa_completion_depth,
						   &ep->dpa_completion);
			if (result != DOCA_SUCCESS)
				goto fail;

			result = doca_dpa_completion_start(ep->dpa_completion);
			if (result != DOCA_SUCCESS)
				goto fail;

			result = doca_dpa_completion_get_dpa_handle(ep->dpa_completion, &ep->dpa_completion_handle);
			if (result != DOCA_SUCCESS)
				goto fail;
			ep->owns_dpa_completion = true;
		}

		result = doca_rdma_dpa_completion_attach(ep->rdma, ep->dpa_completion);
		if (result != DOCA_SUCCESS)
			goto fail;
	}

	result = doca_pe_connect_ctx(ep->pe, ep->ctx);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_ctx_start(ep->ctx);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = wait_for_ctx_state(ep, DOCA_CTX_STATE_RUNNING);
	if (result != DOCA_SUCCESS)
		goto fail;

	if (mode == QP_POST_ENDPOINT_DPA_CLIENT) {
		result = doca_rdma_get_dpa_handle(ep->rdma, &ep->dpa_rdma_handle);
		if (result != DOCA_SUCCESS)
			goto fail;

		result = doca_mmap_dev_get_dpa_handle(ep->local_mmap, rdma_dev, &ep->local_mmap_handle);
		if (result != DOCA_SUCCESS)
			goto fail;
	}

	result = doca_rdma_export(ep->rdma, &ep->connection_desc, &ep->connection_desc_len, &ep->connection);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_mmap_export_rdma(ep->local_mmap,
					 rdma_dev,
					 &ep->local_mmap_export,
					 &ep->local_mmap_export_len);
	if (result != DOCA_SUCCESS)
		goto fail;

	return DOCA_SUCCESS;

fail:
	(void)qp_post_endpoint_destroy(ep);
	return result;
}

doca_error_t qp_post_endpoint_init_shared_connection(struct qp_post_endpoint *ep,
					     const struct qp_post_endpoint *shared_ep)
{
	doca_error_t result;

	if (shared_ep == NULL || shared_ep->rdma == NULL || shared_ep->ctx == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	memset(ep, 0, sizeof(*ep));
	ep->rdma_dev = shared_ep->rdma_dev;
	ep->rdma_dpa = shared_ep->rdma_dpa;
	ep->pe = shared_ep->pe;
	ep->rdma = shared_ep->rdma;
	ep->ctx = shared_ep->ctx;
	ep->dpa_completion = shared_ep->dpa_completion;
	ep->dpa_completion_handle = shared_ep->dpa_completion_handle;
	ep->dpa_rdma_handle = shared_ep->dpa_rdma_handle;
	ep->local_mmap = shared_ep->local_mmap;
	ep->local_mmap_handle = shared_ep->local_mmap_handle;
	ep->local_mmap_export = shared_ep->local_mmap_export;
	ep->local_mmap_export_len = shared_ep->local_mmap_export_len;
	ep->local_buf = shared_ep->local_buf;
	ep->local_buf_len = shared_ep->local_buf_len;
	ep->payload_size = shared_ep->payload_size;
	ep->write_depth = shared_ep->write_depth;
	ep->mode = shared_ep->mode;
	ep->owns_pe = false;
	ep->owns_rdma = false;
	ep->owns_local_mmap = false;
	ep->owns_local_buf = false;
	ep->owns_buf_inventory = false;
	ep->owns_dpa_completion = false;

	result = doca_rdma_export(ep->rdma, &ep->connection_desc, &ep->connection_desc_len, &ep->connection);
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

doca_error_t qp_post_endpoint_connect_remote(struct qp_post_endpoint *ep)
{
	doca_error_t result;

	result = doca_mmap_create_from_export(NULL,
					 ep->remote_mmap_export,
					 ep->remote_mmap_export_len,
					 ep->rdma_dev,
					 &ep->remote_mmap);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_mmap_get_memrange(ep->remote_mmap, &ep->remote_mmap_base, &ep->remote_mmap_len);
	if (result != DOCA_SUCCESS)
		return result;

	if (ep->remote_buf_len > ep->remote_mmap_len)
		return DOCA_ERROR_INVALID_VALUE;

	result = doca_rdma_connect(ep->rdma,
				   ep->remote_connection_desc,
				   ep->remote_connection_desc_len,
				   ep->connection);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_rdma_connection_get_id(ep->connection, &ep->connection_id);
	if (result != DOCA_SUCCESS)
		return result;

	if (ep->mode == QP_POST_ENDPOINT_DPA_CLIENT) {
		return doca_mmap_dev_get_dpa_handle(ep->remote_mmap, ep->rdma_dev, &ep->remote_mmap_handle);
	}

	if (ep->mode == QP_POST_ENDPOINT_HOST_CLIENT)
		return qp_post_endpoint_prepare_host_write(ep);

	return DOCA_SUCCESS;
}

doca_error_t qp_post_endpoint_post_write(struct qp_post_endpoint *ep)
{
	union doca_data user_data = {0};
	doca_error_t result;
	uint32_t i;

	if (ep->mode != QP_POST_ENDPOINT_HOST_CLIENT || ep->write_slots == NULL)
		return DOCA_ERROR_BAD_STATE;

	for (i = 0; i < ep->write_depth; ++i) {
		if (!ep->write_slots[i].inflight)
			break;
	}
	if (i == ep->write_depth)
		return DOCA_ERROR_AGAIN;

	qp_post_reset_write_wait(&ep->write_slots[i].wait);
	user_data.ptr = &ep->write_slots[i].wait;
	doca_task_set_user_data(doca_rdma_task_write_as_task(ep->write_slots[i].write_task), user_data);
	ep->write_slots[i].inflight = true;
	ep->write_outstanding++;

	result = doca_task_submit(doca_rdma_task_write_as_task(ep->write_slots[i].write_task));
	if (result != DOCA_SUCCESS) {
		ep->write_slots[i].inflight = false;
		ep->write_outstanding--;
	}

	return result;
}

doca_error_t qp_post_endpoint_poll_write(struct qp_post_endpoint *ep, uint32_t *completed_count)
{
	uint32_t i;
	doca_error_t result = DOCA_SUCCESS;

	if (completed_count != NULL)
		*completed_count = 0;

	if (ep->write_outstanding == 0)
		return DOCA_SUCCESS;

	while (doca_pe_progress(ep->pe) != 0)
		;

	for (i = 0; i < ep->write_depth; ++i) {
		if (!ep->write_slots[i].inflight || !ep->write_slots[i].wait.done)
			continue;

		ep->write_slots[i].inflight = false;
		ep->write_outstanding--;
		if (completed_count != NULL)
			(*completed_count)++;
		if (result == DOCA_SUCCESS)
			result = ep->write_slots[i].wait.status;
	}

	return result;
}

static doca_error_t send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *ptr = buf;
	size_t offset = 0;

	while (offset < len) {
		ssize_t n = send(fd, ptr + offset, len - offset, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return DOCA_ERROR_IO_FAILED;
		}
		if (n == 0)
			return DOCA_ERROR_IO_FAILED;
		offset += (size_t)n;
	}

	return DOCA_SUCCESS;
}

static doca_error_t recv_all(int fd, void *buf, size_t len)
{
	uint8_t *ptr = buf;
	size_t offset = 0;

	while (offset < len) {
		ssize_t n = recv(fd, ptr + offset, len - offset, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return DOCA_ERROR_IO_FAILED;
		}
		if (n == 0)
			return DOCA_ERROR_IO_FAILED;
		offset += (size_t)n;
	}

	return DOCA_SUCCESS;
}

static doca_error_t connect_socket(const char *server_ip, uint16_t port, int *fd)
{
	struct sockaddr_in addr = {0};
	int sock;

	*fd = -1;
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return DOCA_ERROR_IO_FAILED;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
		(void)close(sock);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		(void)close(sock);
		return DOCA_ERROR_IO_FAILED;
	}

	*fd = sock;
	return DOCA_SUCCESS;
}

static doca_error_t accept_socket(uint16_t port, int *fd)
{
	struct sockaddr_in addr = {0};
	int conn_fd;
	int listen_fd;
	int opt = 1;

	*fd = -1;
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
		return DOCA_ERROR_IO_FAILED;

	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
		(void)close(listen_fd);
		return DOCA_ERROR_IO_FAILED;
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		(void)close(listen_fd);
		return DOCA_ERROR_IO_FAILED;
	}

	if (listen(listen_fd, 1) != 0) {
		(void)close(listen_fd);
		return DOCA_ERROR_IO_FAILED;
	}

	conn_fd = accept(listen_fd, NULL, NULL);
	(void)close(listen_fd);
	if (conn_fd < 0)
		return DOCA_ERROR_IO_FAILED;

	*fd = conn_fd;
	return DOCA_SUCCESS;
}

static doca_error_t exchange_send_desc(int fd, const struct qp_post_endpoint *ep)
{
	struct qp_post_desc_header hdr;

	if (ep->connection_desc_len > UINT32_MAX || ep->local_mmap_export_len > UINT32_MAX ||
	    ep->local_buf_len > UINT32_MAX)
		return DOCA_ERROR_INVALID_VALUE;

	hdr.connection_len = htonl((uint32_t)ep->connection_desc_len);
	hdr.mmap_len = htonl((uint32_t)ep->local_mmap_export_len);
	hdr.remote_addr = qp_post_cpu_to_be64((uint64_t)(uintptr_t)ep->local_buf);
	hdr.remote_len = htonl((uint32_t)ep->local_buf_len);
	hdr.reserved = 0;

	if (send_all(fd, &hdr, sizeof(hdr)) != DOCA_SUCCESS)
		return DOCA_ERROR_IO_FAILED;
	if (send_all(fd, ep->connection_desc, ep->connection_desc_len) != DOCA_SUCCESS)
		return DOCA_ERROR_IO_FAILED;
	if (send_all(fd, ep->local_mmap_export, ep->local_mmap_export_len) != DOCA_SUCCESS)
		return DOCA_ERROR_IO_FAILED;

	return DOCA_SUCCESS;
}

static doca_error_t exchange_recv_desc(int fd, struct qp_post_endpoint *ep)
{
	struct qp_post_desc_header hdr;
	doca_error_t result;

	result = recv_all(fd, &hdr, sizeof(hdr));
	if (result != DOCA_SUCCESS)
		return result;

	ep->remote_connection_desc_len = ntohl(hdr.connection_len);
	ep->remote_mmap_export_len = ntohl(hdr.mmap_len);
	ep->remote_buf_addr = qp_post_be64_to_cpu(hdr.remote_addr);
	ep->remote_buf_len = ntohl(hdr.remote_len);
	if (ep->remote_connection_desc_len == 0 || ep->remote_mmap_export_len == 0 || ep->remote_buf_len == 0)
		return DOCA_ERROR_INVALID_VALUE;

	ep->remote_connection_desc = malloc(ep->remote_connection_desc_len);
	if (ep->remote_connection_desc == NULL)
		return DOCA_ERROR_NO_MEMORY;

	ep->remote_mmap_export = malloc(ep->remote_mmap_export_len);
	if (ep->remote_mmap_export == NULL)
		return DOCA_ERROR_NO_MEMORY;

	result = recv_all(fd, ep->remote_connection_desc, ep->remote_connection_desc_len);
	if (result != DOCA_SUCCESS)
		return result;

	return recv_all(fd, ep->remote_mmap_export, ep->remote_mmap_export_len);
}

doca_error_t qp_post_exchange_client(struct qp_post_endpoint *eps,
				    unsigned int num_eps,
				    const char *server_ip,
				    uint16_t port)
{
	int fd = -1;
	doca_error_t result;
	unsigned int i;

	result = connect_socket(server_ip, port, &fd);
	if (result != DOCA_SUCCESS)
		return result;

	for (i = 0; i < num_eps; ++i) {
		result = exchange_send_desc(fd, &eps[i]);
		if (result != DOCA_SUCCESS)
			break;
		result = exchange_recv_desc(fd, &eps[i]);
		if (result != DOCA_SUCCESS)
			break;
	}

	(void)close(fd);
	return result;
}

doca_error_t qp_post_exchange_server(struct qp_post_endpoint *eps,
				    unsigned int num_eps,
				    uint16_t port)
{
	int fd = -1;
	doca_error_t result;
	unsigned int i;

	result = accept_socket(port, &fd);
	if (result != DOCA_SUCCESS)
		return result;

	for (i = 0; i < num_eps; ++i) {
		result = exchange_recv_desc(fd, &eps[i]);
		if (result != DOCA_SUCCESS)
			break;
		result = exchange_send_desc(fd, &eps[i]);
		if (result != DOCA_SUCCESS)
			break;
	}

	(void)close(fd);
	return result;
}

doca_error_t qp_post_endpoint_destroy(struct qp_post_endpoint *ep)
{
	enum doca_ctx_states state = DOCA_CTX_STATE_IDLE;
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp;

	if (ep->connection != NULL && ep->pe != NULL) {
		tmp = doca_rdma_connection_disconnect(ep->connection);
		if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
			set_first_error(&result, tmp);
		for (int i = 0; i < 256; ++i)
			(void)doca_pe_progress(ep->pe);
	}

	if (ep->owns_rdma && ep->ctx != NULL && ep->pe != NULL) {
		tmp = doca_ctx_get_state(ep->ctx, &state);
		set_first_error(&result, tmp);
		if (tmp == DOCA_SUCCESS && state != DOCA_CTX_STATE_IDLE) {
			tmp = doca_ctx_stop(ep->ctx);
			if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
				set_first_error(&result, tmp);
			if (tmp == DOCA_SUCCESS) {
				while (doca_ctx_get_state(ep->ctx, &state) == DOCA_SUCCESS && state != DOCA_CTX_STATE_IDLE) {
					if (doca_pe_progress(ep->pe) == 0)
						sleep_poll_interval();
				}
			}
		}
	}

	if (ep->write_slots != NULL) {
		for (uint32_t i = 0; i < ep->write_depth; ++i) {
			if (ep->write_slots[i].write_task != NULL) {
				doca_task_free(doca_rdma_task_write_as_task(ep->write_slots[i].write_task));
				ep->write_slots[i].write_task = NULL;
			}

			if (ep->write_slots[i].remote_doca_buf != NULL) {
				tmp = doca_buf_dec_refcount(ep->write_slots[i].remote_doca_buf, NULL);
				set_first_error(&result, tmp);
				ep->write_slots[i].remote_doca_buf = NULL;
			}

			if (ep->write_slots[i].local_doca_buf != NULL) {
				tmp = doca_buf_dec_refcount(ep->write_slots[i].local_doca_buf, NULL);
				set_first_error(&result, tmp);
				ep->write_slots[i].local_doca_buf = NULL;
			}
		}

		free(ep->write_slots);
		ep->write_slots = NULL;
	}

	if (ep->remote_mmap != NULL) {
		tmp = doca_mmap_destroy(ep->remote_mmap);
		set_first_error(&result, tmp);
		ep->remote_mmap = NULL;
	}

	if (ep->rdma != NULL && ep->owns_rdma) {
		tmp = doca_rdma_destroy(ep->rdma);
		set_first_error(&result, tmp);
		ep->rdma = NULL;
	}

	if (ep->pe != NULL && ep->owns_pe) {
		tmp = doca_pe_destroy(ep->pe);
		set_first_error(&result, tmp);
		ep->pe = NULL;
	}

	if (ep->dpa_completion != NULL && ep->owns_dpa_completion) {
		tmp = doca_dpa_completion_stop(ep->dpa_completion);
		if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
			set_first_error(&result, tmp);
		tmp = doca_dpa_completion_destroy(ep->dpa_completion);
		set_first_error(&result, tmp);
		ep->dpa_completion = NULL;
	}

	if (ep->buf_inventory != NULL && ep->owns_buf_inventory) {
		tmp = doca_buf_inventory_stop(ep->buf_inventory);
		if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
			set_first_error(&result, tmp);
		tmp = doca_buf_inventory_destroy(ep->buf_inventory);
		set_first_error(&result, tmp);
		ep->buf_inventory = NULL;
	}

	if (ep->local_mmap != NULL && ep->owns_local_mmap) {
		tmp = doca_mmap_destroy(ep->local_mmap);
		set_first_error(&result, tmp);
		ep->local_mmap = NULL;
	}

	free(ep->remote_connection_desc);
	free(ep->remote_mmap_export);
	if (ep->owns_local_buf)
		free(ep->local_buf);

	memset(ep, 0, sizeof(*ep));
	return result;
}
