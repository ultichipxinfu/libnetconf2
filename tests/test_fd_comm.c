/**
 * \file test_full_session.c
 * \author Michal Vasko <mvasko@cesnet.cz>
 * \brief libnetconf2 tests - session creation, communication, and termination
 *
 * Copyright (c) 2015 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <cmocka.h>
#include <libyang/libyang.h>

#include <session_client.h>
#include <session_server.h>
#include <session_p.h>
#include <messages_p.h>
#include "config.h"

struct nc_session *server_session;
struct nc_session *client_session;

struct nc_server_reply *
my_get_rpc_clb(struct lyd_node *rpc, struct nc_session *session)
{
    assert_string_equal(rpc->schema->name, "get");
    assert_ptr_equal(session, server_session);

    return nc_server_reply_ok();
}

struct nc_server_reply *
my_getconfig_rpc_clb(struct lyd_node *rpc, struct nc_session *session)
{
    struct lyd_node *data;
    const struct lys_module *module;

    assert_string_equal(rpc->schema->name, "get-config");
    assert_ptr_equal(session, server_session);

    module = ly_ctx_get_module(session->ctx, "ietf-netconf-acm", NULL);
    assert_non_null(module);

    data = lyd_new(NULL, module, "nacm");
    assert_non_null(data);

    return nc_server_reply_data(data, NC_PARAMTYPE_FREE);
}

static int
setup_sessions(void **state)
{
    (void)state;
    struct ly_ctx *ctx;
    const struct lys_module *module;
    const struct lys_node *node;
    int sock[2];

    /* create ctx */
    ctx = ly_ctx_new(TESTS_DIR"../schemas");
    assert_non_null(ctx);

    /* load modules */
    module = ly_ctx_load_module(ctx, "ietf-netconf-acm", NULL);
    assert_non_null(module);

    module = ly_ctx_load_module(ctx, "ietf-netconf", NULL);
    assert_non_null(module);

    /* set RPC callbacks */
    node = lys_get_node(module, "/get");
    assert_non_null(node);
    lys_set_private(node, my_get_rpc_clb);

    node = lys_get_node(module, "/get-config");
    assert_non_null(node);
    lys_set_private(node, my_getconfig_rpc_clb);

    /* create communication channel */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sock);

    nc_server_init(ctx);

    /* create server session */
    server_session = calloc(1, sizeof *server_session);
    server_session->status = NC_STATUS_RUNNING;
    server_session->side = NC_SERVER;
    server_session->id = 1;
    server_session->version = NC_VERSION_11;
    server_session->ti_type = NC_TI_FD;
    server_session->ti_lock = malloc(sizeof *server_session->ti_lock);
    pthread_mutex_init(server_session->ti_lock, NULL);
    server_session->ti.fd.in = sock[0];
    server_session->ti.fd.out = sock[0];
    server_session->ctx = ctx;
    server_session->flags = NC_SESSION_SHAREDCTX;

    /* create client session */
    client_session = calloc(1, sizeof *server_session);
    client_session->status = NC_STATUS_RUNNING;
    client_session->side = NC_CLIENT;
    client_session->id = 1;
    client_session->version = NC_VERSION_11;
    client_session->ti_type = NC_TI_FD;
    client_session->ti_lock = malloc(sizeof *client_session->ti_lock);
    pthread_mutex_init(client_session->ti_lock, NULL);
    client_session->ti.fd.in = sock[1];
    client_session->ti.fd.out = sock[1];
    client_session->ctx = ctx;
    client_session->flags = NC_SESSION_SHAREDCTX;
    client_session->msgid = 50;

    return 0;
}

static int
teardown_sessions(void **state)
{
    (void)state;

    nc_server_destroy();

    ly_ctx_destroy(server_session->ctx);

    close(server_session->ti.fd.in);
    pthread_mutex_destroy(server_session->ti_lock);
    free(server_session->ti_lock);
    free(server_session);

    close(client_session->ti.fd.in);
    pthread_mutex_destroy(client_session->ti_lock);
    free(client_session->ti_lock);
    free(client_session);

    return 0;
}

