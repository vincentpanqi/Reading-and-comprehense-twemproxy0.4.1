/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <nc_core.h>
#include <nc_server.h>

struct msg *
rsp_get(struct conn *conn)
{
    struct msg *msg;

    ASSERT(!conn->client && !conn->proxy);

    msg = msg_get(conn, false, conn->redis);
    if (msg == NULL) {
        conn->err = errno;
    }

    return msg;
}

void
rsp_put(struct msg *msg)
{
    ASSERT(!msg->request);
    ASSERT(msg->peer == NULL);
    msg_put(msg);
}

static struct msg *
rsp_make_error(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg;        /* peer message (response) */
    struct msg *cmsg, *nmsg; /* current and next message (request) */
    uint64_t id;
    err_t err;

    ASSERT(conn->client && !conn->proxy);
    ASSERT(msg->request && req_error(conn, msg));
    ASSERT(msg->owner == conn);

    id = msg->frag_id;
    if (id != 0) {
        for (err = 0, cmsg = TAILQ_NEXT(msg, c_tqe);
             cmsg != NULL && cmsg->frag_id == id;
             cmsg = nmsg) {
            nmsg = TAILQ_NEXT(cmsg, c_tqe);

            /* dequeue request (error fragment) from client outq */
            conn->dequeue_outq(ctx, conn, cmsg);
            if (err == 0 && cmsg->err != 0) {
                err = cmsg->err;
            }

            req_put(cmsg);
        }
    } else {
        err = msg->err;
    }

    pmsg = msg->peer;
    if (pmsg != NULL) {
        ASSERT(!pmsg->request && pmsg->peer == msg);
        msg->peer = NULL;
        pmsg->peer = NULL;
        rsp_put(pmsg);
    }

    return msg_get_error(conn->redis, err);
}


/*
*             Client+             Proxy           Server+
*                              (nutcracker)
*                                   .
*       msg_recv {read event}       .       msg_recv {read event}
*         +                         .                         +
*         |                         .                         |
*         \                         .                         /
*         req_recv_next             .             rsp_recv_next
*           +                       .                       +
*           |                       .                       |       Rsp
*           req_recv_done           .           rsp_recv_done      <===
*             +                     .                     +
*             |                     .                     |
*    Req      \                     .                     /
*    ===>     req_filter*           .           *rsp_filter
*               +                   .                   +
*               |                   .                   |
*               \                   .                   /
*               req_forward-//  (a) . (c)  \\-rsp_forward
*                                   .
*                                   .
*       msg_send {write event}      .      msg_send {write event}
*         +                         .                         +
*         |                         .                         |
*    Rsp' \                         .                         /     Req'
*   <===  rsp_send_next             .             req_send_next     ===>
*           +                       .                       +
*           |                       .                       |
*           \                       .                       /
*           rsp_send_done-//    (d) . (b)    //-req_send_done
*
*
* (a) -> (b) -> (c) -> (d) is the normal flow of transaction consisting
* of a single request response, where (a) and (b) handle request from
* client, while (c) and (d) handle the corresponding response from the
* server.
*/

struct msg *
rsp_recv_next(struct context *ctx, struct conn *conn, bool alloc)
{//msg_recv中执行，获取一个空闲msg
    struct msg *msg;

    ASSERT(!conn->client && !conn->proxy);

    if (conn->eof) { //后端进程异常，读出错，会进入这里面
        msg = conn->rmsg;

        /* server sent eof before sending the entire request */
        if (msg != NULL) {
            conn->rmsg = NULL;

            ASSERT(msg->peer == NULL);
            ASSERT(!msg->request);

            log_error("eof s %d discarding incomplete rsp %"PRIu64" len "
                      "%"PRIu32"", conn->sd, msg->id, msg->mlen);

            rsp_put(msg);
        }

        /*
         * We treat TCP half-close from a server different from how we treat
         * those from a client. On a FIN from a server, we close the connection
         * immediately by sending the second FIN even if there were outstanding
         * or pending requests. This is actually a tricky part in the FA, as
         * we don't expect this to happen unless the server is misbehaving or
         * it crashes
         */
        conn->done = 1;
        log_error("s %d active %d is done", conn->sd, conn->active(conn));

        return NULL;
    }

    msg = conn->rmsg;
    if (msg != NULL) {
        ASSERT(!msg->request);
        return msg;
    }

    if (!alloc) {
        return NULL;
    }

    msg = rsp_get(conn);
    if (msg != NULL) {
        conn->rmsg = msg;
    }

    return msg;
}

