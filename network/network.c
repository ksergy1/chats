#include "network.h"
#include "io-service.h"
#include "memory.h"

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <netinet/in.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>

#include <linux/sockios.h>

#include <errno.h>
#include <assert.h>

typedef ssize_t (*NET_OPERATOR)(int sockfd, struct msghdr *msg, int flags);

struct NET_OPERATION {
    NET_OPERATOR oper;
    int ioctl_request;
    io_svc_op_t iosvc_op;
};

static const struct NET_OPERATION NET_OPERATIONS[] = {
    [SRB_OP_SEND] = {
        .oper = sendmsg,
        .ioctl_request = SIOCOUTQ,
        .iosvc_op = IO_SVC_OP_WRITE
    },
    [SRB_OP_RECV] = {
        .oper = recvmsg,
        .ioctl_request = SIOCINQ,
        .iosvc_op = IO_SVC_OP_READ
    }
};

typedef void (*OPERATOR)(srb_t *srb);
typedef network_result_t (*OPERATOR_NO_CB)(srb_t *srb);

static void tcp_send_recv_async(srb_t *srb);
static void tcp_send_recv_sync(srb_t *srb);
static void udp_send_async(srb_t *srb);
static void udp_send_sync(srb_t *srb);
static void udp_recv_async(srb_t *srb);
static void udp_recv_sync(srb_t *srb);

static network_result_t NETWORK_WARN_UNUSED
tcp_send_recv_sync_no_cb(srb_t *srb);
static network_result_t NETWORK_WARN_UNUSED
udp_send_sync_no_cb(srb_t *srb);
static network_result_t NETWORK_WARN_UNUSED
udp_recv_sync_no_cb(srb_t *srb);


#define OPERATOR_IDX(proto, op) \
    (\
     (((proto) & 0x01) << 0x01) | \
     ((op) & 0x01) \
    )

static const OPERATOR OPERATORS[] = {
    [OPERATOR_IDX(EPT_TCP, SRB_OP_SEND)] = tcp_send_recv_async,
    [OPERATOR_IDX(EPT_TCP, SRB_OP_RECV)] = tcp_send_recv_async,

    [OPERATOR_IDX(EPT_UDP, SRB_OP_SEND)] = udp_send_async,
    [OPERATOR_IDX(EPT_UDP, SRB_OP_RECV)] = udp_recv_async,
};

static const OPERATOR_NO_CB OPERATORS_NO_CB[] = {
    [OPERATOR_IDX(EPT_TCP, SRB_OP_SEND)] = tcp_send_recv_sync_no_cb,
    [OPERATOR_IDX(EPT_TCP, SRB_OP_RECV)] = tcp_send_recv_sync_no_cb,

    [OPERATOR_IDX(EPT_UDP, SRB_OP_SEND)] = udp_send_sync_no_cb,
    [OPERATOR_IDX(EPT_UDP, SRB_OP_RECV)] = udp_recv_sync_no_cb,
};

/***************** functions *********************/
/**************** non-callbacked functions ***********/
static network_result_t
tcp_send_recv_sync_no_cb(srb_t *srb) {
    buffer_t *buffer;
    size_t bytes_op;
    size_t bytes_to_op;
    ssize_t bytes_op_cur;
    srb_operation_t op;
    NET_OPERATOR oper;
    int more_bytes;
    endpoint_socket_t *ep_skt_ptr;
    network_result_t ret;

    assert(srb);

    ep_skt_ptr = op == SRB_OP_SEND
                  ? &srb->aux.dst
                  : &srb->aux.src;

    assert(ep_skt_ptr->skt >= 0 && ep_skt_ptr->ep.ep_type == EPT_TCP);

    buffer = srb->buffer;
    op = srb->operation.op;
    oper = NET_OPERATIONS[op].oper;

    assert(buffer != NULL);

    srb->mhdr.msg_iovlen = 1;
    srb->mhdr.msg_iov = &srb->vec;
    srb->mhdr.msg_control = NULL;
    srb->mhdr.msg_controllen = 0;
    srb->mhdr.msg_flags = 0;
    srb->mhdr.msg_name = NULL;
    srb->mhdr.msg_namelen = 0;

    bytes_to_op = buffer_size(buffer) - srb->bytes_operated;
    bytes_op = 0;

    while (bytes_op < bytes_to_op) {
        errno = 0;
        /* refresh mhdr struct */
        srb->vec.iov_base = buffer_data(buffer) + srb->bytes_operated + bytes_op;
        srb->vec.iov_len = buffer_size(buffer) - srb->bytes_operated - bytes_op;

        bytes_op_cur = (*oper)(ep_skt_ptr->skt,
                               &srb->mhdr,
                               MSG_NOSIGNAL);
        if (bytes_op_cur < 0) break;
        bytes_op += bytes_op_cur;
    }

    srb->bytes_operated += bytes_op;
    assert(0 == ioctl(ep_skt_ptr->skt, NET_OPERATIONS[op].ioctl_request, &more_bytes));

    ret.buffer = srb->buffer;
    ret.bytes_operated = srb->bytes_operated;
    ret.ctx = srb->ctx;
    ret.ep = ep_skt_ptr->ep;
    ret.err = errno;
    ret.has_more_bytes = more_bytes;

    deallocate(srb);
    return ret;
}

