/*
 *
 * Copyright (c) 2018 The University of Waikato, Hamilton, New Zealand.
 * All rights reserved.
 *
 * This file is part of OpenLI.
 *
 * This code has been developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * OpenLI is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenLI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <libtrace_parallel.h>
#include <libwandder.h>
#include <libwandder_etsili.h>

#include "logger.h"
#include "collector.h"
#include "configparser.h"
#include "collector_sync.h"
#include "collector_export.h"
#include "ipcc.h"
#include "ipmmcc.h"
#include "sipparsing.h"

volatile int collector_halt = 0;
volatile int reload_config = 0;

static void cleanup_signal(int signal UNUSED)
{
    collector_halt = 1;
}

static void reload_signal(int signal) {
    reload_config = 1;
}

static void usage(char *prog) {

    fprintf(stderr, "Usage: %s -c configfile\n", prog);
}

#if 0
static void dump_ip_intercept(ipintercept_t *ipint) {
    char ipbuf[256];

    printf("Intercept %u  %s\n", ipint->internalid,
            ipint->active ? "ACTIVE": "INACTIVE");
    printf("LI ID: %s\n", ipint->liid);
    printf("Auth CC: %s     Delivery CC: %s\n", ipint->authcc,
            ipint->delivcc);
    if (ipint->username) {
        printf("Username: %s\n", ipint->username);
    } else {
        printf("Username: Unknown\n");
    }

    if (ipint->ipaddr && ipint->ai_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)ipint->ipaddr;
        inet_ntop(AF_INET, (void *)&(sin->sin_addr), ipbuf, 256);
        printf("User IPv4 address: %s\n", ipbuf);
    } else {
        printf("User IP address: Unknown\n");
    }

    printf("Communication ID: %u\n", ipint->cin);
    printf("------\n");
}
#endif

static void dump_rtp_intercept(rtpstreaminf_t *rtp) {
    char ipbuf[256];

    printf("LI ID: %s\n", rtp->common.liid);
    printf("Auth CC: %s     Delivery CC: %s\n", rtp->common.authcc,
            rtp->common.delivcc);

    if (rtp->targetaddr && rtp->ai_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)rtp->targetaddr;
        inet_ntop(AF_INET, (void *)&(sin->sin_addr), ipbuf, 256);
        printf("Target RTP endpoint: %s:%u\n", ipbuf, rtp->targetport);
    }

    if (rtp->otheraddr && rtp->ai_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)rtp->otheraddr;
        inet_ntop(AF_INET, (void *)&(sin->sin_addr), ipbuf, 256);
        printf("Remote RTP endpoint: %s:%u\n", ipbuf, rtp->otherport);
    }

    printf("Communication ID: %u\n", rtp->cin);
    printf("------\n");
}

static void *start_processing_thread(libtrace_t *trace, libtrace_thread_t *t,
        void *global) {

    collector_global_t *glob = (collector_global_t *)global;
    colthread_local_t *loc = NULL;

    loc = (colthread_local_t *)malloc(sizeof(colthread_local_t));

    libtrace_message_queue_init(&(loc->tosyncq),
            sizeof(openli_state_update_t));
    libtrace_message_queue_init(&(loc->fromsyncq),
            sizeof(openli_pushed_t));
    libtrace_message_queue_init(&(loc->exportq),
            sizeof(openli_export_recv_t));

    loc->activeipv4intercepts = NULL;
    loc->activeipv6intercepts = NULL;
    loc->activertpintercepts = NULL;
    loc->sip_targets = NULL;
    loc->radiusservers = NULL;

    register_sync_queues(glob, &(loc->tosyncq), &(loc->fromsyncq), t);
    register_export_queue(glob, &(loc->exportq));

    loc->encoder = NULL;
    loc->sipparser = NULL;
    loc->knownsipservers = libtrace_list_init(sizeof(sipserverdetails_t));

    return loc;
}

static void stop_processing_thread(libtrace_t *trace, libtrace_thread_t *t,
        void *global, void *tls) {

    collector_global_t *glob = (collector_global_t *)global;
    colthread_local_t *loc = (colthread_local_t *)tls;
    ipv4_target_t *v4, *tmp;
    ipv6_target_t *v6, *tmp2;

    deregister_sync_queues(glob, t);

    /* TODO drain fromsync message queue so we don't leak SIP URIs
     * and any other malloced memory in the messages.
     */

    libtrace_message_queue_destroy(&(loc->tosyncq));
    libtrace_message_queue_destroy(&(loc->fromsyncq));
    libtrace_message_queue_destroy(&(loc->exportq));
    libtrace_list_deinit(loc->knownsipservers);

    HASH_ITER(hh, loc->activeipv4intercepts, v4, tmp) {
        free_all_ipsessions(v4->intercepts);
        HASH_DELETE(hh, loc->activeipv4intercepts, v4);
        free(v4);
    }

    HASH_ITER(hh, loc->activeipv6intercepts, v6, tmp2) {
        free_all_ipsessions(v6->intercepts);
        HASH_DELETE(hh, loc->activeipv6intercepts, v6);
        free(v6);
    }

    free_all_rtpstreams(loc->activertpintercepts);

    free_coreserver_list(loc->radiusservers);

    if (loc->sip_targets) {
        sipuri_hash_t *sip, *tmp;
        HASH_ITER(hh, loc->sip_targets, sip, tmp) {
            HASH_DEL(loc->sip_targets, sip);
            free(sip->uri);
            free(sip);
        }
    }

    if (loc->sipparser) {
        release_sip_parser(loc->sipparser);
    }


    if (loc->encoder) {
        free_wandder_encoder(loc->encoder);
    }

    free(loc);
}