/*
*             Client+             Proxy           Server+
*                              (nutcracker)
*                                   .
*       msg_recv {read event}       .       msg_recv {read event}
*         +                         .                         +
*         |                         .                         |
*         \                         .                         /
*         req_recv_next             .             rsp_recv_next
*           +                       .                       +
*           |                       .                       |       Rsp
*           req_recv_done           .           rsp_recv_done      <===
*             +                     .                     +
*             |                     .                     |
*    Req      \                     .                     /
*    ===>     req_filter*           .           *rsp_filter
*               +                   .                   +
*               |                   .                   |
*               \                   .                   /
*               req_forward-//  (a) . (c)  \\-rsp_forward
*                                   .
*                                   .
*       msg_send {write event}      .      msg_send {write event}
*         +                         .                         +
*         |                         .                         |
*    Rsp' \                         .                         /     Req'
*   <===  rsp_send_next             .             req_send_next     ===>
*           +                       .                       +
*           |                       .                       |
*           \                       .                       /
*           rsp_send_done-//    (d) . (b)    //-req_send_done
*
*
* (a) -> (b) -> (c) -> (d) is the normal flow of transaction consisting
* of a single request response, where (a) and (b) handle request from
* client, while (c) and (d) handle the corresponding response from the
* server.
*/

static bool
rsp_filter(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg;

    ASSERT(!conn->client && !conn->proxy);

    if (msg_empty(msg)) {
        ASSERT(conn->rmsg == NULL);
        log_debug(LOG_VERB, "filter empty rsp %"PRIu64" on s %d", msg->id,
                  conn->sd);
        rsp_put(msg);
        return true;
    }

    pmsg = TAILQ_FIRST(&conn->omsg_q);
    if (pmsg == NULL) {
        log_debug(LOG_ERR, "filter stray rsp %"PRIu64" len %"PRIu32" on s %d",
                  msg->id, msg->mlen, conn->sd);
        rsp_put(msg);

        /*
         * Memcached server can respond with an error response before it has
         * received the entire request. This is most commonly seen for set
         * requests that exceed item_size_max. IMO, this behavior of memcached
         * is incorrect. The right behavior for update requests that are over
         * item_size_max would be to either:
         * - close the connection Or,
         * - read the entire item_size_max data and then send CLIENT_ERROR
         *
         * We handle this stray packet scenario in nutcracker by closing the
         * server connection which would end up sending SERVER_ERROR to all
         * clients that have requests pending on this server connection. The
         * fix is aggressive, but not doing so would lead to clients getting
         * out of sync with the server and as a result clients end up getting
         * responses that don't correspond to the right request.
         *
         * See: https://github.com/twitter/twemproxy/issues/149
         */
        conn->err = EINVAL;
        conn->done = 1;
        return true;
    }
    ASSERT(pmsg->peer == NULL);
    ASSERT(pmsg->request && !pmsg->done);

    /*
     * If the response from a server suggests a protocol level transient
     * failure, close the server connection and send back a generic error
     * response to the client.
     *
     * If auto_eject_host is enabled, this will also update the failure_count
     * and eject the server if it exceeds the failure_limit
     */
    if (msg->failure(msg)) {
        log_debug(LOG_INFO, "server failure rsp %"PRIu64" len %"PRIu32" "
                  "type %d on s %d", msg->id, msg->mlen, msg->type, conn->sd);
        rsp_put(msg);

        conn->err = EINVAL;
        conn->done = 1;

        return true;
    }

    if (pmsg->swallow) {
        conn->swallow_msg(conn, pmsg, msg);

        conn->dequeue_outq(ctx, conn, pmsg);
        pmsg->done = 1;

        log_debug(LOG_INFO, "swallow rsp %"PRIu64" len %"PRIu32" of req "
                  "%"PRIu64" on s %d", msg->id, msg->mlen, pmsg->id,
                  conn->sd);

        rsp_put(msg);
        req_put(pmsg);
        return true;
    }

    return false;
}

