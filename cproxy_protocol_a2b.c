/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sysexits.h>
#include <pthread.h>
#include <assert.h>
#include <math.h>
#include <glib.h>
#include <libmemcached/memcached.h>
#include "memcached.h"
#include "cproxy.h"
#include "work.h"

#ifndef HAVE_HTONLL
extern uint64_t ntohll(uint64_t);
extern uint64_t htonll(uint64_t);
#endif

// Internal declarations.
//
#define CMD_TOKEN  0
#define KEY_TOKEN  1
#define MAX_TOKENS 9

// A2B means ascii-to-binary (or, ascii upstream and binary downstream).
//
struct A2BSpec {
    char *line;

    protocol_binary_command cmd;
    protocol_binary_command cmdq;

    int     size;         // Number of bytes in request header.
    token_t tokens[MAX_TOKENS];
    int     ntokens;
    bool    noreply_allowed;
    int     num_optional; // Number of optional arguments in cmd.
    bool    broadcast;    // True if cmd does scatter/gather.
};

// The a2b_specs are immutable after init.
//
// The arguments are carefully named with unique first characters.
//
struct A2BSpec a2b_specs[] = {
    { .line = "set <key> <flags> <exptime> <bytes> [noreply]",
      .cmd  = PROTOCOL_BINARY_CMD_SET,
      .cmdq = PROTOCOL_BINARY_CMD_SETQ,
      .size = sizeof(protocol_binary_request_set)
    },
    { .line = "add <key> <flags> <exptime> <bytes> [noreply]",
      .cmd  = PROTOCOL_BINARY_CMD_ADD,
      .cmdq = PROTOCOL_BINARY_CMD_ADDQ,
      .size = sizeof(protocol_binary_request_add)
    },
    { .line = "replace <key> <flags> <exptime> <bytes> [noreply]",
      .cmd  = PROTOCOL_BINARY_CMD_REPLACE,
      .cmdq = PROTOCOL_BINARY_CMD_REPLACEQ,
      .size = sizeof(protocol_binary_request_replace)
    },
    { .line = "append <key> <skip_flags> <skip_exptime> <bytes> [noreply]",
      .cmd  = PROTOCOL_BINARY_CMD_APPEND,
      .cmdq = PROTOCOL_BINARY_CMD_APPENDQ,
      .size = sizeof(protocol_binary_request_append)
    },
    { .line = "prepend <key> <skip_flags> <skip_exptime> <bytes> [noreply]" ,
      .cmd  = PROTOCOL_BINARY_CMD_PREPEND,
      .cmdq = PROTOCOL_BINARY_CMD_PREPENDQ,
      .size = sizeof(protocol_binary_request_prepend)
    },
    { .line = "cas <key> <flags> <exptime> <bytes> <cas> [noreply]",
      .cmd  = PROTOCOL_BINARY_CMD_SET,
      .cmdq = PROTOCOL_BINARY_CMD_SETQ,
      .size = sizeof(protocol_binary_request_set)
    },
    { .line = "delete <key> [noreply]",
      .cmd  = PROTOCOL_BINARY_CMD_DELETE,
      .cmdq = PROTOCOL_BINARY_CMD_DELETEQ,
      .size = sizeof(protocol_binary_request_delete)
    },
    { .line = "incr <key> <value> [noreply]",
      .cmd  = PROTOCOL_BINARY_CMD_INCREMENT,
      .cmdq = PROTOCOL_BINARY_CMD_INCREMENTQ,
      .size = sizeof(protocol_binary_request_incr)
    },
    { .line = "decr <key> <value> [noreply]",
      .cmd  = PROTOCOL_BINARY_CMD_DECREMENT,
      .cmdq = PROTOCOL_BINARY_CMD_DECREMENTQ,
      .size = sizeof(protocol_binary_request_decr)
    },
    { .line = "flush_all [xpiration] [noreply]", // TODO: noreply tricky here.
      .cmd  = PROTOCOL_BINARY_CMD_FLUSH,
      .cmdq = PROTOCOL_BINARY_CMD_FLUSHQ,
      .size = sizeof(protocol_binary_request_flush),
      .broadcast = true
    },
    { .line = "get <key>*",
      .cmd  = PROTOCOL_BINARY_CMD_GETK,
      .cmdq = PROTOCOL_BINARY_CMD_GETKQ,
      .size = sizeof(protocol_binary_request_getk)
    },
    { .line = "gets <key>*",
      .cmd  = PROTOCOL_BINARY_CMD_GETK,
      .cmdq = PROTOCOL_BINARY_CMD_GETKQ,
      .size = sizeof(protocol_binary_request_getk)
    },
    { .line = "stats [args]*",
      .cmd  = PROTOCOL_BINARY_CMD_STAT,
      .cmdq = PROTOCOL_BINARY_CMD_NOOP,
      .size = sizeof(protocol_binary_request_stats),
      .broadcast = true
    },
    { 0 } // NULL sentinel.
};

