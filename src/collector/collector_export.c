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

#include <unistd.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <assert.h>

#include <libtrace.h>
#include <libtrace_parallel.h>

#include "collector.h"
#include "collector_export.h"
#include "export_buffer.h"
#include "configparser.h"
#include "ipmmcc.h"
#include "ipcc.h"
#include "ipmmiri.h"
#include "ipiri.h"
#include "logger.h"
#include "util.h"
#include "internal.pb-c.h"
#include "encoder_worker.h"

#define BUF_BATCH_SIZE (10 * 1024 * 1024)
enum {
    EXP_EPOLL_MQUEUE = 0,
    EXP_EPOLL_TIMER = 1,
    EXP_EPOLL_FLAG_TIMEOUT = 2,
};

collector_export_t *init_exporter(export_thread_data_t *glob) {

    collector_export_t *exp = (collector_export_t *)malloc(
            sizeof(collector_export_t));
    int ret, i;

    exp->glob = glob;
    exp->dests = libtrace_list_init(sizeof(export_dest_t));
    exp->intercepts = NULL;
    exp->freeresults = NULL;

    exp->failed_conns = 0;
    exp->flagged = 0;
    exp->flagtimerfd = -1;

    exp->count = 0;
    exp->zmq_subsock = NULL;
    exp->zmq_pushjobsock = NULL;
    exp->zmq_pullressock = NULL;

    exp->zmq_control = zmq_socket(exp->glob->zmq_ctxt, ZMQ_PUB);
    if (zmq_bind(exp->zmq_control, "inproc://openliexportercontrol") != 0) {
        logger(LOG_INFO, "OpenLI: unable to create control socket for encoding threads");
        logger(LOG_INFO, "OpenLI: no export worker threads will be started");
        exp->zmq_control = NULL;
        exp->workercount = 0;
        exp->workers = NULL;
        return exp;
    }

    exp->workers = (openli_encoder_t *)calloc(exp->glob->workers,
            sizeof(openli_encoder_t));
    exp->workercount = exp->glob->workers;
    for (i = 0; i < exp->workercount; i++) {
        exp->workers[i].zmq_ctxt = exp->glob->zmq_ctxt;
        exp->workers[i].zmq_recvjob = NULL;
        exp->workers[i].zmq_pushresult = NULL;
        exp->workers[i].zmq_control = NULL;
        exp->workers[i].workerid = i;
        exp->workers[i].shared = exp->glob->shared;
        exp->workers[i].encoder = NULL;
        exp->workers[i].freegenerics = NULL;

        ret = pthread_create(&(exp->workers[i].threadid), NULL,
                run_encoder_worker, &(exp->workers[i]));
        if (ret != 0) {
            logger(LOG_INFO, "Warning: unable to start encoder worker thread %d\n", i);
            continue;
        }
    }

    return exp;
}

static int connect_single_target(export_dest_t *dest) {

    int sockfd;

    if (dest->details.ipstr == NULL) {
        /* This is an unannounced mediator */
        return -1;
    }

    sockfd = connect_socket(dest->details.ipstr, dest->details.portstr,
            dest->failmsg, 0);

    if (sockfd == -1) {
        /* TODO should probably bail completely on this dest if this
         * happens. */
        return -1;
    }

    if (sockfd == 0) {
        dest->failmsg = 1;
        return -1;
    }

    dest->failmsg = 0;
    /* If we disconnected after a partial send, make sure we re-send the
     * whole record and trust that downstream will figure out how to deal
     * with any duplication.
     */
    dest->buffer.partialfront = 0;
    return sockfd;
}

int connect_export_targets(collector_export_t *exp) {

    int success = 0;
    libtrace_list_node_t *n;
    export_dest_t *d;

    exp->failed_conns = 0;

    n = exp->dests->head;

    while (n) {
        d = (export_dest_t *)n->data;
        n = n->next;

        if (d->halted) {
            continue;
        }

        if (d->fd != -1) {
            /* Already connected */
            success ++;
            continue;
        }

        d->fd = connect_single_target(d);
        if (d->fd != -1) {
            if (get_buffered_amount(&(d->buffer)) > 0 &&
                    transmit_buffered_records(&(d->buffer), d->fd,
                            BUF_BATCH_SIZE) == -1) {
                close(d->fd);
                d->fd = -1;
                exp->failed_conns ++;
            } else {
                success ++;
            }

        } else {
            exp->failed_conns ++;
        }
    }

    /* Return number of targets which we connected to */
    return success;

}

static inline void free_intercept_msg(exporter_intercept_msg_t *msg) {
    free(msg->liid);
    free(msg->authcc);
    free(msg->delivcc);
    free(msg);
}

static inline void free_cinsequencing(exporter_intercept_state_t *intstate) {
    cin_seqno_t *c, *tmp;

    HASH_ITER(hh, intstate->cinsequencing, c, tmp) {
        HASH_DELETE(hh, intstate->cinsequencing, c);
        free(c);
    }
}