static void
rsp_forward_stats(struct context *ctx, struct server *server, struct msg *msg, uint32_t msgsize)
{
    ASSERT(!msg->request);

    stats_server_incr(ctx, server, responses);
    stats_server_incr_by(ctx, server, response_bytes, msgsize);
}


/*
*             Client+             Proxy           Server+
*                              (nutcracker)
*                                   .
*       msg_recv {read event}       .       msg_recv {read event}
*         +                         .                         +
*         |                         .                         |
*         \                         .                         /
*         req_recv_next             .             rsp_recv_next
*           +                       .                       +
*           |                       .                       |       Rsp
*           req_recv_done           .           rsp_recv_done      <===
*             +                     .                     +
*             |                     .                     |
*    Req      \                     .                     /
*    ===>     req_filter*           .           *rsp_filter
*               +                   .                   +
*               |                   .                   |
*               \                   .                   /
*               req_forward-//  (a) . (c)  \\-rsp_forward
*                                   .
*                                   .
*       msg_send {write event}      .      msg_send {write event}
*         +                         .                         +
*         |                         .                         |
*    Rsp' \                         .                         /     Req'
*   <===  rsp_send_next             .             req_send_next     ===>
*           +                       .                       +
*           |                       .                       |
*           \                       .                       /
*           rsp_send_done-//    (d) . (b)    //-req_send_done
*
*
* (a) -> (b) -> (c) -> (d) is the normal flow of transaction consisting
* of a single request response, where (a) and (b) handle request from
* client, while (c) and (d) handle the corresponding response from the
* server.
*/
//rsp_send_done从客户端连接conn->dequeue_outq中出对  rsp_forward从服务端连接s_conn->dequeue_outq中出对
static void
rsp_forward(struct context *ctx, struct conn *s_conn, struct msg *msg)
{
    rstatus_t status;
    struct msg *pmsg;
    struct conn *c_conn;
    uint32_t msgsize;

    ASSERT(!s_conn->client && !s_conn->proxy);
    msgsize = msg->mlen;

    /* response from server implies that server is ok and heartbeating */
    server_ok(ctx, s_conn);

    /* dequeue peer message (request) from server */
    pmsg = TAILQ_FIRST(&s_conn->omsg_q);
    ASSERT(pmsg != NULL && pmsg->peer == NULL);
    ASSERT(pmsg->request && !pmsg->done);

    /* pmsg为接收客户端报文的msg信息，参数msg为后端应答回来的msg信息 */
    s_conn->dequeue_outq(ctx, s_conn, pmsg); //这里取出接收的客户端msg信息，这里面存储有该msg所对应的conn信息，从而可以把后端应答回来的msg通过该conn发送出去
    pmsg->done = 1;

    /* 把接收的客户端msg信息和后端应答回来的msg信息进行关联 */
    /* establish msg <-> pmsg (response <-> request) link */
    pmsg->peer = msg;
    msg->peer = pmsg;

    msg->pre_coalesce(msg);

    c_conn = pmsg->owner;
    ASSERT(c_conn->client && !c_conn->proxy);

    if (req_done(c_conn, TAILQ_FIRST(&c_conn->omsg_q))) {
        status = event_add_out(ctx->evb, c_conn);
        if (status != NC_OK) {
            c_conn->err = errno;
        }
    }

    rsp_forward_stats(ctx, s_conn->owner, msg, msgsize);
}

