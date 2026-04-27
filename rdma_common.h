#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define BUF_SIZE 8192
#define TCP_PORT 18515
#define CQ_SIZE  16

static inline struct ibv_context *open_device_by_name(const char *name) {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        return NULL;
    }
    struct ibv_context *ctx = NULL;
    for (int i = 0; dev_list[i]; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), name) == 0) {
            ctx = ibv_open_device(dev_list[i]);
            break;
        }
    }
    ibv_free_device_list(dev_list);
    return ctx;
}

struct conn_data {
    uint64_t addr;
    uint32_t rkey;
    uint32_t qp_num;
    uint16_t lid;
    uint8_t  gid[16];
} __attribute__((packed));

static inline int modify_qp_to_init(struct ibv_qp *qp, int port) {
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = port,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    };
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static inline int modify_qp_to_rtr(struct ibv_qp *qp, int port, int gid_index,
                                   uint32_t remote_qpn, uint16_t dlid,
                                   uint8_t *dgid, int use_gid) {
    struct ibv_qp_attr attr = {
        .qp_state              = IBV_QPS_RTR,
        .path_mtu              = IBV_MTU_1024,
        .dest_qp_num           = remote_qpn,
        .rq_psn                = 0,
        .max_dest_rd_atomic    = 1,
        .min_rnr_timer         = 12,
        .ah_attr = {
            .dlid          = dlid,
            .sl            = 0,
            .src_path_bits = 0,
            .port_num      = port
        }
    };
    if (use_gid) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.dgid.global.subnet_prefix = ((uint64_t*)dgid)[0];
        attr.ah_attr.grh.dgid.global.interface_id  = ((uint64_t*)dgid)[1];
        attr.ah_attr.grh.sgid_index = gid_index;
        attr.ah_attr.grh.hop_limit  = 1;
    }
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

static inline int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state      = IBV_QPS_RTS,
        .timeout       = 14,
        .retry_cnt     = 7,
        .rnr_retry     = 7,
        .sq_psn        = 0,
        .max_rd_atomic = 1
    };
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

static inline int post_recv(struct ibv_qp *qp) {
    struct ibv_recv_wr wr = {0}, *bad;
    return ibv_post_recv(qp, &wr, &bad);
}

static inline int rdma_write(struct ibv_qp *qp, struct ibv_mr *mr, void *buf,
                             uint64_t remote_addr, uint32_t rkey, uint32_t imm) {
    struct ibv_sge sge = {
        .addr   = (uintptr_t)buf,
        .length = BUF_SIZE,
        .lkey   = mr->lkey
    };
    struct ibv_send_wr wr = {
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_RDMA_WRITE_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED,
        .imm_data   = htonl(imm),
        .wr.rdma = { .remote_addr = remote_addr, .rkey = rkey }
    }, *bad;
    return ibv_post_send(qp, &wr, &bad);
}

#endif /* RDMA_COMMON_H */