static inline void remove_all_destinations(collector_export_t *exp) {
    export_dest_t *d;
    libtrace_list_node_t *n;
    struct epoll_event ev;

    /* Close all dest fds */
    n = exp->dests->head;
    while (n) {
        d = (export_dest_t *)n->data;

        if (d->fd != -1) {
            close(d->fd);
        }
        /* Don't free d->details, let the sync thread tidy this up */
        release_export_buffer(&(d->buffer));

        if (d->details.portstr) {
            free(d->details.portstr);
        }

        if (d->details.ipstr) {
            free(d->details.ipstr);
        }
        n = n->next;
    }

    libtrace_list_deinit(exp->dests);
    exp->dests = NULL;

}

void destroy_exporter(collector_export_t *exp) {
    exporter_intercept_state_t *intstate, *tmpexp;
    int i, x;
    openli_encoded_result_t res;

    /* push halt messages to all workers */

    if (exp->zmq_control) {
        if (zmq_send(exp->zmq_control, NULL, 0, 0) < 0) {
            logger(LOG_INFO, "OpenLI: error while sending halt message to export worker thread %d", i);
        }
        zmq_close(exp->zmq_control);
    }

    /* purge all incoming results */
    do {
        x = zmq_recv(exp->zmq_pullressock, &res, sizeof(res), ZMQ_DONTWAIT);
        if (x < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            break;
        }

        if (res.msgbody) {
            free(res.msgbody->encoded);
            free(res.msgbody);
        }

        if (res.ipcontents) {
            free(res.ipcontents);
        }
    } while (x > 0);

    /* join on all workers, then delete worker array */
    for (i = 0; i < exp->workercount; i++) {
        pthread_join(exp->workers[i].threadid, NULL);
        destroy_encoder_worker(&(exp->workers[i]));
    }
    if (exp->workers) {
        free(exp->workers);
    }

    remove_all_destinations(exp);

    HASH_ITER(hh, exp->intercepts, intstate, tmpexp) {
        HASH_DELETE(hh, exp->intercepts, intstate);
        free_intercept_msg(intstate->details);
        free_cinsequencing(intstate);
        free(intstate);
    }

    while (exp->freeresults) {
        wandder_encoded_result_t *r = exp->freeresults;
        exp->freeresults = exp->freeresults->next;

        free(r->encoded);
        free(r);
    }

    if (exp->zmq_subsock) {
        zmq_close(exp->zmq_subsock);
    }

    if (exp->zmq_pushjobsock) {
        zmq_close(exp->zmq_pushjobsock);
    }

    if (exp->zmq_pullressock) {
        zmq_close(exp->zmq_pullressock);
    }

    printf("exporter sent %d messages\n", exp->count);

    free(exp);
}


static int forward_fd(export_dest_t *dest, openli_encoded_result_t *msg) {

    uint32_t enclen = msg->msgbody->len - msg->ipclen;
    int ret;
    struct iovec iov[4];
    struct msghdr mh;
    int ind = 0;
    int total = 0;
    char liidbuf[65542];

    iov[ind].iov_base = &(msg->header);
    iov[ind].iov_len = sizeof(msg->header);
    ind ++;
    total += sizeof(msg->header);

    if (msg->intstate->details->liid) {
        int used = 0;
        uint16_t etsiwraplen = 0;
        uint16_t l = htons(msg->intstate->details->liid_len);

        memcpy(liidbuf + used, &l, sizeof(uint16_t));
        used += sizeof(uint16_t);
        memcpy(liidbuf + used, msg->intstate->details->liid,
                msg->intstate->details->liid_len);

        iov[ind].iov_base = liidbuf;
        iov[ind].iov_len = msg->intstate->details->liid_len + used;
        total += iov[ind].iov_len;
        ind ++;
    }

    iov[ind].iov_base = msg->msgbody->encoded;
    iov[ind].iov_len = enclen;
    total += enclen;
    ind ++;

    if (msg->ipclen > 0) {
        iov[ind].iov_base = msg->ipcontents;
        iov[ind].iov_len = msg->ipclen;
        ind ++;
        total += msg->ipclen;
    }

    mh.msg_name = NULL;
    mh.msg_namelen = 0;
    mh.msg_iov = iov;
    mh.msg_iovlen = ind;
    mh.msg_control = NULL;
    mh.msg_controllen = 0;
    mh.msg_flags = 0;

    ret = sendmsg(dest->fd, &mh, MSG_DONTWAIT);
    if (ret < 0) {
        if (append_message_to_buffer(&(dest->buffer), msg, 0) == 0) {

            /* TODO do something if we run out of memory? */

        }
        if (errno != EAGAIN) {
            logger(LOG_INFO, "OpenLI: Error exporting to target %s:%s -- %s.",
                dest->details.ipstr, dest->details.portstr, strerror(errno));
            return -1;
        }

        return 0;
    } else if (ret < total && ret >= 0) {
        /* Partial send, save whole message but make sure the buffer knows
         * how much we've already sent so it can continue from there.
         */
        if (append_message_to_buffer(&(dest->buffer), msg, (uint32_t)ret)
                    == 0) {
            /* TODO do something if we run out of memory? */
        }
    }
    return 0;
}