static int process_sip_packet(libtrace_packet_t *pkt,
        colthread_local_t *loc) {

    char *uri;
    sipuri_hash_t *siphash;
    int ret;

    if ((ret = parse_sip_packet(&(loc->sipparser), pkt)) == -1) {
        logger(LOG_DAEMON, "Error while attempting to parse SIP packet");
        return 0;
    }

    if (ret == 0) {
        /* Not a usable SIP packet -- missing payload etc. */
        return 0;
    }

    /* Check the From URI */
    uri = get_sip_from_uri(loc->sipparser);
    if (uri != NULL) {
        HASH_FIND_STR(loc->sip_targets, uri, siphash);
        free(uri);
        if (siphash) {
            /* Matches a known target -- push to sync thread */
            return 1;
        }
    }


    /* Check the To URI */
    uri = get_sip_to_uri(loc->sipparser);
    if (uri != NULL) {
        HASH_FIND_STR(loc->sip_targets, uri, siphash);
        free(uri);
        if (siphash) {
            /* Matches a known target -- push to sync thread */
            return 1;
        }
    }

    /* None of the URIs in this SIP packet belong to a known target */
    return 0;
}

static inline void send_sip_update(libtrace_packet_t *pkt,
        colthread_local_t *loc) {
    openli_state_update_t sipup;

    sipup.type = OPENLI_UPDATE_SIP;
    sipup.data.pkt = pkt;
    sipup.data.pkt = pkt;

    trace_increment_packet_refcount(pkt);
    libtrace_message_queue_put(&(loc->tosyncq), (void *)(&sipup));

}

static int remove_rtp_stream(colthread_local_t *loc, char *rtpstreamkey) {
    rtpstreaminf_t *rtp;

    HASH_FIND(hh, loc->activertpintercepts, rtpstreamkey, strlen(rtpstreamkey),
            rtp);

    if (rtp == NULL) {
        logger(LOG_DAEMON, "OpenLI: collector thread was unable to remove RTP stream %s, as it was not present in its intercept set.",
                rtpstreamkey);
        return 0;
    }

    HASH_DELETE(hh, loc->activertpintercepts, rtp);

    return 1;
}