/*
*             Client+             Proxy           Server+
*                              (nutcracker)
*                                   .
*       msg_recv {read event}       .       msg_recv {read event}
*         +                         .                         +
*         |                         .                         |
*         \                         .                         /
*         req_recv_next             .             rsp_recv_next
*           +                       .                       +
*           |                       .                       |       Rsp
*           req_recv_done           .           rsp_recv_done      <===
*             +                     .                     +
*             |                     .                     |
*    Req      \                     .                     /
*    ===>     req_filter*           .           *rsp_filter
*               +                   .                   +
*               |                   .                   |
*               \                   .                   /
*               req_forward-//  (a) . (c)  \\-rsp_forward
*                                   .
*                                   .
*       msg_send {write event}      .      msg_send {write event}
*         +                         .                         +
*         |                         .                         |
*    Rsp' \                         .                         /     Req'
*   <===  rsp_send_next             .             req_send_next     ===>
*           +                       .                       +
*           |                       .                       |
*           \                       .                       /
*           rsp_send_done-//    (d) . (b)    //-req_send_done
*
*
* (a) -> (b) -> (c) -> (d) is the normal flow of transaction consisting
* of a single request response, where (a) and (b) handle request from
* client, while (c) and (d) handle the corresponding response from the
* server.
*/

//如果读取出来的KV都是完整的，则conn->rmsg = NULL,如果读取内核协议栈缓冲区的数据最好一个KV没有读取完整，则conn->rmsg = nmsg(也就是新的一个msg)
void
rsp_recv_done(struct context *ctx, struct conn *conn, struct msg *msg,
              struct msg *nmsg)
{
    ASSERT(!conn->client && !conn->proxy);
    ASSERT(msg != NULL && conn->rmsg == msg);
    ASSERT(!msg->request);
    ASSERT(msg->owner == conn);
    ASSERT(nmsg == NULL || !nmsg->request);

    /* enqueue next message (response), if any */
    conn->rmsg = nmsg;

    if (rsp_filter(ctx, conn, msg)) {
        return;
    }

    rsp_forward(ctx, conn, msg);
}

/*
*             Client+             Proxy           Server+
*                              (nutcracker)
*                                   .
*       msg_recv {read event}       .       msg_recv {read event}
*         +                         .                         +
*         |                         .                         |
*         \                         .                         /
*         req_recv_next             .             rsp_recv_next
*           +                       .                       +
*           |                       .                       |       Rsp
*           req_recv_done           .           rsp_recv_done      <===
*             +                     .                     +
*             |                     .                     |
*    Req      \                     .                     /
*    ===>     req_filter*           .           *rsp_filter
*               +                   .                   +
*               |                   .                   |
*               \                   .                   /
*               req_forward-//  (a) . (c)  \\-rsp_forward
*                                   .
*                                   .
*       msg_send {write event}      .      msg_send {write event}
*         +                         .                         +
*         |                         .                         |
*    Rsp' \                         .                         /     Req'
*   <===  rsp_send_next             .             req_send_next     ===>
*           +                       .                       +
*           |                       .                       |
*           \                       .                       /
*           rsp_send_done-//    (d) . (b)    //-req_send_done
*
*
* (a) -> (b) -> (c) -> (d) is the normal flow of transaction consisting
* of a single request response, where (a) and (b) handle request from
* client, while (c) and (d) handle the corresponding response from the
* server.
*/
//发往客户端用rsp_send_next 发往后端服务器用req_send_next    msg_send或者msg_send_chain中执行
struct msg *
rsp_send_next(struct context *ctx, struct conn *conn)
{//取出需要发送的msg队列上的一条msg
    rstatus_t status;
    struct msg *msg, *pmsg; /* response and it's peer request */

    ASSERT(conn->client && !conn->proxy);

    pmsg = TAILQ_FIRST(&conn->omsg_q);
    if (pmsg == NULL || !req_done(conn, pmsg)) {
        /* nothing is outstanding, initiate close? */
        if (pmsg == NULL && conn->eof) {
            conn->done = 1;
            log_debug(LOG_INFO, "c %d is done", conn->sd);
        }

        status = event_del_out(ctx->evb, conn);
        if (status != NC_OK) {
            conn->err = errno;
        }

        return NULL;
    }

    msg = conn->smsg;
    if (msg != NULL) {
        ASSERT(!msg->request && msg->peer != NULL);
        ASSERT(req_done(conn, msg->peer));
        pmsg = TAILQ_NEXT(msg->peer, c_tqe);
    }

    if (pmsg == NULL || !req_done(conn, pmsg)) {
        conn->smsg = NULL;
        return NULL;
    }
    ASSERT(pmsg->request && !pmsg->swallow);

    if (req_error(conn, pmsg)) { //msg是否error
        msg = rsp_make_error(ctx, conn, pmsg);
        if (msg == NULL) {
            conn->err = errno;
            return NULL;
        }
        msg->peer = pmsg;
        pmsg->peer = msg;
        stats_pool_incr(ctx, conn->owner, forward_error);
    } else {
        msg = pmsg->peer;
    }
    ASSERT(!msg->request);

    conn->smsg = msg;

    log_debug(LOG_VVERB, "send next rsp %"PRIu64" on c %d", msg->id, conn->sd);

    return msg;
}


