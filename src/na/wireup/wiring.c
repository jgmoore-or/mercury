#include <err.h>
#include <inttypes.h> /* PRIu32 */
#include <stdalign.h>
#include <stdlib.h> /* calloc, malloc */
#include <string.h> /* memcpy */
#include <time.h> /* clock_gettime(2) */
#include <unistd.h> /* size_t, SIZE_MAX */

#include "../../na/na_plugin.h"
#include "../../hlog/src/hlog.h"
#include "rxpool.h"
#include "tag.h"
#include "util.h"
#include "wiring_impl.h"

struct wire_state {
    const wire_state_t *(*expire)(wiring_t *, wire_t *);
    const wire_state_t *(*wakeup)(wiring_t *, wire_t *);
    const wire_state_t *(*receive)(wiring_t *, wire_t *, const wireup_msg_t *);
    const char *descr;
};

enum {
  WIRE_S_INITIAL
, WIRE_S_LIVE
, WIRE_S_CLOSING
, WIRE_S_FREE
};

HLOG_OUTLET_SHORT_DEFN(wireup_noisy, all);
HLOG_OUTLET_SHORT_DEFN(wireup, wireup_noisy);
HLOG_OUTLET_SHORT_DEFN(wireup_rx, wireup_noisy);
HLOG_OUTLET_SHORT_DEFN(wireup_tx, wireup_noisy);
HLOG_OUTLET_SHORT_DEFN(wireup_ep, wireup_tx);
HLOG_OUTLET_SHORT_DEFN(wireup_req, wireup_noisy);
HLOG_OUTLET_SHORT_DEFN(wire_state, wireup);
HLOG_OUTLET_SHORT_DEFN(reclaim, wireup);
HLOG_OUTLET_SHORT_DEFN(timeout_noisy, all);
HLOG_OUTLET_SHORT_DEFN(interval, timeout_noisy);
HLOG_OUTLET_SHORT_DEFN(timeout, timeout_noisy);
HLOG_OUTLET_SHORT_DEFN(countdown, timeout);

static char wire_no_data;
void * const wire_data_nil = &wire_no_data;

static const ucp_tag_t wireup_start_tag = TAG_CHNL_WIREUP | TAG_ID_MASK;

#define KEEPALIVE_INTERVAL ((uint64_t)1000000000)
#define RETRY_INTERVAL ((uint64_t)1000000000 / 4)
static const uint64_t retry_interval = RETRY_INTERVAL;          // 1/4 second
static const uint64_t keepalive_interval = KEEPALIVE_INTERVAL;  // 1 second
static const uint64_t timeout_interval = UINT64_MAX; // 10 * KEEPALIVE_INTERVAL;

static uint64_t getnanos(void);
static uint64_t gettimeout(void);

static void wireup_rx_req(wiring_t *, const wireup_msg_t *);

static void wireup_stop_internal(wiring_t *, wire_t *, bool);
static void wireup_send_callback(void *, ucs_status_t, void *);
static void wireup_last_send_callback(void *, ucs_status_t, void *);

static wstorage_t *wiring_enlarge(wiring_t *);
static bool wireup_send(wiring_t *, wire_t *);
static const wire_state_t *continue_life(wiring_t *, wire_t *,
    const wireup_msg_t *);
static const wire_state_t *destroy(wiring_t *, wire_t *);
static const wire_state_t *reject_msg(wiring_t *, wire_t *,
    const wireup_msg_t *);
static const wire_state_t *ignore_expire(wiring_t *, wire_t *);
static const wire_state_t *ignore_wakeup(wiring_t *, wire_t *);
static const wire_state_t *send_keepalive(wiring_t *, wire_t *);
static const wire_state_t *retry(wiring_t *, wire_t *);
static const wire_state_t *start_life(wiring_t *, wire_t *,
    const wireup_msg_t *);

static void wiring_timeout_remove(wstorage_t *, wire_t *, int);
static void wiring_timeout_put(wstorage_t *, wire_t *, int, uint64_t);
static wire_t *wiring_timeout_peek(wstorage_t *, int);
static wire_t *wiring_timeout_get(wstorage_t *, int);

static void *wiring_free_request_get(wiring_t *);
static void wiring_outst_request_put(wiring_t *, wiring_request_t *);
static void wiring_free_request_put(wiring_t *, wiring_request_t *);
static bool wiring_requests_check_status(wiring_t *);
static void wiring_requests_discard(wiring_t *);
static void wiring_garbage_init(wiring_garbage_schedule_t *);
static bool wiring_reclaim(wiring_t *, bool, bool *);
static void wiring_closing_put(wiring_t *, sender_id_t);

static wiring_ref_t reclaimed_bin_sentinel;

static wire_state_t state[] = {
  [WIRE_S_INITIAL] = {.expire = ignore_expire,
                      .wakeup = retry,
                      .receive = start_life,
                      .descr = "initial"}
, [WIRE_S_LIVE] = {.expire = destroy,
                   .wakeup = send_keepalive,
                   .receive = continue_life,
                   .descr = "live"}
, [WIRE_S_CLOSING] = {.expire = ignore_expire,
                      .wakeup = ignore_wakeup,
                      .receive = reject_msg,
                      .descr = "closing"}
, [WIRE_S_FREE] = {.expire = ignore_expire,
                   .wakeup = ignore_wakeup,
                   .receive = reject_msg,
                   .descr = "free"}
};

static const char *
timo_string(int which)
{
    switch (which) {
    case timo_expire:
        return "expire";
    case timo_wakeup:
        return "wakeup";
    default:
        return "unknown";
    }
}

static wire_t *
wiring_timeout_peek(wstorage_t *storage, int which)
{
    sender_id_t id;
    timeout_head_t *head = &storage->thead[which];

    if ((id = head->first) == sender_id_nil)
        return NULL;

    assert(id < storage->nwires);

    return &storage->wire[id];
}

static wire_t *
wiring_timeout_get(wstorage_t *storage, int which)
{
    sender_id_t id;
    wire_t *w;
    timeout_head_t *head = &storage->thead[which];
    timeout_link_t *link;

    if ((id = head->first) == sender_id_nil)
        return NULL;

    w = &storage->wire[id];
    link = &w->tlink[which];
    head->first = link->next;

    assert(link->next != id && link->prev != id);

    assert((head->first == sender_id_nil) == (id == head->last));

    if (head->first == sender_id_nil)
        head->last = sender_id_nil;
    else {
        timeout_link_t *lastlink =
            &storage->wire[head->first].tlink[which];
        lastlink->prev = sender_id_nil;
    }

    link->next = link->prev = id;
    return w;
}

static void
wiring_timeout_remove(wstorage_t *storage, wire_t *w, int which)
{
    sender_id_t id = wire_index(storage, w);
    timeout_head_t *head = &storage->thead[which];
    timeout_link_t *link = &w->tlink[which];

    assert(id < storage->nwires);

    assert((link->next == id) == (link->prev == id));

    if (link->next == id) {
        hlog_fast(timeout, "%s: wire %p not present on %s queue",
            __func__, (void *)w, timo_string(which));
        return;
    }

    if (link->next == sender_id_nil) {
        assert(head->last == id);
        head->last = link->prev;
    } else {
        storage->wire[link->next].tlink[which].prev = link->prev;
    }

    if (link->prev == sender_id_nil) {
        assert(head->first == id);
        head->first = link->next;
    } else {
        storage->wire[link->prev].tlink[which].next = link->next;
    }

    hlog_fast(timeout, "%s: wire %p %s %" PRId64,
        __func__, (void *)w, timo_string(which), link->due - getnanos());

    link->due = 0;
    link->next = link->prev = id;
}

