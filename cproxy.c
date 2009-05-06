/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sysexits.h>
#include <pthread.h>
#include <assert.h>
#include <math.h>
#include <libmemcached/memcached.h>
#include "memcached.h"
#include "cproxy.h"
#include "work.h"

// From libmemcached.
//
memcached_return memcached_connect(memcached_server_st *ptr);
uint32_t         memcached_generate_hash(memcached_st *ptr,
                                         const char *key,
                                         size_t key_length);
void             memcached_quit_server(memcached_server_st *ptr,
                                       uint8_t io_death);

// Internal forward declarations.
//
downstream *downstream_list_remove(downstream *head, downstream *d);
void        downstream_timeout(const int fd,
                               const short which,
                               void *arg);
void        wait_queue_timeout(const int fd,
                               const short which,
                               void *arg);
void        upstream_error(conn *uc);
void        upstream_retry(void *data0, void *data1);
conn       *conn_list_remove(conn *head, conn **tail,
                             conn *c, bool *found);
bool        is_compatible_request(conn *existing, conn *candidate);

// Function tables.
//
conn_funcs cproxy_listen_funcs = {
    .conn_init                   = cproxy_init_upstream_conn,
    .conn_close                  = NULL,
    .conn_process_ascii_command  = NULL,
    .conn_process_binary_command = NULL,
    .conn_complete_nread_ascii   = NULL,
    .conn_complete_nread_binary  = NULL,
    .conn_pause                  = NULL,
    .conn_realtime               = NULL,
    .conn_binary_command_magic   = 0
};

conn_funcs cproxy_upstream_funcs = {
    .conn_init                   = NULL,
    .conn_close                  = cproxy_on_close_upstream_conn,
    .conn_process_ascii_command  = cproxy_process_upstream_ascii,
    .conn_process_binary_command = NULL,
    .conn_complete_nread_ascii   = cproxy_process_upstream_ascii_nread,
    .conn_complete_nread_binary  = NULL,
    .conn_pause                  = NULL,
    .conn_realtime               = cproxy_realtime,
    .conn_binary_command_magic   = PROTOCOL_BINARY_REQ
};

conn_funcs cproxy_downstream_funcs = {
    .conn_init                   = cproxy_init_downstream_conn,
    .conn_close                  = cproxy_on_close_downstream_conn,
    .conn_process_ascii_command  = cproxy_process_a2a_downstream,
    .conn_process_binary_command = cproxy_process_a2b_downstream,
    .conn_complete_nread_ascii   = cproxy_process_a2a_downstream_nread,
    .conn_complete_nread_binary  = cproxy_process_a2b_downstream_nread,
    .conn_pause                  = cproxy_on_pause_downstream_conn,
    .conn_realtime               = cproxy_realtime,
    .conn_binary_command_magic   = PROTOCOL_BINARY_RES
};

/* Main function to create a proxy struct.
 */
proxy *cproxy_create(char    *name,
                     int      port,
                     char    *config,
                     uint32_t config_ver,
                     proxy_behavior behavior) {
    assert(name != NULL);
    assert(port > 0);
    assert(config != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "cproxy_create on port %d, downstream %s\n",
                port, config);

    proxy *p = (proxy *) calloc(1, sizeof(proxy));
    if (p != NULL) {
        p->name       = strdup(name);
        p->port       = port;
        p->config     = strdup(config);
        p->config_ver = config_ver;
        p->behavior   = behavior;

        p->listening        = 0;
        p->listening_failed = 0;

        pthread_mutex_init(&p->proxy_lock, NULL);

        assert(p->behavior.nthreads == settings.num_threads);

        p->thread_data_num = p->behavior.nthreads;
        p->thread_data = (proxy_td *) calloc(p->thread_data_num,
                                             sizeof(proxy_td));
        if (p->thread_data != NULL) {
            // We start at 1, because thread[0] is the main listen/accept
            // thread, and not a true worker thread.  Too lazy to save
            // the wasted thread[0] slot memory.
            //
            for (int i = 1; i < p->thread_data_num; i++) {
                proxy_td *ptd = &p->thread_data[i];
                ptd->proxy = p;
                ptd->waiting_any_downstream_head = NULL;
                ptd->waiting_any_downstream_tail = NULL;
                ptd->downstream_reserved = NULL;
                ptd->downstream_released = NULL;
                ptd->downstream_tot = 0;
                ptd->downstream_num = 0;
                ptd->downstream_max = p->behavior.downstream_max;
                ptd->downstream_assigns = 0;

                // TODO: Handle ascii-to-binary protocol.
                //
                assert(IS_PROXY(p->behavior.downstream_prot));

                if (IS_BINARY(p->behavior.downstream_prot))
                    ptd->propagate_downstream =
                        cproxy_forward_a2b_downstream;
                else
                    ptd->propagate_downstream =
                        cproxy_forward_a2a_downstream;

                ptd->timeout_tv.tv_sec = 0;
                ptd->timeout_tv.tv_usec = 0;

                ptd->stats.num_upstream = 0;
                ptd->stats.num_downstream_conn = 0;

                cproxy_reset_stats(&ptd->stats);
            }

            return p;
        }

        free(p->name);
        free(p->config);
    }

    free(p);

    return NULL;
}

void cproxy_reset_stats(proxy_stats *ps) {
    assert(ps);

    // Only clear the tot_xxx stats, not the num_xxx ones.
    //
    ps->tot_upstream = 0;
    ps->tot_downstream_conn = 0;
    ps->tot_downstream_released = 0;
    ps->tot_downstream_reserved = 0;
    ps->tot_downstream_freed = 0;
    ps->tot_downstream_quit_server = 0;
    ps->tot_downstream_max_reached = 0;
    ps->tot_downstream_create_failed = 0;
    ps->tot_assign_downstream = 0;
    ps->tot_assign_upstream = 0;
    ps->tot_reset_upstream_avail = 0;
    ps->tot_oom = 0;
    ps->tot_retry = 0;
}