// These are immutable after init.
//
GHashTable *a2b_spec_map = NULL; // Key: command string, value: A2BSpec.
int         a2b_size_max = 0;    // Max header + extra frame bytes size.

int a2b_fill_request(token_t *cmd_tokens,
                     int      cmd_ntokens,
                     bool     noreply,
                     protocol_binary_request_header *header,
                     uint8_t **out_key,
                     uint16_t *out_keylen,
                     uint8_t  *out_extlen);

bool a2b_fill_request_token(struct A2BSpec *spec,
                            int      cur_token,
                            token_t *cmd_tokens,
                            int      cmd_ntokens,
                            protocol_binary_request_header *header,
                            uint8_t **out_key,
                            uint16_t *out_keylen,
                            uint8_t  *out_extlen);

void a2b_process_downstream_response(conn *c);

void cproxy_init_a2b() {
    if (a2b_spec_map == NULL) {
        a2b_spec_map = g_hash_table_new(skey_hash, skey_equal);
        if (a2b_spec_map == NULL)
            return; // TODO: Better oom error handling.

        // Run through the a2b_specs to populate the a2b_spec_map.
        //
        int i = 0;
        while (true) {
            struct A2BSpec *spec = &a2b_specs[i];
            if (spec->line == NULL)
                break;

            spec->ntokens = scan_tokens(spec->line,
                                        spec->tokens,
                                        MAX_TOKENS);
            assert(spec->ntokens > 2);

            int noreply_index = spec->ntokens - 2;
            if (spec->tokens[noreply_index].value &&
                strcmp(spec->tokens[noreply_index].value,
                       "[noreply]") == 0)
                spec->noreply_allowed = true;
            else
                spec->noreply_allowed = false;

            spec->num_optional = 0;
            for (int j = 0; j < spec->ntokens; j++) {
                if (spec->tokens[j].value &&
                    spec->tokens[j].value[0] == '[')
                    spec->num_optional++;
            }

            if (a2b_size_max < spec->size)
                a2b_size_max = spec->size;

            g_hash_table_insert(a2b_spec_map,
                                spec->tokens[CMD_TOKEN].value,
                                spec);

            i = i + 1;
        }
    }
}

int a2b_fill_request(token_t *cmd_tokens,
                     int      cmd_ntokens,
                     bool     noreply,
                     protocol_binary_request_header *header,
                     uint8_t **out_key,
                     uint16_t *out_keylen,
                     uint8_t  *out_extlen) {
    assert(header);
    assert(cmd_tokens);
    assert(cmd_ntokens > 1);
    assert(cmd_tokens[CMD_TOKEN].value);
    assert(cmd_tokens[CMD_TOKEN].length > 0);
    assert(out_key);
    assert(out_keylen);
    assert(out_extlen);
    assert(a2b_spec_map);

    struct A2BSpec *spec = g_hash_table_lookup(a2b_spec_map,
                                               cmd_tokens[CMD_TOKEN].value);
    if (spec != NULL) {
        if (cmd_ntokens >= (spec->ntokens - spec->num_optional) &&
            cmd_ntokens <= (spec->ntokens)) {
            header->request.magic  = PROTOCOL_BINARY_REQ;

            if (noreply)
                header->request.opcode = spec->cmdq;
            else
                header->request.opcode = spec->cmd;

            // Start at 1 to skip the CMD_TOKEN.
            //
            for (int i = 1; i < cmd_ntokens - 1; i++) {
                if (a2b_fill_request_token(spec, i,
                                           cmd_tokens, cmd_ntokens,
                                           header,
                                           out_key,
                                           out_keylen,
                                           out_extlen) == false) {
                    return 0;
                }
            }

            return spec->size; // Success.
        }
    }

    return 0;
}