static void
wiring_timeout_put(wstorage_t *storage, wire_t *w, int which, uint64_t when)
{
    sender_id_t id = wire_index(storage, w);
    timeout_link_t *link = &w->tlink[which];
    timeout_head_t *head = &storage->thead[which];

    hlog_fast(timeout, "%s: wire %p %s %" PRId64,
        __func__, (void *)w, timo_string(which), when - getnanos());

    link->due = when;
    link->next = sender_id_nil;
    link->prev = head->last;

    if (head->last == sender_id_nil) {
        assert(head->first == sender_id_nil);
        head->first = id;
    } else {
        timeout_link_t *lastlink = &storage->wire[head->last].tlink[which];
        assert(lastlink->due <= when);
        lastlink->next = id;
    }
    head->last = id;
}

static inline void
wiring_expiration_put(wstorage_t *storage, wire_t *w, uint64_t when)
{
    wiring_timeout_put(storage, w, timo_expire, when);
}

static inline wire_t *
wiring_expiration_peek(wstorage_t *storage)
{
    return wiring_timeout_peek(storage, timo_expire);
}

static inline wire_t *
wiring_expiration_get(wstorage_t *storage)
{
    return wiring_timeout_get(storage, timo_expire);
}

static inline void
wiring_expiration_remove(wstorage_t *storage, wire_t *w)
{
    wiring_timeout_remove(storage, w, timo_expire);
}

static inline void
wiring_wakeup_put(wstorage_t *storage, wire_t *w, uint64_t wakeup)
{
    wiring_timeout_put(storage, w, timo_wakeup, wakeup);
}

static inline wire_t *
wiring_wakeup_peek(wstorage_t *storage)
{
    return wiring_timeout_peek(storage, timo_wakeup);
}

static inline wire_t *
wiring_wakeup_get(wstorage_t *storage)
{
    return wiring_timeout_get(storage, timo_wakeup);
}

static inline void
wiring_wakeup_remove(wstorage_t *storage, wire_t *w)
{
    wiring_timeout_remove(storage, w, timo_wakeup);
}

static void *
zalloc(size_t sz)
{
    return calloc(1, sz);
}

/* Return the next larger buffer length to try if `buflen` did not fit a
 * received packet.
 *
 * Twice the message length is twice the header length plus twice the
 * payload length, so subtract one header length to double only the
 * payload length.
 */
static size_t
next_buflen(size_t buflen)
{
        const size_t hdrlen = offsetof(wireup_msg_t, addr[0]);
        if (buflen == 0)
            return sizeof(wireup_msg_t) + 93;
        return twice_or_max(buflen) - hdrlen;
}

static void
wiring_finalize_wire(wiring_t *wiring, wire_t *w)
{
    wireup_msg_t *msg;
    ucp_ep_h ep;

    /* w->msg will not be NULL if `w` made the ->CLOSED transition
     * while a transmission was pending.
     */
    if ((msg = w->msg) != NULL) {
        w->msg = NULL;
        w->msglen = 0;
        free(msg);
    }
    if ((ep = w->ep) != NULL) {
        void *request;
        ucp_request_param_t close_params = {
          .op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS | UCP_OP_ATTR_FIELD_REQUEST
        , .flags = 0 // UCP_EP_CLOSE_FLAG_FORCE
        , .request = wiring_free_request_get(wiring)
        };

        if (close_params.request == NULL) {
            // TBD count/log non-critical error
            return;
        }

        w->ep = NULL;
        request = ucp_ep_close_nbx(ep, &close_params);

        if (UCS_PTR_IS_ERR(request)) {
            hlog_fast(wireup_ep, "%s: ucp_ep_close_nbx: %s", __func__,
                ucs_status_string(UCS_PTR_STATUS(request)));
            wiring_free_request_put(wiring, close_params.request);
        } else if (request == UCS_OK) {
            wiring_free_request_put(wiring, close_params.request);
            hlog_fast(wireup_ep,
                "%s: no outstanding EP close request", __func__);
        } else {
            wiring_outst_request_put(wiring, close_params.request);
            hlog_fast(wireup_ep, "%s: outstanding EP close request %p",
                __func__, (void *)request);
        }
    }
}

static void
wiring_close_wire(wiring_t *wiring, wire_t *w)
{
    wstorage_t *st = wiring->storage;
    sender_id_t id = wire_index(st, w);

    wiring_assert_locked(wiring);

    assert(id < st->nwires);

    wiring->assoc[id] = NULL;   /* TBD move to _finalize_wire? */

    w->id = sender_id_nil;
    wiring_expiration_remove(st, w);
    wiring_wakeup_remove(st, w);
    wiring_closing_put(wiring, id);
}

static void
wireup_storage_transition(wstorage_t *st, wire_t *w, const wire_state_t *nstate)
{
    const wire_state_t *ostate;
    bool reset_cb;

    ostate = w->state;
    w->state = nstate;

    hlog_fast(wire_state, "%s: wire %td state change %s -> %s",
        __func__, w - &st->wire[0], ostate->descr, nstate->descr);

    if (w->cb == NULL || ostate == nstate) {
        reset_cb = false; // no callback or no state change: do nothing
    } else if (nstate == &state[WIRE_S_FREE]) {
        reset_cb = !(*w->cb)((wire_event_info_t){
            .event = wire_ev_reclaimed
          , .ep = NULL
          , .sender_id = sender_id_nil
        }, w->cb_arg);
    } else if (nstate == &state[WIRE_S_CLOSING]) {
        reset_cb = !(*w->cb)((wire_event_info_t){
            .event = wire_ev_closed
          , .ep = NULL
          , .sender_id = sender_id_nil
        }, w->cb_arg);
    } else if (nstate == &state[WIRE_S_LIVE]) {
        reset_cb = !(*w->cb)((wire_event_info_t){
            .event = wire_ev_estd
          , .ep = w->ep
          , .sender_id = w->id
        }, w->cb_arg);
    } else {
        reset_cb = false;
    }

    if (reset_cb) {
        w->cb = NULL;
        w->cb_arg = NULL;
    }
}

static void
wireup_transition(wiring_t *wiring, wire_t *w, const wire_state_t *nstate)
{
    wstorage_t *st = wiring->storage;

    wiring_assert_locked(wiring);

    wireup_storage_transition(st, w, nstate);
}

static void
wireup_msg_transition(wiring_t *wiring, const ucp_tag_t sender_tag,
    const wireup_msg_t *msg)
{
    wstorage_t *st = wiring->storage;
    wire_t *w;
    const uint64_t proto_id = TAG_GET_ID(sender_tag);
    sender_id_t id;

    if (proto_id >= SENDER_ID_MAX) {
        hlog_fast(wireup_rx, "%s: illegal sender ID %" PRIu64, __func__,
            proto_id);
        return;
    }
    if (proto_id >= st->nwires) {
        hlog_fast(wireup_rx, "%s: out of bounds sender ID %" PRIu64,
            __func__, proto_id);
        return;
    }

    id = (sender_id_t)proto_id;
    w = &st->wire[id];

    hlog_fast(wireup_rx, "%s: wire %" PRIuSENDER " %s message",
        __func__, id, wireup_op_string(msg->op));

    wireup_transition(wiring, w, (*w->state->receive)(wiring, w, msg));
}

static void
wireup_wakeup_transition(wiring_t *wiring, uint64_t now)
{
    wstorage_t *st = wiring->storage;
    wire_t *w;

    wiring_assert_locked(wiring);

    while ((w = wiring_wakeup_peek(st)) != NULL) {
        if (w->tlink[timo_wakeup].due > now) {
            hlog_fast(timeout_noisy,
                "%s: stop at wire %td due in %" PRIu64 "ns", __func__,
                w - &st->wire[0], w->tlink[timo_wakeup].due - now);
            break;
        }
        wiring_wakeup_remove(st, w);
        hlog_fast(wire_state, "%s: wire %td woke", __func__, w - &st->wire[0]);
        wireup_transition(wiring, w, (*w->state->wakeup)(wiring, w));
    }
}