/* Must be called on the main listener thread.
 */
int cproxy_listen(proxy *p) {
    assert(p != NULL);
    assert(is_listen_thread());

    if (settings.verbose > 1)
        fprintf(stderr, "cproxy_listen on port %d, downstream %s\n",
                p->port, p->config);

    conn *listen_conn_orig = listen_conn;

    // Idempotent, remembers if it already created listening socket(s).
    //
    if (p->listening == 0) {
        if (server_socket(p->port, proxy_upstream_ascii_prot) == 0) {
            assert(listen_conn != NULL);

            // The listen_conn global list is changed by server_socket(),
            // which adds a new listening conn on p->port for each bindable
            // host address.
            //
            // For example, after the call to server_socket(), there
            // might be two new listening conn's -- one for localhost,
            // another for 127.0.0.1.
            //
            conn *c = listen_conn;
            while (c != NULL &&
                   c != listen_conn_orig) {
                if (settings.verbose > 1)
                    fprintf(stderr,
                            "<%d cproxy listening on port %d, downstream %s\n",
                            c->sfd, p->port, p->config);

                p->listening++;

                // TODO: Listening conn's never seem to close,
                //       but need to handle cleanup if they do,
                //       such as if we handle graceful shutdown one day.
                //
                c->extra = p;
                c->funcs = &cproxy_listen_funcs;
                c = c->next;
            }
        } else {
            p->listening_failed++;
        }
    }

    return p->listening;
}

/* Finds the proxy_td associated with a worker thread.
 */
proxy_td *cproxy_find_thread_data(proxy *p, pthread_t thread_id) {
    if (p != NULL) {
        int i = thread_index(thread_id);

        // 0 is the main listen thread, not a worker thread.
        assert(i > 0);
        assert(i < p->thread_data_num);

        if (i > 0 && i < p->thread_data_num)
            return &p->thread_data[i];
    }

    return NULL;
}

void cproxy_init_upstream_conn(conn *c) {
    assert(c != NULL);

    // We're called once per client/upstream conn early in its
    // lifecycle, on the worker thread, so it's a good place
    // to record the proxy_td into the conn->extra.
    //
    proxy *p = c->extra;
    assert(p != NULL);

    proxy_td *ptd = cproxy_find_thread_data(p, pthread_self());
    assert(ptd != NULL);

    ptd->stats.num_upstream++;
    ptd->stats.tot_upstream++;

    c->extra = ptd;
    c->funcs = &cproxy_upstream_funcs;
}

void cproxy_init_downstream_conn(conn *c) {
    downstream *d = c->extra;
    assert(d != NULL);

    d->ptd->stats.num_downstream_conn++;
    d->ptd->stats.tot_downstream_conn++;
}