static network_result_t
udp_send_sync_no_cb(srb_t *srb) {
    buffer_t *buffer;
    size_t bytes_op;
    ssize_t bytes_op_cur;
    srb_operation_t op;
    NET_OPERATOR oper;
    int more_bytes;
    network_result_t ret;

    assert(srb &&
           srb->aux.dst.skt >= 0 &&
           srb->aux.dst.ep.ep_type == EPT_UDP);

    buffer = srb->buffer;
    op = srb->operation.op;
    oper = NET_OPERATIONS[op].oper;

    assert(buffer != NULL);

    srb->mhdr.msg_iovlen = 1;
    srb->mhdr.msg_iov = &srb->vec;
    srb->mhdr.msg_control = NULL;
    srb->mhdr.msg_controllen = 0;
    srb->mhdr.msg_flags = 0;
    srb->mhdr.msg_name = (struct sockaddr *)&srb->aux.dst.ep.addr;
    srb->mhdr.msg_namelen = srb->aux.dst.ep.ep_class == EPC_IP4
                             ? sizeof(struct sockaddr_in)
                             : sizeof(struct sockaddr_in6);

    srb->vec.iov_base = buffer_data(buffer);
    srb->vec.iov_len = buffer_size(buffer);

    bytes_op = srb->bytes_operated = 0;

    while (bytes_op < buffer_size(buffer)) {
        errno = 0;
        bytes_op_cur = (*oper)(srb->aux.dst.skt,
                               &srb->mhdr,
                               MSG_NOSIGNAL);
        if (bytes_op_cur < 0) break;
        bytes_op += bytes_op_cur;
    }

    srb->bytes_operated = bytes_op;
    assert(0 == ioctl(srb->aux.dst.skt, NET_OPERATIONS[op].ioctl_request, &more_bytes));

    ret.buffer = srb->buffer;
    ret.bytes_operated = srb->bytes_operated;
    ret.ctx = srb->ctx;
    ret.ep = srb->aux.dst.ep;
    ret.err = errno;
    ret.has_more_bytes = more_bytes;

    deallocate(srb);

    return ret;
}

static network_result_t
udp_recv_sync_no_cb(srb_t *srb) {
    buffer_t *buffer;
    size_t bytes_op, bytes_to_op;
    ssize_t bytes_op_cur;
    srb_operation_t op;
    NET_OPERATOR oper;
    int bytes_pending;
    network_result_t ret;

    assert(srb &&
           srb->aux.src.skt >= 0 &&
           srb->aux.src.ep.ep_type == EPT_UDP);

    buffer = srb->buffer;
    op = srb->operation.op;
    oper = NET_OPERATIONS[op].oper;

    assert(buffer != NULL);

    srb->mhdr.msg_iovlen = 1;
    srb->mhdr.msg_iov = &srb->vec;
    srb->mhdr.msg_control = NULL;
    srb->mhdr.msg_controllen = 0;
    srb->mhdr.msg_flags = 0;
    srb->mhdr.msg_name = (struct sockaddr *)&srb->aux.src.ep.addr;
    srb->mhdr.msg_namelen = sizeof(srb->aux.src.ep.addr);

    srb->vec.iov_base = buffer_data(buffer) + srb->bytes_operated;
    srb->vec.iov_len = buffer_size(buffer) - srb->bytes_operated;

    bytes_to_op = buffer_size(buffer) - srb->bytes_operated;
    bytes_op = 0;

    ret.buffer = srb->buffer;
    ret.ctx = srb->ctx;
    ret.ep = srb->aux.src.ep;

    assert(0 == ioctl(srb->aux.src.skt, NET_OPERATIONS[op].ioctl_request, &bytes_pending));
    if (bytes_pending > bytes_to_op) {
        errno = 0;
        bytes_op_cur = (*oper)(srb->aux.src.skt,
                               &srb->mhdr,
                               MSG_NOSIGNAL | MSG_PEEK);

        ret.err = errno ? errno : NSRCE_BUFFER_TOO_SMALL;
        ret.bytes_operated = bytes_op_cur;
        ret.has_more_bytes = bytes_pending;

        deallocate(srb);

        return ret;
    }

    errno = 0;
    bytes_op_cur = (*oper)(srb->aux.src.skt,
                           &srb->mhdr,
                           MSG_NOSIGNAL);
    if (bytes_op_cur >= 0) bytes_op += bytes_op_cur;

    srb->bytes_operated += bytes_op;

    translate_endpoint(&srb->aux.src.ep);

    ret.bytes_operated = bytes_op;
    ret.err = errno;
    ret.has_more_bytes = 0;

    deallocate(srb);

    return ret;
}

