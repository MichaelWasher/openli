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
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#ifndef OPENLI_NETCOMMS_H_
#define OPENLI_NETCOMMS_H_

#include <inttypes.h>

#define NETBUF_ALLOC_SIZE (4096)

#define OPENLI_PROTO_MAGIC 0x5c4c6c5c
#define OPENLI_COLLECTOR_MAGIC 0x00180014202042a8
#define OPENLI_MEDIATOR_MAGIC 0x01153200d6f12905

#define NETBUF_SPACE_REM(nbuf) (nbuf->alloced - (nbuf->appendptr - nbuf->buf))
#define NETBUF_FRONT_FREE(nbuf) (nbuf->actptr - nbuf->buf)
#define NETBUF_CONTENT_SIZE(nbuf) (nbuf->appendptr - nbuf->actptr)

#include "intercept.h"
#include "mediator.h"
#include "agency.h"

typedef enum {
    NETBUF_RECV,
    NETBUF_SEND,
} net_buffer_type_t;

typedef enum {
    OPENLI_PROTO_DISCONNECT,
    OPENLI_PROTO_NO_MESSAGE,
    OPENLI_PROTO_START_IPINTERCEPT,
    OPENLI_PROTO_HALT_IPINTERCEPT,
    OPENLI_PROTO_ANNOUNCE_MEDIATOR,
    OPENLI_PROTO_ANNOUNCE_LEA,
    OPENLI_PROTO_MEDIATE_INTERCEPT,
    OPENLI_PROTO_COLLECTOR_AUTH,
    OPENLI_PROTO_MEDIATOR_AUTH,
} openli_proto_msgtype_t;

typedef struct net_buffer {
    int fd;
    char *buf;
    char *appendptr;
    char *actptr;
    int alloced;
    net_buffer_type_t buftype;
} net_buffer_t;


net_buffer_t *create_net_buffer(net_buffer_type_t buftype, int fd);
void destroy_net_buffer(net_buffer_t *nb);

int push_mediator_onto_net_buffer(net_buffer_t *nb, openli_mediator_t *med);
int push_intercept_onto_net_buffer(net_buffer_t *nb, ipintercept_t *ipint);
int push_lea_onto_net_buffer(net_buffer_t *nb, liagency_t *ipint);
int push_intercept_dest_onto_net_buffer(net_buffer_t *nb, char *liid,
        char *agencyid);
int push_auth_onto_net_buffer(net_buffer_t *nb, openli_proto_msgtype_t
        authtype);
int transmit_net_buffer(net_buffer_t *nb);

openli_proto_msgtype_t receive_net_buffer(net_buffer_t *nb, uint8_t **msgbody,
        uint16_t *msglen, uint64_t *intid);



#endif

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
