/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "qpid/dispatch/ctools.h"
#include "qpid/dispatch/protocol_adaptor.h"
#include "delivery.h"
#include "qpid/dispatch/timer.h"
#include "qpid/dispatch/message.h"
#include <stdio.h>
#include <inttypes.h>

static char *address1 = "examples";
static char *address2 = "stream";

typedef struct qdr_ref_adaptor_t {
    qdr_core_t             *core;
    qdr_protocol_adaptor_t *adaptor;
    qd_timer_t             *startup_timer;
    qd_timer_t             *activate_timer;
    qd_timer_t             *stream_timer;
    qdr_connection_t       *conn;
    qdr_link_t             *out_link_1;
    qdr_link_t             *out_link_2;
    qdr_link_t             *in_link_2;
    qdr_link_t             *dynamic_in_link;
    char                   *reply_to;
    qd_message_t           *streaming_message;
    qdr_delivery_t         *streaming_delivery;
    int                     stream_count;
} qdr_ref_adaptor_t;


void qdr_ref_connection_activate_CT(void *context, qdr_connection_t *conn)
{
    //
    // Use a zero-delay timer to defer this call to an IO thread
    //
    // Note that this may not be generally safe to do.  There's no guarantee that multiple
    // activations won't schedule multiple IO threads running this code concurrently.
    // Normally, we would rely on assurances provided by the IO scheduler (Proton) that no
    // connection shall ever be served by more than one thread concurrently.
    //
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) context;
    qd_timer_schedule(adaptor->activate_timer, 0);
}


static void qdr_ref_first_attach(void *context, qdr_connection_t *conn, qdr_link_t *link,
                                 qdr_terminus_t *source, qdr_terminus_t *target,
                                 qd_session_class_t session_class)
{
}


static void qdr_ref_second_attach(void *context, qdr_link_t *link,
                                  qdr_terminus_t *source, qdr_terminus_t *target)
{
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) context;
#define TERM_SIZE 200
    char ftarget[TERM_SIZE];
    char fsource[TERM_SIZE];

    ftarget[0] = '\0';
    fsource[0] = '\0';

    if (!!source) {
        size_t size = TERM_SIZE;
        qdr_terminus_format(source, fsource, &size);
    }

    if (!!target) {
        size_t size = TERM_SIZE;
        qdr_terminus_format(target, ftarget, &size);
    }

    printf("qdr_ref_second_attach: source=%s target=%s\n", fsource, ftarget);

    if (link == adaptor->dynamic_in_link) {
        //
        // The dynamic in-link has been attached.  Get the reply-to address and open
        // a couple of out-links.
        //
        qd_iterator_t *reply_iter = qdr_terminus_get_address(source);
        adaptor->reply_to = (char*) qd_iterator_copy(reply_iter);
        printf("qdr_ref_second_attach: reply-to=%s\n", adaptor->reply_to);

        //
        // Open an out-link for each address
        //
        uint64_t        link_id;
        qdr_terminus_t *target = qdr_terminus(0);

        qdr_terminus_set_address(target, address1);
        adaptor->out_link_1 = qdr_link_first_attach(adaptor->conn,
                                                    QD_INCOMING,
                                                    qdr_terminus(0),  //qdr_terminus_t   *source,
                                                    target,           //qdr_terminus_t   *target,
                                                    "ref.1",          //const char       *name,
                                                    0,                //const char       *terminus_addr,
                                                    false,            //bool              no_route
                                                    0,                //qdr_delivery_t   *initial_delivery
                                                    &link_id);

        target = qdr_terminus(0);
        qdr_terminus_set_address(target, address2);
        adaptor->out_link_2 = qdr_link_first_attach(adaptor->conn,
                                                    QD_INCOMING,
                                                    qdr_terminus(0),  //qdr_terminus_t   *source,
                                                    target,           //qdr_terminus_t   *target,
                                                    "ref.2",          //const char       *name,
                                                    0,                //const char       *terminus_addr,
                                                    false,            //bool              no_route
                                                    0,                //qdr_delivery_t   *initial_delivery
                                                    &link_id);

        source = qdr_terminus(0);
        qdr_terminus_set_address(source, address2);
        adaptor->in_link_2 = qdr_link_first_attach(adaptor->conn,
                                                   QD_OUTGOING,
                                                   source,           //qdr_terminus_t   *source,
                                                   qdr_terminus(0),  //qdr_terminus_t   *target,
                                                   "ref.3",          //const char       *name,
                                                   0,                //const char       *terminus_addr,
                                                   false,            //bool              no_route
                                                   0,                //qdr_delivery_t   *initial_delivery
                                                   &link_id);
    }
}