static int add_ipv4_intercept(colthread_local_t *loc, ipsession_t *sess) {

    uint32_t v4addr;
    struct sockaddr_in *sin;
    ipv4_target_t *tgt;
    ipsession_t *check;

    sin = (struct sockaddr_in *)(sess->targetip);
    if (sin == NULL) {
        logger(LOG_DAEMON, "OpenLI: attempted to add IPv4 intercept but target IP was NULL?");
        return -1;
    }

    v4addr = sin->sin_addr.s_addr;

    HASH_FIND(hh, loc->activeipv4intercepts, &v4addr, sizeof(v4addr), tgt);
    if (tgt == NULL) {
        tgt = (ipv4_target_t *)malloc(sizeof(ipv4_target_t));
        if (!tgt) {
            logger(LOG_DAEMON, "OpenLI: ran out of memory while adding IPv4 intercept address.");
            return -1;
        }
        tgt->address = v4addr;
        tgt->intercepts = NULL;
        HASH_ADD(hh, loc->activeipv4intercepts, address, sizeof(uint32_t),
                tgt);
    }

    HASH_FIND(hh, tgt->intercepts, sess->streamkey, strlen(sess->streamkey),
            check);
    assert(check == NULL);
    HASH_ADD_KEYPTR(hh, tgt->intercepts, sess->streamkey,
            strlen(sess->streamkey), sess);

    return 0;
}

static int add_ipv6_intercept(colthread_local_t *loc, ipsession_t *sess) {

    uint8_t *v6addr;
    struct sockaddr_in6 *sin6;
    ipv6_target_t *tgt;
    ipsession_t *check;

    sin6 = (struct sockaddr_in6 *)(sess->targetip);
    if (sin6 == NULL) {
        logger(LOG_DAEMON, "OpenLI: attempted to add IPv6 intercept but target IP was NULL?");
        return -1;
    }

    v6addr = sin6->sin6_addr.s6_addr;

    HASH_FIND(hh, loc->activeipv6intercepts, v6addr, 16, tgt);
    if (tgt == NULL) {
        tgt = (ipv6_target_t *)malloc(sizeof(ipv6_target_t));
        if (!tgt) {
            logger(LOG_DAEMON, "OpenLI: ran out of memory while adding IPv6 intercept address.");
            return -1;
        }
        memcpy(tgt->address, v6addr, 16);
        tgt->intercepts = NULL;
        HASH_ADD_KEYPTR(hh, loc->activeipv6intercepts, tgt->address, 16, tgt);
    }

    HASH_FIND(hh, tgt->intercepts, sess->streamkey, strlen(sess->streamkey),
            check);
    assert(check == NULL);
    HASH_ADD_KEYPTR(hh, tgt->intercepts, sess->streamkey,
            strlen(sess->streamkey), sess);

    return 0;
}

static int remove_ipv4_intercept(colthread_local_t *loc, ipsession_t *torem) {

    ipv4_target_t *v4;
    struct sockaddr_in *sin;
    uint32_t v4addr;
    ipsession_t *found;

    sin = (struct sockaddr_in *)(torem->targetip);
    if (sin == NULL) {
        logger(LOG_DAEMON, "OpenLI: attempted to remove IPv4 intercept but target IP was NULL?");
        return -1;
    }

    v4addr = sin->sin_addr.s_addr;
    HASH_FIND(hh, loc->activeipv4intercepts, &v4addr, sizeof(v4addr), v4);
    if (!v4) {
        return 0;
    }

    HASH_FIND(hh, v4->intercepts, torem->streamkey, strlen(torem->streamkey),
            found);
    if (!found) {
        return 0;
    }

    HASH_DELETE(hh, v4->intercepts, found);
    free_single_ipsession(found);

    if (HASH_CNT(hh, v4->intercepts) == 0) {
        HASH_DELETE(hh, loc->activeipv4intercepts, v4);
        free(v4);
    }

    return 1;
}