void cproxy_on_close_upstream_conn(conn *c) {
    assert(c != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d cproxy_on_close_upstream_conn\n", c->sfd);

    proxy_td *ptd = c->extra;
    assert(ptd != NULL);
    c->extra = NULL;

    ptd->stats.num_upstream--;
    assert(ptd->stats.num_upstream >= 0);

    // Delink from any reserved downstream.
    //
    for (downstream *d = ptd->downstream_reserved; d != NULL; d = d->next) {
        bool found = false;

        d->upstream_conn = conn_list_remove(d->upstream_conn, NULL,
                                            c, &found);
        if (d->upstream_conn == NULL) {
            d->upstream_suffix = NULL;

            // Don't need to do anything else, as we'll now just
            // read and drop any remaining inflight downstream replies.
            // Eventually, the downstream will be released.
        }

        // If the downstream was reserved for this upstream conn,
        // also clear the upstream from any multiget de-duplication
        // tracking structures.
        //
        if (found) {
            if (d->multiget != NULL)
                g_hash_table_foreach(d->multiget,
                                     multiget_remove_upstream, c);

            // The downstream conn's might have iov's that
            // point to the upstream conn's buffers.  Also, the
            // downstream conn might be in all sorts of states
            // (conn_read, write, mwrite, pause), and we want
            // to be careful about the downstream channel being
            // half written.
            //
            // The safest, but inefficient, thing to do then is
            // to close any conn_mwrite downstream conns.
            //
            int n = memcached_server_count(&d->mst);

            for (int i = 0; i < n; i++) {
                conn *c = d->downstream_conns[i];
                if (c != NULL &&
                    c->state == conn_mwrite) {
                    c->msgcurr = 0;
                    c->msgused = 0;
                    c->iovused = 0;

                    cproxy_close_conn(c);
                }
            }
        }
    }

    // Delink from wait queue.
    //
    ptd->waiting_any_downstream_head =
        conn_list_remove(ptd->waiting_any_downstream_head,
                         &ptd->waiting_any_downstream_tail,
                         c, NULL);
}

void cproxy_on_close_downstream_conn(conn *c) {
    assert(c != NULL);
    assert(c->sfd >= 0);
    assert(c->state == conn_closing);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d cproxy_on_close_downstream_conn\n", c->sfd);

    downstream *d = c->extra;

    // Might have been set to NULL during cproxy_free_downstream().
    //
    if (d == NULL)
        return;

    c->extra = NULL;

    int n = memcached_server_count(&d->mst);

    for (int i = 0; i < n; i++) {
        if (d->downstream_conns[i] == c) {
            d->downstream_conns[i] = NULL;

            if (settings.verbose > 1)
                fprintf(stderr,
                        "<%d cproxy_on_close_downstream_conn quit_server\n",
                        c->sfd);

            d->ptd->stats.tot_downstream_quit_server++;

            assert(d->mst.hosts[i].fd == c->sfd);
            memcached_quit_server(&d->mst.hosts[i], 1);
            assert(d->mst.hosts[i].fd == -1);
        }
    }

    proxy_td *ptd = d->ptd;
    assert(ptd);

    ptd->stats.num_downstream_conn--;
    assert(ptd->stats.num_downstream_conn >= 0);

    conn *uc_retry = NULL;

    if (d->upstream_conn != NULL &&
        d->downstream_used == 1) {
        // TODO: Revisit downstream close error handling.
        //       Should we propagate error when...
        //       - any downstream conn closes?
        //       - all downstream conns closes?
        //       - last downstream conn closes?  Current behavior.
        //
        if (d->upstream_suffix == NULL)
            d->upstream_suffix = "SERVER_ERROR proxy downstream closed\r\n";

        // We sometimes see that drive_machine/transmit will not see
        // a closed connection error during conn_mwrite, possibly
        // due to non-blocking sockets.  Because of this, drive_machine
        // thinks it has a successful downstream request send and
        // moves the state forward trying to read a response from
        // the downstream conn (conn_new_cmd, conn_read, etc), and
        // only then do we finally see the conn close situation,
        // ending up here.  That is, drive_machine only
        // seems to move to conn_closing from conn_read.
        //
        // If we haven't received any reply yet, we retry once.
        //
        // TODO: Reconsider retry behavior, is it right in all situations?
        //
        if (c->rcurr != NULL &&
            c->rbytes == 0 &&
            d->downstream_used_start == d->downstream_used &&
            d->downstream_used_start == 1 &&
            d->upstream_conn->next == NULL &&
            d->upstream_conn->cmd_retries < 1) {
            d->upstream_conn->cmd_retries++;
            uc_retry = d->upstream_conn;
            d->upstream_suffix = NULL;
        }
    }

    // Are we over-decrementing here, and in handling conn_pause?
    //
    // Case 1: we're in conn_pause, and socket is closed concurrently.
    // We unpause due to reserve, we move to conn_write/conn_mwrite,
    // fail and move to conn_closing.  So, no over-decrement.
    //
    // Case 2: we're waiting for a downstream response in conn_new_cmd,
    // and socket is closed concurrently.  State goes to conn_closing,
    // so, no over-decrement.
    //
    // Case 3: we've finished processing downstream response (in
    // conn_parse_cmd or conn_nread), and the downstream socket
    // is closed concurrently.  We then move to conn_pause,
    // and same as Case 1.
    //
    cproxy_release_downstream_conn(d, c);

    // Setup a retry after unwinding the call stack.
    // We use the work_queue, because our caller, conn_close(),
    // is likely to blow away our fd if we try to reconnect
    // right now.
    //
    if (uc_retry != NULL) {
        if (settings.verbose > 1)
            fprintf(stderr, "%d cproxy retrying\n", uc_retry->sfd);

        ptd->stats.tot_retry++;

        assert(uc_retry->thread);
        assert(uc_retry->thread->work_queue);

        work_send(uc_retry->thread->work_queue,
                  upstream_retry, ptd, uc_retry);
    }
}

void upstream_retry(void *data0, void *data1) {
    proxy_td *ptd = data0;
    assert(ptd);

    conn *uc = data1;
    assert(uc);

    cproxy_pause_upstream_for_downstream(ptd, uc);
}

void cproxy_add_downstream(proxy_td *ptd) {
    assert(ptd != NULL);
    assert(ptd->proxy != NULL);

    if (ptd->downstream_num < ptd->downstream_max) {
        if (settings.verbose > 1)
            fprintf(stderr, "cproxy_add_downstream %d %d\n",
                    ptd->downstream_num,
                    ptd->downstream_max);

        char *config = NULL;

        pthread_mutex_lock(&ptd->proxy->proxy_lock);

        if (ptd->proxy->config != NULL)
            config = strdup(ptd->proxy->config);

        uint32_t       config_ver = ptd->proxy->config_ver;
        proxy_behavior behavior   = ptd->proxy->behavior;

        pthread_mutex_unlock(&ptd->proxy->proxy_lock);

        // The config will be NULL if the proxy is shutting down.
        //
        if (config != NULL) {
            downstream *d =
                cproxy_create_downstream(config,
                                         config_ver,
                                         behavior);
            if (d != NULL) {
                d->ptd = ptd;
                ptd->downstream_tot++;
                ptd->downstream_num++;
                cproxy_release_downstream(d, true);
            } else {
                ptd->stats.tot_downstream_create_failed++;
            }

            free(config);
        }
    } else {
        ptd->stats.tot_downstream_max_reached++;
    }
}

downstream *cproxy_reserve_downstream(proxy_td *ptd) {
    assert(ptd != NULL);

    // Loop in case we need to clear out downstreams
    // that have outdated configs.
    //
    while (true) {
        downstream *d;

        d = ptd->downstream_released;
        if (d == NULL)
            cproxy_add_downstream(ptd);

        d = ptd->downstream_released;
        if (d == NULL)
            return NULL;

        ptd->downstream_released = d->next;

        assert(d->upstream_conn == NULL);
        assert(d->upstream_suffix == NULL);
        assert(d->downstream_used == 0);
        assert(d->downstream_used_start == 0);
        assert(d->multiget == NULL);
        assert(d->merger == NULL);
        assert(d->timeout_tv.tv_sec == 0);
        assert(d->timeout_tv.tv_usec == 0);

        d->upstream_conn = NULL;
        d->upstream_suffix = NULL;
        d->downstream_used = 0;
        d->downstream_used_start = 0;
        d->multiget = NULL;
        d->merger = NULL;
        d->timeout_tv.tv_sec = 0;
        d->timeout_tv.tv_usec = 0;

        if (cproxy_check_downstream_config(d)) {
            ptd->downstream_reserved =
                downstream_list_remove(ptd->downstream_reserved, d);
            ptd->downstream_released =
                downstream_list_remove(ptd->downstream_released, d);

            d->next = ptd->downstream_reserved;
            ptd->downstream_reserved = d;

            ptd->stats.tot_downstream_reserved++;

            return d;
        }

        cproxy_free_downstream(d);
    }
}

bool cproxy_release_downstream(downstream *d, bool force) {
    assert(d != NULL);
    assert(d->ptd != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "release_downstream\n");

    d->ptd->stats.tot_downstream_released++;

    // Delink upstream conns.
    //
    while (d->upstream_conn != NULL) {
        if (d->merger != NULL) {
            // TODO: Allow merger callback to be func pointer.
            //
            g_hash_table_foreach(d->merger,
                                 protocol_stats_foreach_write,
                                 d->upstream_conn);

            if (update_event(d->upstream_conn, EV_WRITE | EV_PERSIST)) {
                conn_set_state(d->upstream_conn, conn_mwrite);
            } else {
                d->ptd->stats.tot_oom++;
                cproxy_close_conn(d->upstream_conn);
            }
        }

        if (d->upstream_suffix != NULL) {
            // Do a last write on the upstream.  For example,
            // the upstream_suffix might be "END\r\n" or other
            // way to mark the end of a scatter-gather or
            // multiline response.
            //
            if (add_iov(d->upstream_conn,
                        d->upstream_suffix,
                        strlen(d->upstream_suffix)) == 0 &&
                update_event(d->upstream_conn, EV_WRITE | EV_PERSIST)) {
                conn_set_state(d->upstream_conn, conn_mwrite);
            } else {
                d->ptd->stats.tot_oom++;
                cproxy_close_conn(d->upstream_conn);
            }
        }

        conn *curr = d->upstream_conn;
        d->upstream_conn = d->upstream_conn->next;
        curr->next = NULL;
    }

    // Free extra hash tables.
    //
    if (d->multiget != NULL) {
        g_hash_table_foreach(d->multiget, multiget_foreach_free, NULL);
        g_hash_table_destroy(d->multiget);
        d->multiget = NULL;
    }

    if (d->merger != NULL) {
        g_hash_table_foreach(d->merger, protocol_stats_foreach_free, NULL);
        g_hash_table_destroy(d->merger);
        d->merger = NULL;
    }

    if (d->timeout_tv.tv_sec != 0 ||
        d->timeout_tv.tv_usec != 0)
        evtimer_del(&d->timeout_event);

    d->timeout_tv.tv_sec = 0;
    d->timeout_tv.tv_usec = 0;

    d->upstream_conn = NULL;
    d->upstream_suffix = NULL; // No free(), expecting a static string.
    d->downstream_used = 0;
    d->downstream_used_start = 0;
    d->multiget = NULL;
    d->merger = NULL;

    // If this downstream still has the same configuration as our top-level
    // proxy config, go back onto the available, released downstream list.
    //
    if (cproxy_check_downstream_config(d) || force) {
        // TODO: Consider adding a downstream->prev backpointer
        //       or doubly-linked list to save on this scan.
        //
        d->ptd->downstream_reserved =
            downstream_list_remove(d->ptd->downstream_reserved, d);
        d->ptd->downstream_released =
            downstream_list_remove(d->ptd->downstream_released, d);

        d->next = d->ptd->downstream_released;
        d->ptd->downstream_released = d;

        return true;
    }

    cproxy_free_downstream(d);

    return false;
}

void cproxy_free_downstream(downstream *d) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->upstream_conn == NULL);
    assert(d->multiget == NULL);
    assert(d->merger == NULL);
    assert(d->timeout_tv.tv_sec == 0);
    assert(d->timeout_tv.tv_usec == 0);

    if (settings.verbose > 1)
        fprintf(stderr, "cproxy_free_downstream\n");

    d->ptd->stats.tot_downstream_freed++;

    d->ptd->downstream_reserved =
        downstream_list_remove(d->ptd->downstream_reserved, d);
    d->ptd->downstream_released =
        downstream_list_remove(d->ptd->downstream_released, d);

    d->ptd->downstream_num--;
    assert(d->ptd->downstream_num >= 0);

    int n = memcached_server_count(&d->mst);

    for (int i = 0; i < n; i++)
        d->downstream_conns[i]->extra = NULL;

    // This will close sockets, which will force associated conn's
    // to go to conn_closing state.  Since we've already cleared
    // the conn->extra pointers, there's no extra release/free.
    //
    memcached_free(&d->mst);

    if (d->timeout_tv.tv_sec != 0 ||
        d->timeout_tv.tv_usec != 0)
        evtimer_del(&d->timeout_event);

    d->timeout_tv.tv_sec = 0;
    d->timeout_tv.tv_usec = 0;

    if (d->downstream_conns != NULL)
        free(d->downstream_conns);

    if (d->config != NULL)
        free(d->config);

    free(d);
}

