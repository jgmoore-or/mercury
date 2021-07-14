/*
 * Copyright (C) 2013-2020 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#include "na_test.h"

#include "mercury_request.h" /* For convenience */
#include "mercury_time.h"

#include <stdlib.h>
#include <string.h>

/****************/
/* Local Macros */
/****************/
#define BENCHMARK_NAME "Message latency"
#define STRING(s)      #s
#define XSTRING(s)     STRING(s)
#define VERSION_NAME                                                           \
    XSTRING(0)                                                                 \
    "." XSTRING(1) "." XSTRING(0)

#define SMALL_SKIP 1000

#define NDIGITS          2
#define NWIDTH           20
#define NA_TEST_TAG_DONE 111

/************************************/
/* Local Type and Struct Definition */
/************************************/

struct na_test_lat_info {
    na_class_t *na_class;
    na_context_t *context;
    hg_request_class_t *request_class;
    na_addr_t target_addr;
    struct na_test_info na_test_info;
};

/********************/
/* Local Prototypes */
/********************/

static NA_INLINE int
na_test_request_progress(unsigned int timeout, void *arg);

static NA_INLINE int
na_test_request_trigger(unsigned int timeout, unsigned int *flag, void *arg);

static na_return_t
na_test_target_lookup(struct na_test_lat_info *na_test_lat_info);

static NA_INLINE int
na_test_send_recv_cb(const struct na_cb_info *na_cb_info);

static na_return_t
na_test_measure_latency(
    struct na_test_lat_info *na_test_lat_info, na_size_t size);

/*******************/
/* Local Variables */
/*******************/