static int remove_ipv6_intercept(colthread_local_t *loc, ipsession_t *torem) {

    ipv6_target_t *v6;
    struct sockaddr_in6 *sin6;
    uint8_t *v6addr;
    ipsession_t *found;

    sin6 = (struct sockaddr_in6 *)(torem->targetip);
    if (sin6 == NULL) {
        logger(LOG_DAEMON, "OpenLI: attempted to remove IPv6 intercept but target IP was NULL?");
        return -1;
    }

    v6addr = sin6->sin6_addr.s6_addr;
    HASH_FIND(hh, loc->activeipv6intercepts, v6addr, 16, v6);
    if (!v6) {
        return 0;
    }

    HASH_FIND(hh, v6->intercepts, torem->streamkey, strlen(torem->streamkey),
            found);
    if (!found) {
        return 0;
    }

    HASH_DELETE(hh, v6->intercepts, found);
    free_single_ipsession(found);

    if (HASH_CNT(hh, v6->intercepts) == 0) {
        HASH_DELETE(hh, loc->activeipv6intercepts, v6);
        free(v6);
    }

    return 1;
}

static void process_incoming_messages(libtrace_thread_t *t,
        collector_global_t *glob, colthread_local_t *loc,
        openli_pushed_t *syncpush) {

    coreserver_t *found;
    coreserver_t **servlist;

    if (syncpush->type == OPENLI_PUSH_IPINTERCEPT) {
        if (syncpush->data.ipsess->ai_family == AF_INET) {
            if (add_ipv4_intercept(loc, syncpush->data.ipsess) != 0) {
                free_single_ipsession(syncpush->data.ipsess);
            }
        } else if (syncpush->data.ipsess->ai_family == AF_INET6) {
            if (add_ipv6_intercept(loc, syncpush->data.ipsess) != 0) {
                free_single_ipsession(syncpush->data.ipsess);
            }
        } else {
            logger(LOG_DAEMON,
                    "OpenLI: invalid address family for new IP intercept: %d",
                    syncpush->data.ipsess->ai_family);
            free_single_ipsession(syncpush->data.ipsess);
        }
    }

    if (syncpush->type == OPENLI_PUSH_HALT_IPINTERCEPT) {
        if (syncpush->data.ipsess->ai_family == AF_INET) {
            if (remove_ipv4_intercept(loc, syncpush->data.ipsess) > 0) {
                logger(LOG_DAEMON, "OpenLI: collector thread %d has stopped intercepting IP session %s",
                        trace_get_perpkt_thread_id(t),
                        syncpush->data.ipsess->streamkey);
            }
        } else if (syncpush->data.ipsess->ai_family == AF_INET6) {
            if (remove_ipv6_intercept(loc, syncpush->data.ipsess) > 0) {
                logger(LOG_DAEMON, "OpenLI: collector thread %d has stopped intercepting IP session %s",
                        trace_get_perpkt_thread_id(t),
                        syncpush->data.ipsess->streamkey);
            }
        } else {
            logger(LOG_DAEMON,
                    "OpenLI: invalid address family for removing IP intercept: %d",
                    syncpush->data.ipsess->ai_family);
        }
        free_single_ipsession(syncpush->data.ipsess);
    }

    if (syncpush->type == OPENLI_PUSH_IPMMINTERCEPT) {
        HASH_ADD_KEYPTR(hh, loc->activertpintercepts,
                syncpush->data.ipmmint->streamkey,
                strlen(syncpush->data.ipmmint->streamkey),
                syncpush->data.ipmmint);
    }

    if (syncpush->type == OPENLI_PUSH_HALT_IPMMINTERCEPT) {
        if (remove_rtp_stream(loc, syncpush->data.rtpstreamkey) != 0) {
            logger(LOG_DAEMON, "OpenLI: collector thread %d has stopped intercepting RTP stream %s",
                    trace_get_perpkt_thread_id(t),
                    syncpush->data.rtpstreamkey);
        }
        free(syncpush->data.rtpstreamkey);
    }

    if (syncpush->type == OPENLI_PUSH_SIPURI) {
        sipuri_hash_t *newsip;
        HASH_FIND_STR(loc->sip_targets, syncpush->data.sipuri, newsip);

        if (newsip) {
            newsip->references ++;
            free(syncpush->data.sipuri);
        } else {
            newsip = (sipuri_hash_t *)malloc(
                    sizeof(sipuri_hash_t));
            newsip->uri = syncpush->data.sipuri;
            newsip->references = 1;
            HASH_ADD_KEYPTR(hh, loc->sip_targets, newsip->uri,
                    strlen(newsip->uri), newsip);
            logger(LOG_DAEMON, "OpenLI: collector thread %d has added %s to list of SIP URIs.",
                    trace_get_perpkt_thread_id(t),
                    syncpush->data.sipuri);
        }
    }

    if (syncpush->type == OPENLI_PUSH_HALT_SIPURI) {
        sipuri_hash_t *torem;
        HASH_FIND_STR(loc->sip_targets, syncpush->data.sipuri, torem);
        if (torem == NULL) {
            logger(LOG_DAEMON, "OpenLI: asked to halt SIP intercept for target %s, but that is not in our set of known URIs", syncpush->data.sipuri);
            return;
        } else {
            torem->references --;
        }
        assert(torem->references >= 0);

        if (torem->references == 0) {
            logger(LOG_DAEMON, "OpenLI: collector thread %d has removed %s from list of SIP URIs.",
                    trace_get_perpkt_thread_id(t),
                    syncpush->data.sipuri);
            HASH_DEL(loc->sip_targets, torem);
            free(torem->uri);
            free(torem);
        }
        free(syncpush->data.sipuri);
    }

    if (syncpush->type == OPENLI_PUSH_CORESERVER) {
        switch(syncpush->data.coreserver->servertype) {
            case OPENLI_CORE_SERVER_RADIUS:
                servlist = &(loc->radiusservers);
                break;
            default:
                logger(LOG_DAEMON,
                        "OpenLI: unexpected core server type received by collector thread %d: %d",
                        syncpush->data.coreserver->servertype,
                        trace_get_perpkt_thread_id(t));
                return;
        }
        HASH_FIND(hh, *servlist, syncpush->data.coreserver->serverkey,
                strlen(syncpush->data.coreserver->serverkey), found);
        if (!found) {
            HASH_ADD_KEYPTR(hh, *servlist, syncpush->data.coreserver->serverkey,
                    strlen(syncpush->data.coreserver->serverkey),
                    syncpush->data.coreserver);
            logger(LOG_DAEMON, "OpenLI: collector thread %d has added %s to its %s core server list.",
                    trace_get_perpkt_thread_id(t),
                    syncpush->data.coreserver->serverkey,
                    coreserver_type_to_string(
                            syncpush->data.coreserver->servertype));
        } else {
            free_single_coreserver(syncpush->data.coreserver);
        }
    }

    if (syncpush->type == OPENLI_PUSH_REMOVE_CORESERVER) {
        switch(syncpush->data.coreserver->servertype) {
            case OPENLI_CORE_SERVER_RADIUS:
                servlist = &(loc->radiusservers);
                break;
            default:
                logger(LOG_DAEMON,
                        "OpenLI: unexpected core server type received by collector thread %d: %d",
                        syncpush->data.coreserver->servertype,
                        trace_get_perpkt_thread_id(t));
                return;
        }
        HASH_FIND(hh, *servlist, syncpush->data.coreserver->serverkey,
                strlen(syncpush->data.coreserver->serverkey), found);
        if (found) {
            HASH_DELETE(hh, *servlist, found);
            logger(LOG_DAEMON, "OpenLI: collector thread %d has removed %s from its %s core server list.",
                    trace_get_perpkt_thread_id(t),
                    syncpush->data.coreserver->serverkey,
                    coreserver_type_to_string(
                        syncpush->data.coreserver->servertype));
            free_single_coreserver(found);
        }
        free_single_coreserver(syncpush->data.coreserver);
    }

}