static bool
wireup_expire_transition(wiring_t *wiring, uint64_t now)
{
    wstorage_t *st = wiring->storage;
    wire_t *w;
    bool progress = false;

    wiring_assert_locked(wiring);

    while ((w = wiring_expiration_peek(st)) != NULL) {
        if (w->tlink[timo_expire].due > now)
            break;

        progress = true;

        wiring_expiration_remove(st, w);
        hlog_fast(wire_state, "%s: wire %td expired",
            __func__, w - &st->wire[0]);
        wireup_transition(wiring, w, (*w->state->expire)(wiring, w));
    }
    return progress;
}

static const wire_state_t *
start_life(wiring_t *wiring, wire_t *w, const wireup_msg_t *msg)
{
    wstorage_t *st = wiring->storage;
    sender_id_t id = wire_index(st, w);

    if (msg->sender_id >= SENDER_ID_MAX) {
        hlog_fast(wireup_rx,
            "%s: bad foreign sender ID %" PRIu32 " for wire %" PRIuSENDER,
            __func__, msg->sender_id, id);
        return w->state;
    }

    if (msg->op == OP_STOP) {
        wiring_close_wire(wiring, w);
        return &state[WIRE_S_CLOSING];
    } else if (msg->op != OP_ACK) {
        hlog_fast(wireup_rx,
            "%s: unexpected opcode %" PRIu16 " for wire %" PRIuSENDER,
            __func__, msg->op, id);
        return w->state;
    }

    if (msg->addrlen != 0) {
        hlog_fast(wireup_rx,
            "%s: unexpected addr. len. %" PRIu16 " for wire %" PRIuSENDER,
            __func__, msg->addrlen, id);
        return w->state;
    }

    w->id = msg->sender_id;
    free(w->msg);
    w->msg = NULL;
    w->msglen = 0;
    wiring_expiration_remove(st, w);
    wiring_expiration_put(st, w, gettimeout());
    wiring_wakeup_remove(st, w);
    wiring_wakeup_put(st, w, getnanos() + keepalive_interval);

    return &state[WIRE_S_LIVE];
}

static const wire_state_t *
continue_life(wiring_t *wiring, wire_t *w, const wireup_msg_t *msg)
{
    wstorage_t *st = wiring->storage;
    sender_id_t id = wire_index(st, w);

    if (msg->sender_id >= SENDER_ID_MAX) {
        hlog_fast(wireup_rx,
            "%s: bad foreign sender ID %" PRIu32 " for wire %" PRIuSENDER,
            __func__, msg->sender_id, id);
        return w->state;
    }

    if (msg->op == OP_STOP) {
        wiring_close_wire(wiring, w);
        return &state[WIRE_S_CLOSING];
    } else if (msg->op != OP_KEEPALIVE) {
        hlog_fast(wireup_rx,
            "%s: unexpected opcode %" PRIu16 " for wire %" PRIuSENDER,
            __func__, msg->op, id);
        return w->state;
    }

    if (msg->addrlen != 0) {
        hlog_fast(wireup_rx,
            "%s: unexpected addr. len. %" PRIu16 " for wire %" PRIuSENDER,
            __func__, msg->addrlen, id);
        return w->state;
    }

    if (msg->sender_id != (uint32_t)w->id) {
        hlog_fast(wireup_rx,
            "%s: sender ID %" PRIu32 " mismatches assignment %" PRIuSENDER
            " for wire %" PRIuSENDER, __func__, msg->sender_id, w->id, id);
        wiring_close_wire(wiring, w);
        return &state[WIRE_S_CLOSING];
    }

    wiring_expiration_remove(st, w);
    wiring_expiration_put(st, w, gettimeout());

    return &state[WIRE_S_LIVE];
}

static const wire_state_t *
send_keepalive(wiring_t *wiring, wire_t *w)
{
    wstorage_t *st = wiring->storage;
    ucp_request_param_t tx_params;
    wireup_msg_t *msg;
    ucs_status_ptr_t request;
    const ucp_tag_t tag = TAG_CHNL_WIREUP | SHIFTIN(w->id, TAG_ID_MASK);
    const sender_id_t id = wire_index(st, w);

    hlog_fast(wireup_tx, "%s: enter", __func__);

    wiring_assert_locked(wiring);

    if ((msg = zalloc(sizeof(*msg))) == NULL) {
        hlog_fast(wireup_tx, "%s: failed, no memory", __func__);
        return w->state;
    }

    *msg = (wireup_msg_t){.op = OP_KEEPALIVE, .sender_id = id, .addrlen = 0};

    tx_params = (ucp_request_param_t){
      .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                    | UCP_OP_ATTR_FIELD_USER_DATA
                    | UCP_OP_ATTR_FIELD_REQUEST
    , .cb = {.send = wireup_last_send_callback}
    , .user_data = msg
    , .request = wiring_free_request_get(wiring)
    };

    if (tx_params.request == NULL) {
        hlog_fast(wireup_tx, "%s: failed, no requests free", __func__);
        free(msg);
        return w->state;
    }

    request = ucp_tag_send_nbx(w->ep, msg, sizeof(*msg), tag, &tx_params);

    if (UCS_PTR_IS_ERR(request)) {
        hlog_fast(wireup_tx, "%s: ucp_tag_send_nbx: %s", __func__,
            ucs_status_string(UCS_PTR_STATUS(request)));
        wiring_free_request_put(wiring, tx_params.request);
        free(msg);
    } else if (request == UCS_OK) {
        hlog_fast(wireup_tx, "%s: sent immediately", __func__);
        wiring_free_request_put(wiring, tx_params.request);
        free(msg);
    } else {
        hlog_fast(wireup_tx, "%s: enqueued send", __func__);
        wiring_outst_request_put(wiring, tx_params.request);
    }

    wiring_wakeup_put(st, w, getnanos() + keepalive_interval);

    return w->state;
}

static const wire_state_t *
ignore_wakeup(wiring_t *wiring, wire_t *w)
{
    wstorage_t *st = wiring->storage;
    sender_id_t id = wire_index(st, w);

    hlog_fast(wire_state, "%s: ignoring wakeup for wire %" PRIuSENDER,
        __func__, id);

    return w->state;
}

static const wire_state_t *
ignore_expire(wiring_t *wiring, wire_t *w)
{
    wstorage_t *st = wiring->storage;
    sender_id_t id = wire_index(st, w);

    hlog_fast(wire_state, "%s: rejecting expiration for wire %" PRIuSENDER,
        __func__, id);

    return &state[WIRE_S_CLOSING];
}

static const wire_state_t *
reject_msg(wiring_t *wiring, wire_t *w, const wireup_msg_t *msg)
{
    wstorage_t *st = wiring->storage;
    sender_id_t id = wire_index(st, w);

    hlog_fast(wireup_rx,
        "%s: rejecting message from %" PRIuSENDER " for wire %" PRIuSENDER,
        __func__, msg->sender_id, id);

    return &state[WIRE_S_CLOSING];
}

static const wire_state_t *
retry(wiring_t *wiring, wire_t *w)
{
    wstorage_t *st = wiring->storage;
    sender_id_t id = wire_index(st, w);

    wiring_assert_locked(wiring);

    hlog_fast(wire_state, "%s: retrying establishment of wire %" PRIuSENDER,
        __func__, id);

    if (!wireup_send(wiring, w)) {
        wiring_close_wire(wiring, w);
        return &state[WIRE_S_CLOSING];
    }

    wiring_wakeup_put(st, w, getnanos() + retry_interval);

    return &state[WIRE_S_INITIAL];
}