/* 
*             Client+             Proxy           Server+
*                              (nutcracker)
*                                   .
*       msg_recv {read event}       .       msg_recv {read event}
*         +                         .                         +
*         |                         .                         |
*         \                         .                         /
*         req_recv_next             .             rsp_recv_next
*           +                       .                       +
*           |                       .                       |       Rsp
*           req_recv_done           .           rsp_recv_done      <===
*             +                     .                     +
*             |                     .                     |
*    Req      \                     .                     /
*    ===>     req_filter*           .           *rsp_filter
*               +                   .                   +
*               |                   .                   |
*               \                   .                   /
*               req_forward-//  (a) . (c)  \\-rsp_forward
*                                   .
*                                   .
*       msg_send {write event}      .      msg_send {write event}
*         +                         .                         +
*         |                         .                         |
*    Rsp' \                         .                         /     Req'
*   <===  rsp_send_next             .             req_send_next     ===>
*           +                       .                       +
*           |                       .                       |
*           \                       .                       /
*           rsp_send_done-//    (d) . (b)    //-req_send_done
*
*
* (a) -> (b) -> (c) -> (d) is the normal flow of transaction consisting
* of a single request response, where (a) and (b) handle request from
* client, while (c) and (d) handle the corresponding response from the
* server.
*/
void
rsp_send_done(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg; /* peer message (request) */

    ASSERT(conn->client && !conn->proxy);
    ASSERT(conn->smsg == NULL);

    log_debug(LOG_VVERB, "send done rsp %"PRIu64" on c %d", msg->id, conn->sd);
   // log_debug(LOG_ERR, "send done rsp %"PRIu64" on c %d", msg->id, conn->sd);

    pmsg = msg->peer;

    ASSERT(!msg->request && pmsg->request);
    ASSERT(pmsg->peer == msg);
    ASSERT(pmsg->done && !pmsg->swallow);

    /* dequeue request from client outq */
    //rsp_send_done从客户端连接conn->dequeue_outq中出对  rsp_forward从服务端连接s_conn->dequeue_outq中出对
    conn->dequeue_outq(ctx, conn, pmsg); 
    //rsp_send_done从客户端连接conn->dequeue_outq中出对  rsp_forward从服务端连接s_conn->dequeue_outq中出对

    req_put(pmsg);
}