static int forward_message(export_dest_t *dest, openli_encoded_result_t *msg) {

    int ret = 0;

    if (dest->fd == -1) {
        /* buffer this message for when we are able to connect */
        if (append_message_to_buffer(&(dest->buffer), msg, 0) == 0) {
            /* TODO do something if we run out of memory? */

        }
        goto endforward;
    }

    if (get_buffered_amount(&(dest->buffer)) == 0) {
        ret = forward_fd(dest, msg);
        goto endforward;
    }

    if (transmit_buffered_records(&(dest->buffer), dest->fd, BUF_BATCH_SIZE)
                == -1) {
        ret = -1;
        goto endforward;

    }

    if (get_buffered_amount(&(dest->buffer)) == 0) {
        /* buffer is now empty, try to push out this message too */
        ret = forward_fd(dest, msg);
        goto endforward;
    }

    /* buffer was not completely drained, so we have to buffer this
     * message too -- hopefully we'll catch up soon */
    if (append_message_to_buffer(&(dest->buffer), msg, 0) == 0) {
        /* TODO do something if we run out of memory? */

    }

endforward:
    return ret;
}

static inline export_dest_t *add_unknown_destination(collector_export_t *exp,
        uint32_t medid) {
    export_dest_t newdest, *dest;

    newdest.failmsg = 0;
    newdest.fd = -1;
    newdest.details.mediatorid = medid;
    newdest.details.ipstr = NULL;
    newdest.details.portstr = NULL;
    newdest.awaitingconfirm = 0;
    newdest.halted = 0;
    init_export_buffer(&(newdest.buffer), 1);

    libtrace_list_push_back(exp->dests, &newdest);

    dest = (export_dest_t *)(exp->dests->tail->data);
    return dest;
}

static void remove_destination(collector_export_t *exp,
        openli_mediator_t *med) {

    libtrace_list_node_t *n;
    export_dest_t *dest;
    struct epoll_event ev;

    n = exp->dests->head;
    while (n) {
        dest = (export_dest_t *)(n->data);
        n = n->next;

        if (dest->details.mediatorid != med->mediatorid) {
            continue;
        }

        logger(LOG_INFO,
                "OpenLI exporter: removing mediator %u from export destination list",
                med->mediatorid);

        if (dest->fd != -1) {
            close(dest->fd);
            dest->fd = -1;
        }
        dest->halted = 1;
    }

    free(med->ipstr);
    free(med->portstr);
}

static int add_new_destination(collector_export_t *exp,
        openli_mediator_t *med) {

    libtrace_list_node_t *n;
    export_dest_t newdest, *dest;
    struct itimerspec its;

    n = exp->dests->head;
    while (n) {
        dest = (export_dest_t *)(n->data);
        if (dest->details.ipstr == NULL &&
                dest->details.mediatorid == med->mediatorid) {
            /* This is the announcement for a previously unannounced
             * mediator. */
            dest->failmsg = 0;
            dest->fd = -1;
            dest->details = *(med);
            goto destepoll;
        } else if (dest->details.mediatorid == med->mediatorid) {

            /* This is a re-announcement of an existing mediator -- this
             * could be due to reconnecting to the provisioner so don't
             * panic just yet. */
            if (strcmp(dest->details.ipstr, med->ipstr) != 0 ||
                    strcmp(dest->details.portstr, med->portstr) != 0) {
                logger(LOG_INFO, "OpenLI: mediator %u has changed location from %s:%s to %s:%s.",
                        med->mediatorid, dest->details.ipstr,
                        dest->details.portstr, med->ipstr, med->portstr);

                dest->details = *(med);
                if (dest->fd != -1) {
                    close(dest->fd);
                    dest->fd = -1;
                }
            }
            dest->awaitingconfirm = 0;
            dest->halted = 0;
            goto destepoll;
        }
        n = n->next;
    }

    /* Entirely new mediator ID */

    newdest.failmsg = 0;
    newdest.fd = -1;
    newdest.awaitingconfirm = 0;
    newdest.halted = 0;
    newdest.details = *(med);
    init_export_buffer(&(newdest.buffer), 1);

    libtrace_list_push_back(exp->dests, &newdest);

destepoll:
    if (!exp->flagged) {
        return 1;
    }

    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;

    if (exp->flagtimerfd == -1) {
        exp->flagtimerfd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (exp->flagtimerfd == -1) {
            logger(LOG_INFO, "OpenLI: failed to create export timer fd: %s.",
                    strerror(errno));
            return -1;
        }
    }
    timerfd_settime(exp->flagtimerfd, 0, &its, NULL);
    return 1;
}

static void purge_unconfirmed_mediators(collector_export_t *exp) {
    libtrace_list_node_t *n;
    export_dest_t *dest;

    n = exp->dests->head;
    while (n) {
        dest = (export_dest_t *)(n->data);
        if (dest->awaitingconfirm) {
            if (dest->fd != -1) {
                logger(LOG_INFO,
                        "OpenLI exporter: closing connection to unwanted mediator on fd %d", dest->fd);
                close(dest->fd);
                dest->fd = -1;
            }
            dest->halted = 1;
        }
        n = n->next;
    }
}