bool a2b_fill_request_token(struct A2BSpec *spec,
                            int      cur_token,
                            token_t *cmd_tokens,
                            int      cmd_ntokens,
                            protocol_binary_request_header *header,
                            uint8_t **out_key,
                            uint16_t *out_keylen,
                            uint8_t  *out_extlen) {
    assert(header);
    assert(spec);
    assert(spec->tokens);
    assert(spec->ntokens > 1);
    assert(spec->tokens[cur_token].value);
    assert(cur_token > 0);
    assert(cur_token < cmd_ntokens);
    assert(cur_token < spec->ntokens);

    uint64_t delta;

    if (settings.verbose > 1)
        fprintf(stderr, "a2b_fill_request_token %s\n",
                spec->tokens[cur_token].value);

    char t = spec->tokens[cur_token].value[1];
    switch (t) {
    case 'k': // key
        assert(out_key);
        assert(out_keylen);
        *out_key    = (uint8_t *) cmd_tokens[cur_token].value;
        *out_keylen = (uint16_t)  cmd_tokens[cur_token].length;
        header->request.keylen =
            htons((uint16_t) cmd_tokens[cur_token].length);
        break;
    case 'v': // value (for incr/decr)
        delta = 0;
        if (safe_strtoull(cmd_tokens[cur_token].value, &delta)) {
            assert(out_extlen);

            header->request.extlen   = *out_extlen = 20;
            header->request.datatype = PROTOCOL_BINARY_RAW_BYTES;

            protocol_binary_request_incr *req =
                (protocol_binary_request_incr *) header;

            req->message.body.delta = htonll(delta);
            req->message.body.initial = 0;
            req->message.body.expiration = 0xffffffff;
        } else {
            // TODO: Send back better error.
            return false;
        }
        break;

    case 'x': // xpiration (for flush_all)
        // TODO.
        return false;
        break;
    case 'a': // args (for stats)
        // TODO.
        return false;
        break;

    // The noreply was handled in a2b_fill_request().
    //
    // case 'n': // noreply
    //
    // The above are handled by looking at the item struct.
    //
    // case 'f': // FALLTHRU, flags
    // case 'e': // FALLTHRU, exptime
    // case 'b': // FALLTHRU, bytes
    // case 's': // FALLTHRU, skip_xxx
    // case 'c': // FALLTHRU, cas
    //
    default:
        break;
    }

    return true;
}

/* Called when we receive a binary response header from
 * a downstream server, via try_read_command()/drive_machine().
 */
void cproxy_process_a2b_downstream(conn *c) {
    assert(c != NULL);
    assert(c->cmd >= 0);
    assert(c->next == NULL);
    assert(c->item == NULL);
    assert(IS_BINARY(c->protocol));
    assert(IS_PROXY(c->protocol));

    if (settings.verbose > 1)
        fprintf(stderr, "<%d cproxy_process_a2b_downstream\n",
                c->sfd);

    // Snapshot rcurr, because the caller, try_read_command(), changes it.
    //
    c->cmd_start = c->rcurr;

    protocol_binary_response_header *header =
        (protocol_binary_response_header *) &c->binary_header;

    header->response.status = (uint16_t) ntohs(header->response.status);

    assert(header->response.magic == (uint8_t) PROTOCOL_BINARY_RES);
    assert(header->response.opcode == c->cmd);

    process_bin_noreply(c); // Map quiet c->cmd values into non-quiet.

    int      extlen  = header->response.extlen;
    int      keylen  = header->response.keylen;
    uint32_t bodylen = header->response.bodylen;

    // Our approach is to read everything we can before
    // getting into big switch/case statements for the
    // actual processing.
    //
    // If status is non-zero (an err code), then bodylen should be small.
    // If status is 0, then bodylen might be for a huge item during
    // a GET family of response.
    //
    // If bodylen > extlen + keylen, then we should nread
    // then ext+key and set ourselves up for a later item nread.
    //
    // We overload the meaning of the conn substates...
    // - bin_reading_get_key means do nread for ext and key data.
    // - bin_read_set_value means do nread for item data.
    //
    if (settings.verbose > 1)
        fprintf(stderr, "<%d cproxy_process_a2b_downstream %x\n",
                c->sfd, c->cmd);

    if (keylen > 0 || extlen > 0) {
        assert(bodylen >= keylen + extlen);

        // One reason we reach here is during a
        // GET/GETQ/GETK/GETKQ hit response, because extlen
        // will be > 0 for the flags.
        //
        // Also, we reach here during a GETK miss response, since
        // keylen will be > 0.  Oddly, a GETK miss response will have
        // a non-zero status of PROTOCOL_BINARY_RESPONSE_KEY_ENOENT,
        // but won't have any extra error message string.
        //
        // Also, we reach here during a STAT response, with
        // keylen > 0, extlen == 0, and bodylen == keylen.
        //
        assert(c->cmd == PROTOCOL_BINARY_CMD_GET ||
               c->cmd == PROTOCOL_BINARY_CMD_GETK ||
               c->cmd == PROTOCOL_BINARY_CMD_STAT);

        bin_read_key(c, bin_reading_get_key, extlen);
    } else {
        assert(keylen == 0 && extlen == 0);

        if (bodylen > 0) {
            // We reach here on error response, version response,
            // or incr/decr responses, which all have only (relatively
            // small) body bytes, and with no ext bytes and no key bytes.
            //
            // For example, error responses will have 0 keylen,
            // 0 extlen, with an error message string for the body.
            //
            // We'll just reuse the key-reading code path, rather
            // than allocating an item.
            //
            assert(header->response.status != 0 ||
                   c->cmd == PROTOCOL_BINARY_CMD_VERSION ||
                   c->cmd == PROTOCOL_BINARY_CMD_INCREMENT ||
                   c->cmd == PROTOCOL_BINARY_CMD_DECREMENT);

            bin_read_key(c, bin_reading_get_key, bodylen);
        } else {
            assert(keylen == 0 && extlen == 0 && bodylen == 0);

            // We have the entire response in the header,
            // such as due to a general success response,
            // including a no-op response.
            //
            a2b_process_downstream_response(c);
        }
    }
}