/* The config input is something libmemcached can parse.
 * See memcached_servers_parse().
 */
downstream *cproxy_create_downstream(char *config,
                                     uint32_t config_ver,
                                     proxy_behavior behavior) {
    assert(config != NULL);

    downstream *d = (downstream *) calloc(1, sizeof(downstream));
    if (d != NULL) {
        d->config     = strdup(config);
        d->config_ver = config_ver;
        d->behavior   = behavior;

        if (settings.verbose > 1)
            fprintf(stderr,
                    "cproxy_create_downstream: %s, %u\n",
                    config, config_ver);

        if (memcached_create(&d->mst) != NULL) {
            memcached_behavior_set(&d->mst, MEMCACHED_BEHAVIOR_NO_BLOCK, 1);

            memcached_server_st *mservers;

            mservers = memcached_servers_parse(d->config);
            if (mservers != NULL) {
                memcached_server_push(&d->mst, mservers);
                memcached_server_list_free(mservers);
                mservers = NULL;

                int nconns = memcached_server_count(&d->mst);

                d->downstream_conns = (conn **)
                    calloc(nconns, sizeof(conn *));
                if (d->downstream_conns != NULL) {
                    return d;
                }
            } else {
                if (settings.verbose > 1)
                    fprintf(stderr, "mserver_parse failed: %s\n",
                            config);
            }

            if (mservers != NULL)
                memcached_server_list_free(mservers);

            memcached_free(&d->mst);
        }

        free(d->config);
        free(d);
    }

    return NULL;
}