/**************** callbacked functions ***********/
static
void tcp_send_recv_async_tpl(int fd, io_svc_op_t op_, void *ctx) {
    srb_t *srb = ctx;
    buffer_t *buffer = srb->buffer;
    size_t bytes_op = srb->bytes_operated;
    srb_operation_t op = srb->operation.op;
    io_service_t *iosvc = srb->iosvc;
    NET_OPERATOR oper = NET_OPERATIONS[op].oper;
    io_svc_op_t io_svc_op = NET_OPERATIONS[op].iosvc_op;
    ssize_t bytes_op_cur;
    int more_bytes;
    endpoint_t *ep_ptr;

    errno = 0;
    srb->vec.iov_base = buffer_data(buffer) + bytes_op;
    srb->vec.iov_len = buffer_size(buffer) - bytes_op;
    bytes_op_cur = (*oper)(fd,
                           &srb->mhdr,
                           MSG_NOSIGNAL | MSG_DONTWAIT);

    if (bytes_op_cur < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            io_service_post_job(iosvc,
                                fd,
                                io_svc_op,
                                true,
                                tcp_send_recv_async_tpl,
                                srb);
        else {
            ep_ptr = op == SRB_OP_SEND
                            ? &srb->aux.dst.ep
                            : &srb->aux.src.ep;
            if (srb->cb)
                (*srb->cb)(*ep_ptr, errno, bytes_op, more_bytes, buffer, srb->ctx);
            deallocate(srb);
        }
    }
    else {
        bytes_op += bytes_op_cur;
        srb->bytes_operated = bytes_op;
        if (bytes_op < buffer_size(buffer))
            io_service_post_job(iosvc,
                                fd,
                                io_svc_op,
                                true,
                                tcp_send_recv_async_tpl,
                                srb);
        else {
            assert(0 == ioctl(fd, NET_OPERATIONS[op].ioctl_request, &more_bytes));
            ep_ptr = op == SRB_OP_SEND
                            ? &srb->aux.dst.ep
                            : &srb->aux.src.ep;
            if (srb->cb)
                (*srb->cb)(*ep_ptr, errno, bytes_op, more_bytes, buffer, srb->ctx);
            deallocate(srb);
        }
    }
}

static
void udp_send_async_tpl(int fd, io_svc_op_t op_, void *ctx) {
    srb_t *srb = ctx;
    buffer_t *buffer = srb->buffer;
    size_t bytes_op = srb->bytes_operated;
    srb_operation_t op = srb->operation.op;
    io_service_t *iosvc = srb->iosvc;
    NET_OPERATOR oper = NET_OPERATIONS[op].oper;
    io_svc_op_t io_svc_op = NET_OPERATIONS[op].iosvc_op;
    ssize_t bytes_op_cur;
    int more_bytes;

    errno = 0;
    srb->vec.iov_base = buffer_data(buffer) + bytes_op;
    srb->vec.iov_len = buffer_size(buffer) - bytes_op;
    bytes_op_cur = (*oper)(fd,
                           &srb->mhdr,
                           MSG_NOSIGNAL | MSG_DONTWAIT);

    if (bytes_op_cur < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            io_service_post_job(iosvc,
                                fd,
                                io_svc_op,
                                true,
                                udp_send_async_tpl,
                                srb);
        else {
            if (srb->cb)
                (*srb->cb)(srb->aux.dst.ep, errno, bytes_op, more_bytes, buffer, srb->ctx);
            deallocate(srb);
        }
    }
    else {
        bytes_op += bytes_op_cur;
        srb->bytes_operated = bytes_op;
        if (bytes_op < buffer_size(buffer))
            io_service_post_job(iosvc,
                                fd,
                                io_svc_op,
                                true,
                                udp_send_async_tpl,
                                srb);
        else {
            assert(0 == ioctl(fd, NET_OPERATIONS[op].ioctl_request, &more_bytes));
            if (srb->cb)
                (*srb->cb)(srb->aux.dst.ep, errno, bytes_op, more_bytes, buffer, srb->ctx);
            deallocate(srb);
        }
    }
}