static void exporter_new_intercept(collector_export_t *exp,
        exporter_intercept_msg_t *msg) {

    exporter_intercept_state_t *intstate;
    etsili_intercept_details_t intdetails;

    /* If this LIID already exists, we'll need to replace it */
    HASH_FIND(hh, exp->intercepts, msg->liid, strlen(msg->liid), intstate);

    if (intstate) {
        free_intercept_msg(intstate->details);
        etsili_clear_preencoded_fields(intstate->preencoded);
        /* leave the CIN seqno state as is for now */
        intstate->details = msg;
        return;
    }

    /* New LIID, create fresh intercept state */
    intstate = (exporter_intercept_state_t *)malloc(
            sizeof(exporter_intercept_state_t));
    intstate->details = msg;
    intstate->cinsequencing = NULL;

    intdetails.liid = msg->liid;
    intdetails.authcc = msg->authcc;
    intdetails.delivcc = msg->delivcc;
    intdetails.operatorid = exp->glob->shared->operatorid;
    intdetails.networkelemid = exp->glob->shared->networkelemid;
    intdetails.intpointid = exp->glob->shared->intpointid;

    etsili_preencode_static_fields(intstate->preencoded, &intdetails);

    HASH_ADD_KEYPTR(hh, exp->intercepts, msg->liid, strlen(msg->liid),
            intstate);
}

static int exporter_end_intercept(collector_export_t *exp,
        exporter_intercept_msg_t *msg) {

    exporter_intercept_state_t *intstate;

    HASH_FIND(hh, exp->intercepts, msg->liid, strlen(msg->liid), intstate);

    if (!intstate) {
        logger(LOG_INFO, "Exporter thread was told to end intercept LIID %s, but it is not a valid ID?",
                msg->liid);
        return -1;
    }

    HASH_DELETE(hh, exp->intercepts, intstate);
    free_intercept_msg(msg);
    free_intercept_msg(intstate->details);
    etsili_clear_preencoded_fields(intstate->preencoded);
    free_cinsequencing(intstate);
    free(intstate);
    return 1;
}

static inline char *extract_liid_from_job(openli_export_recv_t *recvd) {

    switch(recvd->type) {
        case OPENLI_EXPORT_IPMMCC:
            return recvd->data.ipmmcc.liid;
        case OPENLI_EXPORT_IPCC:
            return recvd->data.ipcc.liid;
        case OPENLI_EXPORT_IPIRI:
            return recvd->data.ipiri.liid;
        case OPENLI_EXPORT_IPMMIRI:
            return recvd->data.ipmmiri.liid;
    }
    return NULL;
}

static inline uint32_t extract_cin_from_job(openli_export_recv_t *recvd) {

    switch(recvd->type) {
        case OPENLI_EXPORT_IPMMCC:
            return recvd->data.ipmmcc.cin;
        case OPENLI_EXPORT_IPCC:
            return recvd->data.ipcc.cin;
        case OPENLI_EXPORT_IPIRI:
            return recvd->data.ipiri.cin;
        case OPENLI_EXPORT_IPMMIRI:
            return recvd->data.ipmmiri.cin;
    }
    logger(LOG_INFO,
            "OpenLI: invalid message type in extract_cin_from_job: %u",
            recvd->type);
    return 0;
}

static inline void free_job_request(openli_export_recv_t *recvd) {
    switch(recvd->type) {
        case OPENLI_EXPORT_IPMMCC:
            free(recvd->data.ipmmcc.liid);
            break;
        case OPENLI_EXPORT_IPCC:
            free(recvd->data.ipcc.liid);
            break;
        case OPENLI_EXPORT_IPIRI:
            free(recvd->data.ipiri.liid);
            /*
            if (recvd->data.ipiri.plugin) {
                recvd->data.ipiri.plugin->destroy_parsed_data(
                        recvd->data.ipiri.plugin,
                        recvd->data.ipiri.plugin_data);
            }
            */
            break;
        case OPENLI_EXPORT_IPMMIRI:
            free(recvd->data.ipmmiri.liid);
            break;
    }
}

static int export_encoded_record(collector_export_t *exp,
        openli_encoded_result_t *tosend) {

    libtrace_list_node_t *n;
    export_dest_t *dest;
    int x;

    n = exp->dests->head;

    /* TODO replace with a hash map? */
    while (n) {
        dest = (export_dest_t *)(n->data);

        if (dest->details.mediatorid == tosend->destid) {
            x = forward_message(dest, tosend);
            if (x == -1) {
                close(dest->fd);
                dest->fd = -1;
                return -1;
            }
            break;
        }
        n = n->next;
    }

    if (n == NULL) {
        /* We don't recognise this mediator ID, but the
         * announcement for it could be coming soon. Create an
         * export_dest for it and buffer received messages
         * until we get an announcement.
         *
         * TODO need some way to recognise that an announcement
         * is NOT coming so we don't buffer forever...
         */
        dest = add_unknown_destination(exp, tosend->destid);
        if (forward_message(dest, tosend) < 0) {
            return -1;
        }
    }
    return 1;
}