/* See if the downstream config matches the top-level proxy config.
 */
bool cproxy_check_downstream_config(downstream *d) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->ptd->proxy != NULL);

    int rv = false;

    pthread_mutex_lock(&d->ptd->proxy->proxy_lock);

    if (d->config_ver == d->ptd->proxy->config_ver) {
        rv = true;
    } else if (d->config != NULL &&
               d->ptd->proxy->config != NULL &&
               strcmp(d->config, d->ptd->proxy->config) == 0) {
        d->config_ver = d->ptd->proxy->config_ver;
        d->behavior   = d->ptd->proxy->behavior;
        rv = true;
    }

    pthread_mutex_unlock(&d->ptd->proxy->proxy_lock);

    return rv;
}

int cproxy_connect_downstream(downstream *d, LIBEVENT_THREAD *thread) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->ptd->downstream_released != d); // Should not be in free list.
    assert(d->downstream_conns != NULL);
    assert(IS_PROXY(d->behavior.downstream_prot));
    assert(memcached_server_count(&d->mst) > 0);
    assert(thread != NULL);
    assert(thread->base != NULL);

    memcached_return rc;

    int s = 0; // Number connected.
    int n = memcached_server_count(&d->mst);

    for (int i = 0; i < n; i++) {
        if (d->downstream_conns[i] == NULL) {
            rc = memcached_connect(&d->mst.hosts[i]);
            if (rc == MEMCACHED_SUCCESS) {
                int fd = d->mst.hosts[i].fd;
                if (fd >= 0) {
                    d->downstream_conns[i] =
                        conn_new(fd, conn_pause, 0, DATA_BUFFER_SIZE,
                                 d->behavior.downstream_prot,
                                 thread->base,
                                 &cproxy_downstream_funcs, d);
                    if (d->downstream_conns[i] != NULL)
                        d->downstream_conns[i]->thread = thread;
                }
            }
        }
        if (d->downstream_conns[i] != NULL)
            s++;
    }

    return s;
}

conn *cproxy_find_downstream_conn(downstream *d,
                                  char *key, int key_length) {
    assert(d != NULL);
    assert(d->downstream_conns != NULL);
    assert(key != NULL);
    assert(key_length > 0);

    int s = cproxy_server_index(d, key, key_length);
    if (s >= 0 &&
        s < memcached_server_count(&d->mst)) {
        return d->downstream_conns[s];
    }
    return NULL;
}

bool cproxy_prep_conn_for_write(conn *c) {
    if (c != NULL) {
        assert(c->item == NULL);
        assert(IS_PROXY(c->protocol));
        assert(c->ilist != NULL);
        assert(c->isize > 0);
        assert(c->suffixlist != NULL);
        assert(c->suffixsize > 0);

        c->icurr      = c->ilist;
        c->ileft      = 0;
        c->suffixcurr = c->suffixlist;
        c->suffixleft = 0;

        c->msgcurr = 0; // TODO: Mem leak just by blowing these to 0?
        c->msgused = 0;
        c->iovused = 0;

        if (add_msghdr(c) == 0)
            return true;

        if (settings.verbose > 1)
            fprintf(stderr,
                    "%d: cproxy_prep_conn_for_write failed\n", c->sfd);
    }

    return false;
}

int cproxy_server_index(downstream *d, char *key, size_t key_length) {
    assert(d != NULL);
    assert(key != NULL);
    assert(key_length > 0);

    // memcached_return rc;
    //
    // rc = memcached_validate_key_length(key_length,
    //          d->mst.flags & MEM_BINARY_PROTOCOL);
    // unlikely (rc != MEMCACHED_SUCCESS)
    //    return -1;

    if (memcached_server_count(&d->mst) <= 0)
        return -1;

    // if (memcached_key_test((char **) &key, &key_length, 1) ==
    //     MEMCACHED_BAD_KEY_PROVIDED)
    //     return -1;

    return (int) memcached_generate_hash(&d->mst, key, key_length);
}