/* We reach here after nread'ing a ext+key or item.
 */
void cproxy_process_a2b_downstream_nread(conn *c) {
    assert(c != NULL);
    assert(c->cmd >= 0);
    assert(c->next == NULL);
    assert(c->cmd_start != NULL);
    assert(IS_BINARY(c->protocol));
    assert(IS_PROXY(c->protocol));

    if (settings.verbose > 1)
        fprintf(stderr,
                "<%d cproxy_process_a2b_downstream_nread %d %d\n",
                c->sfd, c->ileft, c->isize);

    protocol_binary_response_header *header =
        (protocol_binary_response_header *) &c->binary_header;

    int      extlen  = header->response.extlen;
    int      keylen  = header->response.keylen;
    uint32_t bodylen = header->response.bodylen;

    if (c->substate == bin_reading_get_key &&
        header->response.status == 0 &&
        (c->cmd == PROTOCOL_BINARY_CMD_GET ||
         c->cmd == PROTOCOL_BINARY_CMD_GETK ||
         c->cmd == PROTOCOL_BINARY_CMD_STAT)) {
        assert(c->item == NULL);

        // Alloc an item and continue with an item nread.
        // We item_alloc() even if vlen is 0, so that later
        // code can assume an item exists.
        //
        char  *key   = binary_get_key(c);
        int    vlen  = bodylen - (keylen + extlen);
        int    flags = 0;

        assert(key);
        assert(keylen > 0);
        assert(vlen >= 0);

        if (c->cmd == PROTOCOL_BINARY_CMD_GET ||
            c->cmd == PROTOCOL_BINARY_CMD_GETK) {
            protocol_binary_response_get *response_get =
                (protocol_binary_response_get *) header;

            assert(extlen == sizeof(response_get->message.body));

            flags = ntohl(response_get->message.body.flags);
        }

        item *it = item_alloc(key, keylen, flags, 0, vlen + 2);
        if (it != NULL) {
            c->item = it;
            c->ritem = ITEM_data(it);
            c->rlbytes = vlen;
            c->substate = bin_read_set_value;

            uint64_t cas = CPROXY_NOT_CAS;

            // TODO: Handle cas.
            //
            ITEM_set_cas(it, cas);

            conn_set_state(c, conn_nread);
        } else {
            // TODO: Error, probably swallow bytes.
        }
    } else {
        a2b_process_downstream_response(c);
    }
}

/* Invoked when we have read a complete downstream binary response,
 * including header, ext, key, and item data, as appropriate.
 */
