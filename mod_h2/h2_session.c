/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <apr_thread_cond.h>
#include <apr_base64.h>
#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_config.h>
#include <http_log.h>

#include "h2_private.h"
#include "h2_config.h"
#include "h2_bucket.h"
#include "h2_mplx.h"
#include "h2_response.h"
#include "h2_stream.h"
#include "h2_stream_set.h"
#include "h2_from_h1.h"
#include "h2_task.h"
#include "h2_bucket.h"
#include "h2_session.h"
#include "h2_util.h"

static int frame_print(const nghttp2_frame *frame, char *buffer, size_t maxlen);

static int h2_session_status_from_apr_status(apr_status_t rv)
{
    switch (rv) {
        case APR_SUCCESS:
            return NGHTTP2_NO_ERROR;
        case APR_EAGAIN:
        case APR_TIMEUP:
            return NGHTTP2_ERR_WOULDBLOCK;
        case APR_EOF:
            return NGHTTP2_ERR_EOF;
        default:
            return NGHTTP2_ERR_PROTO;
    }
}

static int stream_open(h2_session *session, int stream_id)
{
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    h2_stream * stream = h2_stream_create(stream_id, session->pool,
                                          session->c->bucket_alloc, 
                                          session->mplx);
    if (!stream) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_ENOMEM, session->c,
                      "h2_session: stream(%ld-%d): unable to create",
                      session->id, stream_id);
        return NGHTTP2_ERR_INVALID_STREAM_ID;
    }
    
    apr_status_t status = h2_stream_set_add(session->streams, stream);
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, session->c,
                      "h2_session: stream(%ld-%d): unable to add to pool",
                      session->id, h2_stream_get_id(stream));
        return NGHTTP2_ERR_INVALID_STREAM_ID;
    }
    
    stream->state = H2_STREAM_ST_OPEN;
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                  "h2_session: stream(%ld-%d): opened",
                  session->id, stream_id);
    
    h2_mplx_open_io(session->mplx, stream_id);
    return 0;
}

static apr_status_t stream_end_headers(h2_session *session,
                                       h2_stream *stream, int eos)
{
    apr_status_t status = h2_stream_write_eoh(stream);
    if (status == APR_SUCCESS) {
        if (eos) {
            status = h2_stream_write_eos(stream);
        }
        
        if (status == APR_SUCCESS && session->after_stream_opened_cb) {
            h2_task *task = h2_stream_create_task(stream, session->c);
            session->after_stream_opened_cb(session, stream, task);
        }
    }
    return status;
}


/*
 * Callback when nghttp2 wants to send bytes back to the client.
 */
static ssize_t send_cb(nghttp2_session *ngh2,
                       const uint8_t *data, size_t length,
                       int flags, void *userp)
{
    h2_session *session = (h2_session *)userp;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    size_t written = 0;
    apr_status_t status = h2_conn_io_write(&session->io, (const char*)data,
                                      length, &written);
    if (status == APR_SUCCESS) {
        return written;
    }
    else if (status == APR_EAGAIN || status == APR_TIMEUP) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, session->c,
                  "h2_session: send error");
    return h2_session_status_from_apr_status(status);
}

static int on_invalid_frame_recv_cb(nghttp2_session *ngh2,
                                    const nghttp2_frame *frame,
                                    uint32_t error_code, void *userp)
{
    h2_session *session = (h2_session *)userp;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if (APLOGctrace2(session->c)) {
        char buffer[256];
        
        frame_print(frame, buffer, sizeof(buffer)/sizeof(buffer[0]));
        ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, session->c,
                      "h2_session: callback on_invalid_frame_recv error=%d %s",
                      (int)error_code, buffer);
    }
    return 0;
}

static int on_data_chunk_recv_cb(nghttp2_session *ngh2, uint8_t flags,
                                 int32_t stream_id,
                                 const uint8_t *data, size_t len, void *userp)
{
    h2_session *session = (h2_session *)userp;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    h2_stream * stream = h2_stream_set_get(session->streams, stream_id);
    if (!stream) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                      "h2_session:  stream(%ld-%d): on_data_chunk for unknown stream",
                      session->id, (int)stream_id);
        return NGHTTP2_ERR_INVALID_STREAM_ID;
    }
    
    apr_status_t status = h2_stream_write_data(stream, (const char *)data, len);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, status, session->c,
                  "h2_stream(%ld-%d): written DATA, length %ld",
                  session->id, stream_id, len);
    return (status == APR_SUCCESS)? 0 : NGHTTP2_ERR_PROTO;
}

static int before_frame_send_cb(nghttp2_session *ngh2,
                                const nghttp2_frame *frame,
                                void *userp)
{
    h2_session *session = (h2_session *)userp;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if (APLOGctrace2(session->c)) {
        char buffer[256];
        frame_print(frame, buffer, sizeof(buffer)/sizeof(buffer[0]));
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_session(%ld): before_frame_send %s", 
                      session->id, buffer);
    }
    return 0;
}