static int run_encoding_job(collector_export_t *exp,
        openli_export_recv_t *recvd) {

    char *liid;
    uint32_t cin;
    cin_seqno_t *cinseq;
    exporter_intercept_state_t *intstate;
    int ret = 0;
    int ind = 0;
    openli_encoding_job_t job;

    liid = extract_liid_from_job(recvd);
    cin = extract_cin_from_job(recvd);

    HASH_FIND(hh, exp->intercepts, liid, strlen(liid), intstate);
    if (!intstate) {
        logger(LOG_INFO, "Received encoding job for an unknown LIID: %s??",
                liid);
        free_job_request(recvd);
        return 0;
    }

    HASH_FIND(hh, intstate->cinsequencing, &cin, sizeof(cin), cinseq);
    if (!cinseq) {
        cinseq = (cin_seqno_t *)malloc(sizeof(cin_seqno_t));

        if (!cinseq) {
            logger(LOG_INFO,
                    "OpenLI: out of memory when creating CIN seqno tracker in exporter thread");
            return -1;
        }

        cinseq->cin = cin;
        cinseq->iri_seqno = 0;
        cinseq->cc_seqno = 0;

        HASH_ADD_KEYPTR(hh, intstate->cinsequencing, &(cinseq->cin),
                sizeof(cin), cinseq);
    }

    job.intstate = intstate;
    job.type = recvd->type;
    job.seqno = cinseq->cc_seqno;
    job.ts = recvd->ts;
    job.destid = recvd->destid;

    if (exp->freeresults) {
        job.toreturn = exp->freeresults;
        exp->freeresults = exp->freeresults->next;
        job.toreturn->next = NULL;
    } else {
        job.toreturn = NULL;
    }

    switch (recvd->type) {
        case OPENLI_EXPORT_IPCC:
            job.data.ipcc = recvd->data.ipcc;
            break;
        case OPENLI_EXPORT_IPMMCC:
            job.data.ipmmcc = recvd->data.ipmmcc;
            break;
        case OPENLI_EXPORT_IPIRI:
            job.data.ipiri = recvd->data.ipiri;
            break;
        case OPENLI_EXPORT_IPMMIRI:
            job.data.ipmmiri = recvd->data.ipmmiri;
            break;
    }

    if (zmq_send(exp->zmq_pushjobsock, (char *)&job,
            sizeof(openli_encoding_job_t), 0) < 0) {
        logger(LOG_INFO,
                "Error while pushing encoding job to worker threads: %s",
                strerror(errno));
        return -1;
    }

    /* TODO deal with RADIUS multi-iteration jobs... */

    if (recvd->type == OPENLI_EXPORT_IPMMCC ||
            recvd->type == OPENLI_EXPORT_IPCC) {
        cinseq->cc_seqno ++;
    } else {
        cinseq->iri_seqno ++;
    }
    ind ++;

#if 0
    while (ret != 0) {
        switch(recvd->type) {
            case OPENLI_EXPORT_IPMMCC:
                ret = encode_ipmmcc(&(exp->encoder), &(recvd->data.ipmmcc),
                        intstate->details, cinseq->cc_seqno, &tosend);
                cinseq->cc_seqno ++;
                trace_decrement_packet_refcount(recvd->data.ipmmcc.packet);
                break;
            case OPENLI_EXPORT_IPCC:
                ret = encode_ipcc(&(exp->encoder), intstate->preencoded,
                        &(recvd->data.ipcc), cinseq->cc_seqno, &tosend);
                cinseq->cc_seqno ++;
                break;
                //return 0;
            case OPENLI_EXPORT_IPMMIRI:
                ret = encode_ipmmiri(&(exp->encoder), &(recvd->data.ipmmiri),
                        intstate->details, cinseq->iri_seqno, &tosend,
                        &(recvd->ts));
                if (ret >= 0) {
                    cinseq->iri_seqno ++;
                }
                if (recvd->data.ipmmiri.packet) {
                    trace_decrement_packet_refcount(recvd->data.ipmmiri.packet);
                }
                break;
            case OPENLI_EXPORT_IPIRI:
                ret = encode_ipiri(&(exp->freegenerics),
                        &(exp->encoder), exp->glob->shared,
                        &(recvd->data.ipiri),
                        intstate->details, cinseq->iri_seqno, &tosend, ind);
                if (ret >= 0) {
                    cinseq->iri_seqno ++;
                    ind ++;
                }
                break;
        }
        if (ret < 0) {
            break;
        }

        tosend.destid = recvd->destid;
        if (export_encoded_record(exp, &tosend) < 0) {
            ret = -1;
            break;
        }
    }
#endif

    return ret;
}

#define ZMQ_READ_NEXT_PART \
    zmq_msg_init(&part); \
    x = zmq_msg_recv(&part, exp->zmq_subsock, ZMQ_DONTWAIT); \
    if (x < 0) { \
        if (errno == EAGAIN) { \
            return 0; \
        } \
        return -1; \
    } \
    zmq_getsockopt(exp->zmq_subsock, ZMQ_RCVMORE, &more, &moresize); \
    ptr = zmq_msg_data(&part);

