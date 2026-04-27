#define _POSIX_C_SOURCE 200809L

#include "rdma_common.h"
#include <time.h>

static double get_time_us(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1000.0;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <ib_hca> <ib_port> <gid_index> <server_ip>\n", argv[0]);
        return 1;
    }
    const char *ib_hca = argv[1];
    int ib_port = atoi(argv[2]);
    int gid_index = atoi(argv[3]);
    const char *server_ip = argv[4];

    if (ib_port < 1 || ib_port > 255) {
        fprintf(stderr, "Invalid port number: %s (must be 1-255)\n", argv[2]);
        return 1;
    }
    if (gid_index < 0) {
        fprintf(stderr, "Invalid GID index: %s (must be >= 0)\n", argv[3]);
        return 1;
    }

    printf("Device: %s, Port: %d, GID Index: %d\n", ib_hca, ib_port, gid_index);

    struct ibv_context *ctx = open_device_by_name(ib_hca);
    if (!ctx) {
        fprintf(stderr, "Failed to open RDMA device: %s (run ibv_devinfo to list devices)\n", ib_hca);
        return 1;
    }

    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx, ib_port, &port_attr)) {
        fprintf(stderr, "Failed to query port\n");
        return 1;
    }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        return 1;
    }
    struct ibv_cq *cq = ibv_create_cq(ctx, CQ_SIZE, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Failed to create CQ\n");
        return 1;
    }

    void *buf = malloc(BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return 1;
    }
    memset(buf, 'A', BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, BUF_SIZE,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        fprintf(stderr, "Failed to register MR\n");
        return 1;
    }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq, .recv_cq = cq,
        .cap = { .max_send_wr = 4, .max_recv_wr = 4, .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
    if (!qp) {
        fprintf(stderr, "Failed to create QP\n");
        return 1;
    }
    if (modify_qp_to_init(qp, ib_port)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return 1;
    }

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT)
    };
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP address: %s\n", server_ip);
        return 1;
    }

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    union ibv_gid my_gid;
    if (ibv_query_gid(ctx, ib_port, gid_index, &my_gid)) {
        fprintf(stderr, "Failed to query GID\n");
        return 1;
    }
    int use_gid = (port_attr.link_layer == IBV_LINK_LAYER_ETHERNET);

    struct conn_data local = {
        .addr   = (uintptr_t)buf,
        .rkey   = mr->rkey,
        .qp_num = qp->qp_num,
        .lid    = port_attr.lid
    };
    memcpy(local.gid, &my_gid, 16);

    struct conn_data remote;
    if (write(sock_fd, &local, sizeof(local)) != sizeof(local)) {
        fprintf(stderr, "Failed to write connection data\n");
        return 1;
    }
    if (read(sock_fd, &remote, sizeof(remote)) != sizeof(remote)) {
        fprintf(stderr, "Failed to read connection data\n");
        return 1;
    }
    close(sock_fd);

    if (modify_qp_to_rtr(qp, ib_port, gid_index, remote.qp_num, remote.lid, remote.gid, use_gid)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }
    if (modify_qp_to_rts(qp)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    printf("PING %s: %d bytes\n", server_ip, BUF_SIZE);

    struct ibv_wc wc;
    uint32_t seq = 0;

    while (1) {
        seq++;
        if (post_recv(qp)) {
            fprintf(stderr, "Failed to post receive\n");
            break;
        }

        double t_start = get_time_us();
        if (rdma_write(qp, mr, buf, remote.addr, remote.rkey, seq)) {
            fprintf(stderr, "Failed to post RDMA write\n");
            break;
        }

        while (ibv_poll_cq(cq, 1, &wc) == 0);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Send error: %d\n", wc.status);
            break;
        }

        while (ibv_poll_cq(cq, 1, &wc) == 0);
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Recv error: %d\n", wc.status);
            break;
        }

        double t_end = get_time_us();
        printf("seq=%u time=%.2f us\n", seq, t_end - t_start);

    }

    ibv_destroy_qp(qp);
    ibv_dereg_mr(mr);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    free(buf);
    return 0;
}