static int on_frame_send_cb(nghttp2_session *ngh2,
                            const nghttp2_frame *frame,
                            void *userp)
{
    h2_session *session = (h2_session *)userp;
    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, session->c,
                  "h2_session(%ld): on_frame_send", session->id);
    return 0;
}

static int on_frame_not_send_cb(nghttp2_session *ngh2,
                                const nghttp2_frame *frame,
                                int lib_error_code, void *userp)
{
    h2_session *session = (h2_session *)userp;
    if (APLOGctrace2(session->c)) {
        char buffer[256];
        
        frame_print(frame, buffer, sizeof(buffer)/sizeof(buffer[0]));
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_session: callback on_frame_not_send error=%d %s",
                      lib_error_code, buffer);
    }
    return 0;
}

static apr_status_t close_active_stream(h2_session *session,
                                        h2_stream *stream,
                                        int join)
{
    apr_status_t status = APR_SUCCESS;
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                  "h2_stream(%ld-%d): closing",
                  session->id, (int)stream->id);
    
    h2_stream_set_remove(session->streams, stream);
    if (session->before_stream_close_cb && stream->task) {
        status = session->before_stream_close_cb(session, stream,
                                                 stream->task, join);
    }
    
    if (status == APR_SUCCESS) {
        h2_stream_destroy(stream);
    }
    else if (status == APR_EAGAIN) {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, session->c,
                      "h2_stream(%ld-%d): close delayed by callback",
                      session->id, (int)stream->id);
        h2_stream_set_add(session->zombies, stream);
    }
    return status;
}

static apr_status_t join_zombie_stream(h2_session *session, h2_stream *stream)
{
    apr_status_t status = APR_SUCCESS;
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                  "h2_stream(%ld-%d): join zombie",
                  session->id, (int)stream->id);
    
    h2_stream_set_remove(session->zombies, stream);
    if (session->before_stream_close_cb && stream->task) {
        status = session->before_stream_close_cb(session, stream,
                                                 stream->task, 1);
    }
    h2_stream_destroy(stream);
    return status;
}

static int on_stream_close_cb(nghttp2_session *ngh2, int32_t stream_id,
                              uint32_t error_code, void *userp)
{
    h2_session *session = (h2_session *)userp;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    h2_stream *stream = h2_stream_set_get(session->streams, stream_id);
    if (stream) {
        apr_status_t status = close_active_stream(session, stream, 0);
    }
    
    if (error_code) {
        ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, session->c,
                      "h2_stream(%ld-%d): close error %d",
                      session->id, (int)stream_id, error_code);
    }
    
    return 0;
}

static int on_begin_headers_cb(nghttp2_session *ngh2,
                               const nghttp2_frame *frame, void *userp)
{
    /* This starts a new stream. */
    return stream_open((h2_session *)userp, frame->hd.stream_id);
}

static int on_header_cb(nghttp2_session *ngh2, const nghttp2_frame *frame,
                        const uint8_t *name, size_t namelen,
                        const uint8_t *value, size_t valuelen,
                        uint8_t flags,
                        void *userp)
{
    h2_session *session = (h2_session *)userp;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    h2_stream * stream = h2_stream_set_get(session->streams,
                                           frame->hd.stream_id);
    if (!stream) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                      "h2_session:  stream(%ld-%d): on_header for unknown stream",
                      session->id, (int)frame->hd.stream_id);
        return NGHTTP2_ERR_INVALID_STREAM_ID;
    }
    
    apr_status_t status = h2_stream_write_header(stream,
                                               (const char *)name, namelen,
                                               (const char *)value, valuelen);
    return (status == APR_SUCCESS)? 0 : NGHTTP2_ERR_PROTO;
}

/**
 * nghttp2 session has received a complete frame. Most, it uses
 * for processing of internal state. HEADER and DATA frames however
 * we need to handle ourself.
 */
