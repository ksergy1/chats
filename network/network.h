#ifndef _CHATS_NETWORK_COMMON_H_
# define _CHATS_NETWORK_COMMON_H_

# include "endpoint.h"
# include "memory.h"
# include "io-service.h"

# include <stddef.h>
# include <stdbool.h>
# include <sys/types.h>
# include <sys/socket.h>

/****************** forward declarations *************/
struct connection;
typedef struct connection connection_t;

struct send_recv_buffer;
typedef struct send_recv_buffer srb_t;

/****************** callback types *******************/
/** callback on connection accept
 * \param [in] ep connected remote endpoint
 * \param [in] ctx user context
 * \param [in] err errno
 * \return \c true until the connection is rejected
 */
typedef bool (*tcp_connection_cb_t)(const connection_t *ep, int err,
                                    void *ctx);
typedef void (*tcp_client_connection_cb_t)(const endpoint_t *ep, int err,
                                           void *ctx);
/** callback on data sent
 * \param [in] err errno
 * \param [in] bytes bytes sent
 * \param [in] buffer buffer sent
 * \param [in] ctx user context
 */
typedef void (*network_send_recv_cb_t)(endpoint_t ep,
                                       int err,
                                       size_t bytes_operated,
                                       size_t has_more_bytes,
                                       buffer_t *buffer,
                                       void *ctx);

typedef void (*srb_cb_t)(srb_t *srb, endpoint_t ep, int err, void *ctx);

/******************* enumerations ********************/
typedef enum network_send_recv_custom_error_enum {
    NSRCE_BUFFER_TOO_SMALL = -1
} network_send_recv_custom_error_t;

typedef enum srb_operation_enum {
    SRB_OP_SEND = 0,
    SRB_OP_RECV,
    SRB_OP_MAX,
    SRB_OP_NONE = SRB_OP_MAX
} srb_operation_t;

/******************* struct definitions *****************/
struct connection_acceptor {
    void *host;
    tcp_connection_cb_t connection_cb;
    void *connection_ctx;
};

struct connector {
    void *host;
    tcp_client_connection_cb_t connection_cb;
    void *connection_ctx;
};

struct send_recv_buffer {
    /* user provided */
    struct {
        endpoint_type_t type;
        srb_operation_t op;
    } operation;

    struct {
        endpoint_socket_t src;
        endpoint_socket_t dst;
    } aux;

    io_service_t *iosvc;
    buffer_t *buffer;
    size_t bytes_operated;                                  ///< internaly initialized

    network_send_recv_cb_t cb;
    void *ctx;

    /* internal */
    struct msghdr mhdr;
    struct iovec vec;
};

/****************** functions prototypes **********************/
void srb_operate(srb_t *srb);

#endif /* _CHATS_NETWORK_COMMON_H_ */