void a2b_process_downstream_response(conn *c) {
    assert(c != NULL);
    assert(c->cmd >= 0);
    assert(c->next == NULL);
    assert(c->cmd_start != NULL);
    assert(IS_BINARY(c->protocol));
    assert(IS_PROXY(c->protocol));

    if (settings.verbose > 1)
        fprintf(stderr,
                "<%d cproxy_process_a2b_downstream_response\n",
                c->sfd);

    protocol_binary_response_header *header =
        (protocol_binary_response_header *) &c->binary_header;

    int      extlen  = header->response.extlen;
    int      keylen  = header->response.keylen;
    uint32_t bodylen = header->response.bodylen;
    uint16_t status  = header->response.status;

    // We reach here when we have the entire response,
    // including header, ext, key, and possibly item data.
    // Now we can get into big switch/case processing.
    //
    downstream *d = c->extra;
    assert(d != NULL);

    item *it = c->item;

    // Clear c->item because we either move it to the upstream or
    // item_remove() it on error.
    //
    c->item = NULL;

    conn *uc = d->upstream_conn;

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_GET:   /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_GETK:
        if (c->noreply) {
            // We should keep processing for a non-quiet
            // terminating response.
            //
            conn_set_state(c, conn_new_cmd);
        } else
            conn_set_state(c, conn_pause);

        if (status != 0) {
            assert(it == NULL);

            if (status == PROTOCOL_BINARY_RESPONSE_KEY_ENOENT)
                return; // Swallow miss response.

            // TODO: Handle error case.  Should we pause the conn
            //       or keep looking for more responses?
            //
            assert(false);
            return;
        }

        assert(status == 0);
        assert(it != NULL);
        assert(it->nbytes >= 2);
        assert(keylen > 0);
        assert(extlen > 0);

        if (bodylen >= keylen + extlen) {
            *(ITEM_data(it) + it->nbytes - 2) = '\r';
            *(ITEM_data(it) + it->nbytes - 1) = '\n';

            if (d->multiget != NULL) {
                char key_buf[KEY_MAX_LENGTH + 10];

                memcpy(key_buf, ITEM_key(it), it->nkey);
                key_buf[it->nkey] = '\0';

                multiget_entry *entry =
                    g_hash_table_lookup(d->multiget, key_buf);

                while (entry != NULL) {
                    // The upstream might have been closed mid-request.
                    //
                    uc = entry->upstream_conn;
                    if (uc != NULL)
                        cproxy_upstream_ascii_item_response(it, uc);

                    entry = entry->next;
                }
            } else {
                while (uc != NULL) {
                    cproxy_upstream_ascii_item_response(it, uc);
                    uc = uc->next;
                }
            }
        } else {
            assert(false); // TODO.
        }

        item_remove(it);
        break;

    case PROTOCOL_BINARY_CMD_FLUSH: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_NOOP:
        conn_set_state(c, conn_pause);
        break;

    case PROTOCOL_BINARY_CMD_SET: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_ADD: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_REPLACE:
    case PROTOCOL_BINARY_CMD_DELETE:
    case PROTOCOL_BINARY_CMD_APPEND:
    case PROTOCOL_BINARY_CMD_PREPEND:
        assert(c->noreply == false);

        if (uc != NULL) {
            assert(uc->next == NULL);

            switch (status) {
            case 0:
                out_string(uc, "STORED");
                break;
            case PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS:
                out_string(uc, "EXISTS");
                break;
            case PROTOCOL_BINARY_RESPONSE_KEY_ENOENT:
                out_string(uc, "NOT_FOUND");
                break;
            case PROTOCOL_BINARY_RESPONSE_NOT_STORED:
                out_string(uc, "NOT_STORED");
                break;
            case PROTOCOL_BINARY_RESPONSE_ENOMEM: // TODO.
            default:
                out_string(uc, "SERVER_ERROR a2b error");
                break;
            }

            if (update_event(uc, EV_WRITE | EV_PERSIST)) {
                conn_set_state(c, conn_pause);
            } else {
                if (settings.verbose > 1)
                    fprintf(stderr,
                            "Can't write upstream a2b event\n");

                d->ptd->stats.tot_oom++;
                cproxy_close_conn(uc);
            }
        }
        break;

    case PROTOCOL_BINARY_CMD_INCREMENT: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_DECREMENT:
        if (uc != NULL) {
            assert(uc->next == NULL);

            // TODO: Any weird alignment/padding issues on different
            //       platforms in this cast to worry about here?
            //
            protocol_binary_response_incr *response_incr =
                (protocol_binary_response_incr *) c->cmd_start;

            switch (status) {
            case 0: {
                char *s = add_conn_suffix(uc);
                if (s != NULL) {
                    uint64_t v = swap64(response_incr->message.body.value);
                    sprintf(s, "%llu", v);
                    out_string(uc, s);
                } else {
                    d->ptd->stats.tot_oom++;
                    cproxy_close_conn(uc);
                }
                break;
            }
            case PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS: // Due to CAS.
                out_string(uc, "EXISTS");
                break;
            case PROTOCOL_BINARY_RESPONSE_KEY_ENOENT:
                out_string(uc, "NOT_FOUND");
                break;
            case PROTOCOL_BINARY_RESPONSE_NOT_STORED:
                out_string(uc, "NOT_STORED");
                break;
            case PROTOCOL_BINARY_RESPONSE_ENOMEM: // TODO.
            default:
                out_string(uc, "SERVER_ERROR a2b arith error");
                break;
            }

            if (update_event(uc, EV_WRITE | EV_PERSIST)) {
                conn_set_state(c, conn_pause);
            } else {
                if (settings.verbose > 1)
                    fprintf(stderr,
                            "Can't write upstream a2b arith event\n");

                d->ptd->stats.tot_oom++;
                cproxy_close_conn(uc);
            }
        }
        break;

    case PROTOCOL_BINARY_CMD_STAT:
        if (keylen > 0) {
            assert(it != NULL);
            assert(bodylen > keylen);
            // TODO.
            item_remove(it);
            conn_set_state(c, conn_new_cmd);
        } else {
            // This is stats terminator.
            //
            assert(it == NULL);
            assert(bodylen == 0);
            conn_set_state(c, conn_pause);
        }
        break;

    case PROTOCOL_BINARY_CMD_VERSION:
    case PROTOCOL_BINARY_CMD_QUIT:
    default:
        assert(false); // TODO: Handled unexpected responses.
        break;
    }
}

/* Do the actual work of forwarding the command from an
 * upstream ascii conn to its assigned binary downstream.
 */