static int on_frame_recv_cb(nghttp2_session *ng2s,
                            const nghttp2_frame *frame,
                            void *userp)
{
    h2_session *session = (h2_session *)userp;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    apr_status_t status = APR_SUCCESS;
    
    ++session->frames_received;
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, session->c,
                  "h2_session(%ld): on_frame_rcv #%ld, type=%d", session->id,
                  session->frames_received, frame->hd.type);
    switch (frame->hd.type) {
        case NGHTTP2_HEADERS: {
            h2_stream * stream = h2_stream_set_get(session->streams,
                                                   frame->hd.stream_id);
            if (stream == NULL) {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                              "h2_session:  stream(%ld-%d): HEADERS frame "
                              "for unknown stream", session->id,
                              (int)frame->hd.stream_id);
                return NGHTTP2_ERR_INVALID_STREAM_ID;
            }
            
            if (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
                int eos = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM);
                status = stream_end_headers(session, stream, eos);
            }
            break;
        }
        case NGHTTP2_DATA: {
            h2_stream * stream = h2_stream_set_get(session->streams,
                                                   frame->hd.stream_id);
            if (stream == NULL) {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                              "h2_session:  stream(%ld-%d): DATA frame "
                              "for unknown stream", session->id,
                              (int)frame->hd.stream_id);
                return NGHTTP2_ERR_PROTO;
            }
            break;
        }
        default:
            if (APLOGctrace2(session->c)) {
                char buffer[256];
                
                frame_print(frame, buffer,
                            sizeof(buffer)/sizeof(buffer[0]));
                ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, session->c,
                              "h2_session: on_frame_rcv %s", buffer);
            }
            break;
    }
    
    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        h2_stream * stream = h2_stream_set_get(session->streams,
                                               frame->hd.stream_id);
        if (stream != NULL) {
            status = h2_stream_write_eos(stream);
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, session->c,
                          "h2_stream(%ld-%d): input closed",
                          session->id, (int)frame->hd.stream_id);
        }
    }
    
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      "h2_session: stream(%ld-%d): error handling frame",
                      session->id, (int)frame->hd.stream_id);
        return NGHTTP2_ERR_INVALID_STREAM_STATE;
    }
    
    return 0;
}

#define NGH2_SET_CALLBACK(callbacks, name, fn)\
nghttp2_session_callbacks_set_##name##_callback(callbacks, fn)

static apr_status_t init_callbacks(conn_rec *c, nghttp2_session_callbacks **pcb)
{
    int rv = nghttp2_session_callbacks_new(pcb);
    if (rv != 0) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                      "nghttp2_session_callbacks_new: %s",
                      nghttp2_strerror(rv));
        return APR_EGENERAL;
    }
    
    NGH2_SET_CALLBACK(*pcb, send, send_cb);
    NGH2_SET_CALLBACK(*pcb, on_frame_recv, on_frame_recv_cb);
    NGH2_SET_CALLBACK(*pcb, on_invalid_frame_recv, on_invalid_frame_recv_cb);
    NGH2_SET_CALLBACK(*pcb, on_data_chunk_recv, on_data_chunk_recv_cb);
    NGH2_SET_CALLBACK(*pcb, before_frame_send, before_frame_send_cb);
    NGH2_SET_CALLBACK(*pcb, on_frame_send, on_frame_send_cb);
    NGH2_SET_CALLBACK(*pcb, on_frame_not_send, on_frame_not_send_cb);
    NGH2_SET_CALLBACK(*pcb, on_stream_close, on_stream_close_cb);
    NGH2_SET_CALLBACK(*pcb, on_begin_headers, on_begin_headers_cb);
    NGH2_SET_CALLBACK(*pcb, on_header, on_header_cb);
    
    return APR_SUCCESS;
}

static h2_session *h2_session_create_int(conn_rec *c,
                                         request_rec *r,
                                         h2_config *config)
{
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_option *options = NULL;

    apr_pool_t *pool = NULL;
    apr_status_t status = apr_pool_create(&pool, r? r->pool : c->pool);
    if (status != APR_SUCCESS) {
        return NULL;
    }

    h2_session *session = apr_pcalloc(pool, sizeof(h2_session));
    if (session) {
        session->id = c->id;
        session->c = c;
        session->r = r;
        session->ngh2 = NULL;
        
        session->pool = pool;
        session->bbtmp = apr_brigade_create(session->pool, c->bucket_alloc);
        
        status = apr_thread_cond_create(&session->iowait, session->pool);
        if (status != APR_SUCCESS) {
            return NULL;
        }
        
        session->streams = h2_stream_set_create(session->pool);
        session->zombies = h2_stream_set_create(session->pool);
        
        session->mplx = h2_mplx_create(c, session->pool);
        
        h2_conn_io_init(&session->io, c, 0);
        
        apr_status_t status = init_callbacks(c, &callbacks);
        if (status != APR_SUCCESS) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, status, c,
                          "nghttp2: error in init_callbacks");
            h2_session_destroy(session);
            return NULL;
        }
        
        int rv = nghttp2_option_new(&options);
        if (rv != 0) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_EGENERAL, c,
                          "nghttp2_option_new: %s", nghttp2_strerror(rv));
            h2_session_destroy(session);
            return NULL;
        }

        /* Nowadays, we handle the preface ourselves. We had problems
         * with nghttp2 internal state machine when traffic action occured
         * before the preface was read. 
         */
        nghttp2_option_set_recv_client_preface(options, 1);
        /* Set a value, to be observed before we receive any SETTINGS
         * from the client. */
        nghttp2_option_set_peer_max_concurrent_streams(options, 
             h2_config_geti(config, H2_CONF_MAX_STREAMS));

        /* We need to handle window updates ourself, otherwise we
         * get flooded by nghttp2. */
        nghttp2_option_set_no_auto_window_update(options, 1);
        
        rv = nghttp2_session_server_new2(&session->ngh2, callbacks,
                                         session, options);
        nghttp2_session_callbacks_del(callbacks);
        nghttp2_option_del(options);
        
        if (rv != 0) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_EGENERAL, c,
                          "nghttp2_session_server_new: %s",
                          nghttp2_strerror(rv));
            h2_session_destroy(session);
            return NULL;
        }
        
    }
    return session;
}

