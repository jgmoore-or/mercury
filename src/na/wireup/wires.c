#include <assert.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>   /* PRIx8, etc. */
#include <libgen.h>     /* basename */
#include <pthread.h>    /* pthread_mutex_t */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>     /* sigaction */

#include <sys/param.h>  /* for MIN, MAX */

#include <ucp/api/ucp.h>

#include "rxpool.h"
#include "util.h"
#include "wiring.h"

static sig_atomic_t go = 1;
 
static void
handle_intr(int wiring_unused signo)
{
    go = 0;
}

static void
usage(const char *_progname)
{
    char *progname = strdup(_progname);
    assert(progname != NULL);
    fprintf(stderr, "usage: %s [remote address]\n", basename(progname));
    free(progname);
    exit(EXIT_FAILURE);
}

static bool
event_cb(wire_event_info_t evinfo, void wiring_unused *arg)
{
    printf("wire event '%s'\n", wire_event_string(evinfo.event));
    return true;
}

static bool
run_client(wiring_t *wiring, ucp_worker_h wiring_unused worker,
    ucp_address_t *laddr, size_t laddrlen,
    ucp_address_t *raddr, size_t raddrlen)
{
    wire_id_t id = wireup_start(wiring, laddr, laddrlen, raddr, raddrlen,
        event_cb, NULL, NULL);
    return wire_is_valid(id);
}

static void
custom_lock(wiring_t wiring_unused *wiring, void *arg)
{
    pthread_mutex_t *mtx = arg;

    const int rc = pthread_mutex_lock(mtx);
    assert(rc == 0);
}

static void
custom_unlock(wiring_t wiring_unused *wiring, void *arg)
{
    pthread_mutex_t *mtx = arg;

    const int rc = pthread_mutex_unlock(mtx);
    assert(rc == 0);
}

static bool
custom_assert_locked(wiring_t wiring_unused *wiring, void *arg)
{
    pthread_mutex_t *mtx = arg;
    const int rc = pthread_mutex_trylock(mtx);

    if (rc == EBUSY)
        return true;
    if (rc == 0) {
        pthread_mutex_unlock(mtx);
        return false;
    }
    abort();
}

int
main(int argc, char **argv)
{
    ucp_context_attr_t context_attrs;
    ucp_params_t global_params = {
      .field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_REQUEST_SIZE
    , .features = UCP_FEATURE_TAG | UCP_FEATURE_RMA
    , .request_size = sizeof(rxdesc_t)
    };
    ucp_worker_params_t worker_params = {
      .field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE
    , .thread_mode = UCS_THREAD_MODE_MULTI
    };
    struct sigaction sa = {.sa_handler = handle_intr}, osa;
    wiring_t *wiring;
    ucp_config_t *config;
    ucp_context_h context;
    ucp_worker_h worker;
    ucp_address_t *laddr;
    ucp_address_t *raddr;
    const char *delim = "";
    size_t i, laddrlen, raddrlen;
    ucs_status_t status;
    int rc = EXIT_SUCCESS;
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    wiring_lock_bundle_t lkb = {
      .arg = &mtx
    , .lock = custom_lock
    , .unlock = custom_unlock
    , .assert_locked = custom_assert_locked
    };

    if (sigemptyset(&sa.sa_mask) == -1)
        err(EXIT_FAILURE, "%s: sigemptyset", __func__);

    if (argc > 2)
        usage(argv[0]);
    if (argc == 2) {
        uint8_t *buf;

        if (colon_separated_octets_to_bytes(argv[1], &buf, &raddrlen) == -1)
            errx(EXIT_FAILURE, "could not parse remote address `%s`", argv[1]);
        printf("parsed %zu-byte remote address\n", raddrlen);
        raddr = (void *)buf;
    } else {
        raddr = NULL;
    }

    if ((status = ucp_config_read(NULL, NULL, &config)) != UCS_OK)
        errx(EXIT_FAILURE, "%s: ucp_config_read", __func__);

    status = ucp_init(&global_params, config, &context);

    ucp_config_release(config);

    if (status != UCS_OK) {
        errx(EXIT_FAILURE, "%s: ucp_init: %s", __func__,
            ucs_status_string(status));
    }

    context_attrs.field_mask = UCP_ATTR_FIELD_REQUEST_SIZE;
    status = ucp_context_query(context, &context_attrs);

    if (status != UCS_OK)
        errx(EXIT_FAILURE, "%s: ucp_context_query", __func__);

    if ((context_attrs.field_mask & UCP_ATTR_FIELD_REQUEST_SIZE) == 0)
        errx(EXIT_FAILURE, "context attributes contain no request size");

    status = ucp_worker_create(context, &worker_params, &worker);
    if (status != UCS_OK) {
        warnx("%s: ucp_worker_create", __func__);
        goto cleanup_context;
    }

    status = ucp_worker_get_address(worker, &laddr, &laddrlen);
    if (status != UCS_OK) {
        warnx("%s: ucp_worker_get_address", __func__);
        goto cleanup_worker;
    }

    printf("%zu-byte local address ", laddrlen);
    for (i = 0; i < laddrlen; i++) {
        printf("%s%02" PRIx8, delim, ((uint8_t *)laddr)[i]);
        delim = ":";
    }
    printf("\n");

    wiring = wiring_create(worker, context_attrs.request_size, &lkb, NULL,
        NULL);

    if (wiring == NULL)
        errx(EXIT_FAILURE, "%s: could not create wiring", __func__);

    if (raddr != NULL) {      /* * * client mode * * */
        bool ok;
        ok = run_client(wiring, worker, laddr, laddrlen, raddr, raddrlen);
        ucp_worker_release_address(worker, laddr);
        free(raddr);

        if (!ok) {
            warnx("%s: could not start wireup", __func__);
            rc = EXIT_FAILURE;
            goto cleanup_wiring;
        }
    } else
        ucp_worker_release_address(worker, laddr);

    if (sigaction(SIGINT, &sa, &osa) == -1)
        err(EXIT_FAILURE, "%s.%d: sigaction", __func__, __LINE__);

    while (go) {
        int ret;

        while ((ret = wireup_once(wiring)) > 0)
            ;

        if (ret < 0)
            break;

        ucp_worker_progress(worker);
    }

    if (sigaction(SIGINT, &osa, NULL) == -1)
        err(EXIT_FAILURE, "%s.%d: sigaction", __func__, __LINE__);

cleanup_wiring:
    wiring_destroy(wiring, true);
cleanup_worker:
    ucp_worker_destroy(worker);
cleanup_context:
    ucp_cleanup(context);
    return rc;
}