bool cproxy_forward_a2b_downstream(downstream *d) {
    assert(d != NULL);

    conn *uc = d->upstream_conn;

    assert(uc != NULL);
    assert(uc->state == conn_pause);
    assert(uc->cmd_start != NULL);
    assert(uc->thread != NULL);
    assert(uc->thread->base != NULL);
    assert(IS_ASCII(uc->protocol));
    assert(IS_PROXY(uc->protocol));

    if (cproxy_connect_downstream(d, uc->thread) > 0) {
        assert(d->downstream_conns != NULL);

        if (uc->cmd == -1) {
            return cproxy_forward_a2b_simple_downstream(d, uc->cmd_start, uc);
        } else {
            return cproxy_forward_a2b_item_downstream(d, uc->cmd, uc->item, uc);
        }
    }

    return false;
}

/* Forward a simple one-liner ascii command to a binary downstream.
 * For example, get, incr/decr, delete, etc.
 * The response, though, might be a simple line or
 * multiple VALUE+END lines.
 */
bool cproxy_forward_a2b_simple_downstream(downstream *d,
                                          char *command, conn *uc) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->downstream_conns != NULL);
    assert(command != NULL);
    assert(uc != NULL);
    assert(uc->item == NULL);
    assert(d->multiget == NULL);
    assert(d->merger == NULL);

    if (strncmp(command, "get", 3) == 0)
        return cproxy_forward_a2b_multiget_downstream(d, uc);

    assert(uc->next == NULL);

    if (strncmp(command, "flush_all", 9) == 0)
        return cproxy_broadcast_a2b_downstream(d, command, uc,
                                               "OK\r\n");

    if (strncmp(command, "stats", 5) == 0) {
        if (strncmp(command + 5, " reset", 6) == 0)
            return cproxy_broadcast_a2b_downstream(d, command, uc,
                                                   "RESET\r\n");

        if (cproxy_broadcast_a2b_downstream(d, command, uc,
                                            "END\r\n")) {
            d->merger = g_hash_table_new(protocol_stats_key_hash,
                                         protocol_stats_key_equal);
            return true;
        } else {
            return false;
        }
    }

    token_t  tokens[MAX_TOKENS];
    size_t   ntokens = scan_tokens(command, tokens, MAX_TOKENS);
    char    *key     = tokens[KEY_TOKEN].value;
    int      key_len = tokens[KEY_TOKEN].length;

    if (ntokens <= 1) { // This was checked long ago, while parsing
        assert(false);  // the upstream conn.
        return false;
    }

    // Assuming we're already connected to downstream.
    //
    conn *c = cproxy_find_downstream_conn(d, key, key_len);
    if (c != NULL &&
        cproxy_prep_conn_for_write(c)) {
        assert(c->state == conn_pause);
        assert(c->wbuf);
        assert(c->wsize >= a2b_size_max);

        protocol_binary_request_header *header =
            (protocol_binary_request_header *) c->wbuf;

        uint8_t *out_key    = NULL;
        uint16_t out_keylen = 0;
        uint8_t  out_extlen = 0;

        memset(header, 0, a2b_size_max);

        int size = a2b_fill_request(tokens, ntokens,
                                    uc->noreply,
                                    header,
                                    &out_key,
                                    &out_keylen,
                                    &out_extlen);
        if (size > 0) {
            assert(size <= a2b_size_max);
            assert(key     == (char *) out_key);
            assert(key_len == (int)    out_keylen);
            assert(header->request.bodylen == 0);

            header->request.bodylen =
                htonl(out_keylen + out_extlen);

            add_iov(c, header, size);

            if (out_key != NULL &&
                out_keylen > 0)
                add_iov(c, out_key, out_keylen);

            if (settings.verbose > 1)
                fprintf(stderr, "forwarding a2b to %d, noreply %d\n",
                        c->sfd, uc->noreply);

            conn_set_state(c, conn_mwrite);
            c->write_and_go = conn_new_cmd;

            if (update_event(c, EV_WRITE | EV_PERSIST)) {
                d->downstream_used_start = 1; // TODO: Need timeout?
                d->downstream_used       = 1;

                if (cproxy_dettach_if_noreply(d, uc) == false) {
                    cproxy_start_downstream_timeout(d);
                } else {
                    c->write_and_go = conn_pause;
                }

                return true;
            } else {
                // TODO: Error handling.
                //
                if (settings.verbose > 1)
                    fprintf(stderr, "Couldn't a2b update write event\n");

                if (d->upstream_suffix == NULL)
                    d->upstream_suffix = "SERVER_ERROR a2b event oom\r\n";
            }
        } else {
            // TODO: Error handling.
            //
            if (settings.verbose > 1)
                fprintf(stderr, "Couldn't a2b fill request: %s\n",
                        command);

            if (d->upstream_suffix == NULL)
                d->upstream_suffix = "CLIENT_ERROR a2b parse request\r\n";
        }

        d->ptd->stats.tot_oom++;
        cproxy_close_conn(c);
    }

    return false;
}