static int stream_close_finished(void *ctx, h2_stream *stream) {
    assert(ctx);
    h2_session *session = (h2_session *)ctx;
    h2_task *task = stream->task;
    if (!task || h2_task_has_finished(task)) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, session->c,
                      "h2_session(%ld): reaping zombie stream(%d)",
                      session->id, stream->id);
        h2_stream_set_remove(session->zombies, stream);
        h2_stream_destroy(stream);
    }
    return 1;
}

static void reap_zombies(h2_session *session) {
    h2_mplx_cleanup(session->mplx);
    if (session->zombies) {
        /* remove all zombies, where the task has run */
        h2_stream_set_iter(session->zombies, stream_close_finished, session);
    }
}

h2_session *h2_session_create(conn_rec *c, h2_config *config)
{
    return h2_session_create_int(c, NULL, config);
}

h2_session *h2_session_rcreate(request_rec *r, h2_config *config)
{
    return h2_session_create_int(r->connection, r, config);
}

static int close_active_iter(void *ctx, h2_stream *stream) {
    assert(ctx);
    close_active_stream((h2_session *)ctx, stream, 1);
    return 1;
}

static int close_zombie_iter(void *ctx, h2_stream *stream) {
    assert(ctx);
    join_zombie_stream((h2_session *)ctx, stream);
    return 1;
}

void h2_session_destroy(h2_session *session)
{
    assert(session);
    if (session->streams) {
        if (h2_stream_set_size(session->streams)) {
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, session->c,
                          "h2_session(%ld): destroy, %ld streams open",
                          session->id, h2_stream_set_size(session->streams));
            /* destroy all sessions, join all existing tasks */
            h2_stream_set_iter(session->streams, close_active_iter, session);
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, session->c,
                          "h2_session(%ld): destroy, %ld streams remain",
                          session->id, h2_stream_set_size(session->streams));
        }
        h2_stream_set_destroy(session->streams);
        session->streams = NULL;
    }
    if (session->zombies) {
        if (h2_stream_set_size(session->zombies)) {
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, session->c,
                          "h2_session(%ld): destroy, %ld zombie streams",
                          session->id, h2_stream_set_size(session->zombies));
            /* destroy all zombies, join all existing tasks */
            h2_stream_set_iter(session->zombies, close_zombie_iter, session);
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, session->c,
                          "h2_session(%ld): destroy, %ld zombies remain",
                          session->id, h2_stream_set_size(session->zombies));
        }
        h2_stream_set_destroy(session->zombies);
        session->zombies = NULL;
    }
    if (session->ngh2) {
        nghttp2_session_del(session->ngh2);
        session->ngh2 = NULL;
    }
    if (session->mplx) {
        h2_mplx_destroy(session->mplx);
        session->mplx = NULL;
    }
    h2_conn_io_destroy(&session->io);
    
    if (session->iowait) {
        apr_thread_cond_destroy(session->iowait);
        session->iowait = NULL;
    }
    
    if (session->pool) {
        apr_pool_destroy(session->pool);
    }
}

apr_status_t h2_session_goaway(h2_session *session, apr_status_t reason)
{
    assert(session);
    apr_status_t status = APR_SUCCESS;
    if (session->aborted) {
        return APR_EINVAL;
    }
    
    int rv = 0;
    if (reason == APR_SUCCESS) {
        rv = nghttp2_submit_shutdown_notice(session->ngh2);
    }
    else {
        int err = 0;
        int last_id = nghttp2_session_get_last_proc_stream_id(session->ngh2);
        rv = nghttp2_submit_goaway(session->ngh2, last_id,
                                   NGHTTP2_FLAG_NONE, err, NULL, 0);
    }
    if (rv != 0) {
        status = APR_EGENERAL;
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      "session(%ld): submit goaway: %s",
                      session->id, nghttp2_strerror(rv));
    }
    return status;
}

static apr_status_t h2_session_abort_int(h2_session *session, int reason)
{
    assert(session);
    if (!session->aborted) {
        session->aborted = 1;
        if (session->ngh2) {
            ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, session->c,
                          "session(%ld): aborting session, reason=%d %s",
                          session->id, reason, nghttp2_strerror(reason));
            nghttp2_session_terminate_session(session->ngh2, reason);
            nghttp2_submit_goaway(session->ngh2, 0, 0, reason, NULL, 0);
            nghttp2_session_send(session->ngh2);
            h2_conn_io_flush(&session->io);
        }
        h2_mplx_abort(session->mplx);
    }
    return APR_SUCCESS;
}

