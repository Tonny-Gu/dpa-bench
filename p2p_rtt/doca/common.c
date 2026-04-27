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
		.tv_nsec = LAT_POLL_NS,
	};

	(void)nanosleep(&ts, NULL);
}

void reset_task_wait(struct task_wait *wait)
{
	memset(wait, 0, sizeof(*wait));
	wait->status = DOCA_SUCCESS;
}

const char *doca_strerror(doca_error_t err)
{
	return doca_error_get_descr(err);
}

static doca_error_t host_rdma_caps(const struct doca_devinfo *devinfo)
{
	doca_error_t result;

	result = doca_rdma_cap_task_receive_is_supported(devinfo);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_rdma_cap_task_write_imm_is_supported(devinfo);
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

static doca_error_t exchange_send(int fd, const void *conn, size_t conn_len, const void *mmap, size_t mmap_len)
{
	struct descriptor_header hdr;

	if (conn_len > UINT32_MAX || mmap_len > UINT32_MAX)
		return DOCA_ERROR_INVALID_VALUE;

	hdr.connection_len = htonl((uint32_t)conn_len);
	hdr.mmap_len = htonl((uint32_t)mmap_len);

	if (send_all(fd, &hdr, sizeof(hdr)) != DOCA_SUCCESS)
		return DOCA_ERROR_IO_FAILED;
	if (send_all(fd, conn, conn_len) != DOCA_SUCCESS)
		return DOCA_ERROR_IO_FAILED;
	if (send_all(fd, mmap, mmap_len) != DOCA_SUCCESS)
		return DOCA_ERROR_IO_FAILED;

	return DOCA_SUCCESS;
}

static doca_error_t exchange_recv(int fd, void **conn, size_t *conn_len, void **mmap, size_t *mmap_len)
{
	struct descriptor_header hdr;
	doca_error_t result;

	*conn = NULL;
	*mmap = NULL;
	*conn_len = 0;
	*mmap_len = 0;

	result = recv_all(fd, &hdr, sizeof(hdr));
	if (result != DOCA_SUCCESS)
		return result;

	*conn_len = ntohl(hdr.connection_len);
	*mmap_len = ntohl(hdr.mmap_len);
	if (*conn_len == 0 || *mmap_len == 0)
		return DOCA_ERROR_INVALID_VALUE;

	*conn = malloc(*conn_len);
	if (*conn == NULL)
		return DOCA_ERROR_NO_MEMORY;

	*mmap = malloc(*mmap_len);
	if (*mmap == NULL) {
		free(*conn);
		*conn = NULL;
		return DOCA_ERROR_NO_MEMORY;
	}

	result = recv_all(fd, *conn, *conn_len);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = recv_all(fd, *mmap, *mmap_len);
	if (result != DOCA_SUCCESS)
		goto fail;

	return DOCA_SUCCESS;

fail:
	free(*conn);
	free(*mmap);
	*conn = NULL;
	*mmap = NULL;
	*conn_len = 0;
	*mmap_len = 0;
	return result;
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
	int listen_fd;
	int opt = 1;
	int conn_fd;

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

void task_wait_receive_done(struct doca_rdma_task_receive *task,
			    union doca_data task_user_data,
			    union doca_data ctx_user_data)
{
	struct task_wait *wait = task_user_data.ptr;

	(void)ctx_user_data;
	if (wait == NULL)
		return;

	wait->opcode = doca_rdma_task_receive_get_result_opcode(task);
	wait->imm = (uint32_t)doca_rdma_task_receive_get_result_immediate_data(task);
	wait->status = DOCA_SUCCESS;
	wait->done = true;
}

void task_wait_receive_error(struct doca_rdma_task_receive *task,
			     union doca_data task_user_data,
			     union doca_data ctx_user_data)
{
	struct task_wait *wait = task_user_data.ptr;
	struct doca_task *base_task = doca_rdma_task_receive_as_task(task);

	(void)ctx_user_data;
	if (wait == NULL)
		return;

	wait->status = doca_task_get_status(base_task);
	wait->done = true;
}

void task_wait_write_done(struct doca_rdma_task_write_imm *task,
			  union doca_data task_user_data,
			  union doca_data ctx_user_data)
{
	struct task_wait *wait = task_user_data.ptr;

	(void)ctx_user_data;
	if (wait == NULL)
		return;

	wait->status = DOCA_SUCCESS;
	wait->done = true;
}

void task_wait_write_error(struct doca_rdma_task_write_imm *task,
			   union doca_data task_user_data,
			   union doca_data ctx_user_data)
{
	struct task_wait *wait = task_user_data.ptr;
	struct doca_task *base_task = doca_rdma_task_write_imm_as_task(task);

	(void)ctx_user_data;
	if (wait == NULL)
		return;

	wait->status = doca_task_get_status(base_task);
	wait->done = true;
}

static doca_error_t host_endpoint_prepare_reusable_objects(struct host_endpoint *ep)
{
	union doca_data user_data = {0};
	doca_error_t result;

	if (ep->local_buf == NULL) {
		result = doca_buf_inventory_buf_get_by_addr(ep->buf_inventory,
						    ep->local_mmap,
						    ep->buffer,
						    LAT_BUF_SIZE,
						    &ep->local_buf);
		if (result != DOCA_SUCCESS)
			return result;
	}

	if (ep->remote_buf == NULL) {
		result = doca_buf_inventory_buf_get_by_addr(ep->buf_inventory,
						    ep->remote_mmap,
						    ep->remote_addr,
						    LAT_BUF_SIZE,
						    &ep->remote_buf);
		if (result != DOCA_SUCCESS)
			return result;
	}

	if (ep->recv_task == NULL) {
		result = doca_rdma_task_receive_allocate_init(ep->rdma, NULL, user_data, &ep->recv_task);
		if (result != DOCA_SUCCESS)
			return result;
	}

	if (ep->write_task == NULL) {
		const struct doca_buf *src_buf = LAT_PAYLOAD_SIZE == 0 ? NULL : ep->local_buf;
		struct doca_buf *dst_buf = LAT_PAYLOAD_SIZE == 0 ? NULL : ep->remote_buf;

		result = doca_rdma_task_write_imm_allocate_init(ep->rdma,
							ep->connection,
							src_buf,
							dst_buf,
							0,
							user_data,
							&ep->write_task);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t host_endpoint_init(struct host_endpoint *ep,
			       const char *device_name,
			       bool has_gid_index,
			       uint32_t gid_index,
			       uint32_t mmap_permissions,
			       uint32_t rdma_permissions)
{
	doca_error_t result;

	memset(ep, 0, sizeof(*ep));

	result = open_doca_device_with_caps(device_name, host_rdma_caps, &ep->dev);
	if (result != DOCA_SUCCESS)
		goto fail;

	ep->buffer = calloc(1, LAT_BUF_SIZE);
	if (ep->buffer == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto fail;
	}

	result = create_local_cpu_mmap(ep->dev, ep->buffer, LAT_BUF_SIZE, mmap_permissions, &ep->local_mmap);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_buf_inventory_create(LAT_TASK_DEPTH * 2, &ep->buf_inventory);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_buf_inventory_start(ep->buf_inventory);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_pe_create(&ep->pe);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_rdma_create(ep->dev, &ep->rdma);
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

	result = doca_rdma_set_max_num_connections(ep->rdma, 1);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_rdma_task_receive_set_conf(ep->rdma,
						 task_wait_receive_done,
						 task_wait_receive_error,
						 LAT_TASK_DEPTH);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_rdma_task_receive_set_dst_buf_list_len(ep->rdma, 1);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_rdma_task_write_imm_set_conf(ep->rdma,
						   task_wait_write_done,
						   task_wait_write_error,
						   LAT_TASK_DEPTH);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_pe_connect_ctx(ep->pe, ep->ctx);
	if (result != DOCA_SUCCESS)
		goto fail;

	return DOCA_SUCCESS;

fail:
	(void)host_endpoint_destroy(ep);
	return result;
}

static doca_error_t wait_for_ctx_state(struct host_endpoint *ep, enum doca_ctx_states wanted)
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
		if (doca_pe_progress(ep->pe) == 0)
			sleep_poll_interval();
	}
}

doca_error_t host_endpoint_start(struct host_endpoint *ep)
{
	doca_error_t result;

	result = doca_ctx_start(ep->ctx);
	if (result != DOCA_SUCCESS)
		return result;

	result = wait_for_ctx_state(ep, DOCA_CTX_STATE_RUNNING);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_rdma_export(ep->rdma, &ep->connection_desc, &ep->connection_desc_len, &ep->connection);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_mmap_export_rdma(ep->local_mmap,
					 ep->dev,
					 &ep->local_mmap_export,
					 &ep->local_mmap_export_len);
}

doca_error_t exchange_descriptors_client(const char *server_ip,
					 uint16_t port,
					 const void *local_connection_desc,
					 size_t local_connection_desc_len,
					 const void *local_mmap_export,
					 size_t local_mmap_export_len,
					 void **remote_connection_desc,
					 size_t *remote_connection_desc_len,
					 void **remote_mmap_export,
					 size_t *remote_mmap_export_len)
{
	int fd = -1;
	doca_error_t result;

	result = connect_socket(server_ip, port, &fd);
	if (result != DOCA_SUCCESS)
		return result;

	result = exchange_send(fd,
			       local_connection_desc,
			       local_connection_desc_len,
			       local_mmap_export,
			       local_mmap_export_len);
	if (result == DOCA_SUCCESS) {
		result = exchange_recv(fd,
				       remote_connection_desc,
				       remote_connection_desc_len,
				       remote_mmap_export,
				       remote_mmap_export_len);
	}

	(void)close(fd);
	return result;
}

doca_error_t exchange_descriptors_server(uint16_t port,
					 const void *local_connection_desc,
					 size_t local_connection_desc_len,
					 const void *local_mmap_export,
					 size_t local_mmap_export_len,
					 void **remote_connection_desc,
					 size_t *remote_connection_desc_len,
					 void **remote_mmap_export,
					 size_t *remote_mmap_export_len)
{
	int fd = -1;
	doca_error_t result;

	result = accept_socket(port, &fd);
	if (result != DOCA_SUCCESS)
		return result;

	result = exchange_recv(fd,
			       remote_connection_desc,
			       remote_connection_desc_len,
			       remote_mmap_export,
			       remote_mmap_export_len);
	if (result == DOCA_SUCCESS) {
		result = exchange_send(fd,
				       local_connection_desc,
				       local_connection_desc_len,
				       local_mmap_export,
				       local_mmap_export_len);
	}

	(void)close(fd);
	return result;
}

doca_error_t host_endpoint_exchange_client(struct host_endpoint *ep, const char *server_ip, uint16_t port)
{
	return exchange_descriptors_client(server_ip,
					  port,
					  ep->connection_desc,
					  ep->connection_desc_len,
					  ep->local_mmap_export,
					  ep->local_mmap_export_len,
					  &ep->remote_connection_desc,
					  &ep->remote_connection_desc_len,
					  &ep->remote_mmap_export,
					  &ep->remote_mmap_export_len);
}

doca_error_t host_endpoint_exchange_server(struct host_endpoint *ep, uint16_t port)
{
	return exchange_descriptors_server(port,
					  ep->connection_desc,
					  ep->connection_desc_len,
					  ep->local_mmap_export,
					  ep->local_mmap_export_len,
					  &ep->remote_connection_desc,
					  &ep->remote_connection_desc_len,
					  &ep->remote_mmap_export,
					  &ep->remote_mmap_export_len);
}

doca_error_t host_endpoint_connect_remote(struct host_endpoint *ep)
{
	doca_error_t result;

	result = doca_mmap_create_from_export(NULL,
					 ep->remote_mmap_export,
					 ep->remote_mmap_export_len,
					 ep->dev,
					 &ep->remote_mmap);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_mmap_get_memrange(ep->remote_mmap, &ep->remote_addr, &ep->remote_len);
	if (result != DOCA_SUCCESS)
		return result;
	if (ep->remote_len < LAT_BUF_SIZE)
		return DOCA_ERROR_INVALID_VALUE;

	result = doca_rdma_connect(ep->rdma,
				   ep->remote_connection_desc,
				   ep->remote_connection_desc_len,
				   ep->connection);
	if (result != DOCA_SUCCESS)
		return result;

	return host_endpoint_prepare_reusable_objects(ep);
}

doca_error_t host_endpoint_post_receive(struct host_endpoint *ep, struct task_wait *wait)
{
	union doca_data user_data = {0};

	if (ep->recv_task == NULL)
		return DOCA_ERROR_BAD_STATE;

	reset_task_wait(wait);
	user_data.ptr = wait;
	doca_task_set_user_data(doca_rdma_task_receive_as_task(ep->recv_task), user_data);

	return doca_task_submit(doca_rdma_task_receive_as_task(ep->recv_task));
}

doca_error_t host_endpoint_post_write_imm(struct host_endpoint *ep, struct task_wait *wait, uint32_t imm)
{
	union doca_data user_data = {0};

	if (ep->write_task == NULL)
		return DOCA_ERROR_BAD_STATE;

	reset_task_wait(wait);
	user_data.ptr = wait;
	doca_task_set_user_data(doca_rdma_task_write_imm_as_task(ep->write_task), user_data);
	doca_rdma_task_write_imm_set_immediate_data(ep->write_task, imm);

	return doca_task_submit(doca_rdma_task_write_imm_as_task(ep->write_task));
}

doca_error_t host_endpoint_wait_task(struct host_endpoint *ep, struct task_wait *wait)
{
	while (!wait->done && !g_stop) {
		(void)doca_pe_progress(ep->pe);
	}

	if (!wait->done)
		return DOCA_ERROR_AGAIN;
	return wait->status;
}

doca_error_t host_endpoint_destroy(struct host_endpoint *ep)
{
	enum doca_ctx_states state = DOCA_CTX_STATE_IDLE;
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp;

	if (ep->ctx != NULL && ep->pe != NULL) {
		tmp = doca_ctx_get_state(ep->ctx, &state);
		set_first_error(&result, tmp);
		if (tmp == DOCA_SUCCESS && state != DOCA_CTX_STATE_IDLE) {
			if (state == DOCA_CTX_STATE_RUNNING && ep->connection != NULL) {
				tmp = doca_rdma_connection_disconnect(ep->connection);
				if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
					set_first_error(&result, tmp);
				for (int i = 0; i < 1024; ++i)
					(void)doca_pe_progress(ep->pe);
			}

			tmp = doca_ctx_stop(ep->ctx);
			set_first_error(&result, tmp);
			if (tmp == DOCA_SUCCESS) {
				while (doca_ctx_get_state(ep->ctx, &state) == DOCA_SUCCESS && state != DOCA_CTX_STATE_IDLE) {
					if (doca_pe_progress(ep->pe) == 0)
						sleep_poll_interval();
				}
			} else {
				set_first_error(&result, tmp);
			}
		}
	}

	if (ep->recv_task != NULL) {
		tmp = doca_task_get_status(doca_rdma_task_receive_as_task(ep->recv_task));
		if (tmp == DOCA_SUCCESS || tmp == DOCA_ERROR_BAD_STATE || tmp == DOCA_ERROR_IN_PROGRESS) {
			doca_task_free(doca_rdma_task_receive_as_task(ep->recv_task));
			ep->recv_task = NULL;
		}
	}

	if (ep->write_task != NULL) {
		tmp = doca_task_get_status(doca_rdma_task_write_imm_as_task(ep->write_task));
		if (tmp == DOCA_SUCCESS || tmp == DOCA_ERROR_BAD_STATE || tmp == DOCA_ERROR_IN_PROGRESS) {
			doca_task_free(doca_rdma_task_write_imm_as_task(ep->write_task));
			ep->write_task = NULL;
		}
	}

	if (ep->remote_buf != NULL) {
		tmp = doca_buf_dec_refcount(ep->remote_buf, NULL);
		set_first_error(&result, tmp);
		ep->remote_buf = NULL;
	}

	if (ep->local_buf != NULL) {
		tmp = doca_buf_dec_refcount(ep->local_buf, NULL);
		set_first_error(&result, tmp);
		ep->local_buf = NULL;
	}

	if (ep->remote_mmap != NULL) {
		tmp = doca_mmap_destroy(ep->remote_mmap);
		set_first_error(&result, tmp);
	}

	if (ep->rdma != NULL) {
		tmp = doca_rdma_destroy(ep->rdma);
		set_first_error(&result, tmp);
	}

	if (ep->pe != NULL) {
		tmp = doca_pe_destroy(ep->pe);
		set_first_error(&result, tmp);
	}

	if (ep->buf_inventory != NULL) {
		tmp = doca_buf_inventory_stop(ep->buf_inventory);
		if (tmp != DOCA_SUCCESS && tmp != DOCA_ERROR_BAD_STATE)
			set_first_error(&result, tmp);
		tmp = doca_buf_inventory_destroy(ep->buf_inventory);
		set_first_error(&result, tmp);
	}

	if (ep->local_mmap != NULL) {
		tmp = doca_mmap_destroy(ep->local_mmap);
		set_first_error(&result, tmp);
	}

	free(ep->remote_connection_desc);
	free(ep->remote_mmap_export);
	free(ep->buffer);

	if (ep->dev != NULL) {
		tmp = doca_dev_close(ep->dev);
		set_first_error(&result, tmp);
	}

	memset(ep, 0, sizeof(*ep));
	return result;
}
