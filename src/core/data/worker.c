#include "worker.h"

#include "core/context.h"
#include "shared.h"

#include "time/time.h"

#include <buffer/cc_buf.h>
#include <buffer/cc_dbuf.h>
#include <cc_debug.h>
#include <cc_event.h>
#include <cc_ring_array.h>
#include <channel/cc_channel.h>
#include <channel/cc_pipe.h>
#include <channel/cc_tcp.h>

#include <stream/cc_sockio.h>

#include <sysexits.h>

#define WORKER_MODULE_NAME "core::worker"

worker_metrics_st *worker_metrics = NULL;

static struct context context;
static struct context *ctx = &context;

static channel_handler_st handlers;
static channel_handler_st *hdl = &handlers;

struct data_processor *processor;

/* the caller only needs to check the return status of this function if
 * it previously received a write event and wants to re-register the
 * read event upon full, successful write.
 */
static inline rstatus_i
_worker_event_write(struct buf_sock *s)
{
    ASSERT(s != NULL);

    rstatus_i status;
    struct tcp_conn *c = s->ch;

    log_verb("writing on buf_sock %p", s);
    status = buf_tcp_write(s);
    if (status == CC_ERETRY || status == CC_EAGAIN) { /* retry write */
        /* by removing current masks and only listen to write event(s), we are
         * effectively stopping processing incoming data until we can write
         * something to the (kernel) buffer for the channel. This is sensible
         * because either the local network or the client is backed up when
         * kernel write buffer is full, and this allows us to propagate back
         * pressure to the sending side.
         */

        event_del(ctx->evb, hdl->wid(c));
        event_add_write(ctx->evb, hdl->wid(c), s);
    } else if (status == CC_ERROR) {
        c->state = CHANNEL_TERM;
    }

    if (processor->write(&s->rbuf, &s->wbuf, &s->data) < 0) {
        log_debug("handler signals channel termination");
        s->ch->state = CHANNEL_TERM;
        return CC_ERROR;
    }

    return status;
}

/* read event over an existing connection */
static inline void
_worker_event_read(struct buf_sock *s)
{
    ASSERT(s != NULL);

    log_verb("reading on buf_sock %p", s);
    /* TODO(kyang): consider refactoring dbuf_tcp_read and buf_tcp_read to have no return status
       at all, since the return status is already given by the connection state */
    buf_tcp_read(s);
    if (processor->read(&s->rbuf, &s->wbuf, &s->data) < 0) {
        log_debug("handler signals channel termination");
        s->ch->state = CHANNEL_TERM;
        return;
    }
    if (buf_rsize(s->wbuf) > 0) {
        log_verb("attempt to write");
        _worker_event_write(s);
    }
}

static void
worker_add_stream(void)
{
    struct buf_sock *s;
    char buf[RING_ARRAY_DEFAULT_CAP]; /* buffer for discarding pipe data */
    int i;
    rstatus_i status;

    /* server pushes connection on to the ring array before writing to the pipe,
     * therefore, we should read from the pipe first and take the connections
     * off the ring array to match the number of bytes received.
     *
     * Once we move server to its own thread, it is possible that there are more
     * connections added to the queue when we are processing, it is OK to wait
     * for the next read event in that case.
     */

    i = pipe_recv(pipe_new, buf, RING_ARRAY_DEFAULT_CAP);
    if (i < 0) { /* errors, do not read from ring array */
        log_warn("not adding new connections due to pipe error");
        return;
    }

    /* each byte in the pipe corresponds to a new connection, which we will
     * now get from the ring array
     */
    for (; i > 0; --i) {
        status = ring_array_pop(&s, conn_new);
        if (status != CC_OK) {
            log_warn("event number does not match conn queue: missing %d conns",
                    i);
            return;
        }
        INCR(worker_metrics, worker_add_stream);
        log_verb("Adding new buf_sock %p to worker thread", s);
        s->owner = ctx;
        s->hdl = hdl;
        event_add_read(ctx->evb, hdl->rid(s->ch), s); /* event activated */
    }
}

static inline void
_worker_pipe_write(void)
{
    ASSERT(pipe_term != NULL);

    ssize_t status = pipe_send(pipe_term, "", 1);

    if (status == 0 || status == CC_EAGAIN) {
        /* retry write */
        log_verb("server core: retry send on pipe");
        event_add_write(ctx->evb, pipe_write_id(pipe_term), NULL);
    } else if (status == CC_ERROR) {
        log_error("could not write to pipe - %s", strerror(pipe_term->err));
    }
}