apr_status_t h2_session_abort(h2_session *session, apr_status_t reason, int rv)
{
    assert(session);
    if (rv == 0) {
        rv = NGHTTP2_ERR_PROTO;
        switch (reason) {
            case APR_ENOMEM:
                rv = NGHTTP2_ERR_NOMEM;
                break;
            case APR_EOF:
                rv = 0;
                break;
            case APR_ECONNABORTED:
                rv = NGHTTP2_ERR_EOF;
                break;
            default:
                break;
        }
    }
    return h2_session_abort_int(session, rv);
}

apr_status_t h2_session_start(h2_session *session, int *rv)
{
    assert(session);
    /* Start the conversation by submitting our SETTINGS frame */
    apr_status_t status = APR_SUCCESS;
    *rv = 0;
    h2_config *config = h2_config_get(session->c);
    if (session->r) {
        /* better for vhost matching */
        config = h2_config_rget(session->r);
        
        /* 'h2c' mode: we should have a 'HTTP2-Settings' header with
         * base64 encoded client settings. */
        const char *s = apr_table_get(session->r->headers_in, "HTTP2-Settings");
        if (!s) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_EINVAL, session->r,
                          "HTTP2-Settings header missing in request");
            return APR_EINVAL;
        }
        unsigned char *cs = NULL;
        int dlen = h2_util_base64url_decode(&cs, s, session->pool);
        
        if (APLOGrdebug(session->r)) {
            char buffer[128];
            h2_util_hex_dump(buffer, 128, (char*)cs, dlen);
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, session->r,
                          "upgrading h2c session with HTTP2-Settings: %s -> %s (%d)",
                          s, buffer, dlen);
        }
        
        *rv = nghttp2_session_upgrade(session->ngh2, (uint8_t*)cs, dlen, NULL);
        if (*rv != 0) {
            status = APR_EINVAL;
            ap_log_rerror(APLOG_MARK, APLOG_ERR, status, session->r,
                          "nghttp2_session_upgrade: %s", nghttp2_strerror(*rv));
            return status;
        }
        
        /* Now we need to auto-open stream 1 for the request we got. */
        *rv = stream_open(session, 1);
        if (*rv != 0) {
            status = APR_EGENERAL;
            ap_log_rerror(APLOG_MARK, APLOG_ERR, status, session->r,
                          "open stream 1: %s", nghttp2_strerror(*rv));
            return status;
        }
        
        h2_stream * stream = h2_stream_set_get(session->streams, 1);
        if (stream == NULL) {
            status = APR_EGENERAL;
            ap_log_rerror(APLOG_MARK, APLOG_ERR, status, session->r,
                          "lookup of stream 1");
            return status;
        }
        
        status = h2_stream_rwrite(stream, session->r);
        if (status != APR_SUCCESS) {
            return status;
        }
        status = stream_end_headers(session, stream, 1);
        if (status != APR_SUCCESS) {
            return status;
        }
        status = h2_stream_write_eos(stream);
        if (status != APR_SUCCESS) {
            return status;
        }
    }

    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
            h2_config_geti(config, H2_CONF_MAX_HL_SIZE) },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
            h2_config_geti(config, H2_CONF_WIN_SIZE) },
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 
            h2_config_geti(config, H2_CONF_MAX_STREAMS) }, 
    };
    *rv = nghttp2_submit_settings(session->ngh2, NGHTTP2_FLAG_NONE,
                                 settings,
                                 sizeof(settings)/sizeof(settings[0]));
    if (*rv != 0) {
        status = APR_EGENERAL;
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      "nghttp2_submit_settings: %s", nghttp2_strerror(*rv));
    }
    
    return status;
}

h2_response *h2_session_pop_response(h2_session *session, 
                                     apr_bucket_brigade *data)
{
    assert(session);
    return h2_mplx_pop_response(session->mplx, data);
}


static int h2_session_want_read(h2_session *session)
{
    return nghttp2_session_want_read(session->ngh2);
}

static int h2_session_want_write(h2_session *session)
{
    return nghttp2_session_want_write(session->ngh2);
}

typedef struct {
    h2_session *session;
    int resume_count;
} resume_ctx;

static h2_stream *resume_on_data(void *ctx, h2_stream *stream) {
    resume_ctx *rctx = (resume_ctx*)ctx;
    h2_session *session = rctx->session;
    assert(session);
    assert(stream);
    
    if (h2_stream_is_suspended(stream)) {
        ap_log_perror(APLOG_MARK, APLOG_DEBUG, 0, stream->pool,
                      "h2_stream(%ld-%d): suspended, checking for DATA",
                      h2_mplx_get_id(stream->m), stream->id);
        if (h2_mplx_out_has_data_for(stream->m, h2_stream_get_id(stream))) {
            h2_stream_set_suspended(stream, 0);
            ++rctx->resume_count;
            
            int rv = nghttp2_session_resume_data(session->ngh2,
                                                 h2_stream_get_id(stream));
            ap_log_cerror(APLOG_MARK, nghttp2_is_fatal(rv)?
                          APLOG_ERR : APLOG_DEBUG, 0, session->c,
                          "h2_stream(%ld-%d): resuming stream %s",
                          session->id, stream->id, nghttp2_strerror(rv));
        }
    }
    return NULL;
}