static libtrace_packet_t *process_packet(libtrace_t *trace,
        libtrace_thread_t *t, void *global, void *tls,
        libtrace_packet_t *pkt) {

    collector_global_t *glob = (collector_global_t *)global;
    colthread_local_t *loc = (colthread_local_t *)tls;
    void *l3;
    uint16_t ethertype;
    uint32_t rem;
    int forwarded = 0;

    openli_pushed_t syncpush;

    /* Check for any messages from the sync thread */
    while (libtrace_message_queue_try_get(&(loc->fromsyncq),
            (void *)&syncpush) != LIBTRACE_MQ_FAILED) {

        process_incoming_messages(t, glob, loc, &syncpush);
    }

    l3 = trace_get_layer3(pkt, &ethertype, &rem);
    if (l3 == NULL || rem == 0) {
        return pkt;
    }

    /* Is this a RADIUS packet? -- if yes, create a state update */

    /* Is this a SIP packet? -- if yes, create a state update */
    if (identified_as_sip(pkt, loc->knownsipservers)) {
        if (process_sip_packet(pkt, loc) == 1) {
            send_sip_update(pkt, loc);
            forwarded = 1;
        }
    }

    if (ethertype == TRACE_ETHERTYPE_IP) {
        /* Is this an IP packet? -- if yes, possible IP CC */
        if (ipv4_comm_contents(pkt, (libtrace_ip_t *)l3, rem, glob, loc)) {
            forwarded = 1;
        }
        /* Is this an RTP packet? -- if yes, possible IPMM CC */
        if (ip4mm_comm_contents(pkt, (libtrace_ip_t *)l3, rem, glob, loc)) {
            forwarded = 1;
        }

    }

    /* TODO IPV6 CC */


    if (forwarded) {
        return NULL;
    }
    return pkt;


}