static void qdr_ref_detach(void *context, qdr_link_t *link, qdr_error_t *error, bool first, bool close)
{
}


static void qdr_ref_flow(void *context, qdr_link_t *link, int credit)
{
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) context;
    qd_buffer_list_t   buffers;
    qd_buffer_t       *buf;
    
    printf("qdr_ref_flow: %d credits issued\n", credit);

    if (link == adaptor->out_link_1) {
        qd_composed_field_t *props = qd_compose(QD_PERFORMATIVE_PROPERTIES, 0);
        qd_compose_start_list(props);
        qd_compose_insert_null(props);                      // message-id
        qd_compose_insert_null(props);                      // user-id
        qd_compose_insert_null(props);                      // to
        qd_compose_insert_null(props);                      // subject
        qd_compose_insert_string(props, adaptor->reply_to); // reply-to
        /*
        qd_compose_insert_null(props);                      // correlation-id
        qd_compose_insert_null(props);                      // content-type
        qd_compose_insert_null(props);                      // content-encoding
        qd_compose_insert_timestamp(props, 0);              // absolute-expiry-time
        qd_compose_insert_timestamp(props, 0);              // creation-time
        qd_compose_insert_null(props);                      // group-id
        qd_compose_insert_uint(props, 0);                   // group-sequence
        qd_compose_insert_null(props);                      // reply-to-group-id
        */
        qd_compose_end_list(props);

        qd_message_t *msg = qd_message();
        DEQ_INIT(buffers);
        buf = qd_buffer();
        char *insert = (char*) qd_buffer_cursor(buf);
        memcpy(insert, "\x00\x53\x77\xa1\x0cTest Payload", 17);
        qd_buffer_insert(buf, 17);
        DEQ_INSERT_HEAD(buffers, buf);

        qd_message_compose_5(msg, props, &buffers, true);
        qd_compose_free(props);

        qdr_link_deliver(adaptor->out_link_1, msg, 0, false, 0, 0);
        // Keep return-protection delivery reference as the adaptor's reference
    } else if (link == adaptor->out_link_2) {
        //
        // Begin streaming a long message on the link.
        //
        qd_composed_field_t *props = qd_compose(QD_PERFORMATIVE_PROPERTIES, 0);
        qd_compose_start_list(props);
        qd_compose_insert_null(props);                      // message-id
        qd_compose_insert_null(props);                      // user-id
        qd_compose_insert_null(props);                      // to
        qd_compose_insert_null(props);                      // subject
        qd_compose_insert_string(props, adaptor->reply_to); // reply-to
        qd_compose_end_list(props);

        props = qd_compose(QD_PERFORMATIVE_BODY_DATA, props);

        adaptor->streaming_message = qd_message();

        qd_message_compose_5(adaptor->streaming_message, props, 0, false);
        qd_compose_free(props);

        printf("qdr_ref_flow: Starting a streaming delivery\n");
        adaptor->streaming_delivery =
            qdr_link_deliver(adaptor->out_link_2, adaptor->streaming_message, 0, false, 0, 0);
        adaptor->stream_count = 0;
        // Keep return-protection delivery reference as the adaptor's reference

        qd_timer_schedule(adaptor->stream_timer, 1000);
    }
}


static void qdr_ref_offer(void *context, qdr_link_t *link, int delivery_count)
{
}


static void qdr_ref_drained(void *context, qdr_link_t *link)
{
}


static void qdr_ref_drain(void *context, qdr_link_t *link, bool mode)
{
}


static int qdr_ref_push(void *context, qdr_link_t *link, int limit)
{
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) context;
    printf("qdr_ref_push: limit=%d\n", limit);
    return qdr_link_process_deliveries(adaptor->core, link, limit);
}