static int h2_session_resume_streams_with_data(h2_session *session) {
    assert(session);
    if (!h2_stream_set_is_empty(session->streams)
        && session->mplx && !session->aborted) {
        resume_ctx ctx = { session, 0 };
        /* Resume all streams where we have data in the out queue and
         * which had been suspended before. */
        h2_stream_set_find(session->streams, resume_on_data, &ctx);
        return ctx.resume_count;
    }
    return 0;
}

static void update_window(void *ctx, int stream_id, apr_size_t bytes_read)
{
    h2_session *session = (h2_session*)ctx;
    nghttp2_session_consume(session->ngh2, stream_id, bytes_read);
}

static apr_status_t h2_session_update_windows(h2_session *session)
{
    return h2_mplx_in_update_windows(session->mplx, update_window, session);
}

apr_status_t h2_session_write(h2_session *session, apr_interval_time_t timeout)
{
    apr_status_t status = APR_EAGAIN;
    h2_response *response = NULL;
    int have_written = 0;
    
    assert(session);
    
    /* Check that any pending window updates are sent. */
    status = h2_session_update_windows(session);
    if (status == APR_SUCCESS) {
        have_written = 1;
    }
    else if (status != APR_EAGAIN) {
        return status;
    }
    
    if (h2_session_want_write(session)) {
        status = APR_SUCCESS;
        int rv = nghttp2_session_send(session->ngh2);
        if (rv != 0) {
            ap_log_cerror( APLOG_MARK, APLOG_INFO, 0, session->c,
                          "h2_session: send: %s", nghttp2_strerror(rv));
            if (nghttp2_is_fatal(rv)) {
                h2_session_abort_int(session, rv);
                status = APR_ECONNABORTED;
            }
        }
        have_written = 1;
    }
    
    /* If we have responses ready, submit them now. */
    apr_brigade_cleanup(session->bbtmp);
    while ((response = h2_session_pop_response(session, 
                                               session->bbtmp)) != NULL) {
        h2_stream *stream = h2_session_get_stream(session, response->stream_id);
        if (stream) {
            h2_stream_set_response(stream, response, session->bbtmp);
            status = h2_session_handle_response(session, stream);
            have_written = 1;
        }
        h2_response_destroy(response);
        response = NULL;
        apr_brigade_cleanup(session->bbtmp);
    }
    
    if (h2_session_resume_streams_with_data(session) > 0) {
        have_written = 1;
    }
    
    if (!have_written && timeout > 0 && !h2_session_want_write(session)) {
        status = h2_mplx_out_trywait(session->mplx, timeout, session->iowait);
        if (h2_session_resume_streams_with_data(session) > 0) {
            have_written = 1;
        }
    }
    
    if (h2_session_want_write(session)) {
        status = APR_SUCCESS;
        int rv = nghttp2_session_send(session->ngh2);
        if (rv != 0) {
            ap_log_cerror( APLOG_MARK, APLOG_INFO, 0, session->c,
                          "h2_session: send: %s", nghttp2_strerror(rv));
            if (nghttp2_is_fatal(rv)) {
                h2_session_abort_int(session, rv);
                status = APR_ECONNABORTED;
            }
        }
        have_written = 1;
    }
    
    if (have_written) {
        h2_conn_io_flush(&session->io);
    }
    
    reap_zombies(session);

    return status;
}

h2_stream *h2_session_get_stream(h2_session *session, int stream_id)
{
    assert(session);
    return h2_stream_set_get(session->streams, stream_id);
}

/* h2_io_on_read_cb implementation that offers the data read
 * directly to the session for consumption.
 */
static apr_status_t session_receive(const char *data, apr_size_t len,
                                    apr_size_t *readlen, int *done,
                                    void *puser)
{
    h2_session *session = (h2_session *)puser;
    assert(session);
    if (len > 0) {
        ssize_t n = nghttp2_session_mem_recv(session->ngh2,
                                             (const uint8_t *)data, len);
        if (n < 0) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, APR_EGENERAL,
                          session->c,
                          "h2_session: nghttp2_session_mem_recv error %d",
                          (int)n);
            if (nghttp2_is_fatal(n)) {
                *done = 1;
                h2_session_abort_int(session, n);
                return APR_EGENERAL;
            }
        }
        else {
            *readlen = n;
        }
    }
    return APR_SUCCESS;
}

apr_status_t h2_session_read(h2_session *session, apr_read_type_e block)
{
    assert(session);
    return h2_conn_io_read(&session->io, block, session_receive, session);
}

apr_status_t h2_session_close(h2_session *session)
{
    assert(session);
    return h2_conn_io_flush(&session->io);
}