void cproxy_assign_downstream(proxy_td *ptd) {
    assert(ptd != NULL);
    assert(ptd->propagate_downstream != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "assign_downstream\n");

    ptd->downstream_assigns++;

    uint64_t da = ptd->downstream_assigns;

    // Key loop that tries to reserve any available, released
    // downstream resources to waiting upstream conns.
    //
    // Remember the wait list tail when we start, in case more
    // upstream conns are tacked onto the wait list while we're
    // processing.  This helps avoid infinite loop where upstream
    // conns just keep on moving to the tail.
    //
    conn *tail = ptd->waiting_any_downstream_tail;
    bool  stop = false;

    while (ptd->waiting_any_downstream_head != NULL && !stop) {
        if (ptd->waiting_any_downstream_head == tail)
            stop = true;

        downstream *d = cproxy_reserve_downstream(ptd);
        if (d == NULL)
            break; // If no downstreams are available, stop loop.

        assert(d->upstream_conn == NULL);
        assert(d->downstream_used == 0);
        assert(d->downstream_used_start == 0);
        assert(d->multiget == NULL);
        assert(d->merger == NULL);
        assert(d->timeout_tv.tv_sec == 0);
        assert(d->timeout_tv.tv_usec == 0);

        // We have a downstream reserved, so assign the first
        // waiting upstream conn to it.
        //
        d->upstream_conn = ptd->waiting_any_downstream_head;
        ptd->waiting_any_downstream_head =
            ptd->waiting_any_downstream_head->next;
        if (ptd->waiting_any_downstream_head == NULL)
            ptd->waiting_any_downstream_tail = NULL;
        d->upstream_conn->next = NULL;

        ptd->stats.tot_assign_downstream++;
        ptd->stats.tot_assign_upstream++;

        // Add any compatible upstream conns to the downstream.
        // By compatible, for example, we mean multi-gets from
        // different upstreams so we can de-deplicate get keys.
        //
        conn *uc_last = d->upstream_conn;

        while (is_compatible_request(uc_last,
                                     ptd->waiting_any_downstream_head)) {
            uc_last->next = ptd->waiting_any_downstream_head;

            ptd->waiting_any_downstream_head =
                ptd->waiting_any_downstream_head->next;
            if (ptd->waiting_any_downstream_head == NULL)
                ptd->waiting_any_downstream_tail = NULL;

            uc_last = uc_last->next;
            uc_last->next = NULL;

            ptd->stats.tot_assign_upstream++;
        }

        if (settings.verbose > 1)
            fprintf(stderr, "assign_downstream, matched to upstream %d\n",
                    d->upstream_conn->sfd);

        if (!ptd->propagate_downstream(d)) {
            // During propagate_downstream(), we might have
            // recursed, especially in error situation if
            // a downstream conn got closed and released.
            // Check before we touch d anymore.
            //
            if (da != ptd->downstream_assigns)
                break;

            while (d->upstream_conn != NULL) {
                conn *uc = d->upstream_conn;

                if (settings.verbose > 1)
                    fprintf(stderr,
                            "%d could not forward upstream to downstream\n",
                            uc->sfd);

                upstream_error(uc);

                conn *curr = d->upstream_conn;
                d->upstream_conn = d->upstream_conn->next;
                curr->next = NULL;
            }

            cproxy_release_downstream(d, false);
        }
    }

    if (settings.verbose > 1)
        fprintf(stderr, "assign_downstream, done\n");
}

void upstream_error(conn *uc) {
    assert(uc);
    assert(uc->state == conn_pause);

    proxy_td *ptd = uc->extra;
    assert(ptd != NULL);

    // TODO: Handle upstream binary protocol.
    //
    // Send an END on get/gets instead of generic SERVER_ERROR.
    //
    if (uc->cmd == -1 &&
        uc->cmd_start != NULL &&
        strncmp(uc->cmd_start, "get", 3) == 0)
        out_string(uc, "END");
    else
        out_string(uc, "SERVER_ERROR proxy write to downstream");

    if (!update_event(uc, EV_WRITE | EV_PERSIST)) {
        ptd->stats.tot_oom++;
        cproxy_close_conn(uc);
    }
}

void cproxy_reset_upstream(conn *uc) {
    assert(uc != NULL);

    proxy_td *ptd = uc->extra;
    assert(ptd != NULL);

    conn_set_state(uc, conn_new_cmd);

    if (uc->rbytes <= 0) {
        if (!update_event(uc, EV_READ | EV_PERSIST)) {
            ptd->stats.tot_oom++;
            cproxy_close_conn(uc);
        }

        return; // Return either way.
    }

    // TODO: Subtle potential bug, where we may have already
    // read incoming bytes into the uc's buffer, so that
    // libevent never sees any EV_READ events, leaving the
    // uc seemingly stuck, never hitting drive_machine() loop.
    //
    // This depends on what libevent does here.
    //
    // May need to use the work_queue to call drive_machine() on the uc?
    //
    if (settings.verbose > 1)
        fprintf(stderr, "cproxy_reset_upstream with bytes available\n");

    ptd->stats.tot_reset_upstream_avail++;
}

bool cproxy_dettach_if_noreply(downstream *d, conn *uc) {
    if (uc->noreply) {
        uc->noreply        = false;
        d->upstream_conn   = NULL;
        d->upstream_suffix = NULL;

        cproxy_reset_upstream(uc);

        return true;
    }

    return false;
}

void cproxy_wait_any_downstream(proxy_td *ptd, conn *uc) {
    assert(uc != NULL);
    assert(ptd != NULL);
    assert(!ptd->waiting_any_downstream_tail ||
           !ptd->waiting_any_downstream_tail->next);

    // Add the upstream conn to the wait list.
    //
    uc->next = NULL;
    if (ptd->waiting_any_downstream_tail != NULL)
        ptd->waiting_any_downstream_tail->next = uc;
    ptd->waiting_any_downstream_tail = uc;
    if (ptd->waiting_any_downstream_head == NULL)
        ptd->waiting_any_downstream_head = uc;
}

void cproxy_release_downstream_conn(downstream *d, conn *c) {
    assert(c != NULL);
    assert(d != NULL);

    proxy_td *ptd = d->ptd;
    assert(ptd != NULL);

    if (settings.verbose > 1)
        fprintf(stderr,
                "%d release_downstream_conn, downstream_used %d %d\n",
                c->sfd, d->downstream_used, d->downstream_used_start);

    d->downstream_used--;
    if (d->downstream_used <= 0) {
        // The downstream_used count might go < 0 when if there's
        // an early error and we decide to close the downstream
        // conn, before anything gets sent or before the
        // downstream_used was able to be incremented.
        //
        cproxy_release_downstream(d, false);
        cproxy_assign_downstream(ptd);
    }
}