static void
test_send_recv_ok(void **state)
{
    (void)state;
    int ret;
    uint64_t msgid;
    NC_MSG_TYPE msgtype;
    struct nc_rpc *rpc;
    struct nc_reply *reply;
    struct nc_pollsession *ps;

    /* client RPC */
    rpc = nc_rpc_get(NULL, 0, 0);
    assert_non_null(rpc);

    msgtype = nc_send_rpc(client_session, rpc, 0, &msgid);
    assert_int_equal(msgtype, NC_MSG_RPC);

    /* server RPC, send reply */
    ps = nc_ps_new();
    assert_non_null(ps);
    nc_ps_add_session(ps, server_session);

    ret = nc_ps_poll(ps, 0);
    assert_int_equal(ret, 1);

    /* server finished */
    nc_ps_free(ps);

    /* client reply */
    msgtype = nc_recv_reply(client_session, rpc, msgid, 0, &reply);
    assert_int_equal(msgtype, NC_MSG_REPLY);

    nc_rpc_free(rpc);
    assert_int_equal(reply->type, NC_RPL_OK);
    nc_reply_free(reply);
}

static void
test_send_recv_error(void **state)
{
    (void)state;
    int ret;
    uint64_t msgid;
    NC_MSG_TYPE msgtype;
    struct nc_rpc *rpc;
    struct nc_reply *reply;
    struct nc_pollsession *ps;

    /* client RPC */
    rpc = nc_rpc_kill(1);
    assert_non_null(rpc);

    msgtype = nc_send_rpc(client_session, rpc, 0, &msgid);
    assert_int_equal(msgtype, NC_MSG_RPC);

    /* server RPC, send reply */
    ps = nc_ps_new();
    assert_non_null(ps);
    nc_ps_add_session(ps, server_session);

    ret = nc_ps_poll(ps, 0);
    assert_int_equal(ret, 1);

    /* server finished */
    nc_ps_free(ps);

    /* client reply */
    msgtype = nc_recv_reply(client_session, rpc, msgid, 0, &reply);
    assert_int_equal(msgtype, NC_MSG_REPLY);

    nc_rpc_free(rpc);
    assert_int_equal(reply->type, NC_RPL_ERROR);
    assert_string_equal(((struct nc_reply_error *)reply)->err->tag, "operation-not-supported");
    nc_reply_free(reply);
}

static void
test_send_recv_data(void **state)
{
    (void)state;
    int ret;
    uint64_t msgid;
    NC_MSG_TYPE msgtype;
    struct nc_rpc *rpc;
    struct nc_reply *reply;
    struct nc_pollsession *ps;

    /* client RPC */
    rpc = nc_rpc_getconfig(NC_DATASTORE_RUNNING, NULL, 0, 0);
    assert_non_null(rpc);

    msgtype = nc_send_rpc(client_session, rpc, 0, &msgid);
    assert_int_equal(msgtype, NC_MSG_RPC);

    /* server RPC, send reply */
    ps = nc_ps_new();
    assert_non_null(ps);
    nc_ps_add_session(ps, server_session);

    ret = nc_ps_poll(ps, 0);
    assert_int_equal(ret, 1);

    /* server finished */
    nc_ps_free(ps);

    /* client reply */
    msgtype = nc_recv_reply(client_session, rpc, msgid, 0, &reply);
    assert_int_equal(msgtype, NC_MSG_REPLY);

    nc_rpc_free(rpc);
    assert_int_equal(reply->type, NC_RPL_DATA);
    nc_reply_free(reply);
}

/* TODO
static void
test_send_recv_notif(void **state)
{

}*/

int
main(void)
{
    const struct CMUnitTest comm[] = {
        cmocka_unit_test_setup_teardown(test_send_recv_ok, setup_sessions, teardown_sessions),
        cmocka_unit_test_setup_teardown(test_send_recv_error, setup_sessions, teardown_sessions),
        cmocka_unit_test_setup_teardown(test_send_recv_data, setup_sessions, teardown_sessions)
    };

    return cmocka_run_group_tests(comm, NULL, NULL);
}