void h2_session_set_stream_close_cb(h2_session *session, before_stream_close *cb)
{
    assert(session);
    session->before_stream_close_cb = cb;
}

void h2_session_set_stream_open_cb(h2_session *session, after_stream_open *cb)
{
    assert(session);
    session->after_stream_opened_cb = cb;
}

static h2_stream *match_any(void *ctx, h2_stream *stream) {
    return stream;
}

/* The session wants to send more DATA for the given stream.
 */
static ssize_t stream_data_cb(nghttp2_session *ng2s,
                              int32_t stream_id,
                              uint8_t *buf,
                              size_t length,
                              uint32_t *data_flags,
                              nghttp2_data_source *source,
                              void *puser)
{
    h2_session *session = (h2_session *)puser;
    assert(session);
    
    h2_stream *stream = h2_stream_set_get(session->streams, stream_id);
    if (!stream) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_NOTFOUND, session->c,
                      "h2_stream(%ld-%d): data requested but stream not found",
                      session->id, (int)stream_id);
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    assert(!h2_stream_is_suspended(stream));
    
    /* Try to pop data buckets from our queue for this stream
     * until we see EOS or the buffer is full.
     */
    apr_size_t nread = length;
    int eos = 0;
    apr_status_t status = h2_stream_read(stream, (char*)buf, &nread, &eos);

    switch (status) {
        case APR_SUCCESS:
            break;
            
        case APR_EAGAIN:
            /* If there is no data available, our session will automatically
             * suspend this stream and not ask for more data until we resume
             * it. Remember at our h2_stream that we need to do this.
             */
            nread = 0;
            h2_stream_set_suspended(stream, 1);
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                          "h2_stream(%ld-%d): suspending stream",
                          session->id, (int)stream_id);
            return NGHTTP2_ERR_DEFERRED;
            
        case APR_EOF:
            nread = 0;
            eos = 1;
            break;
            
        default:
            nread = 0;
            ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                          "h2_stream(%ld-%d): reading data",
                          session->id, (int)stream_id);
            return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    if (eos) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, session->c,
                  "h2_stream(%ld-%d): requested %ld, "
                  "sending %ld data bytes (eos=%d)",
                  session->id, (int)stream_id, (long)length, 
                  (ssize_t)nread, eos);
    
    return (ssize_t)nread;
}

typedef struct {
    nghttp2_nv *nv;
    size_t nvlen;
    size_t offset;
} nvctx_t;

static int count_headers(void *ctx, const char *key, const char *value)
{
    nvctx_t *nvctx = (nvctx_t*)ctx;
    nvctx->nvlen++;
    return 1;
}

static int add_headers(void *ctx, const char *key, const char *value)
{
    nvctx_t *nvctx = (nvctx_t*)ctx;
    if (nvctx->offset < nvctx->nvlen) {
        H2_CREATE_NV_CS_CS((&nvctx->nv[nvctx->offset]), key, value);
        nvctx->offset++;
    }
    return 1;
}

static int submit_response(h2_session *session, h2_response *response)
{
    nvctx_t nvctx = { NULL, 1 };
    nghttp2_data_provider provider = {
        response->stream_id, stream_data_cb
    };
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, session->c,
                  "h2_stream(%ld-%d): submitting response %s",
                  session->id, response->stream_id, response->http_status);
    
    apr_table_do(count_headers, &nvctx, response->headers, NULL);
    nvctx.nv = calloc(nvctx.nvlen, sizeof(nghttp2_nv));
    if (!nvctx.nv) {
        return NGHTTP2_ERR_NOMEM;
    }
    
    H2_CREATE_NV_LIT_CS((&nvctx.nv[0]), ":status", response->http_status);
    nvctx.offset = 1;
    apr_table_do(add_headers, &nvctx, response->headers, NULL);
    
    if (APLOGctrace2(session->c)) {
        for (int i = 0; i < nvctx.nvlen; ++i) {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, session->c,
                          "h2_stream(%ld-%d): resp header %s: %s",
                          session->id, response->stream_id, 
                          nvctx.nv[i].name, nvctx.nv[i].value);
        }
    }
    
    int rv = nghttp2_submit_response(session->ngh2, response->stream_id,
                                     nvctx.nv, nvctx.nvlen, &provider);
    free(nvctx.nv);
    
    if (rv != 0) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                      "h2_stream(%ld-%d): submit_response: %s",
                      session->id, response->stream_id, nghttp2_strerror(rv));
    }
    else {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_stream(%ld-%d): submitted response %s, rv=%d",
                      session->id, response->stream_id, 
                      response->http_status, rv);
    }
    return rv;
}

/* Start submitting the response to a stream request. This is possible
 * once we have all the response headers. The response body will be
 * read by the session using the callback we supply.
 */