static uint64_t qdr_ref_deliver(void *context, qdr_link_t *link, qdr_delivery_t *delivery, bool settled)
{
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) context;
    qd_message_t      *msg     = qdr_delivery_message(delivery);

    qd_message_depth_status_t status = qd_message_check_depth(msg, QD_DEPTH_APPLICATION_PROPERTIES);

    switch (status) {
    case QD_MESSAGE_DEPTH_OK: {
        if (qd_message_receive_complete(msg)) {
            qd_iterator_t *body_iter = qd_message_field_iterator(msg, QD_FIELD_BODY);
            char *body = (char*) qd_iterator_copy(body_iter);
            printf("qdr_ref_deliver: complete message received, body=%s\n", body);
            free(body);
            qd_iterator_free(body_iter);
            qd_message_set_send_complete(msg);
            qdr_link_flow(adaptor->core, link, 1, false);
            return PN_ACCEPTED; // This will cause the delivery to be settled
        }
        break;
    }

    case QD_MESSAGE_DEPTH_INVALID:
        printf("qdr_ref_deliver: message invalid\n");
        qdr_link_flow(adaptor->core, link, 1, false);
        break;

    case QD_MESSAGE_DEPTH_INCOMPLETE:
        printf("qdr_ref_deliver: message incomplete\n");
        break;
    }

    return 0;
}


static int qdr_ref_get_credit(void *context, qdr_link_t *link)
{
    return 8;
}


static void qdr_ref_delivery_update(void *context, qdr_delivery_t *dlv, uint64_t disp, bool settled)
{
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) context;
    char              *dispname;

    switch (disp) {
    case PN_ACCEPTED: dispname = "ACCEPTED"; break;
    case PN_REJECTED: dispname = "REJECTED"; break;
    case PN_RELEASED: dispname = "RELEASED"; break;
    case PN_MODIFIED: dispname = "MODIFIED"; break;
    default:
        dispname = "<UNKNOWN>";
    }
    printf("qdr_ref_delivery_update: disp=%s settled=%s\n", dispname, settled ? "true" : "false");

    if (qdr_delivery_link(dlv) == adaptor->out_link_2 && qdr_delivery_message(dlv) == adaptor->streaming_message) {
        adaptor->streaming_message = 0;
        adaptor->stream_count      = 0;
    }

    if (settled)
        qdr_delivery_decref(adaptor->core, dlv, "qdr_ref_delivery_update - settled delivery");
}


static void qdr_ref_conn_close(void *context, qdr_connection_t *conn, qdr_error_t *error)
{
}


static void qdr_ref_conn_trace(void *context, qdr_connection_t *conn, bool trace)
{
}


static void on_startup(void *context)
{
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) context;

    qdr_connection_info_t *info = qdr_connection_info(false,               //bool            is_encrypted,
                                                      false,               //bool            is_authenticated,
                                                      true,                //bool            opened,
                                                      "",                  //char           *sasl_mechanisms,
                                                      QD_INCOMING,         //qd_direction_t  dir,
                                                      "",                  //const char     *host,
                                                      "",                  //const char     *ssl_proto,
                                                      "",                  //const char     *ssl_cipher,
                                                      "",                  //const char     *user,
                                                      "reference-adaptor", //const char     *container,
                                                      pn_data(0),          //pn_data_t      *connection_properties,
                                                      0,                   //int             ssl_ssf,
                                                      false,               //bool            ssl,
                                                      0);                  //const qdr_router_version_t *version)

    adaptor->conn = qdr_connection_opened(adaptor->core,    // core
                                          adaptor->adaptor, // protocol_adaptor
                                          true,             // incoming
                                          QDR_ROLE_NORMAL,  // role
                                          1,                // cost
                                          qd_server_allocate_connection_id(adaptor->core->qd->server),
                                          0,                // label
                                          0,                // remote_container_id
                                          false,            // strip_annotations_in
                                          false,            // strip_annotations_out
                                          false,            // policy_allow_dynamic_link_routes
                                          false,            // policy_allow_admin_status_update
                                          250,              // link_capacity
                                          0,                // vhost
                                          info,             // connection_info
                                          0,                // context_binder
                                          0);               // bind_token

    uint64_t link_id;

    //
    // Create a dynamic receiver
    //
    qdr_terminus_t *dynamic_source = qdr_terminus(0);
    qdr_terminus_set_dynamic(dynamic_source);

    adaptor->dynamic_in_link = qdr_link_first_attach(adaptor->conn,
                                                     QD_OUTGOING,
                                                     dynamic_source,   //qdr_terminus_t   *source,
                                                     qdr_terminus(0),  //qdr_terminus_t   *target,
                                                     "ref.0",          //const char       *name,
                                                     0,                //const char       *terminus_addr,
                                                     false,            //bool              no_route
                                                     0,                //qdr_delivery_t   *initial_delivery
                                                     &link_id);
}