static
void udp_recv_async_tpl(int fd, io_svc_op_t op_, void *ctx) {
    srb_t *srb = ctx;
    buffer_t *buffer;
    size_t bytes_op;
    ssize_t bytes_op_cur;
    srb_operation_t op;
    NET_OPERATOR oper;
    int bytes_pending;

    buffer = srb->buffer;
    op = srb->operation.op;
    oper = NET_OPERATIONS[op].oper;

    assert(0 == ioctl(srb->aux.src.skt, NET_OPERATIONS[op].ioctl_request, &bytes_pending));
    if (bytes_pending > buffer_size(buffer) - srb->bytes_operated) {
        errno = 0;
        bytes_op_cur = (*oper)(srb->aux.src.skt,
                               &srb->mhdr,
                               MSG_NOSIGNAL | MSG_PEEK);

        if (srb->cb)
            (*srb->cb)(srb->aux.src.ep, errno ? errno : NSRCE_BUFFER_TOO_SMALL,
                       srb->bytes_operated + bytes_op_cur, bytes_pending,
                       buffer, srb->ctx);

        deallocate(srb);

        return;
    }

    bytes_op = 0;

    errno = 0;
    bytes_op_cur = (*oper)(srb->aux.src.skt,
                           &srb->mhdr,
                           MSG_NOSIGNAL);
    if (bytes_op_cur >= 0) bytes_op += bytes_op_cur;

    srb->bytes_operated += bytes_op;

    translate_endpoint(&srb->aux.src.ep);

    if (srb->cb)
        (*srb->cb)(srb->aux.src.ep, errno, bytes_op, 0, buffer, srb->ctx);

    deallocate(srb);
}

static
void tcp_send_recv_sync(srb_t *srb) {
    network_send_recv_cb_t cb;
    void *cb_ctx;
    network_result_t ret;

    assert(srb);

    cb = srb->cb;
    cb_ctx = srb->ctx;

    ret = tcp_send_recv_sync_no_cb(srb);
    if (cb)
        (*cb)(ret.ep, ret.err,
              ret.bytes_operated,
              ret.has_more_bytes,
              ret.buffer,
              cb_ctx);
}

static
void tcp_send_recv_async(srb_t *srb) {
    buffer_t *buffer;
    endpoint_socket_t *ep_skt_ptr;

    assert(srb);

    ep_skt_ptr = srb->operation.op == SRB_OP_SEND
                  ? &srb->aux.dst
                  : &srb->aux.src;

    assert(ep_skt_ptr->skt >= 0 && ep_skt_ptr->ep.ep_type == EPT_TCP);

    buffer = srb->buffer;

    assert(buffer != NULL);

    srb->mhdr.msg_iovlen = 1;
    srb->mhdr.msg_iov = &srb->vec;
    srb->mhdr.msg_control = NULL;
    srb->mhdr.msg_controllen = 0;
    srb->mhdr.msg_flags = 0;
    srb->mhdr.msg_name = NULL;
    srb->mhdr.msg_namelen = 0;

    srb->vec.iov_base = buffer_data(buffer) + srb->bytes_operated;
    srb->vec.iov_len = buffer_size(buffer) - srb->bytes_operated;

    io_service_post_job(srb->iosvc,
                        ep_skt_ptr->skt,
                        NET_OPERATIONS[srb->operation.op].iosvc_op,
                        true,
                        tcp_send_recv_async_tpl,
                        srb);
}