apr_status_t h2_session_handle_response(h2_session *session, h2_stream *stream)
{
    assert(session);
    assert(stream);
    assert(stream->response);
    
    apr_status_t status = APR_SUCCESS;
    int rv = 0;
    if (stream->response->http_status) {
        rv = submit_response(session, stream->response);
    }
    else {
        rv = nghttp2_submit_rst_stream(session->ngh2, 0,
                                       stream->id, NGHTTP2_ERR_INVALID_STATE);
    }
    
    if (nghttp2_is_fatal(rv)) {
        status = APR_EGENERAL;
        h2_session_abort_int(session, rv);
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      "submit_response: %s",
                      nghttp2_strerror(rv));
    }
    return status;
}

int h2_session_is_done(h2_session *session)
{
    assert(session);
    return (session->aborted
            || !session->ngh2
            || (!nghttp2_session_want_read(session->ngh2)
                && !nghttp2_session_want_write(session->ngh2)));
}

static int log_stream(void *ctx, h2_stream *stream)
{
    h2_session *session = (h2_session *)ctx;
    assert(session);
    ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, session->c,
                  "h2_stream(%ld-%d): in set, suspended=%d, aborted=%d, "
                  "has_data=%d",
                  session->id, stream->id, stream->suspended, stream->aborted,
                  h2_mplx_out_has_data_for(session->mplx, stream->id));
    return 1;
}

void h2_session_log_stats(h2_session *session)
{
    assert(session);
    ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, session->c,
                  "h2_session(%ld): %ld open streams",
                  session->id, h2_stream_set_size(session->streams));
    h2_stream_set_iter(session->streams, log_stream, session);
}

static int frame_print(const nghttp2_frame *frame, char *buffer, size_t maxlen)
{
    char scratch[128];
    size_t s_len = sizeof(scratch)/sizeof(scratch[0]);
    
    switch (frame->hd.type) {
        case NGHTTP2_DATA: {
            return apr_snprintf(buffer, maxlen,
                                "DATA[length=%d, flags=%d, stream=%d, padlen=%d]",
                                (int)frame->hd.length, frame->hd.flags,
                                frame->hd.stream_id, (int)frame->data.padlen);
        }
        case NGHTTP2_HEADERS: {
            return apr_snprintf(buffer, maxlen,
                                "HEADERS[length=%d, hend=%d, stream=%d, eos=%d]",
                                (int)frame->hd.length,
                                !!(frame->hd.flags & NGHTTP2_FLAG_END_HEADERS),
                                frame->hd.stream_id,
                                !!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM));
        }
        case NGHTTP2_PRIORITY: {
            return apr_snprintf(buffer, maxlen,
                                "PRIORITY[length=%d, flags=%d, stream=%d]",
                                (int)frame->hd.length,
                                frame->hd.flags, frame->hd.stream_id);
        }
        case NGHTTP2_RST_STREAM: {
            return apr_snprintf(buffer, maxlen,
                                "RST_STREAM[length=%d, flags=%d, stream=%d]",
                                (int)frame->hd.length,
                                frame->hd.flags, frame->hd.stream_id);
        }
        case NGHTTP2_SETTINGS: {
            if (frame->hd.flags & NGHTTP2_FLAG_ACK) {
                return apr_snprintf(buffer, maxlen,
                                    "SETTINGS[ack=1, stream=%d]",
                                    frame->hd.stream_id);
            }
            return apr_snprintf(buffer, maxlen,
                                "SETTINGS[length=%d, stream=%d]",
                                (int)frame->hd.length, frame->hd.stream_id);
        }
        case NGHTTP2_PUSH_PROMISE: {
            return apr_snprintf(buffer, maxlen,
                                "PUSH_PROMISE[length=%d, hend=%d, stream=%d]",
                                (int)frame->hd.length,
                                !!(frame->hd.flags & NGHTTP2_FLAG_END_HEADERS),
                                frame->hd.stream_id);
        }
        case NGHTTP2_PING: {
            return apr_snprintf(buffer, maxlen,
                                "PING[length=%d, ack=%d, stream=%d]",
                                (int)frame->hd.length,
                                frame->hd.flags&NGHTTP2_FLAG_ACK,
                                frame->hd.stream_id);
        }
        case NGHTTP2_GOAWAY: {
            size_t len = (frame->goaway.opaque_data_len < s_len)?
            frame->goaway.opaque_data_len : s_len-1;
            memcpy(scratch, frame->goaway.opaque_data, len);
            scratch[len+1] = '\0';
            return apr_snprintf(buffer, maxlen, "GOAWAY[error=%d, reason='%s']",
                                frame->goaway.error_code, scratch);
        }
        case NGHTTP2_WINDOW_UPDATE: {
            return apr_snprintf(buffer, maxlen,
                                "WINDOW_UPDATE[length=%d, stream=%d]",
                                (int)frame->hd.length, frame->hd.stream_id);
        }
        default:
            return apr_snprintf(buffer, maxlen,
                                "FRAME[type=%d, length=%d, flags=%d, stream=%d]",
                                frame->hd.type, (int)frame->hd.length,
                                frame->hd.flags, frame->hd.stream_id);
    }
}