static const wire_state_t *
destroy(wiring_t *wiring, wire_t *w)
{
    wiring_close_wire(wiring, w);
    return &state[WIRE_S_CLOSING];
}

static void
wireup_send_callback(void wiring_unused *request, ucs_status_t status,
    void *user_data)
{
    wireup_msg_t *msg = user_data;

    hlog_fast(wireup_tx,
        "%s: sent id %" PRIu32 " addr. len. %" PRIu16 " status %s",
        __func__, msg->sender_id, msg->addrlen, ucs_status_string(status));
}

static void
wireup_last_send_callback(void wiring_unused *request, ucs_status_t status,
    void *user_data)
{
    wireup_msg_t *msg = user_data;

    hlog_fast(wireup_tx,
        "%s: sent id %" PRIu32 " addr. len. %" PRIu16 " status %s",
        __func__, msg->sender_id, msg->addrlen, ucs_status_string(status));

    free(msg);
}

/* Release all resources belonging to `wiring`.  If `orderly` is true,
 * then alert our peers that we are discarding all of our wires so that
 * they can clean up their local state.
 */
void
wiring_teardown(wiring_t *wiring, bool orderly)
{
    wstorage_t *st;
    void **assoc = wiring->assoc;
    size_t i;

    wiring_assert_locked(wiring);
    st = wiring->storage;
    if (wiring->rxpool != NULL)
        rxpool_destroy(wiring->rxpool);

    for (i = 0; i < st->nwires; i++)
        wireup_stop_internal(wiring, &st->wire[i], orderly);

    while (wiring_requests_check_status(wiring))
        (void)ucp_worker_progress(wiring->worker);

    // no outstanding ops should hold onto garbage.
    if (!wiring_reclaim(wiring, true, NULL))
        hlog_fast(reclaim, "%s: could not reclaim everything", __func__);

    wiring_requests_discard(wiring);

    free(st);
    free(assoc);
}

/* Release all resources belonging to `wiring` and free `wiring` itself.
 * If `orderly` is true, then alert our peers that we are discarding all
 * of our wires so that they can clean up their local state.
 */
void
wiring_destroy(wiring_t *wiring, bool orderly)
{
    wiring_teardown(wiring, orderly);
    free(wiring);
}

static inline bool
wire_is_connected(wiring_t *wiring, wire_id_t wid)
{
    wstorage_t *st = wiring->storage;
    sender_id_t id = atomic_load_explicit(&wid.id, memory_order_relaxed);

    if (id == sender_id_nil || st->nwires <= id)
        return false;

    return st->wire[id].state == &state[WIRE_S_LIVE];
}

/* Return a pointer to the data associated with the wire at slot `wid`.
 * The associated pointer may be NULL.  If there is not a connected wire
 * at `wid`, then return the special pointer `wire_data_nil`.
 *
 * A caller must hold a reference on the wiring (a wiring_ref_t) to
 * avoid racing with a wiring_enlarge() or wiring_teardown() call to
 * access a slot in the associated-data table before it is relocated or
 * freed.
 *
 * There is no need for callers to hold the wiring lock across
 * `wire_get_data` calls.
 */
void *
wire_get_data(wiring_t *wiring, wire_id_t wid)
{
    sender_id_t id = atomic_load_explicit(&wid.id, memory_order_relaxed);

    if (!wire_is_connected(wiring, wid))
        return wire_data_nil;
    /* There is a TOCTOU race here if the caller does not hold a
     * wiring_ref_t.  Also, `assoc` can be freed between the
     * time we load the pointer and the time we dereference it, unless
     * a reference is held.
     */
    return wiring->assoc[id];
}

/* Stop the wireup protocol on the wire at local slot `wid`.  If
 * `orderly` is true, then send the remote peer a message to tell it to
 * shut down its end of the wire; otherwise, send no message.  Return
 * `true` if the wire was shut down, `false` if there is no wire at slot
 * `wid`.
 *
 * Note well: the caller must hold the wiring lock.  See `wiring_lock`,
 * `wiring_unlock`, et cetera.
 */
bool
wireup_stop(wiring_t *wiring, wire_id_t wid, bool orderly)
{
    wiring_assert_locked(wiring);

    wstorage_t *st = wiring->storage;

    if (wid.id == sender_id_nil || st->nwires <= wid.id) {
        return false;
    }

    wireup_stop_internal(wiring, &st->wire[wid.id], orderly);
    return true;
}

static void
wiring_requests_discard(wiring_t *wiring)
{
    wiring_request_t *req;

    while ((req = wiring->req_free_head) != NULL) {
        wiring->req_free_head = req->next;
        header_free(wiring->request_size, alignof(*req), req);
    }

    assert(wiring->req_outst_head == NULL);
    assert(wiring->req_outst_tailp == &wiring->req_outst_head);
}

static void *
wiring_free_request_get(wiring_t *wiring)
{
    wiring_request_t *req;

    wiring_assert_locked(wiring);

    if ((req = wiring->req_free_head) != NULL) {
        wiring->req_free_head = req->next;
    } else if ((req = header_alloc(wiring->request_size, alignof(*req),
                                   sizeof(*req))) == NULL) {
        return NULL;
    }

    return req;
}

static void
wiring_outst_request_put(wiring_t *wiring, wiring_request_t *req)
{
    req->next = NULL;
    *wiring->req_outst_tailp = req;
    wiring->req_outst_tailp = &req->next;
}

static void
wiring_free_request_put(wiring_t *wiring, wiring_request_t *req)
{
    req->next = wiring->req_free_head;
    wiring->req_free_head = req;
}

/* Move the state machine on wire `w` to CLOSING state and release its
 * resources.  If `orderly` is true, then send a STOP message to the peer
 * so that it can release its wire.
 */
static void
wireup_stop_internal(wiring_t *wiring, wire_t *w, bool orderly)
{
    ucp_request_param_t tx_params;
    wireup_msg_t *msg;
    void *request;
    wstorage_t *st = wiring->storage;
    const ucp_tag_t tag = TAG_CHNL_WIREUP | SHIFTIN(w->id, TAG_ID_MASK);
    const sender_id_t id = wire_index(st, w);

    wiring_assert_locked(wiring);

    if (w->state == &state[WIRE_S_CLOSING] || w->state == &state[WIRE_S_FREE])
        goto out;

    wireup_transition(wiring, w, &state[WIRE_S_CLOSING]);

    if (!orderly)
        goto out;

    if ((msg = zalloc(sizeof(*msg))) == NULL)
        goto out;

    *msg = (wireup_msg_t){.op = OP_STOP, .sender_id = id, .addrlen = 0};

    tx_params = (ucp_request_param_t){
      .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                    | UCP_OP_ATTR_FIELD_USER_DATA
                    | UCP_OP_ATTR_FIELD_REQUEST
    , .cb = {.send = wireup_last_send_callback}
    , .user_data = msg
    , .request = wiring_free_request_get(wiring)
    };

    if (tx_params.request == NULL) {
        free(msg);
        goto out;
    }

    request = ucp_tag_send_nbx(w->ep, msg, sizeof(*msg), tag, &tx_params);

    if (UCS_PTR_IS_ERR(request)) {
        hlog_fast(wireup_tx, "%s: ucp_tag_send_nbx: %s", __func__,
            ucs_status_string(UCS_PTR_STATUS(request)));
        free(msg);
        wiring_free_request_put(wiring, tx_params.request);
    } else if (request == UCS_OK) {
        free(msg);
        wiring_free_request_put(wiring, tx_params.request);
    } else {
        wiring_outst_request_put(wiring, tx_params.request);
    }

out:
    wiring_close_wire(wiring, w);
}