void cproxy_on_pause_downstream_conn(conn *c) {
    assert(c != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d cproxy_on_pause_downstream_conn\n",
                c->sfd);

    downstream *d = c->extra;
    assert(d != NULL);

    // Must update_event() before releasing the downstream conn,
    // because the release might call udpate_event(), too,
    // and we don't want to override its work.
    //
    if (update_event(c, 0))
        cproxy_release_downstream_conn(d, c);
    else
        cproxy_close_conn(c);
}

void cproxy_pause_upstream_for_downstream(proxy_td *ptd, conn *upstream) {
    assert(ptd != NULL);
    assert(upstream != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "%d pause_upstream_for_downstream\n",
                upstream->sfd);

    conn_set_state(upstream, conn_pause);

    cproxy_wait_any_downstream(ptd, upstream);

    if (ptd->timeout_tv.tv_sec == 0 &&
        ptd->timeout_tv.tv_usec == 0)
        cproxy_start_wait_queue_timeout(ptd, upstream);

    cproxy_assign_downstream(ptd);
}

bool cproxy_start_wait_queue_timeout(proxy_td *ptd, conn *uc) {
    assert(ptd);
    assert(uc);
    assert(uc->thread);
    assert(uc->thread->base);

    proxy *p = ptd->proxy;
    assert(p != NULL);

    pthread_mutex_lock(&p->proxy_lock);
    ptd->timeout_tv = p->behavior.wait_queue_timeout;
    pthread_mutex_unlock(&p->proxy_lock);

    if (ptd->timeout_tv.tv_sec != 0 ||
        ptd->timeout_tv.tv_usec != 0) {
        evtimer_set(&ptd->timeout_event, wait_queue_timeout, ptd);

        event_base_set(uc->thread->base, &ptd->timeout_event);

        return evtimer_add(&ptd->timeout_event, &ptd->timeout_tv) == 0;
    }

    return true;
}

void wait_queue_timeout(const int fd,
                        const short which,
                        void *arg) {
    proxy_td *ptd = arg;
    assert(ptd != NULL);

    proxy *p = ptd->proxy;
    assert(p != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "proxy_td_timeout\n");

    // This timer callback is invoked when an upstream conn
    // has been in the wait queue for too long.
    //
    if (ptd->timeout_tv.tv_sec != 0 ||
        ptd->timeout_tv.tv_usec != 0) {
        evtimer_del(&ptd->timeout_event);

        ptd->timeout_tv.tv_sec = 0;
        ptd->timeout_tv.tv_usec = 0;

        if (settings.verbose > 1)
            fprintf(stderr, "proxy_td_timeout cleared\n");

        pthread_mutex_lock(&p->proxy_lock);
        struct timeval wqt = p->behavior.wait_queue_timeout;
        pthread_mutex_unlock(&p->proxy_lock);

        // TODO: Should have better than second resolution,
        //       except current_time is limited to just
        //       second resolution.
        //
        rel_time_t wqt_sec = wqt.tv_sec + (wqt.tv_usec / 1000000.0);

        // Run through all the old upstream conn's in
        // the wait queue, remove them, and emit errors
        // on them.  And then start a new timer if needed.
        //
        conn *uc_curr = ptd->waiting_any_downstream_head;
        while (uc_curr != NULL) {
            conn *uc = uc_curr;

            uc_curr = uc_curr->next;

            // Check if upstream conn is old and should be removed.
            //
            if (uc->cmd_start_time <= (current_time - wqt_sec)) {
                if (settings.verbose > 1)
                    fprintf(stderr, "proxy_td_timeout sending error %d\n",
                            uc->sfd);

                ptd->waiting_any_downstream_head =
                    conn_list_remove(ptd->waiting_any_downstream_head,
                                     &ptd->waiting_any_downstream_tail,
                                     uc, NULL); // TODO: O(N^2).

                upstream_error(uc);
            }
        }

        if (ptd->waiting_any_downstream_head != NULL) {
            cproxy_start_wait_queue_timeout(ptd,
                                            ptd->waiting_any_downstream_head);
        }
    }
}

rel_time_t cproxy_realtime(const time_t exptime) {
    // Input is a long...
    //
    // 0       | (0...REALIME_MAXDELTA] | (REALTIME_MAXDELTA...
    // forever | delta                  | unix_time
    //
    // Storage is an unsigned int.
    //
    // TODO: Handle data loss.
    //
    // The cproxy version of realtime doesn't do any
    // time math munging, just pass through.
    //
    return (rel_time_t) exptime;
}

void cproxy_close_conn(conn *c) {
    assert(c != NULL);

    conn_set_state(c, conn_closing);

    update_event(c, 0);

    // Run through drive_machine just once,
    // to go through close code paths.
    //
    drive_machine(c);
}

bool add_conn_item(conn *c, item *it) {
    assert(it != NULL);
    assert(c != NULL);
    assert(c->ilist != NULL);
    assert(c->icurr != NULL);
    assert(c->isize > 0);

    if (c->ileft >= c->isize) {
        item **new_list =
            realloc(c->ilist, sizeof(item *) * c->isize * 2);
        if (new_list) {
            c->isize *= 2;
            c->ilist = new_list;
            c->icurr = new_list;
        }
    }

    if (c->ileft < c->isize) {
        c->ilist[c->ileft] = it;
        c->ileft++;

        return true;
    }

    return false;
}

char *add_conn_suffix(conn *c) {
    assert(c != NULL);
    assert(c->suffixlist != NULL);
    assert(c->suffixcurr != NULL);
    assert(c->suffixsize > 0);

    if (c->suffixleft >= c->suffixsize) {
        char **new_suffix_list =
            realloc(c->suffixlist,
                    sizeof(char *) * c->suffixsize * 2);
        if (new_suffix_list) {
            c->suffixsize *= 2;
            c->suffixlist = new_suffix_list;
            c->suffixcurr = new_suffix_list;
        }
    }

    if (c->suffixleft < c->suffixsize) {
        char *suffix = cache_alloc(c->thread->suffix_cache);
        if (suffix != NULL) {
            c->suffixlist[c->suffixleft] = suffix;
            c->suffixleft++;

            return suffix;
        }
    }

    return NULL;
}