static int read_ipiri_job(collector_export_t *exp, openli_export_recv_t *job) {

    int64_t more;
    size_t moresize = sizeof(more);
    int x;
    void *ptr;
    zmq_msg_t part;
    int next = 0;

    memset(job, 0, sizeof(openli_export_recv_t));
    job->type = OPENLI_EXPORT_IPIRI;

    do {
        ZMQ_READ_NEXT_PART
        switch(next) {
            case 0:
                assert(zmq_msg_size(&part) == sizeof(uint32_t));
                job->destid = *(uint32_t *)ptr;
                break;
            case 1:
                assert(zmq_msg_size(&part) == sizeof(uint8_t));
                job->data.ipiri.special = *(uint8_t *)ptr;
                break;
            case 2:
                assert(zmq_msg_size(&part) == sizeof(uint32_t));
                job->data.ipiri.cin = *(uint32_t *)ptr;
                break;
            case 3:
                assert(zmq_msg_size(&part) == sizeof(internet_access_method_t));
                job->data.ipiri.access_tech = *(internet_access_method_t *)ptr;
                break;
            case 4:
                assert(zmq_msg_size(&part) == sizeof(uint8_t));
                job->data.ipiri.ipassignmentmethod = *(uint8_t *)ptr;
                break;
            case 5:
                assert(zmq_msg_size(&part) == sizeof(int));
                job->data.ipiri.ipfamily = *(int *)ptr;
                break;
            case 6:
                assert(zmq_msg_size(&part) == sizeof(uint8_t));
                job->data.ipiri.assignedip_prefixbits = *(uint8_t *)ptr;
                break;
            case 7:
                if (job->data.ipiri.ipfamily == AF_INET) {
                    struct sockaddr_in *in;
                    in = (struct sockaddr_in *)&(job->data.ipiri.assignedip);
                    in->sin_addr.s_addr = *(uint32_t *)ptr;
                } else if (job->data.ipiri.ipfamily == AF_INET6) {
                    struct sockaddr_in6 *in6;
                    in6 = (struct sockaddr_in6 *)&(job->data.ipiri.assignedip);
                    memcpy(&(in6->sin6_addr.s6_addr), ptr,
                            sizeof(in6->sin6_addr.s6_addr));
                }
                break;
            case 8:
                job->data.ipiri.sessionstartts.tv_sec = *(time_t *)ptr;
                break;
            case 9:
                job->data.ipiri.sessionstartts.tv_usec = *(suseconds_t *)ptr;
                break;
            case 10:
                job->data.ipiri.liid = strdup((char *)ptr);
                break;
            case 11:
                job->data.ipiri.username = strdup((char *)ptr);
                break;
            /* TODO plugin data */
            default:
                assert(0);
        }

        next++;

        zmq_msg_close(&part);
    } while (more);

    return 1;
}