static
void udp_send_sync(srb_t *srb) {
    network_send_recv_cb_t cb;
    void *cb_ctx;
    network_result_t ret;

    assert(srb);

    cb = srb->cb;
    cb_ctx = srb->ctx;

    ret = udp_send_sync_no_cb(srb);
    if (cb)
        (*cb)(ret.ep, ret.err,
              ret.bytes_operated,
              ret.has_more_bytes,
              ret.buffer,
              cb_ctx);
}

static
void udp_send_async(srb_t *srb) {
    buffer_t *buffer;
    assert(srb &&
           srb->aux.dst.skt >= 0 &&
           srb->aux.dst.ep.ep_type == EPT_UDP);

    buffer = srb->buffer;

    assert(buffer != NULL);

    srb->mhdr.msg_iovlen = 1;
    srb->mhdr.msg_iov = &srb->vec;
    srb->mhdr.msg_control = NULL;
    srb->mhdr.msg_controllen = 0;
    srb->mhdr.msg_flags = 0;
    srb->mhdr.msg_name = (struct sockaddr *)&srb->aux.dst.ep.addr;
    srb->mhdr.msg_namelen = srb->aux.dst.ep.ep_class == EPC_IP4
                             ? sizeof(struct sockaddr_in)
                             : sizeof(struct sockaddr_in6);

    srb->vec.iov_base = buffer_data(buffer);
    srb->vec.iov_len = buffer_size(buffer);

    srb->bytes_operated = 0;

    io_service_post_job(srb->iosvc,
                        srb->aux.dst.skt,
                        NET_OPERATIONS[srb->operation.op].iosvc_op,
                        true,
                        udp_send_async_tpl,
                        srb);
}

static
void udp_recv_sync(srb_t *srb) {
    network_send_recv_cb_t cb;
    void *cb_ctx;
    network_result_t ret;

    assert(srb);

    cb = srb->cb;
    cb_ctx = srb->ctx;

    ret = udp_recv_sync_no_cb(srb);
    if (cb)
        (*cb)(ret.ep, ret.err,
              ret.bytes_operated,
              ret.has_more_bytes,
              ret.buffer,
              cb_ctx);
}

static
void udp_recv_async(srb_t *srb) {
    buffer_t *buffer;
    srb_operation_t op;
    NET_OPERATOR oper;
    int bytes_pending;

    assert(srb &&
           srb->aux.src.skt >= 0 &&
           srb->aux.src.ep.ep_type == EPT_UDP);

    buffer = srb->buffer;
    op = srb->operation.op;
    oper = NET_OPERATIONS[op].oper;

    assert(buffer != NULL);

    srb->mhdr.msg_iovlen = 1;
    srb->mhdr.msg_iov = &srb->vec;
    srb->mhdr.msg_control = NULL;
    srb->mhdr.msg_controllen = 0;
    srb->mhdr.msg_flags = 0;
    srb->mhdr.msg_name = (struct sockaddr *)&srb->aux.src.ep.addr;
    srb->mhdr.msg_namelen = sizeof(srb->aux.src.ep.addr);

    srb->vec.iov_base = buffer_data(buffer) + srb->bytes_operated;
    srb->vec.iov_len = buffer_size(buffer) - srb->bytes_operated;

    srb->bytes_operated = 0;

    io_service_post_job(srb->iosvc,
                        srb->aux.src.skt,
                        NET_OPERATIONS[srb->operation.op].iosvc_op,
                        true,
                        udp_recv_async_tpl,
                        srb);
}

/******************** API *********************/
void srb_operate(srb_t *srb) {
    OPERATOR op;

    if (!srb) return;
    assert(srb->operation.type < EPT_MAX && srb->operation.op < SRB_OP_MAX);
    /* only asynchronous operations allowed */
    assert(srb->iosvc != NULL);

    op = OPERATORS[OPERATOR_IDX(srb->operation.type,
                                srb->operation.op)];

    (*op)(srb);
}

network_result_t srb_operate_no_cb(srb_t *srb) {
    OPERATOR_NO_CB op;

    if (!srb) return;
    assert(srb->operation.type < EPT_MAX && srb->operation.op < SRB_OP_MAX);
    /* only synchronous operations allowed */
    assert(srb->iosvc == NULL);

    op = OPERATORS_NO_CB[OPERATOR_IDX(srb->operation.type,
                                      srb->operation.op)];

    return (*op)(srb);
}