static void
worker_ret_stream(struct buf_sock *s)
{
    log_info("worker core marking buf_sock %p for return", s);

    /* first clean up states that only worker thread understands,
     * and stop receiving event updates. then it's safe to return to server
     */
    processor->error(&s->rbuf, &s->wbuf, &s->data);
    event_del(ctx->evb, hdl->rid(s->ch));

    /* push buf_sock to queue */
    INCR(worker_metrics, worker_ret_stream);
    if (ring_array_push(&s, conn_term) != CC_OK) {
        /* here we have no choice but to clean up the stream to avoid leak */
        log_error("term connetion queue is full");
        hdl->term(s->ch);
        buf_sock_reset(s);
        buf_sock_return(&s);

        return;
    }
    /* conn_term */
    _worker_pipe_write();
}

static void
_worker_event(void *arg, uint32_t events)
{
    struct buf_sock *s = arg;
    log_verb("worker event %06"PRIX32" with data %p", events, s);

    if (s == NULL) { /* event on pipe */
        if (events & EVENT_READ) { /* new connection from server */
            INCR(worker_metrics, worker_event_read);
            worker_add_stream();
        }
        if (events & EVENT_WRITE) { /* retry return notification */
            INCR(worker_metrics, worker_event_write);
            _worker_pipe_write();
        }
        if (events & EVENT_ERR) {
            INCR(worker_metrics, worker_event_error);
            log_error("error event received on pipe");
        }
    } else {
        /* event on one of the connections */

        if (events & EVENT_READ) {
            log_verb("processing worker read event on buf_sock %p", s);
            INCR(worker_metrics, worker_event_read);
            _worker_event_read(s);
        }
        if (events & EVENT_WRITE) {
            /* got here only when a previous write was incompleted/retried */
            log_verb("processing worker write event on buf_sock %p", s);
            INCR(worker_metrics, worker_event_write);
            if (_worker_event_write(s) == CC_OK) {
                /* write backlog cleared up, re-add read event (only) */
                event_del(ctx->evb, hdl->wid(s->ch));
                event_add_read(ctx->evb, hdl->rid(s->ch), s);
            }
        }
        if (events & EVENT_ERR) {
            s->ch->state = CHANNEL_TERM;
            INCR(worker_metrics, worker_event_error);
        }

        /* TODO(yao): come up with a robust policy about channel connection
         * and pending data. Since an error can either be server (usually
         * memory) issues or client issues (bad syntax etc), or requested (quit)
         * it is hard to determine whether the channel should be immediately
         * closed or not. A simplistic approach might be to always close asap,
         * and clients should not initiate closing unless they have received all
         * their responses. This is not as nice as the TCP half-close behavior,
         * but simpler to implement and probably fine initially.
         */
        if (s->ch->state == CHANNEL_TERM || s->ch->state == CHANNEL_ERROR) {
            worker_ret_stream(s);
        }
    }
}

void
core_worker_setup(worker_options_st *options, worker_metrics_st *metrics)
{
    int timeout = WORKER_TIMEOUT;
    int nevent = WORKER_NEVENT;

    log_info("set up the %s module", WORKER_MODULE_NAME);

    if (worker_init) {
        log_warn("worker has already been setup, re-creating");
        core_worker_teardown();
    }

    worker_metrics = metrics;

    if (options != NULL) {
        timeout = option_uint(&options->worker_timeout);
        nevent = option_uint(&options->worker_nevent);
    }

    ctx->timeout = timeout;
    ctx->evb = event_base_create(nevent, _worker_event);
    if (ctx->evb == NULL) {
        log_crit("failed to setup worker thread core; could not create event_base");
        exit(EX_CONFIG);
    }

    /* worker thread does not handle accept/reject/open/term directly */
    hdl->accept = NULL;
    hdl->reject = NULL;
    hdl->open = NULL;
    hdl->term = (channel_term_fn)tcp_close;
    hdl->recv = (channel_recv_fn)tcp_recv;
    hdl->send = (channel_send_fn)tcp_send;
    hdl->rid = (channel_id_fn)tcp_read_id;
    hdl->wid = (channel_id_fn)tcp_write_id;

    event_add_read(ctx->evb, pipe_read_id(pipe_new), NULL);

    worker_init = true;
}

void
core_worker_teardown(void)
{
    log_info("tear down the %s module", WORKER_MODULE_NAME);

    if (!worker_init) {
        log_warn("%s has never been setup", WORKER_MODULE_NAME);
    } else {
        event_base_destroy(&(ctx->evb));
    }
    worker_metrics = NULL;
    worker_init = false;
}

static rstatus_i
_worker_evwait(void)
{
    int n;

    n = event_wait(ctx->evb, ctx->timeout);
    if (n < 0) {
        return n;
    }

    INCR(worker_metrics, worker_event_loop);
    INCR_N(worker_metrics, worker_event_total, n);
    time_update();

    return CC_OK;
}

void *
core_worker_evloop(void *arg)
{
    processor = arg;

    while (__atomic_load_n(&processor->running, __ATOMIC_ACQUIRE)) {
        if (_worker_evwait() != CC_OK) {
            log_crit("worker core event loop exited due to failure");
            exit(1);
        }
    }

    return NULL;
}