/* Check the head of the outstanding requests list.  Move completed
 * requests from the head of the outstanding list to the free list.
 * Return true if there are any requests outstanding.  Otherwise, return
 * false.
 */
static bool
wiring_requests_check_status(wiring_t *wiring)
{
    wiring_request_t *req;

    while ((req = wiring->req_outst_head) != NULL) {
        if (ucp_request_check_status(req) == UCS_INPROGRESS) {
            hlog_fast(wireup_req, "%s: request %p in-progress", __func__,
                (void *)req);
            return true;
        }

        wiring->req_outst_head = req->next;
        if (wiring->req_outst_tailp == &req->next)
            wiring->req_outst_tailp = &wiring->req_outst_head;

        wiring_free_request_put(wiring, req);

        hlog_fast(wireup_req, "%s: reclaimed request %p", __func__,
            (void *)req);
    }

    return false;
}

/* Initialize `wiring` both to answer and to originate wiring requests
 * using `worker`.  Reserve `request_sizes` bytes for UCP-private
 * members of the `ucp_request_t`s used for wireup.  Call `accept_cb`,
 * if it is not NULL, passing `accept_cb_arg` in the second argument,
 * after accepting a wiring request from a remote peer.
 */
bool
wiring_init(wiring_t *wiring, ucp_worker_h worker, size_t request_size,
    wire_accept_cb_t accept_cb, void *accept_cb_arg)
{
    wstorage_t *st;
    const sender_id_t nwires = 1;
    int which;
    sender_id_t i;
    void **assoc;

    hlog_fast(countdown, "%s: countdown initial log", __func__);

    wiring->accept_cb = accept_cb;
    wiring->accept_cb_arg = accept_cb_arg;
    wiring->worker = worker;
    wiring->request_size = request_size;
    wiring->req_free_head = wiring->req_outst_head = NULL;
    wiring->req_outst_tailp = &wiring->req_outst_head;
    wiring->mtx = (hg_thread_mutex_t)HG_THREAD_MUTEX_INITIALIZER;

    st = zalloc(sizeof(*st) + sizeof(wire_t) * nwires);
    if (st == NULL)
        return false;

    assoc = zalloc(sizeof(*assoc) * nwires);
    if (assoc == NULL) {
        free(st);
        return false;
    }
    wiring->storage = st;
    wiring->assoc = assoc;

    st->nwires = nwires;

    for (i = 0; i < nwires; i++) {
        st->wire[i] = (wire_t){
              .next = i + 1
            , .state = &state[WIRE_S_FREE]
            , .tlink = {{.prev = i, .next = i, .due = 0},
                        {.prev = i, .next = i, .due = 0}}
            , .ep = NULL
            , .id = sender_id_nil};
    }

    st->wire[nwires - 1].next = sender_id_nil;
    st->first_free = 0;

    for (which = 0; which < timo_nlinks; which++)
        st->thead[which].first = st->thead[which].last = sender_id_nil;

    wiring_lock(wiring);

    wiring->rxpool = rxpool_create(worker, next_buflen, request_size,
        TAG_CHNL_WIREUP, TAG_CHNL_MASK, 32);

    if (wiring->rxpool == NULL) {
        wiring_teardown(wiring, true);
        wiring_unlock(wiring);
        return false;
    }

    wiring_garbage_init(&wiring->garbage_sched);

    wiring_unlock(wiring);

    return true;
}

/* Allocate a `wiring_t` and initialize it.  On success, return the new
 * `wiring_t`.  On failure, return NULL.  All parameters are forwarded
 * to `wiring_init`; see its documentation for the meaning of the
 * parameters.
 */
wiring_t *
wiring_create(ucp_worker_h worker, size_t request_size,
    wire_accept_cb_t accept_cb, void *accept_cb_arg)
{
    wiring_t *wiring;

    if ((wiring = malloc(sizeof(*wiring))) == NULL)
        return NULL;

    if (!wiring_init(wiring, worker, request_size, accept_cb, accept_cb_arg)) {
        free(wiring);
        return NULL;
    }

    return wiring;
}

static void
wiring_garbage_add(wiring_t *wiring, wstorage_t *storage, void **assoc)
{
    wiring_garbage_schedule_t *sched = &wiring->garbage_sched;
    uint64_t last;
    wiring_garbage_bin_t *bin;

    wiring_assert_locked(wiring);

    while ((last = sched->epoch.last) - sched->epoch.first == NELTS(sched->bin))
        wiring_reclaim(wiring, false, NULL);

    hlog_fast(reclaim, "%s: adding storage %p assoc %p epoch %" PRIu64
        " bin %" PRIu64, __func__, (void *)storage, (void *)assoc, last,
        last % NELTS(sched->bin));

    bin = &sched->bin[last % NELTS(sched->bin)];

    assert(bin->assoc == NULL && bin->storage == NULL);
    bin->storage = storage;
    bin->assoc = assoc;
    sched->epoch.last = last + 1;

    atomic_fetch_add_explicit(&sched->work_available, 1, memory_order_relaxed);
}

static wstorage_t *
wiring_enlarge(wiring_t *wiring)
{
    void **nassoc, ** const oassoc = wiring->assoc;
    wstorage_t *nst, * const ost = wiring->storage;
    const size_t hdrsize = sizeof(wstorage_t),
                 osize = hdrsize + ost->nwires * sizeof(wire_t);
    const size_t proto_nsize = twice_or_max(osize) - hdrsize;
    const sender_id_t nwires = (sender_id_t)MIN(SENDER_ID_MAX - 1,
                                   (proto_nsize - hdrsize) / sizeof(wire_t));
    const size_t nsize = hdrsize + nwires * sizeof(wire_t);
    sender_id_t i;

    wiring_assert_locked(wiring);

    if (nsize <= osize)
        return NULL;

    if ((nst = malloc(nsize)) == NULL)
        return NULL;

    if ((nassoc = malloc(nwires * sizeof(*nassoc))) == NULL) {
        free(nst);
        return NULL;
    }

    memcpy(nst, ost, osize);
    memcpy(nassoc, oassoc, ost->nwires * sizeof(*nassoc));

    for (i = ost->nwires; i < nwires; i++) {
        nassoc[i] = NULL;
        nst->wire[i] = (wire_t){
              .next = i + 1
            , .state = &state[WIRE_S_FREE]
            , .tlink = {{.prev = i, .next = i, .due = 0},
                        {.prev = i, .next = i, .due = 0}}
            , .ep = NULL
            , .id = sender_id_nil};
    }

    nst->wire[nwires - 1].next = ost->first_free;
    nst->first_free = ost->nwires;
    nst->nwires = nwires;

    wiring->assoc = nassoc;
    wiring->storage = nst;

    wiring_garbage_add(wiring, ost, oassoc);

    return nst;
}