static int start_input(collector_global_t *glob, colinput_t *inp) {

    if (inp->running == 1) {
        /* Trace is already running */
        return 1;
    }

    if (!inp->pktcbs) {
        inp->pktcbs = trace_create_callback_set();
        trace_set_starting_cb(inp->pktcbs, start_processing_thread);
        trace_set_stopping_cb(inp->pktcbs, stop_processing_thread);
        trace_set_packet_cb(inp->pktcbs, process_packet);
    }

    assert(!inp->trace);
    inp->trace = trace_create(inp->uri);
    if (trace_is_err(inp->trace)) {
        libtrace_err_t lterr = trace_get_err(inp->trace);
        logger(LOG_DAEMON, "OpenLI: Failed to create trace for input %s: %s",
                inp->uri, lterr.problem);
        return 0;
    }

    trace_set_perpkt_threads(inp->trace, inp->threadcount);
    trace_set_hasher(inp->trace, HASHER_BIDIRECTIONAL, NULL, NULL);

    if (trace_pstart(inp->trace, glob, inp->pktcbs, NULL) == -1) {
        libtrace_err_t lterr = trace_get_err(inp->trace);
        logger(LOG_DAEMON, "OpenLI: Failed to start trace for input %s: %s",
                inp->uri, lterr.problem);
        return 0;
    }

    logger(LOG_DAEMON,
            "OpenLI: collector has started reading packets from %s using %d threads.",
            inp->uri, inp->threadcount);
    inp->running = 1;
    return 1;
}

static void reload_inputs(collector_global_t *glob,
        collector_global_t *newstate) {

    colinput_t *oldinp, *newinp, *tmp;

    HASH_ITER(hh, glob->inputs, oldinp, tmp) {
        HASH_FIND(hh, newstate->inputs, oldinp->uri, strlen(oldinp->uri),
                newinp);
        if (!newinp || newinp->threadcount != oldinp->threadcount) {
            /* This input is no longer wanted at all */
            logger(LOG_DAEMON,
                    "OpenLI collector: stop reading packets from %s\n",
                    oldinp->uri);
            trace_pstop(oldinp->trace);
            HASH_DELETE(hh, glob->inputs, oldinp);
            libtrace_list_push_back(glob->expired_inputs, &oldinp);
            continue;
        }

        /* Mark this input as being present in the original list */
        newinp->running = 1;
    }

    HASH_ITER(hh, newstate->inputs, newinp, tmp) {
        if (newinp->running) {
            continue;
        }

        /* This input is new, move it into the 'official' input list */
        HASH_DELETE(hh, newstate->inputs, newinp);
        HASH_ADD_KEYPTR(hh, glob->inputs, newinp->uri, strlen(newinp->uri),
                newinp);
    }

}