bool cproxy_forward_a2b_multiget_downstream(downstream *d, conn *uc) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->downstream_conns != NULL);
    assert(d->multiget == NULL);
    assert(uc != NULL);
    assert(uc->noreply == false);

    int nwrite = 0;
    int nconns = memcached_server_count(&d->mst);

    for (int i = 0; i < nconns; i++) {
        if (d->downstream_conns[i] != NULL) {
            cproxy_prep_conn_for_write(d->downstream_conns[i]);
            assert(d->downstream_conns[i]->state == conn_pause);
        }
    }

    if (uc->next != NULL) {
        // More than one upstream conn, so we need a hashtable
        // to track keys for de-deplication.
        //
        d->multiget = g_hash_table_new(skey_hash,
                                       skey_equal);
        if (settings.verbose > 1)
            fprintf(stderr, "cproxy multiget hash table new\n");
    }

    int   uc_num = 0;
    conn *uc_cur = uc;

    while (uc_cur != NULL) {
        assert(uc_cur->cmd == -1);
        assert(uc_cur->item == NULL);
        assert(uc_cur->state == conn_pause);
        assert(IS_ASCII(uc_cur->protocol));
        assert(IS_PROXY(uc_cur->protocol));

        char *command = uc_cur->cmd_start;
        assert(command != NULL);

        char *space = strchr(command, ' ');
        assert(space > command);

        int cmd_len = space - command;
        assert(cmd_len == 3 || cmd_len == 4); // Either get or gets.

        if (settings.verbose > 1)
            fprintf(stderr, "forward multiget %s (%d %d)\n",
                    command, cmd_len, uc_num);

        while (space != NULL) {
            char *key = space + 1;
            char *next_space = strchr(key, ' ');
            int   key_len;

            if (next_space != NULL)
                key_len = next_space - key;
            else
                key_len = strlen(key);

            // This key_len check helps skips consecutive spaces.
            //
            if (key_len > 0) {
                // See if we've already requested this key via
                // the multiget hash table, in order to
                // de-deplicate repeated keys.
                //
                bool first_request = true;

                if (d->multiget != NULL) {
                    multiget_entry *entry = calloc(1, sizeof(multiget_entry));
                    if (entry != NULL) {
                        entry->upstream_conn = uc_cur;
                        entry->next = g_hash_table_lookup(d->multiget, key);

                        g_hash_table_insert(d->multiget, key, entry);

                        if (entry->next != NULL)
                            first_request = false;
                    } else {
                        // TODO: Handle out of multiget entry memory.
                    }
                }

                if (first_request) {
                    conn *c = cproxy_find_downstream_conn(d, key, key_len);
                    if (c != NULL) {
                        assert(c->item == NULL);
                        assert(c->state == conn_pause);
                        assert(IS_BINARY(c->protocol));
                        assert(IS_PROXY(c->protocol));
                        assert(c->ilist != NULL);
                        assert(c->isize > 0);

                        c->icurr = c->ilist;
                        c->ileft = 0;

                        if (uc_num <= 0 &&
                            c->msgused <= 1 &&
                            c->msgbytes <= 0) {
                            add_iov(c, command, cmd_len);

                            // TODO: Handle out of iov memory.
                        }

                        // Write the key, including the preceding space.
                        //
                        add_iov(c, key - 1, key_len + 1);
                    } else {
                        // TODO: Handle when downstream conn is down.
                    }
                } else {
                    if (settings.verbose > 1) {
                        char buf[KEY_MAX_LENGTH + 10];
                        memcpy(buf, key, key_len);
                        buf[key_len] = '\0';

                        fprintf(stderr, "%d cproxy multiget squash: %s\n",
                                uc_cur->sfd, buf);
                    }
                }
            }

            space = next_space;
        }

        uc_num++;
        uc_cur = uc_cur->next;
    }

    for (int i = 0; i < nconns; i++) {
        conn *c = d->downstream_conns[i];
        if (c != NULL &&
            (c->msgused > 1 ||
             c->msgbytes > 0)) {
            add_iov(c, "\r\n", 2);

            conn_set_state(c, conn_mwrite);
            c->write_and_go = conn_new_cmd;

            if (update_event(c, EV_WRITE | EV_PERSIST)) {
                nwrite++;

                if (uc->noreply) {
                    c->write_and_go = conn_pause;
                }
            } else {
                if (settings.verbose > 1)
                    fprintf(stderr,
                            "Couldn't update cproxy write event\n");

                d->ptd->stats.tot_oom++;
                cproxy_close_conn(c);
            }
        }
    }

    if (settings.verbose > 1)
        fprintf(stderr, "forward multiget nwrite %d out of %d\n",
                nwrite, nconns);

    d->downstream_used_start = nwrite; // TODO: Need timeout?
    d->downstream_used       = nwrite;

    if (cproxy_dettach_if_noreply(d, uc) == false) {
        d->upstream_suffix = "END\r\n";

        cproxy_start_downstream_timeout(d);
    }

    return nwrite > 0;
}