char *nread_text(short x) {
    char *rv = NULL;
    switch(x) {
    case NREAD_SET:
        rv = "set ";
        break;
    case NREAD_ADD:
        rv = "add ";
        break;
    case NREAD_REPLACE:
        rv = "replace ";
        break;
    case NREAD_APPEND:
        rv = "append ";
        break;
    case NREAD_PREPEND:
        rv = "prepend ";
        break;
    case NREAD_CAS:
        rv = "cas ";
        break;
    }
    return rv;
}

/* Tokenize the command string by updating the token array
 * with pointers to start of each token and length.
 * Does not modify the input command string.
 *
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while (scan_tokens(command, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      command = tokens[ix].value;
 *  }
 */
size_t scan_tokens(char *command, token_t *tokens,
                   const size_t max_tokens) {
    char *s, *e;
    size_t ntokens = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    for (s = e = command; ntokens < max_tokens - 1; ++e) {
        if (*e == '\0' || *e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
            }
            if (*e == '\0')
                break; /* string end */
            s = e + 1;
        }
    }

    /* If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value = (*e == '\0' ? NULL : e);
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* Remove conn c from a conn list.
 * Returns the new head of the list.
 */
conn *conn_list_remove(conn *head, conn **tail, conn *c, bool *found) {
    conn *prev = NULL;
    conn *curr = head;

    if (found != NULL)
        *found = false;

    while (curr != NULL) {
        if (curr == c) {
            if (found != NULL)
                *found = true;

            if (tail != NULL &&
                *tail == curr)
                *tail = prev;

            if (prev != NULL) {
                assert(curr != head);
                prev->next = curr->next;
                curr->next = NULL;
                return head;
            }

            assert(curr == head);
            conn *r = curr->next;
            curr->next = NULL;
            return r;
        }

        prev = curr;
        curr = curr ->next;
    }

    return head;
}

/* Returns the new head of the list.
 */
downstream *downstream_list_remove(downstream *head, downstream *d) {
    downstream *prev = NULL;
    downstream *curr = head;

    while (curr != NULL) {
        if (curr == d) {
            if (prev != NULL) {
                assert(curr != head);
                prev->next = curr->next;
                return head;
            }

            assert(curr == head);
            return curr->next;
        }

        prev = curr;
        curr = curr ->next;
    }

    return head;
}

/* Returns true if a candidate request is squashable
 * or de-duplicatable with an existing request, to
 * save on network hops.
 *
 * TODO: Handle binary upstream protocol.
 */
bool is_compatible_request(conn *existing, conn *candidate) {
    assert(existing);
    assert(IS_ASCII(existing->protocol));
    assert(IS_PROXY(existing->protocol));
    assert(existing->state == conn_pause);

    if (candidate != NULL) {
        assert(IS_ASCII(candidate->protocol));
        assert(IS_PROXY(candidate->protocol));
        assert(candidate->state == conn_pause);

        // TODO: Allow gets (CAS) for de-duplication.
        //
        if (existing->cmd == -1 &&
            candidate->cmd == -1 &&
            existing->cmd_retries <= 0 &&
            candidate->cmd_retries <= 0 &&
            !existing->noreply &&
            !candidate->noreply &&
            strncmp(existing->cmd_start, "get ", 4) == 0 &&
            strncmp(candidate->cmd_start, "get ", 4) == 0) {
            assert(existing->item == NULL);
            assert(candidate->item == NULL);

            return true;
        }
    }

    return false;
}

void downstream_timeout(const int fd,
                        const short which,
                        void *arg) {
    downstream *d = arg;
    assert(d != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "downstream_timeout\n");

    // This timer callback is invoked when one or more of
    // the downstream conns must be really slow.  Handle by
    // closing downstream conns, which might help by
    // freeing up downstream resources.
    //
    if (d->timeout_tv.tv_sec != 0 ||
        d->timeout_tv.tv_usec != 0) {
        evtimer_del(&d->timeout_event);

        d->timeout_tv.tv_sec = 0;
        d->timeout_tv.tv_usec = 0;

        int n = memcached_server_count(&d->mst);

        for (int i = 0; i < n; i++) {
            if (d->downstream_conns[i] != NULL) {
                // Doing drive_machine(), which should only loop once,
                // to get to the connection closing logic.
                //
                cproxy_close_conn(d->downstream_conns[i]);
            }
        }
    }
}

bool cproxy_start_downstream_timeout(downstream *d) {
    assert(d != NULL);
    assert(d->timeout_tv.tv_sec == 0);
    assert(d->timeout_tv.tv_usec == 0);

    if (d->behavior.downstream_timeout.tv_sec == 0 &&
        d->behavior.downstream_timeout.tv_usec == 0)
        return true;

    conn *uc = d->upstream_conn;

    assert(uc != NULL);
    assert(uc->state == conn_pause);
    assert(uc->thread != NULL);
    assert(uc->thread->base != NULL);
    assert(IS_PROXY(uc->protocol));

    if (settings.verbose > 1)
        fprintf(stderr, "cproxy_start_downstream_timeout\n");

    evtimer_set(&d->timeout_event, downstream_timeout, d);

    event_base_set(uc->thread->base, &d->timeout_event);

    d->timeout_tv.tv_sec  = d->behavior.downstream_timeout.tv_sec;
    d->timeout_tv.tv_usec = d->behavior.downstream_timeout.tv_usec;

    return (evtimer_add(&d->timeout_event, &d->timeout_tv) == 0);
}