static int reload_collector_config(collector_global_t *glob,
        collector_sync_t *sync) {

    collector_global_t *newstate;

    newstate = parse_global_config(glob->configfile);
    if (newstate == NULL) {
        logger(LOG_DAEMON,
                "OpenLI: error reloading config file for collector.");
        return -1;
    }

    if (strcmp(newstate->provisionerip, glob->provisionerip) != 0 ||
            strcmp(newstate->provisionerport, glob->provisionerport) != 0) {
        logger(LOG_DAEMON,
                "OpenLI collector: disconnecting from provisioner due to config change.");
        sync_disconnect_provisioner(sync);
        free(glob->provisionerip);
        free(glob->provisionerport);
        glob->provisionerip = strdup(newstate->provisionerip);
        glob->provisionerport = strdup(newstate->provisionerport);
    } else {
        logger(LOG_DAEMON,
                "OpenLI collector: provisioner socket configuration is unchanged.");
    }

    pthread_rwlock_wrlock(&(glob->config_mutex));

    reload_inputs(glob, newstate);

    /* Just update these, regardless of whether they've changed. It's more
     * effort to check for a change than it is worth and there are no
     * flow-on effects to a change.
     */
    if (glob->operatorid) {
        free(glob->operatorid);
    }
    glob->operatorid = newstate->operatorid;
    glob->operatorid_len = newstate->operatorid_len;
    newstate->operatorid = NULL;

    if (glob->networkelemid) {
        free(glob->networkelemid);
    }
    glob->networkelemid = newstate->networkelemid;
    glob->networkelemid_len = newstate->networkelemid_len;
    newstate->networkelemid = NULL;

    if (glob->intpointid) {
        free(glob->intpointid);
    }
    glob->intpointid = newstate->intpointid;
    glob->intpointid_len = newstate->intpointid_len;
    newstate->intpointid = NULL;

    pthread_rwlock_unlock(&(glob->config_mutex));
    clear_global_config(newstate);
    return 0;
}

static void *start_sync_thread(void *params) {
    collector_global_t *glob = (collector_global_t *)params;
    int ret;
    collector_sync_t *sync = init_sync_data(glob);
    sync_sendq_t *sq;

    /* XXX For early development work, we will read intercept instructions
     * from a config file. Eventually this should be replaced with
     * instructions that are received via a network interface.
     */

    register_export_queue(glob, &(sync->exportq));

    while (collector_halt == 0) {
        if (reload_config) {
            if (reload_collector_config(glob, sync) == -1) {
                break;
            }
            reload_config = 0;
        }
        if (sync->instruct_fd == -1) {
            ret = sync_connect_provisioner(sync);
            if (ret < 0) {
                /* Fatal error */
                logger(LOG_DAEMON,
                        "OpenLI: collector is unable to reach provisioner.");
                break;
            }

            if (ret == 0) {
                /* Connection failed, but we should retry */
                usleep(500000);
                continue;
            }
        }

        ret = sync_thread_main(sync);
        if (ret == -1) {
            break;
        }
    }

    /* Collector is halting, stop all processing threads */
    halt_processing_threads(glob);
    clean_sync_data(sync);

    /* Wait for all processing threads to de-register their sync queues */
    while ((sq = (sync_sendq_t *)(glob->syncsendqs)) && HASH_CNT(hh, sq) > 0) {
        usleep(500000);
    }

    free(sync);
    logger(LOG_DAEMON, "OpenLI: exiting sync thread.");
    pthread_exit(NULL);

}