static uint64_t
getnanos(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        err(EXIT_FAILURE, "%s: clock_gettime", __func__);

    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

/* Return the current time plus the timeout interval or UINT64_MAX,
 * whichever is smaller, protecting against overflow.  Since I use
 * timeout_interval == UINT64_MAX to disable timeouts, overflow is
 * a real possibility.
 */
static uint64_t
gettimeout(void)
{
    const uint64_t nanos = getnanos();

    if (UINT64_MAX - nanos < timeout_interval)
        return UINT64_MAX;

    return getnanos() + timeout_interval;
}

const char *
wireup_op_string(wireup_op_t op)
{
    switch (op) {
    case OP_ACK:
        return "ack";
    case OP_KEEPALIVE:
        return "keepalive";
    case OP_REQ:
        return "req";
    case OP_STOP:
        return "stop";
    default:
        return "unknown";
    }
}

/* Answer a request. */
static wire_t *
wireup_respond(wiring_t *wiring, sender_id_t rid,
    const ucp_address_t *raddr, size_t wiring_unused raddrlen)
{
    const ucp_ep_params_t ep_params = {
      .field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                    UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE
    , .address = raddr
    , .err_mode = UCP_ERR_HANDLING_MODE_NONE
    };
    ucp_request_param_t tx_params;
    wstorage_t *st = wiring->storage;
    wireup_msg_t *msg;
    wire_t *w;
    ucp_ep_h ep;
    const ucp_tag_t tag = TAG_CHNL_WIREUP | SHIFTIN(rid, TAG_ID_MASK);
    ucs_status_ptr_t request;
    sender_id_t id;
    const size_t msglen = sizeof(*msg);
    ucs_status_t status;

    wiring_assert_locked(wiring);

    if ((msg = zalloc(msglen)) == NULL) {
        hlog_fast(wireup_tx, "%s: failed, no memory", __func__);
        return NULL;
    }

    if ((id = wiring_free_get(st)) == sender_id_nil) {
        if ((st = wiring_enlarge(wiring)) == NULL) {
            hlog_fast(wireup, "%s.%d: failed, no free wire",
                __func__, __LINE__);
            goto free_msg;
        }
        if ((id = wiring_free_get(st)) == sender_id_nil) {
            hlog_fast(wireup, "%s.%d: failed, no free wire",
                __func__, __LINE__);
            goto free_msg;
        }
    }

    w = &st->wire[id];

    *msg = (wireup_msg_t){.op = OP_ACK, .sender_id = id, .addrlen = 0};

    status = ucp_ep_create(wiring->rxpool->worker, &ep_params, &ep);
    if (status != UCS_OK) {
        hlog_fast(wireup_ep,
            "%s: ucp_ep_create: %s", __func__, ucs_status_string(status));
        goto free_wire;
    }
    *w = (wire_t){.ep = ep, .id = rid, .state = &state[WIRE_S_LIVE]};

    wiring_expiration_put(st, w, gettimeout());
    wiring_wakeup_put(st, w, getnanos() + keepalive_interval);

    tx_params = (ucp_request_param_t){
      .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                    | UCP_OP_ATTR_FIELD_USER_DATA
                    | UCP_OP_ATTR_FIELD_REQUEST
    , .cb = {.send = wireup_last_send_callback}
    , .user_data = msg
    , .request = wiring_free_request_get(wiring)
    };

    if (tx_params.request == NULL) {
        hlog_fast(wireup_tx, "%s: failed, no requests free", __func__);
        goto close_wire;
    }

    request = ucp_tag_send_nbx(ep, msg, msglen, tag, &tx_params);

    if (UCS_PTR_IS_ERR(request)) {
        hlog_fast(wireup_tx, "%s: ucp_tag_send_nbx: %s", __func__,
            ucs_status_string(UCS_PTR_STATUS(request)));
        wiring_free_request_put(wiring, tx_params.request);
        goto close_wire;
    } else if (request == UCS_OK) {
        hlog_fast(wireup_tx, "%s: sent immediately", __func__);
        wiring_free_request_put(wiring, tx_params.request);
        free(msg);
    } else {
        hlog_fast(wireup_tx, "%s: enqueued send", __func__);
        wiring_outst_request_put(wiring, tx_params.request);
    }

    if (wiring->accept_cb != NULL) {
        const wire_accept_info_t info =
            {.addr = raddr, .addrlen = raddrlen, .wire_id = {.id = id},
             .sender_id = rid, .ep = ep};
        wiring->assoc[id] = (*wiring->accept_cb)(info, wiring->accept_cb_arg,
            &w->cb, &w->cb_arg);
    }
    return w;
close_wire:
    wiring_close_wire(wiring, w);
    free(msg);
    return NULL;
free_wire:
    wiring_free_put(st, id);
free_msg:
    free(msg);
    return NULL;
}

static bool
wireup_send(wiring_t *wiring, wire_t *w)
{
    ucp_ep_h ep = w->ep;
    wireup_msg_t *msg = w->msg;
    ucs_status_ptr_t request;
    size_t msglen = w->msglen;

    wiring_assert_locked(wiring);

    ucp_request_param_t tx_params = {
      .op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK
                    | UCP_OP_ATTR_FIELD_USER_DATA
                    | UCP_OP_ATTR_FIELD_REQUEST
    , .cb = {.send = wireup_send_callback}
    , .user_data = msg
    , .request = wiring_free_request_get(wiring)
    };

    if (tx_params.request == NULL)
        return false;

    request = ucp_tag_send_nbx(ep, msg, msglen, wireup_start_tag, &tx_params);

    if (UCS_PTR_IS_ERR(request)) {
        hlog_fast(wireup_tx, "%s: ucp_tag_send_nbx: %s", __func__,
            ucs_status_string(UCS_PTR_STATUS(request)));
        wiring_free_request_put(wiring, tx_params.request);
        return false;
    } else if (request == UCS_OK) {
        wiring_free_request_put(wiring, tx_params.request);
    } else {
        wiring_outst_request_put(wiring, tx_params.request);
    }
    return true;
}

/* Acquire the wiring lock if one was established by
 * `wiring_create`/`wiring_init`.  Otherwise, do nothing.
 */
hg_thread_mutex_t *
wiring_lock(wiring_t *wiring)
{
    const int NA_DEBUG_USED rc = hg_thread_mutex_lock(&wiring->mtx);

    assert(rc == HG_UTIL_SUCCESS);
    return &wiring->mtx;
}

/* Release the wiring lock if one was established by
 * `wiring_create`/`wiring_init`.  Otherwise, do nothing.
 */
void
wiring_unlock(wiring_t *wiring)
{
    const int NA_DEBUG_USED rc = hg_thread_mutex_unlock(&wiring->mtx);

    assert(rc == HG_UTIL_SUCCESS);
}

#if 0
void
wiring_assert_locked_impl(wiring_t *wiring,
    const char *filename, int lineno)
{
    hg_thread_mutex_t *mtx = &wiring->mtx;
    const int rc = hg_thread_mutex_try_lock(mtx);

    if (rc == HG_UTIL_SUCCESS) {
        (void)hg_thread_mutex_unlock(mtx);
        fprintf(stderr, "%s.%d: wiring %p is unlocked, aborting.\n",
            filename, lineno, (void *)wiring);
        abort();
    }
}
#else
void
wiring_assert_locked_impl(wiring_t NA_UNUSED *wiring,
    const char NA_UNUSED *filename, int NA_UNUSED lineno)
{
    return;
}
#endif

/* Initiate wireup: create a wire, configure an endpoint for `raddr`, send
 * a message to the endpoint telling our wire's Sender ID and our address,
 * `laddr`.
 *
 * The length of the addresses is given by `laddrlen` and
 * `raddrlen`.
 *
 * If non-NULL, wireup calls `cb` with the argument `cb_arg` whenever the
 * new wire changes state (closed -> established, established -> closed,
 * closed -> reclaimed).  Calls to `cb` are serialized by `wireup_once()`.
 *
 * The wire's associated-data pointer is initialized to `data`.
 *
 * Note well: the caller must hold the wiring lock.  See `wiring_lock`,
 * `wiring_unlock`, et cetera.
 */
wire_id_t
wireup_start(wiring_t * const wiring, ucp_address_t *laddr, size_t laddrlen,
    ucp_address_t *raddr, size_t wiring_unused raddrlen,
    wire_event_cb_t cb, void *cb_arg, void *data)
{
    const ucp_ep_params_t ep_params = {
      .field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                    UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE
    , .address = raddr
    , .err_mode = UCP_ERR_HANDLING_MODE_NONE
    };
    wireup_msg_t *msg;
    wire_t *w;
    ucp_ep_h ep;
    sender_id_t id;
    wstorage_t *st;
    const size_t msglen = sizeof(*msg) + laddrlen;
    ucs_status_t status;

    if (UINT16_MAX < laddrlen) {
        hlog_fast(wireup,
            "%s: local address too long (%zu)", __func__, laddrlen);
        return (wire_id_t){.id = sender_id_nil};
    }

    if ((msg = zalloc(msglen)) == NULL)
        return (wire_id_t){.id = sender_id_nil};

    status = ucp_ep_create(wiring->rxpool->worker, &ep_params, &ep);
    if (status != UCS_OK) {
        hlog_fast(wireup_ep,
            "%s: ucp_ep_create: %s", __func__, ucs_status_string(status));
        goto free_msg;
    }

    wiring_assert_locked(wiring);

    st = wiring->storage;   // storage could change if we don't hold the lock

    if ((id = wiring_free_get(st)) == sender_id_nil) {
        if ((st = wiring_enlarge(wiring)) == NULL)
            goto free_msg;
        if ((id = wiring_free_get(st)) == sender_id_nil)
            goto free_msg;
    }

    w = &st->wire[id];

    *msg = (wireup_msg_t){.op = OP_REQ, .sender_id = id,
                          .addrlen = (uint16_t)laddrlen};
    memcpy(&msg->addr[0], laddr, laddrlen);

    wiring->assoc[id] = data;
    *w = (wire_t){.ep = ep, .id = sender_id_nil,
        .state = &state[WIRE_S_INITIAL], .msg = msg, .msglen = msglen,
        .cb = cb, .cb_arg = cb_arg};

    wiring_expiration_put(st, w, gettimeout());
    wiring_wakeup_put(st, w, getnanos() + retry_interval);

    if (!wireup_send(wiring, w)) {
        w->state = &state[WIRE_S_CLOSING];
        wiring_close_wire(wiring, w);
        return (wire_id_t){.id = sender_id_nil};
    }

    return (wire_id_t){.id = id};
free_msg:
    free(msg);
    return (wire_id_t){.id = sender_id_nil};
}

static void
wireup_rx_msg(wiring_t * const wiring, const ucp_tag_t sender_tag,
    const void *buf, size_t buflen)
{
    const wireup_msg_t *msg;
    const size_t hdrlen = offsetof(wireup_msg_t, addr[0]);
    wireup_op_t op;

    hlog_fast(wireup_rx, "%s: %zu-byte message", __func__, buflen);

    assert((sender_tag & TAG_CHNL_MASK) == TAG_CHNL_WIREUP);

    if (buflen < hdrlen) {
        hlog_fast(wireup_rx, "%s: message shorter than header, dropping",
            __func__);
        return;
    }

    msg = buf;

    switch (msg->op) {
    case OP_ACK:
    case OP_KEEPALIVE:
    case OP_REQ:
    case OP_STOP:
        op = msg->op;
        break;
    default:
        hlog_fast(wireup_rx,
            "%s: unexpected opcode %" PRIu16 ", dropping", __func__, msg->op);
        return;
    }

    if (buflen < offsetof(wireup_msg_t, addr[0]) + msg->addrlen) {
        hlog_fast(wireup_rx, "%s: address truncated, dropping", __func__);
        return;
    }

    switch (op) {
    case OP_REQ:
        wireup_rx_req(wiring, msg);
        break;
    case OP_ACK:
    case OP_KEEPALIVE:
    case OP_STOP:
        wireup_msg_transition(wiring, sender_tag, msg);
        break;
    }
}

static void
wireup_rx_req(wiring_t *wiring, const wireup_msg_t *msg)
{
    wire_t *w;

    /* XXX In principle, can't the empty string be a valid address? */
    if (msg->addrlen == 0) {
        hlog_fast(wireup_rx, "%s: empty address, dropping", __func__);
        return;
    }

    if (SENDER_ID_MAX <= msg->sender_id) {
        hlog_fast(wireup_rx, "%s: sender ID too large, dropping", __func__);
        return;
    }
    w = wireup_respond(wiring, (sender_id_t)msg->sender_id,
       (const void *)&msg->addr[0], msg->addrlen);

    if (w == NULL) {
        hlog_fast(wireup_rx, "%s: failed to prepare & send wireup response",
            __func__);
        return;
    }

    hlog_fast(wireup_rx, "%s: wire %td, sender id %" PRIuSENDER, __func__,
        w - &wiring->storage->wire[0], w->id);
}

static int
wireup_once_locked(wiring_t *wiring, rxdesc_t *rdesc)
{
    rxpool_t *rxpool = wiring->rxpool;
    uint64_t now = getnanos();
    bool progress;

    wiring_assert_locked(wiring);

    /* Wakeup does not affect the progress determination because
     * no wire changes state.
     */
    wireup_wakeup_transition(wiring, now);
    progress = wireup_expire_transition(wiring, now);

    /* Reclaim requests for any transmissions / endpoint closures.
     * Request reclamation does not affect the progress determination.
     */
    (void)wiring_requests_check_status(wiring);

    wiring_reclaim(wiring, false, &progress);

    if (rdesc == NULL)
        return progress ? 1 : 0;

    if (rdesc->status != UCS_OK) {
        hlog_fast(wireup_rx, "%s: receive error, %s, exiting.",
            __func__, ucs_status_string(rdesc->status));
        return -1;
    }

    hlog_fast(wireup_rx,
        "%s: received %zu-byte message tagged %" PRIu64 ", processing...",
        __func__, rdesc->rxlen, rdesc->sender_tag);
    wireup_rx_msg(wiring, rdesc->sender_tag, rdesc->buf, rdesc->rxlen);

    rxdesc_release(rxpool, rdesc);

    return 1;
}

/* Poll for and process received wireup messages, update the state of
 * all wires based on the elapsed time and the messages received,
 * send any replies or keepalives that are due, and collect disused
 * resources.
 *
 * If any progress was made, return 1.  If no progress was made, and no
 * error occurred, return 0.  Return -1 on an unrecoverable error.  Note
 * well: after an unrecoverable error occurs, routines called on the
 * `wiring_t` will have undefined results.
 *
 * Note well: the caller must hold the wiring lock.  See `wiring_lock`,
 * `wiring_unlock`, et cetera.
 */
int
wireup_once(wiring_t *wiring)
{
    int ret;
    bool progress = false;
    rxpool_t *rxpool = wiring->rxpool;
    rxdesc_t *rdesc;

    if ((rdesc = rxpool_next(rxpool)) == NULL &&
        !atomic_load_explicit(&wiring->ready_to_progress, memory_order_relaxed))
        return 0;

    wiring_lock(wiring);
#if 1
    atomic_store_explicit(&wiring->ready_to_progress, false,
        memory_order_relaxed);
#endif
    while ((ret = wireup_once_locked(wiring, rdesc)) > 0) {
        progress = true;
        rdesc = rxpool_next(rxpool);
    }
    wiring_unlock(wiring);

    if (ret < 0)
        return ret;

    return progress ? 1 : 0;
}

/* Store at `maskp` and `atagp` the mask and tag that wireup reserves
 * for the application program.  For each application message tag,
 * `tag`, `tag & *maskp` must equal `*atagp`.
 *
 * All bits in the mask are consecutive.  The bits include either the
 * most-significant bit or the least-significant bit.
 *
 * If either pointer is NULL, don't try to write through it.
 */
void
wireup_app_tag(wiring_t wiring_unused *wiring, uint64_t *atagp, uint64_t *maskp)
{
    if (atagp != NULL)
        *atagp = TAG_CHNL_APP;
    if (maskp != NULL)
        *maskp = TAG_CHNL_MASK;
}

/* Return a string with (static storage duration) that describes `ev`.
 * If `ev` is unknown, then return "unknown".
 */
const char *
wire_event_string(wire_event_t ev)
{
    switch (ev) {
    case wire_ev_closed:
        return "closed";
    case wire_ev_estd:
        return "estd";
    case wire_ev_reclaimed:
        return "reclaimed";
    default:
        return "unknown";
    }
}

static void
wiring_garbage_init(wiring_garbage_schedule_t *sched)
{
    size_t i;

    for (i = 0; i < NELTS(sched->bin); i++) {
        sched->bin[i] = (wiring_garbage_bin_t){
          .first_ref = NULL
        , .first_closed = sender_id_nil
        , .assoc = NULL
        , .storage = NULL
        };
    }
    sched->epoch.first = sched->epoch.last = 0;
}

/* Initialize the reference `ref` for use by `wiring_ref_get` and
 * `wiring_ref_put`.  When the caller has finished with `ref`---the
 * caller will no longer `_get` or `_put` it---then it should call
 * `wiring_ref_free` to mark it for destruction, later.  Wiring will
 * call back on `reclaim` when it is time for the user to reclaim
 * resources tied with `ref`.
 */
void
wiring_ref_init(wiring_t *wiring, wiring_ref_t *ref,
    void (*reclaim)(wiring_ref_t *))
{
    wiring_garbage_schedule_t *sched = &wiring->garbage_sched;
    wiring_garbage_bin_t *bin;

    ref->reclaim = reclaim;
    atomic_store_explicit(&ref->busy, false, memory_order_relaxed);

    do {
        uint64_t epoch = atomic_load_explicit(&sched->epoch.last,
            memory_order_acquire);
        bin = &sched->bin[epoch % NELTS(sched->bin)];

        /* Do not add a reference to a reclaimed bin.  The last bin can
         * be reclaimed in the unlikely event that one or more threads
         * race in between our loading `epoch.last` and updating it,
         * advancing `epoch.first` over our bin.  
         */

        ref->next = atomic_load_explicit(&bin->first_ref, memory_order_acquire);
        if (ref->next == &reclaimed_bin_sentinel)
            continue;

        atomic_store_explicit(&ref->epoch, epoch, memory_order_release);

    } while (!atomic_compare_exchange_weak_explicit(&bin->first_ref,
                 &ref->next, ref, memory_order_acq_rel, memory_order_acquire));
}

static void
wiring_ref_reclaim(wiring_ref_t *ref)
{
    (*ref->reclaim)(ref);
}

static inline bool
wiring_ref_holds_epoch(const wiring_ref_t *ref, uint64_t epoch_in_past)
{
    /* If `ref` has adopted a later epoch than `epoch_in_past`,
     * then it does not hold `epoch_in_past`.
     */
    if (ref->epoch > epoch_in_past)
        return false;
    /* If `ref` is not busy, then it will adopt an epoch later than
     * `epoch_in_past` once it is acquired, and it does not hold
     * `epoch_in_past`, now.
     *
     * If `ref` is busy, then it may not have adopted an epoch later
     * than `epoch_in_past`, yet.  Return true to be on the safe side.
     */
    return ref->busy;
}

static bool
wiring_reclaim_bin_for_epoch(wiring_t *wiring,
    uint64_t epoch, uint64_t last_epoch, bool *progressp)
{
    wstorage_t *st = wiring->storage;
    wiring_garbage_schedule_t *sched = &wiring->garbage_sched;
    const size_t nbins = NELTS(sched->bin);
    wiring_garbage_bin_t *bin = &sched->bin[epoch % nbins];
    sender_id_t id, next_id;
    wiring_ref_t *ref;

    while ((ref = bin->first_ref) != NULL) {
        wiring_garbage_bin_t *newbin;

        if (wiring_ref_holds_epoch(ref, epoch)) {
            hlog_fast(reclaim, "%s: ref %p holds epoch %" PRIu64,
                __func__, (void *)ref, epoch);
            break;
        }

        if (!atomic_compare_exchange_weak(&bin->first_ref, &ref, ref->next))
            continue;

        newbin = &sched->bin[last_epoch % nbins];
        if (ref->epoch == UINT64_MAX) {
            hlog_fast(reclaim, "%s: reclaiming ref %p", __func__, (void *)ref);
            wiring_ref_reclaim(ref);
            continue;
        }

        hlog_fast(reclaim, "%s: moving ref %p, bin %td -> %td",
            __func__, (void *)ref, bin - &sched->bin[0],
            newbin - &sched->bin[0]);

        ref->next = newbin->first_ref;
        while (!atomic_compare_exchange_weak(&newbin->first_ref,
                                             &ref->next, ref))
            ;   // do nothing
    }

    if (ref != NULL)
        return false;

    for (id = bin->first_closed; id != sender_id_nil; id = next_id) {
        wire_t *w = &st->wire[id];
        next_id = st->wire[id].next;

        hlog_fast(reclaim, "%s: finalizing wire %p", __func__, (void *)w);
        if (progressp != NULL)
            *progressp = true;
        wiring_finalize_wire(wiring, w);
        wireup_transition(wiring, w, &state[WIRE_S_FREE]);

        hlog_fast(reclaim, "%s: freeing wire %" PRIuSENDER, __func__, id);
        wiring_free_put(st, id);
    }
    bin->first_closed = sender_id_nil;
    if (bin->storage != NULL) {
        hlog_fast(reclaim, "%s: reclaiming wstorage_t %p",
            __func__, (void *)bin->storage);
        free(bin->storage);
        bin->storage = NULL;
    }
    if (bin->assoc != NULL) {
        hlog_fast(reclaim, "%s: reclaiming assoc. data %p",
            __func__, (void *)bin->assoc);
        free(bin->assoc);
        bin->assoc = NULL;
    }

    bin->first_ref = &reclaimed_bin_sentinel;
    return true;
}

static void
wiring_closing_put(wiring_t *wiring, sender_id_t id)
{
    wstorage_t *st = wiring->storage;
    wiring_garbage_schedule_t *sched = &wiring->garbage_sched;
    const size_t nbins = NELTS(sched->bin);
    const uint64_t epoch = sched->epoch.last;
    wiring_garbage_bin_t *bin = &sched->bin[epoch % nbins];
    wire_t *w = &st->wire[id];

    wiring_assert_locked(wiring);

    w->next = bin->first_closed;
    bin->first_closed = id;
}

static bool
wiring_reclaim(wiring_t *wiring, bool finalize, bool *progressp)
{
    wiring_garbage_schedule_t *sched = &wiring->garbage_sched;
    uint64_t epoch;

    const uint64_t work_available =
        atomic_load_explicit(&sched->work_available, memory_order_relaxed);

    if (finalize)
        hlog_fast(reclaim, "%s: finalizing", __func__);
    else if (work_available == 0)
        return true;

    hlog_fast(reclaim, "%s: work is available", __func__);

    const uint64_t first = sched->epoch.first, last = sched->epoch.last;

    wiring_assert_locked(wiring);

    for (epoch = first; epoch != last; epoch++) {
        hlog_fast(reclaim, "%s: reclaiming epoch %" PRIu64
            " in [%" PRIu64 ", %" PRIu64 "]", __func__, epoch, first, last);

        if (!wiring_reclaim_bin_for_epoch(wiring, epoch, last, progressp))
            break;
    }
    if (sched->epoch.first != epoch)
        sched->epoch.first = epoch;

    atomic_fetch_add_explicit(&sched->work_available, -work_available,
        memory_order_relaxed);

    if (!finalize)
        return true;
    if (sched->epoch.first < sched->epoch.last)
        return false;
    return wiring_reclaim_bin_for_epoch(wiring, epoch, epoch, progressp);
}