static int read_ipcc_job(collector_export_t *exp, openli_export_recv_t *job) {
    int x;
    void *ptr;
    size_t msglen = 0;
    zmq_msg_t part;
    IPCCJob *unpacked;

    memset(job, 0, sizeof(openli_export_recv_t));
    job->type = OPENLI_EXPORT_IPCC;
    zmq_msg_init(&part);
    x = zmq_msg_recv(&part, exp->zmq_subsock, ZMQ_DONTWAIT);
    if (x < 0) {
        if (errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    ptr = zmq_msg_data(&part);
    msglen = zmq_msg_size(&part);

    unpacked = ipccjob__unpack(NULL, msglen, (uint8_t *)ptr);
    if (unpacked == NULL) {
        logger(LOG_INFO, "OpenLI: Unable to unpack IPCC Job.");
        return -1;
    }

    zmq_msg_close(&part);

    job->destid = unpacked->destid;
    job->ts.tv_sec = unpacked->tvsec;
    job->ts.tv_usec = unpacked->tvusec;
    job->data.ipcc.cin = unpacked->cin;
    job->data.ipcc.dir = unpacked->dir;
    job->data.ipcc.liid = strdup(unpacked->liid);
    job->data.ipcc.ipcontent = (uint8_t *)malloc(unpacked->ipcontent.len);
    memcpy(job->data.ipcc.ipcontent, unpacked->ipcontent.data,
            unpacked->ipcontent.len);
    job->data.ipcc.ipclen = unpacked->ipcontent.len;

    ipccjob__free_unpacked(unpacked, NULL);
    return 1;

}

static int read_new_mediator_message(collector_export_t *exp,
        openli_mediator_t *med) {

    int64_t more;
    size_t moresize = sizeof(more);
    int x;
    void *ptr;
    zmq_msg_t part;
    int next = 0;

    do {
        zmq_msg_init(&part);
        x = zmq_msg_recv(&part, exp->zmq_subsock, ZMQ_DONTWAIT);
        if (x < 0) {
            if (errno == EAGAIN) {
                return 0;
            }
            return -1;
        }
        zmq_getsockopt(exp->zmq_subsock, ZMQ_RCVMORE, &more, &moresize);
        ptr = zmq_msg_data(&part);

        switch(next) {
            case 0:
                assert(zmq_msg_size(&part) == sizeof(uint32_t));
                med->mediatorid = *(uint32_t *)ptr;
                break;
            case 1:
                med->ipstr = strdup((char *)ptr);
                break;
            case 2:
                med->portstr = strdup((char *)ptr);
                break;
            default:
                assert(0);
        }

        next++;

        zmq_msg_close(&part);
    } while (more);

    return 1;
}

static int read_new_intercept_message(collector_export_t *exp,
        exporter_intercept_msg_t **cept) {

    int64_t more;
    size_t moresize = sizeof(more);
    int x;
    char *ptr;
    zmq_msg_t part;
    int next = 0, size;

    *cept = (exporter_intercept_msg_t *)calloc(1,
            sizeof(exporter_intercept_msg_t));

    do {
        zmq_msg_init(&part);
        x = zmq_msg_recv(&part, exp->zmq_subsock, ZMQ_DONTWAIT);
        if (x < 0) {
            if (errno == EAGAIN) {
                return 0;
            }
            return -1;
        }
        zmq_getsockopt(exp->zmq_subsock, ZMQ_RCVMORE, &more, &moresize);
        ptr = zmq_msg_data(&part);
        size = zmq_msg_size(&part);

        switch(next) {
            case 0:
                (*cept)->liid = strdup(ptr);
                (*cept)->liid_len = strlen(ptr);
                break;
            case 1:
                (*cept)->authcc = strdup(ptr);
                (*cept)->authcc_len = strlen(ptr);
                break;
            case 2:
                (*cept)->delivcc = strdup(ptr);
                (*cept)->delivcc_len = strlen(ptr);
                break;
            default:
                assert(0);
        }

        next++;

        zmq_msg_close(&part);
    } while (more);

    return 1;
}

static int handle_returned_result(collector_export_t *exp) {
    int x, ret = 0;
    openli_encoded_result_t res;

    x = zmq_recv(exp->zmq_pullressock, &res, sizeof(res), ZMQ_DONTWAIT);
    if (x < 0) {
        if (errno == EAGAIN) {
            return 0;
        }
        return -1;
    }

    if (export_encoded_record(exp, &res) < 0) {
        ret = -1;
    } else {
        ret = 1;
    }

    if (res.msgbody) {
        if (exp->freeresults == NULL) {
            exp->freeresults = res.msgbody;
            exp->freeresults->next = NULL;
        } else {
            res.msgbody->next = exp->freeresults;
            exp->freeresults = res.msgbody;
        }
    }

    if (res.ipcontents) {
        free(res.ipcontents);
    }
    return ret;
}

static int handle_published_message(collector_export_t *exp) {

    uint8_t msgtype;
    int64_t more;
    size_t moresize = sizeof(more);
    int x, ret;
    void *ptr;
    openli_mediator_t med;
    exporter_intercept_msg_t *cept = NULL;
    openli_export_recv_t job;

    zmq_msg_t part;

    x = zmq_recv(exp->zmq_subsock, &msgtype, 1, ZMQ_DONTWAIT);
    if (x < 0) {
        if (errno == EAGAIN) {
            return 0;
        }
        return -1;
    }

    //printf("%d got message of type %u\n", exp->glob->exportlabel, msgtype);
    ret = 0;

    switch(msgtype) {
        case OPENLI_EXPORT_MEDIATOR:
            ret = read_new_mediator_message(exp, &med);
            if (ret == 1) {
                ret = add_new_destination(exp, &med);
            }
            break;
        case OPENLI_EXPORT_DROP_SINGLE_MEDIATOR:
            ret = read_new_mediator_message(exp, &med);
            if (ret == 1) {
                remove_destination(exp, &med);
            }
            break;
        case OPENLI_EXPORT_INTERCEPT_DETAILS:
            ret = read_new_intercept_message(exp, &cept);
            if (ret == 1) {
                exporter_new_intercept(exp, cept);
            }
            break;
        case OPENLI_EXPORT_INTERCEPT_OVER:
            ret = read_new_intercept_message(exp, &cept);
            if (ret == 1) {
                ret = exporter_end_intercept(exp, cept);
            }
            break;
        case OPENLI_EXPORT_IPIRI:
            ret = read_ipiri_job(exp, &job);
            if (ret == 1) {
                ret = run_encoding_job(exp, &job);
            }
            break;
        case OPENLI_EXPORT_IPCC:
            ret = read_ipcc_job(exp, &job);
            if (ret == 1) {
                exp->count ++;
                if (run_encoding_job(exp, &job) < 0) {
                    ret = -1;
                }
            }
            break;
        default:
            do {
                zmq_msg_init(&part);
                x = zmq_msg_recv(&part, exp->zmq_subsock, ZMQ_DONTWAIT);
                if (x < 0) {
                    if (errno == EAGAIN) {
                        return 0;
                    }
                    return -1;
                }
                zmq_getsockopt(exp->zmq_subsock, ZMQ_RCVMORE, &more, &moresize);
                zmq_msg_close(&part);
            } while (more);
    }

    free_job_request(&job);
    return ret;
}

#if 0
static int read_exported_message(collector_export_t *exp) {
    int x, ret;
    char envelope[24];
	openli_export_recv_t *recvd = NULL;
    openli_exportmsg_t tosend;
    libtrace_list_node_t *n;
    export_dest_t *dest;

    /*
    x = zmq_recv(exp->zmq_subsock, envelope, 23, ZMQ_DONTWAIT);
    if (x < 0) {
        if (errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    envelope[x] = '\0';
    */
    x = zmq_recv(exp->zmq_subsock, (char *)(&recvd),
            sizeof(openli_export_recv_t *), ZMQ_DONTWAIT);
    if (x < 0) {
        if (errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    //printf("%d envelope=%s %d\n", exp->glob->exportlabel, envelope, recvd->type);

    switch(recvd->type) {
        case OPENLI_EXPORT_INTERCEPT_DETAILS:
            exporter_new_intercept(exp, recvd->data.cept);
            free(recvd);
            return 1;

        case OPENLI_EXPORT_INTERCEPT_OVER:
            ret = exporter_end_intercept(exp, recvd->data.cept);
            free(recvd);
            return ret;

        case OPENLI_EXPORT_MEDIATOR:
            ret = add_new_destination(exp, &(recvd->data.med));
            free(recvd);
            return ret;

        case OPENLI_EXPORT_DROP_SINGLE_MEDIATOR:
            remove_destination(exp, &(recvd->data.med));
            free(recvd);
            return 1;

        case OPENLI_EXPORT_DROP_ALL_MEDIATORS:
            logger(LOG_INFO,
                    "OpenLI exporter: dropping connections to all known mediators.");
            remove_all_destinations(exp);
            exp->dests = libtrace_list_init(sizeof(export_dest_t));
            free(recvd);
            return 1;

         case OPENLI_EXPORT_FLAG_MEDIATORS:
            n = exp->dests->head;
            while (n) {
                dest = (export_dest_t *)(n->data);
                dest->awaitingconfirm = 1;
                n = n->next;
            }

            exp->flagged = 1;
            free(recvd);
            return 1;

        case OPENLI_EXPORT_IPCC:
        case OPENLI_EXPORT_IPMMCC:
        case OPENLI_EXPORT_IPMMIRI:
        case OPENLI_EXPORT_IPIRI:
            exp->count ++;
            ret = 1;
            if (run_encoding_job(exp, recvd, &tosend) < 0) {
                ret = -1;
            }
            free_job_request(recvd);
            return ret;

        case OPENLI_EXPORT_PACKET_FIN:
            /* All ETSI records relating to this packet have been seen, so
             * we can safely free the packet.
             */
            trace_decrement_packet_refcount(recvd->data.packet);
            free(recvd);
            return 1;
    }

    logger(LOG_INFO,
            "OpenLI: invalid message type %d received from export queue.",
            recvd->type);
    free(recvd);
    return -1;
}

#endif

static inline int connect_zmq_sock(void *ctxt, void **sock, char *name,
        int socktype) {
    int rc;
    int zero = 0;
    *sock = zmq_socket(ctxt, socktype);
    rc = zmq_bind(*sock, name);

    if (rc != 0) {
        logger(LOG_INFO, "OpenLI: exporter thread was unable to start zmq socket");
        return -1;
    }
    rc = zmq_setsockopt(*sock, ZMQ_LINGER, &zero, sizeof(zero));
    if (rc != 0) {
        logger(LOG_INFO, "OpenLI: exporter thread was unable to set linger period for zeromq");
        return -1;
    }
    return 0;
}

int exporter_thread_main(collector_export_t *exp) {

	int i, nfds, timerfd, itemcount, ret;
    int timerexpired = 0;
    struct itimerspec its;
    zmq_pollitem_t items[4];

    /* XXX this could probably be static, but just to be safe... */

    if (exp->zmq_subsock == NULL) {
        if (connect_zmq_sock(exp->glob->zmq_ctxt, &(exp->zmq_subsock),
                "ipc:///tmp/openliipc", ZMQ_PULL) < 0) {
            return -1;
        }
    }

    if (exp->zmq_pushjobsock == NULL) {
        if (connect_zmq_sock(exp->glob->zmq_ctxt, &(exp->zmq_pushjobsock),
                "inproc://openliexporterpush", ZMQ_PUSH) < 0) {
            return -1;
        }
    }

    if (exp->zmq_pullressock == NULL) {
        if (connect_zmq_sock(exp->glob->zmq_ctxt, &(exp->zmq_pullressock),
                "inproc://openliexporterpull", ZMQ_PULL) < 0) {
            return -1;
        }
    }

    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = 1;
    its.it_value.tv_nsec = 0;

    timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
    timerfd_settime(timerfd, 0, &its, NULL);

    if (timerfd == -1) {
        logger(LOG_INFO, "OpenLI: failed to create export timer fd: %s.", strerror(errno));
        return -1;
    }

    /* Try to connect to any targets which we have buffered records for */
    connect_export_targets(exp);

    items[0].socket = exp->zmq_subsock;
    items[0].fd = 0;
    items[0].events = ZMQ_POLLIN;
    items[0].revents = 0;

    items[1].socket = NULL;
    items[1].fd = timerfd;
    items[1].events = ZMQ_POLLIN;
    items[1].revents = 0;

    items[2].socket = exp->zmq_pullressock;
    items[2].fd = 0;
    items[2].events = ZMQ_POLLIN;
    items[2].revents = 0;

    itemcount = 3;
    if (exp->flagtimerfd != -1) {
        items[3].socket = NULL;
        items[3].fd = exp->flagtimerfd;
        items[3].events = ZMQ_POLLIN;
        items[3].revents = 0;
        itemcount ++;
    }

    while (timerexpired == 0) {
        int processed = 0;
        zmq_poll(items, itemcount, -1);
        if (items[0].revents & ZMQ_POLLIN) {
            do {
                ret = handle_published_message(exp);
                processed ++;
            } while (ret > 0 && processed < 100);

            if (ret == -1) {
                return -1;
            }
        }

        if (items[1].revents & ZMQ_POLLIN) {
            timerexpired = 1;
        }

        if (items[2].revents & ZMQ_POLLIN) {
            /* TODO process encoded result and forward to destination */
            processed = 0;
            do {
                ret = handle_returned_result(exp);
                processed ++;
            } while (ret > 0 && processed < 100);
            if (ret == -1) {
                return -1;
            }
        }

        if (itemcount > 3 && items[3].revents & ZMQ_POLLIN) {
            purge_unconfirmed_mediators(exp);
            exp->flagged = 0;
            close(exp->flagtimerfd);
            exp->flagtimerfd = -1;
        }
    }

    close(timerfd);
    return 1;

}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