static void *start_export_thread(void *params) {
    collector_global_t *glob = (collector_global_t *)params;
    collector_export_t *exp = init_exporter(glob);
    int connected = 0;

    if (exp == NULL) {
        logger(LOG_DAEMON, "OpenLI: exporting thread is not functional!");
        collector_halt = 1;
        pthread_exit(NULL);
    }

    while (collector_halt == 0) {
        if (exporter_thread_main(exp) <= 0) {
            break;
        }
    }

    destroy_exporter(exp);
    logger(LOG_DAEMON, "OpenLI: exiting export thread.");
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {

	struct sigaction sigact;
    sigset_t sig_before, sig_block_all;
    char *configfile = NULL;
    collector_global_t *glob = NULL;
    int i, ret;
    colinput_t *inp, *tmp;

    while (1) {
        int optind;
        struct option long_options[] = {
            { "help", 0, 0, 'h' },
            { "config", 1, 0, 'c'},
            { NULL, 0, 0, 0 }
        };

        int c = getopt_long(argc, argv, "c:h", long_options,
                &optind);
        if (c == -1) {
            break;
        }

        switch(c) {
            case 'c':
                configfile = optarg;
                break;
            case 'h':
                usage(argv[0]);
                return 1;
            default:
                logger(LOG_DAEMON, "OpenLI: unsupported option: %c", c);
                usage(argv[0]);
                return 1;
        }
    }

    if (configfile == NULL) {
        logger(LOG_DAEMON,
                "OpenLI: no config file specified. Use -c to specify one.");
        usage(argv[0]);
        return 1;
    }

    /* Initialise osipparser2 */
    parser_init();

    sigact.sa_handler = cleanup_signal;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESTART;

    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
	signal(SIGPIPE, SIG_IGN);

    sigact.sa_handler = reload_signal;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESTART;

    sigaction(SIGHUP, &sigact, NULL);

    /* Read config to generate list of input sources */
    glob = parse_global_config(configfile);
    if (glob == NULL) {
        return 1;
    }

    /* Start sync thread */
    ret = pthread_create(&(glob->syncthreadid), NULL, start_sync_thread,
            (void *)glob);
    if (ret != 0) {
        logger(LOG_DAEMON, "OpenLI: error creating sync thread. Exiting.");
        return 1;
    }

    /* Start export thread */
    ret = pthread_create(&(glob->exportthreadid), NULL, start_export_thread,
            (void *)glob);
    if (ret != 0) {
        logger(LOG_DAEMON, "OpenLI: error creating export thread. Exiting.");
        return 1;
    }

    /* Start processing threads for each input */
    while (!collector_halt) {
        sigemptyset(&sig_block_all);
        if (pthread_sigmask(SIG_SETMASK, &sig_block_all, &sig_before) < 0) {
            logger(LOG_DAEMON, "Unable to disable signals before starting threads.");
            return 1;
        }

        pthread_rwlock_rdlock(&(glob->config_mutex));
        HASH_ITER(hh, glob->inputs, inp, tmp) {
            if (start_input(glob, inp) == 0) {
                logger(LOG_DAEMON, "OpenLI: failed to start input %s\n",
                        inp->uri);
            }
        }
        pthread_rwlock_unlock(&(glob->config_mutex));

        if (pthread_sigmask(SIG_SETMASK, &sig_before, NULL)) {
            logger(LOG_DAEMON, "Unable to re-enable signals after starting threads.");
            return 1;
        }
        usleep(1000);
    }

    pthread_rwlock_rdlock(&(glob->config_mutex));
    HASH_ITER(hh, glob->inputs, inp, tmp) {
        if (inp->trace) {
            trace_join(inp->trace);
        }
    }
    pthread_rwlock_unlock(&(glob->config_mutex));

    pthread_join(glob->syncthreadid, NULL);
    pthread_join(glob->exportthreadid, NULL);

    logger(LOG_DAEMON, "OpenLI: exiting OpenLI Collector.");
    /* Tidy up, exit */
    clear_global_config(glob);

    return 0;
}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