/*---------------------------------------------------------------------------*/
static NA_INLINE int
na_test_request_progress(unsigned int timeout, void *arg)
{
    struct na_test_lat_info *na_test_lat_info = (struct na_test_lat_info *) arg;
    unsigned int timeout_progress = 0;
    int ret = HG_UTIL_SUCCESS;

    /* Safe to block */
    if (NA_Poll_try_wait(na_test_lat_info->na_class, na_test_lat_info->context))
        timeout_progress = timeout;

    /* Progress */
    if (NA_Progress(na_test_lat_info->na_class, na_test_lat_info->context,
            timeout_progress) != NA_SUCCESS)
        ret = HG_UTIL_FAIL;

    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE int
na_test_request_trigger(unsigned int timeout, unsigned int *flag, void *arg)
{
    struct na_test_lat_info *na_test_lat_info = (struct na_test_lat_info *) arg;
    unsigned int actual_count = 0;
    int ret = HG_UTIL_SUCCESS;

    if (NA_Trigger(na_test_lat_info->context, timeout, 1, NULL,
            &actual_count) != NA_SUCCESS)
        ret = HG_UTIL_FAIL;
    *flag = (actual_count) ? HG_UTIL_TRUE : HG_UTIL_FALSE;

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_test_target_lookup(struct na_test_lat_info *na_test_lat_info)
{
    na_return_t ret = NA_SUCCESS;

    /* Forward call to remote addr and get a new request */
    ret = NA_Addr_lookup(na_test_lat_info->na_class,
        na_test_lat_info->na_test_info.target_name,
        &na_test_lat_info->target_addr);
    if (ret != NA_SUCCESS) {
        NA_TEST_LOG_ERROR(
            "Could not lookup address (%s)", NA_Error_to_string(ret));
        goto done;
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE int
na_test_send_recv_cb(const struct na_cb_info *na_cb_info)
{
    hg_request_t *request = (hg_request_t *) na_cb_info->arg;

    hg_request_complete(request);

    return NA_SUCCESS;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_test_measure_latency(
    struct na_test_lat_info *na_test_lat_info, na_size_t size)
{
    char *send_buf = NULL, *recv_buf = NULL;
    void *send_buf_data, *recv_buf_data;
    size_t loop = (size_t) na_test_lat_info->na_test_info.loop * 100;
    size_t skip = SMALL_SKIP;
    na_op_id_t *send_op_id;
    na_op_id_t *recv_op_id;
    hg_request_t *recv_request;
    hg_request_t *send_request;
    na_size_t unexpected_header_size =
        NA_Msg_get_unexpected_header_size(na_test_lat_info->na_class);
    na_size_t buf_size =
        size < unexpected_header_size ? unexpected_header_size : size;
    size_t avg_iter;
    double time_read = 0, read_lat;
    na_return_t ret = NA_SUCCESS;
    size_t i;

    /* Prepare send_buf */
    if (buf_size == unexpected_header_size)
        buf_size++;
    send_buf =
        NA_Msg_buf_alloc(na_test_lat_info->na_class, buf_size, &send_buf_data);
    NA_Msg_init_unexpected(na_test_lat_info->na_class, send_buf, buf_size);
    for (i = unexpected_header_size; i < buf_size; i++)
        send_buf[i] = (char) i;

    /* Prepare recv buf */
    recv_buf =
        NA_Msg_buf_alloc(na_test_lat_info->na_class, buf_size, &recv_buf_data);
    memset(recv_buf, 0, buf_size);

    /* Create operation IDs */
    send_op_id = NA_Op_create(na_test_lat_info->na_class);
    recv_op_id = NA_Op_create(na_test_lat_info->na_class);

    recv_request = hg_request_create(na_test_lat_info->request_class);
    send_request = hg_request_create(na_test_lat_info->request_class);

    /* Warm up */
    for (i = 0; i < skip; i++) {
        /* Post recv */
        ret = NA_Msg_recv_expected(na_test_lat_info->na_class,
            na_test_lat_info->context, na_test_send_recv_cb, recv_request,
            recv_buf, buf_size, recv_buf_data, na_test_lat_info->target_addr, 0,
            0, recv_op_id);
        if (ret != NA_SUCCESS) {
            NA_TEST_LOG_ERROR(
                "NA_Msg_recv_expected() failed (%s)", NA_Error_to_string(ret));
            goto done;
        }

again:
        /* Post send */
        ret = NA_Msg_send_unexpected(na_test_lat_info->na_class,
            na_test_lat_info->context, na_test_send_recv_cb, send_request,
            send_buf, buf_size, send_buf_data, na_test_lat_info->target_addr,
            0, 0, send_op_id);
        if (ret == NA_AGAIN) {
            hg_request_wait(recv_request, 0, NULL);
            goto again;
        }
        if (ret != NA_SUCCESS) {
            NA_TEST_LOG_ERROR("NA_Msg_send_unexpected() failed (%s)",
                NA_Error_to_string(ret));
            goto done;
        }

        hg_request_wait(recv_request, NA_MAX_IDLE_TIME, NULL);
        hg_request_reset(recv_request);
        hg_request_wait(send_request, NA_MAX_IDLE_TIME, NULL);
        hg_request_reset(send_request);
    }

    NA_Test_barrier(&na_test_lat_info->na_test_info);

    /* Actual benchmark */
    for (avg_iter = 0; avg_iter < loop; avg_iter++) {
        hg_time_t t1, t2;

        hg_time_get_current(&t1);

        /* Post recv */
        ret = NA_Msg_recv_expected(na_test_lat_info->na_class,
            na_test_lat_info->context, na_test_send_recv_cb, recv_request,
            recv_buf, buf_size, recv_buf_data, na_test_lat_info->target_addr, 0,
            1, recv_op_id);
        if (ret != NA_SUCCESS) {
            NA_TEST_LOG_ERROR(
                "NA_Msg_recv_expected() failed (%s)", NA_Error_to_string(ret));
            goto done;
        }

        /* Post send */
        ret = NA_Msg_send_unexpected(na_test_lat_info->na_class,
            na_test_lat_info->context, na_test_send_recv_cb, send_request,
            send_buf, buf_size, send_buf_data, na_test_lat_info->target_addr,
            0, 1, send_op_id);
        if (ret != NA_SUCCESS) {
            NA_TEST_LOG_ERROR("NA_Msg_send_unexpected() failed (%s)",
                NA_Error_to_string(ret));
            goto done;
        }

        hg_request_wait(recv_request, NA_MAX_IDLE_TIME, NULL);
        NA_Test_barrier(&na_test_lat_info->na_test_info);
        hg_time_get_current(&t2);
        time_read += hg_time_to_double(hg_time_subtract(t2, t1));

        hg_request_reset(recv_request);

        hg_request_wait(send_request, NA_MAX_IDLE_TIME, NULL);
        hg_request_reset(send_request);

#ifdef HG_TEST_HAS_VERIFY_DATA
        /* Check recv buf */
        const char *recv_buf_ptr = (const char *) recv_buf;

        for (i = NA_Msg_get_unexpected_header_size(na_test_lat_info->na_class);
             i < buf_size; i++) {
            if (recv_buf_ptr[i] != (char) i) {
                fprintf(stderr,
                    "Error detected in bulk transfer, buf[%d] = %d, "
                    "was expecting %d!\n",
                    (int) i, (char) recv_buf_ptr[i], (char) i);
                break;
            }
        }
#endif

        /* At this point we have received everything so work out the bandwidth
         */
#ifdef HG_TEST_PRINT_PARTIAL
        read_lat = time_read * 1.0e6 /
                   (double) ((avg_iter + 1) * 2 *
                             (unsigned int)
                                 na_test_lat_info->na_test_info.mpi_comm_size);
        if (na_test_lat_info->na_test_info.mpi_comm_rank == 0)
            fprintf(stdout, "%-*d%*.*f\r", 10, (int) size, NWIDTH, NDIGITS,
                read_lat);
#endif
    }
#ifndef HG_TEST_PRINT_PARTIAL
    read_lat =
        time_read * 1.0e6 /
        (double) (loop * 2 *
                  (unsigned int) na_test_lat_info->na_test_info.mpi_comm_size);
    if (na_test_lat_info->na_test_info.mpi_comm_rank == 0)
        fprintf(stdout, "%-*d%*.*f", 10, (int) size, NWIDTH, NDIGITS, read_lat);
#endif
    if (na_test_lat_info->na_test_info.mpi_comm_rank == 0)
        fprintf(stdout, "\n");

done:
    /* Clean up resources */
    hg_request_destroy(recv_request);
    hg_request_destroy(send_request);
    NA_Op_destroy(na_test_lat_info->na_class, send_op_id);
    NA_Op_destroy(na_test_lat_info->na_class, recv_op_id);
    NA_Msg_buf_free(na_test_lat_info->na_class, send_buf, send_buf_data);
    NA_Msg_buf_free(na_test_lat_info->na_class, recv_buf, recv_buf_data);
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_test_send_finalize(struct na_test_lat_info *na_test_lat_info)
{
    char *send_buf = NULL, *recv_buf = NULL;
    void *send_buf_data, *recv_buf_data;
    hg_request_t *recv_request;
    hg_request_t *send_request;
    na_size_t unexpected_header_size =
        NA_Msg_get_unexpected_header_size(na_test_lat_info->na_class);
    na_size_t buf_size =
        (unexpected_header_size) ? unexpected_header_size + 1 : 1;
    na_op_id_t *send_op_id;
    na_op_id_t *recv_op_id;
    na_return_t ret = NA_SUCCESS;

    /* Prepare send_buf */
    send_buf =
        NA_Msg_buf_alloc(na_test_lat_info->na_class, buf_size, &send_buf_data);
    NA_Msg_init_unexpected(na_test_lat_info->na_class, send_buf, buf_size);

    /* Prepare recv buf */
    recv_buf =
        NA_Msg_buf_alloc(na_test_lat_info->na_class, buf_size, &recv_buf_data);
    memset(recv_buf, 0, buf_size);

    send_op_id = NA_Op_create(na_test_lat_info->na_class);
    recv_op_id = NA_Op_create(na_test_lat_info->na_class);

    recv_request = hg_request_create(na_test_lat_info->request_class);
    send_request = hg_request_create(na_test_lat_info->request_class);

    /* Post recv */
    ret = NA_Msg_recv_expected(na_test_lat_info->na_class,
        na_test_lat_info->context, na_test_send_recv_cb, recv_request,
        recv_buf, buf_size, recv_buf_data, na_test_lat_info->target_addr, 0,
        NA_TEST_TAG_DONE, recv_op_id);
    if (ret != NA_SUCCESS) {
        NA_TEST_LOG_ERROR(
            "NA_Msg_recv_expected() failed (%s)", NA_Error_to_string(ret));
        goto done;
    }

    /* Post send */
    ret = NA_Msg_send_unexpected(na_test_lat_info->na_class,
        na_test_lat_info->context, na_test_send_recv_cb, send_request,
        send_buf, buf_size, send_buf_data, na_test_lat_info->target_addr,
        0, NA_TEST_TAG_DONE, send_op_id);
    if (ret != NA_SUCCESS) {
        NA_TEST_LOG_ERROR(
            "NA_Msg_send_unexpected() failed (%s)", NA_Error_to_string(ret));
        goto done;
    }

    hg_request_wait(recv_request, NA_MAX_IDLE_TIME, NULL);
    hg_request_wait(send_request, NA_MAX_IDLE_TIME, NULL);

done:
    /* Clean up resources */
    hg_request_destroy(recv_request);
    hg_request_destroy(send_request);
    NA_Op_destroy(na_test_lat_info->na_class, send_op_id);
    NA_Op_destroy(na_test_lat_info->na_class, recv_op_id);
    NA_Msg_buf_free(na_test_lat_info->na_class, send_buf, send_buf_data);
    NA_Msg_buf_free(na_test_lat_info->na_class, recv_buf, recv_buf_data);
    return ret;
}

/*---------------------------------------------------------------------------*/
int
main(int argc, char *argv[])
{
    struct na_test_lat_info na_test_lat_info = {0};
    na_size_t size, max_size;
    int ret = EXIT_SUCCESS;

    /* Initialize the interface */
    NA_Test_init(argc, argv, &na_test_lat_info.na_test_info);
    na_test_lat_info.na_class = na_test_lat_info.na_test_info.na_class;
    na_test_lat_info.context = NA_Context_create(na_test_lat_info.na_class);
    na_test_lat_info.request_class = hg_request_init(
        na_test_request_progress, na_test_request_trigger, &na_test_lat_info);

    /* Lookup target addr */
    na_test_target_lookup(&na_test_lat_info);

    /* Set max size */
    max_size = NA_Msg_get_max_unexpected_size(na_test_lat_info.na_class);

    if (na_test_lat_info.na_test_info.mpi_comm_rank == 0) {
        fprintf(stdout, "# %s v%s\n", BENCHMARK_NAME, VERSION_NAME);
        fprintf(stdout, "# Loop %d times from size %d to %zu byte(s)\n",
            na_test_lat_info.na_test_info.loop, 1, max_size);
#ifdef HG_TEST_HAS_VERIFY_DATA
        fprintf(stdout, "# WARNING verifying data, output will be slower\n");
#endif
        fprintf(stdout, "%-*s%*s\n", 10, "# Size", NWIDTH, "Latency (us)");
        fflush(stdout);
    }

    /* Msg with different sizes */
    for (size = 1; size <= max_size; size *= 2)
        na_test_measure_latency(&na_test_lat_info, size);

    /* Finalize interface */
    if (na_test_lat_info.na_test_info.mpi_comm_rank == 0)
        na_test_send_finalize(&na_test_lat_info);
    NA_Addr_free(na_test_lat_info.na_class, na_test_lat_info.target_addr);
    hg_request_finalize(na_test_lat_info.request_class, NULL);
    NA_Context_destroy(na_test_lat_info.na_class, na_test_lat_info.context);
    NA_Test_finalize(&na_test_lat_info.na_test_info);

    return ret;
}