/* Used for broadcast commands, like flush_all or stats.
 */
bool cproxy_broadcast_a2b_downstream(downstream *d,
                                     char *command,
                                     conn *uc,
                                     char *suffix) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->downstream_conns != NULL);
    assert(command != NULL);
    assert(uc != NULL);
    assert(uc->next == NULL);
    assert(uc->item == NULL);

    int nwrite = 0;
    int nconns = memcached_server_count(&d->mst);

    for (int i = 0; i < nconns; i++) {
        conn *c = d->downstream_conns[i];
        if (c != NULL &&
            cproxy_prep_conn_for_write(c)) {
            assert(c->state == conn_pause);

            out_string(c, command);

            if (update_event(c, EV_WRITE | EV_PERSIST)) {
                nwrite++;

                if (uc->noreply) {
                    c->write_and_go = conn_pause;
                }
            } else {
                if (settings.verbose > 1)
                    fprintf(stderr,
                            "Update cproxy write event failed\n");

                d->ptd->stats.tot_oom++;
                cproxy_close_conn(c);
            }
        }
    }

    if (settings.verbose > 1)
        fprintf(stderr, "forward multiget nwrite %d out of %d\n",
                nwrite, nconns);

    d->downstream_used_start = nwrite; // TODO: Need timeout?
    d->downstream_used       = nwrite;

    if (cproxy_dettach_if_noreply(d, uc) == false) {
        d->upstream_suffix = suffix;

        cproxy_start_downstream_timeout(d);
    }

    return nwrite > 0;
}

/* Forward an upstream command that came with item data,
 * like set/add/replace/etc.
 */
bool cproxy_forward_a2b_item_downstream(downstream *d, short cmd,
                                        item *it, conn *uc) {
    assert(d != NULL);
    assert(d->ptd != NULL);
    assert(d->downstream_conns != NULL);
    assert(it != NULL);
    assert(uc != NULL);
    assert(uc->next == NULL);

    // Assuming we're already connected to downstream.
    //
    conn *c = cproxy_find_downstream_conn(d, ITEM_key(it), it->nkey);
    if (c != NULL &&
        cproxy_prep_conn_for_write(c)) {
        assert(c->state == conn_pause);

        char *verb = nread_text(cmd);

        assert(verb != NULL);

        char *str_flags   = ITEM_suffix(it);
        char *str_length  = strchr(str_flags + 1, ' ');
        int   len_flags   = str_length - str_flags;
        int   len_length  = it->nsuffix - len_flags - 2;
        char *str_exptime = add_conn_suffix(c);
        char *str_cas     = (cmd == NREAD_CAS ? add_conn_suffix(c) : NULL);

        if (str_flags != NULL &&
            str_length != NULL &&
            len_flags > 1 &&
            len_length > 1 &&
            str_exptime != NULL &&
            (cmd != NREAD_CAS ||
             str_cas != NULL)) {
            sprintf(str_exptime, " %u", it->exptime);

            if (str_cas != NULL)
                sprintf(str_cas, " %llu",
                        (unsigned long long) ITEM_get_cas(it));

            if (add_iov(c, verb, strlen(verb)) == 0 &&
                add_iov(c, ITEM_key(it), it->nkey) == 0 &&
                add_iov(c, str_flags, len_flags) == 0 &&
                add_iov(c, str_exptime, strlen(str_exptime)) == 0 &&
                add_iov(c, str_length, len_length) == 0 &&
                (str_cas == NULL ||
                 add_iov(c, str_cas, strlen(str_cas)) == 0) &&
                (uc->noreply == false ||
                 add_iov(c, " noreply", 8) == 0) &&
                add_iov(c, ITEM_data(it) - 2, it->nbytes + 2) == 0) {
                conn_set_state(c, conn_mwrite);
                c->write_and_go = conn_new_cmd;

                if (update_event(c, EV_WRITE | EV_PERSIST)) {
                    d->downstream_used_start = 1; // TODO: Need timeout?
                    d->downstream_used       = 1;

                    if (cproxy_dettach_if_noreply(d, uc) == false) {
                        cproxy_start_downstream_timeout(d);
                    } else {
                        c->write_and_go = conn_pause;
                    }

                    return true;
                }

                d->ptd->stats.tot_oom++;
                cproxy_close_conn(c);
            }
        }

        if (settings.verbose > 1)
            fprintf(stderr, "Proxy item write out of memory");

        // TODO: Need better out-of-memory behavior.
    }

    return false;
}