static void on_activate(void *context)
{
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) context;

    while (qdr_connection_process(adaptor->conn)) {}
}


static void on_stream(void *context)
{
    qdr_ref_adaptor_t *adaptor        = (qdr_ref_adaptor_t*) context;
    const char        *content        = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const size_t       content_length = strlen(content);

    if (!adaptor->streaming_message)
        return;

    qd_buffer_t      *buf = qd_buffer();
    qd_buffer_list_t  buffers;

    DEQ_INIT(buffers);
    DEQ_INSERT_TAIL(buffers, buf);

    memcpy(qd_buffer_cursor(buf), content, content_length);
    qd_buffer_insert(buf, content_length);
    int depth = qd_message_extend(adaptor->streaming_message, &buffers);
    qdr_delivery_continue(adaptor->core, adaptor->streaming_delivery, false);

    if (adaptor->stream_count < 10) {
        qd_timer_schedule(adaptor->stream_timer, 100);
        adaptor->stream_count++;
        printf("on_stream: sent streamed frame %d, depth=%d\n", adaptor->stream_count, depth);
    } else {
        qd_message_set_receive_complete(adaptor->streaming_message);
        adaptor->streaming_message = 0;
        adaptor->stream_count      = 0;
        printf("on_stream: completed streaming send, depth=%d\n", depth);
    }
}


/**
 * This initialization function will be invoked when the router core is ready for the protocol
 * adaptor to be created.  This function must:
 *
 *   1) Register the protocol adaptor with the router-core.
 *   2) Prepare the protocol adaptor to be configured.
 */
void qdr_ref_adaptor_init(qdr_core_t *core, void **adaptor_context)
{
    qdr_ref_adaptor_t *adaptor = NEW(qdr_ref_adaptor_t);
    ZERO(adaptor);
    adaptor->core    = core;
    adaptor->adaptor = qdr_protocol_adaptor(core,
                                            "reference", // name
                                            adaptor,     // context
                                            qdr_ref_connection_activate_CT,
                                            qdr_ref_first_attach,
                                            qdr_ref_second_attach,
                                            qdr_ref_detach,
                                            qdr_ref_flow,
                                            qdr_ref_offer,
                                            qdr_ref_drained,
                                            qdr_ref_drain,
                                            qdr_ref_push,
                                            qdr_ref_deliver,
                                            qdr_ref_get_credit,
                                            qdr_ref_delivery_update,
                                            qdr_ref_conn_close,
                                            qdr_ref_conn_trace);
    *adaptor_context = adaptor;

    adaptor->startup_timer = qd_timer(core->qd, on_startup, adaptor);
    qd_timer_schedule(adaptor->startup_timer, 0);

    adaptor->activate_timer = qd_timer(core->qd, on_activate, adaptor);
    adaptor->stream_timer   = qd_timer(core->qd, on_stream, adaptor);
}


void qdr_ref_adaptor_final(void *adaptor_context)
{
    qdr_ref_adaptor_t *adaptor = (qdr_ref_adaptor_t*) adaptor_context;
    qdr_protocol_adaptor_free(adaptor->core, adaptor->adaptor);
    qd_timer_free(adaptor->startup_timer);
    qd_timer_free(adaptor->activate_timer);
    free(adaptor->reply_to);
    free(adaptor);
}

/**
 * Declare the adaptor so that it will self-register on process startup.
 */
//QDR_CORE_ADAPTOR_DECLARE("ref-adaptor", qdr_ref_adaptor_init, qdr_ref_adaptor_final)
