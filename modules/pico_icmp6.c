/*********************************************************************
   PicoTCP. Copyright (c) 2012-2015 Altran Intelligent Systems. Some rights reserved.
   See LICENSE and COPYING for usage.

   .

   Authors: Kristof Roelants, Daniele Lacamera
 *********************************************************************/

#include "pico_config.h"
#include "pico_icmp6.h"
#include "pico_ipv6_nd.h"
#include "pico_eth.h"
#include "pico_device.h"
#include "pico_stack.h"
#include "pico_tree.h"
#include "pico_socket.h"
#include "pico_dev_sixlowpan.h"
#include "pico_mld.h"
#define icmp6_dbg(...) do { }while(0); 

static struct pico_queue icmp6_in;
static struct pico_queue icmp6_out;

uint16_t pico_icmp6_checksum(struct pico_frame *f)
{
    struct pico_ipv6_hdr *ipv6_hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    
    struct pico_icmp6_hdr *icmp6_hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    struct pico_ipv6_pseudo_hdr pseudo;
   
    pseudo.src = ipv6_hdr->src;
    pseudo.dst = ipv6_hdr->dst;
    pseudo.len = long_be(f->transport_len);
    pseudo.nxthdr = PICO_PROTO_ICMP6;

    pseudo.zero[0] = 0;
    pseudo.zero[1] = 0;
    pseudo.zero[2] = 0;

    return pico_dualbuffer_checksum(&pseudo, sizeof(struct pico_ipv6_pseudo_hdr), icmp6_hdr, f->transport_len);
}

#ifdef PICO_SUPPORT_PING
static void pico_icmp6_ping_recv_reply(struct pico_frame *f);
#endif

static int pico_icmp6_send_echoreply(struct pico_frame *echo)
{
    struct pico_frame *reply = NULL;
    struct pico_icmp6_hdr *ehdr = NULL, *rhdr = NULL;
    struct pico_ip6 src;
    struct pico_ip6 dst;

    reply = pico_proto_ipv6.alloc(&pico_proto_ipv6, (uint16_t)(echo->transport_len));
    if (!reply) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    echo->payload = echo->transport_hdr + PICO_ICMP6HDR_ECHO_REQUEST_SIZE;
    reply->payload = reply->transport_hdr + PICO_ICMP6HDR_ECHO_REQUEST_SIZE;
    reply->payload_len = echo->transport_len;
    reply->dev = echo->dev;

    ehdr = (struct pico_icmp6_hdr *)echo->transport_hdr;
    rhdr = (struct pico_icmp6_hdr *)reply->transport_hdr;
    rhdr->type = PICO_ICMP6_ECHO_REPLY;
    rhdr->code = 0;
    rhdr->msg.info.echo_reply.id = ehdr->msg.info.echo_reply.id;
    rhdr->msg.info.echo_reply.seq = ehdr->msg.info.echo_request.seq;
    memcpy(reply->payload, echo->payload, (uint32_t)(echo->transport_len - PICO_ICMP6HDR_ECHO_REQUEST_SIZE));
    rhdr->crc = 0;
    rhdr->crc = short_be(pico_icmp6_checksum(reply));
    /* Get destination and source swapped */
    memcpy(dst.addr, ((struct pico_ipv6_hdr *)echo->net_hdr)->src.addr, PICO_SIZE_IP6);
    memcpy(src.addr, ((struct pico_ipv6_hdr *)echo->net_hdr)->dst.addr, PICO_SIZE_IP6);
    pico_ipv6_frame_push(reply, &src, &dst, PICO_PROTO_ICMP6, 0);
    return 0;
}

static int pico_icmp6_process_in(struct pico_protocol *self, struct pico_frame *f)
{
    struct pico_icmp6_hdr *hdr = (struct pico_icmp6_hdr *)f->transport_hdr;

    IGNORE_PARAMETER(self);

    icmp6_dbg("Process IN, type = %d\n", hdr->type);

    switch (hdr->type)
    {
    case PICO_ICMP6_DEST_UNREACH:
        pico_ipv6_unreachable(f, hdr->code);
        break;

    case PICO_ICMP6_ECHO_REQUEST:
        icmp6_dbg("ICMP6: Received ECHO REQ\n");
        f->transport_len = (uint16_t)(f->len - f->net_len - (uint16_t)(f->net_hdr - f->buffer));
        pico_icmp6_send_echoreply(f);
        pico_frame_discard(f);
        break;

    case PICO_ICMP6_ECHO_REPLY:
#ifdef PICO_SUPPORT_PING
        pico_icmp6_ping_recv_reply(f);
#endif
        pico_frame_discard(f);
        break;
#if defined(PICO_SUPPORT_MCAST) && defined(PICO_SUPPORT_MLD)
    case PICO_MLD_QUERY:
    case PICO_MLD_REPORT:
    case PICO_MLD_DONE:
    case PICO_MLD_REPORTV2:
        pico_mld_process_in(f);
        break;        
#endif
    default:
        return pico_ipv6_nd_recv(f); /* CAUTION -- Implies: pico_frame_discard in any case, keep in the default! */
    }
    return -1;
}

static int pico_icmp6_process_out(struct pico_protocol *self, struct pico_frame *f)
{
    IGNORE_PARAMETER(self);
    IGNORE_PARAMETER(f);
    return 0;
}

/* Interface: protocol definition */
struct pico_protocol pico_proto_icmp6 = {
    .name = "icmp6",
    .proto_number = PICO_PROTO_ICMP6,
    .layer = PICO_LAYER_TRANSPORT,
    .process_in = pico_icmp6_process_in,
    .process_out = pico_icmp6_process_out,
    .q_in = &icmp6_in,
    .q_out = &icmp6_out,
};

static int pico_icmp6_notify(struct pico_frame *f, uint8_t type, uint8_t code, uint32_t ptr)
{
    struct pico_frame *notice = NULL;
    struct pico_ipv6_hdr *ipv6_hdr = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    uint16_t len = 0;

    if (!f)
        return -1;

    ipv6_hdr = (struct pico_ipv6_hdr *)(f->net_hdr);
    len = (uint16_t)(short_be(ipv6_hdr->len) + PICO_SIZE_IP6HDR);
    switch (type)
    {
    case PICO_ICMP6_DEST_UNREACH:
        /* as much of invoking packet as possible without exceeding the minimum IPv6 MTU */
        if (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_DEST_UNREACH_SIZE + len > PICO_IPV6_MIN_MTU)
            len = PICO_IPV6_MIN_MTU - (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_DEST_UNREACH_SIZE);

        notice = pico_proto_ipv6.alloc(&pico_proto_ipv6, (uint16_t)(PICO_ICMP6HDR_DEST_UNREACH_SIZE + len));
        if (!notice) {
            pico_err = PICO_ERR_ENOMEM;
            return -1;
        }

        notice->payload = notice->transport_hdr + PICO_ICMP6HDR_DEST_UNREACH_SIZE;
        notice->payload_len = len;
        icmp6_hdr = (struct pico_icmp6_hdr *)notice->transport_hdr;
        icmp6_hdr->msg.err.dest_unreach.unused = 0;
        break;

    case PICO_ICMP6_TIME_EXCEEDED:
        /* as much of invoking packet as possible without exceeding the minimum IPv6 MTU */
        if (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_TIME_XCEEDED_SIZE + len > PICO_IPV6_MIN_MTU)
            len = PICO_IPV6_MIN_MTU - (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_TIME_XCEEDED_SIZE);

        notice = pico_proto_ipv6.alloc(&pico_proto_ipv6, (uint16_t)(PICO_ICMP6HDR_TIME_XCEEDED_SIZE + len));
        if (!notice) {
            pico_err = PICO_ERR_ENOMEM;
            return -1;
        }

        notice->payload = notice->transport_hdr + PICO_ICMP6HDR_TIME_XCEEDED_SIZE;
        notice->payload_len = len;
        icmp6_hdr = (struct pico_icmp6_hdr *)notice->transport_hdr;
        icmp6_hdr->msg.err.time_exceeded.unused = 0;
        break;

    case PICO_ICMP6_PARAM_PROBLEM:
        if (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_PARAM_PROBLEM_SIZE + len > PICO_IPV6_MIN_MTU)
            len = PICO_IPV6_MIN_MTU - (PICO_SIZE_IP6HDR + PICO_ICMP6HDR_PARAM_PROBLEM_SIZE);

        notice = pico_proto_ipv6.alloc(&pico_proto_ipv6, (uint16_t)(PICO_ICMP6HDR_PARAM_PROBLEM_SIZE + len));
        if (!notice) {
            pico_err = PICO_ERR_ENOMEM;
            return -1;
        }

        notice->payload = notice->transport_hdr + PICO_ICMP6HDR_PARAM_PROBLEM_SIZE;
        notice->payload_len = len;
        icmp6_hdr = (struct pico_icmp6_hdr *)notice->transport_hdr;
        icmp6_hdr->msg.err.param_problem.ptr = long_be(ptr);
        break;

    default:
        return -1;
    }

    icmp6_hdr->type = type;
    icmp6_hdr->code = code;
    memcpy(notice->payload, f->net_hdr, notice->payload_len);
    notice->dev = f->dev;
    /* f->src is set in frame_push, checksum calculated there */
    pico_ipv6_frame_push(notice, NULL, &ipv6_hdr->src, PICO_PROTO_ICMP6, 0);
    return 0;
}

int pico_icmp6_port_unreachable(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    if (pico_ipv6_is_multicast(hdr->dst.addr))
        return 0;

    return pico_icmp6_notify(f, PICO_ICMP6_DEST_UNREACH, PICO_ICMP6_UNREACH_PORT, 0);
}

int pico_icmp6_proto_unreachable(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    if (pico_ipv6_is_multicast(hdr->dst.addr))
        return 0;

    return pico_icmp6_notify(f, PICO_ICMP6_DEST_UNREACH, PICO_ICMP6_UNREACH_ADDR, 0);
}

int pico_icmp6_dest_unreachable(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    if (pico_ipv6_is_multicast(hdr->dst.addr))
        return 0;

    return pico_icmp6_notify(f, PICO_ICMP6_DEST_UNREACH, PICO_ICMP6_UNREACH_ADDR, 0);
}

int pico_icmp6_ttl_expired(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    if (pico_ipv6_is_multicast(hdr->dst.addr))
        return 0;

    return pico_icmp6_notify(f, PICO_ICMP6_TIME_EXCEEDED, PICO_ICMP6_TIMXCEED_INTRANS, 0);
}

int pico_icmp6_pkt_too_big(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    if (pico_ipv6_is_multicast(hdr->dst.addr))
        return 0;

    return pico_icmp6_notify(f, PICO_ICMP6_PKT_TOO_BIG, 0, 0);
}

#ifdef PICO_SUPPORT_IPFILTER
int pico_icmp6_packet_filtered(struct pico_frame *f)
{
    return pico_icmp6_notify(f, PICO_ICMP6_DEST_UNREACH, PICO_ICMP6_UNREACH_ADMIN, 0);
}
#endif

int pico_icmp6_parameter_problem(struct pico_frame *f, uint8_t problem, uint32_t ptr)
{
    return pico_icmp6_notify(f, PICO_ICMP6_PARAM_PROBLEM, problem, ptr);
}

MOCKABLE int pico_icmp6_frag_expired(struct pico_frame *f)
{
    struct pico_ipv6_hdr *hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    if (pico_ipv6_is_multicast(hdr->dst.addr))
        return 0;

    return pico_icmp6_notify(f, PICO_ICMP6_TIME_EXCEEDED, PICO_ICMP6_TIMXCEED_REASS, 0);
}

static int pico_icmp6_provide_llao(struct pico_icmp6_opt_lladdr *llao, uint8_t type, struct pico_device *dev, struct pico_ip6 *src)
{
#ifdef PICO_SUPPORT_SIXLOWPAN
    struct pico_ieee_addr *ieee = (struct pico_ieee_addr *)dev->eth;
    uint16_t shortbe = 0;
#endif
    llao->type = type;
    
    if (LL_MODE_ETHERNET == dev->mode && dev->eth) {
        memcpy(llao->addr.mac.addr, dev->eth->mac.addr, PICO_SIZE_ETH);
        llao->len = 1;
    }
#ifdef PICO_SUPPORT_SIXLOWPAN
    else if (LL_MODE_SIXLOWPAN == dev->mode && dev->eth) {
        if (src && pico_sixlowpan_iid_is_derived_16(src->addr + 8)) {
            shortbe = short_be(ieee->_short.addr);
            memcpy(&llao->addr._short.addr, &shortbe, PICO_SIZE_IEEE_EXT);
            memset(llao->addr._ext.addr + PICO_SIZE_IEEE_SHORT, 0x00, 4);
            llao->len = 1;
        } else {
            memcpy(llao->addr._ext.addr, ieee->_ext.addr, PICO_SIZE_IEEE_EXT);
            memset(llao->addr._ext.addr + PICO_SIZE_IEEE_EXT, 0x00, 6);
            llao->len = 2;
        }
    }
#endif /* PICO_SUPPORT_SIXLOWPAN */
    else {
        return -1;
    }
    
    return 0;
}

#ifdef PICO_SUPPORT_SIXLOWPAN
static void pico_icmp6_provide_aro(struct pico_icmp6_opt_aro *aro, struct pico_device *dev)
{
    struct pico_ieee_addr *ieee = (struct pico_ieee_addr *)dev->eth;
    aro->type = PICO_ND_OPT_ARO;
    aro->len = 2;
    aro->status = 0;
    aro->lifetime = short_be(PICO_6LP_ND_DEFAULT_LIFETIME);
    memcpy(aro->eui64.addr, ieee->_ext.addr, PICO_SIZE_IEEE_EXT);
}
#endif

static inline uint8_t pico_icmp6_6lp_calc_llao_len(struct pico_ip6 *dst)
{
    /* Destination address is address you want to sent a neighbor solicitation for,
     * for 6LoWPAN, the LLAO-length depends if its derived from EUI-64 or 16-bit short */
    if (pico_sixlowpan_iid_is_derived_16(dst->addr + 8))
        return PICO_6LP_ND_LLAO_LEN_SHORT;
    else
        return PICO_6LP_ND_LLAO_LEN_EXTENDED;
}

/* RFC 4861 $7.2.2: sending neighbor solicitations */
int pico_icmp6_neighbor_solicitation(struct pico_device *dev, struct pico_ip6 *dst, uint8_t type)
{
    struct pico_ip6 daddr = {{ 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00 }};
    struct pico_icmp6_opt_lladdr *llao = NULL;
    struct pico_icmp6_opt_aro *aro = NULL;
    struct pico_icmp6_hdr *icmp = NULL;
    struct pico_ipv6_route *gw = NULL;
    struct pico_ipv6_link *ll = NULL;
    struct pico_frame *sol = NULL;
    uint8_t i = 0, llao_len = 0;
    uint16_t len = 0;
    
    if (LL_MODE_SIXLOWPAN == dev->mode && (dev->hostvars.routing))
        return -1;

    if (pico_ipv6_is_multicast(dst->addr))
        return -1;

    /* Determine the size frame needs to be for the Neighbor Solicitation */
    len = PICO_ICMP6HDR_NEIGH_SOL_SIZE;
    if (LL_MODE_ETHERNET == dev->mode && PICO_ICMP6_ND_DAD != type)
        len = (uint16_t)(len + 8);
#ifdef PICO_SUPPORT_SIXLOWPAN
    else if (LL_MODE_SIXLOWPAN == dev->mode && PICO_ICMP6_ND_DAD == type) {
        /* Calculate in the SLLAO-length and ARO-length */
        llao_len = pico_icmp6_6lp_calc_llao_len(dst);
        len = (uint16_t)(len + sizeof(struct pico_icmp6_opt_aro));
        len = (uint16_t)(len + llao_len);
    }
#endif

    /* Create pico_frame to contain the Neighbor Solicitation */
    sol = pico_proto_ipv6.alloc(&pico_proto_ipv6, len);
    if (!sol) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    sol->payload = sol->transport_hdr + len;
    sol->payload_len = 0;

    icmp = (struct pico_icmp6_hdr *)sol->transport_hdr;
    icmp->type = PICO_ICMP6_NEIGH_SOL;
    icmp->code = 0;
    icmp->msg.info.neigh_sol.unused = 0;
    icmp->msg.info.neigh_sol.target = *dst;
    
    llao = (struct pico_icmp6_opt_lladdr *)(((uint8_t *)&icmp->msg.info.neigh_sol) + sizeof(struct neigh_sol_s));
    aro = (struct pico_icmp6_opt_aro *)(((uint8_t *)&icmp->msg.info.neigh_sol) + sizeof(struct neigh_sol_s) + llao_len);
    if (LL_MODE_ETHERNET == dev->mode && type != PICO_ICMP6_ND_DAD) {
        pico_icmp6_provide_llao(llao, PICO_ND_OPT_LLADDR_SRC, dev, NULL);
    }
#ifdef PICO_SUPPORT_SIXLOWPAN
    else if (LL_MODE_SIXLOWPAN == dev->mode && type == PICO_ICMP6_ND_DAD) {
        pico_icmp6_provide_llao(llao, PICO_ND_OPT_LLADDR_SRC, dev, dst);
        pico_icmp6_provide_aro(aro, dev);
    }
#endif
    
    if (LL_MODE_ETHERNET == dev->mode && (type == PICO_ICMP6_ND_SOLICITED || type == PICO_ICMP6_ND_DAD)) {
        for (i = 1; i <= 3; ++i) {
            daddr.addr[PICO_SIZE_IP6 - i] = dst->addr[PICO_SIZE_IP6 - i];
        }
    }
#ifdef PICO_SUPPORT_SIXLOWPAN
    else if (LL_MODE_SIXLOWPAN == dev->mode) {
        gw = pico_ipv6_default_gateway_configured(dev);
        if (gw) {
            /* Set the destination address to the default gateway */
            daddr = gw->gateway;
        } else {
            /* No gateway, no party */
            pico_frame_discard(sol);
            return -1;
        }
    }
#endif
    else {
        daddr = *dst;
    }
    
    sol->dev = dev;
    
    if (LL_MODE_ETHERNET == dev->mode) {
        /* f->src is set in frame_push, checksum calculated there */
        pico_ipv6_frame_push(sol, NULL, &daddr, PICO_PROTO_ICMP6, (type == PICO_ICMP6_ND_DAD));
    }
#ifdef PICO_SUPPORT_SIXLOWPAN
    else if (LL_MODE_SIXLOWPAN == dev->mode) {
        /* Force frame to be sent with source address to be registered */
        ll = pico_ipv6_linklocal_get(dev);
        pico_ipv6_frame_push(sol, &ll->address, &daddr, PICO_PROTO_ICMP6, (type == PICO_ICMP6_ND_DAD));
    }
#endif
    else {
        pico_frame_discard(sol);
        return -1;
    }
    
    return 0;
}

/* RFC 4861 $7.2.4: sending solicited neighbor advertisements */
int pico_icmp6_neighbor_advertisement(struct pico_frame *f, struct pico_ip6 *target)
{
    struct pico_frame *adv = NULL;
    struct pico_ipv6_hdr *ipv6_hdr = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_icmp6_opt_lladdr *opt = NULL;
    struct pico_ip6 dst = {{0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

    ipv6_hdr = (struct pico_ipv6_hdr *)f->net_hdr;
    adv = pico_proto_ipv6.alloc(&pico_proto_ipv6, PICO_ICMP6HDR_NEIGH_ADV_SIZE + 8);
    if (!adv) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    adv->payload = adv->transport_hdr + PICO_ICMP6HDR_NEIGH_ADV_SIZE + 8;
    adv->payload_len = 0;

    icmp6_hdr = (struct pico_icmp6_hdr *)adv->transport_hdr;
    icmp6_hdr->type = PICO_ICMP6_NEIGH_ADV;
    icmp6_hdr->code = 0;
    icmp6_hdr->msg.info.neigh_adv.target = *target;
    icmp6_hdr->msg.info.neigh_adv.rsor = long_be(0x60000000); /* !router && solicited && override */
    if (pico_ipv6_is_unspecified(ipv6_hdr->src.addr)) {
        /* solicited = clear && dst = all-nodes address (scope link-local) */
        icmp6_hdr->msg.info.neigh_adv.rsor ^= long_be(0x40000000);
    } else {
        /* solicited = set && dst = source of solicitation */
        dst = ipv6_hdr->src;
    }

    /* XXX if the target address is either an anycast address or a unicast
     * address for which the node is providing proxy service, or the target
     * link-layer Address option is not included, the Override flag SHOULD
     * be set to zero.
     */

    /* XXX if the target address is an anycast address, the sender SHOULD delay
     * sending a response for a random time between 0 and MAX_ANYCAST_DELAY_TIME seconds.
     */

    opt = (struct pico_icmp6_opt_lladdr *)(((uint8_t *)&icmp6_hdr->msg.info.neigh_adv) + sizeof(struct neigh_adv_s));
    opt->type = PICO_ND_OPT_LLADDR_TGT;
    opt->len = 1;
    memcpy(opt->addr.mac.addr, f->dev->eth->mac.addr, PICO_SIZE_ETH);
    adv->dev = f->dev;

    /* f->src is set in frame_push, checksum calculated there */
    pico_ipv6_frame_push(adv, NULL, &dst, PICO_PROTO_ICMP6, 0);
    return 0;
}

/* RFC 4861 $6.3.7: sending router solicitations */
int pico_icmp6_router_solicitation(struct pico_device *dev, struct pico_ip6 *src)
{
    struct pico_ip6 daddr = {{ 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 }};
    struct pico_icmp6_opt_lladdr *lladdr = NULL;
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_frame *sol = NULL;
    uint16_t len = 0;
    
    len = PICO_ICMP6HDR_ROUTER_SOL_SIZE;
    if (!pico_ipv6_is_unspecified(src->addr)) {
        len = (uint16_t)(len + 8);
#ifdef PICO_SUPPORT_SIXLOWPAN
        /* Always send router sollicitations for 6LoWPAN with Extended link-layer address (EUI-64) */
        if (LL_MODE_SIXLOWPAN == dev->mode) {
            len = (uint16_t)(len + 8);
        }
#endif /* PICO_SUPPORT_SIXLOWPAN */
    }

    sol = pico_proto_ipv6.alloc(&pico_proto_ipv6, len);
    if (!sol) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    sol->payload = sol->transport_hdr + len;
    sol->payload_len = 0;

    icmp6_hdr = (struct pico_icmp6_hdr *)sol->transport_hdr;
    icmp6_hdr->type = PICO_ICMP6_ROUTER_SOL;
    icmp6_hdr->code = 0;

    if (!pico_ipv6_is_unspecified(src->addr)) {
        lladdr = (struct pico_icmp6_opt_lladdr *)((uint8_t *)&icmp6_hdr->msg.info.router_sol + sizeof(struct router_sol_s));
        if (pico_icmp6_provide_llao(lladdr, PICO_ND_OPT_LLADDR_SRC, dev, NULL)) {
            pico_frame_discard(sol);
            return -1;
        }
    }

    sol->dev = dev;

    /* f->src is set in frame_push, checksum calculated there */
    if (LL_MODE_ETHERNET == dev->mode) {
        pico_ipv6_frame_push(sol, NULL, &daddr, PICO_PROTO_ICMP6, 0);
    }
#ifdef PICO_SUPPORT_SIXLOWPAN
    /* Force this frame to be send with the EUI-64-address */
    else if (LL_MODE_SIXLOWPAN == dev->mode) {
        pico_ipv6_frame_push(sol, src, &daddr, PICO_PROTO_ICMP6, 0);
    }
#endif /* PICO_SUPPORT_SIXLOWPAN */
    else {
        pico_frame_discard(sol);
        return -1;
    }
    
    return 0;
}

#define PICO_RADV_VAL_LIFETIME (long_be(86400))
#define PICO_RADV_PREF_LIFETIME (long_be(14400))

static struct pico_ip6 pico_icmp6_address_to_prefix(struct pico_ip6 addr, struct pico_ip6 nm)
{
    struct pico_ip6 prefix;
    uint8_t i = 0;
    
    for (i = 0; i < PICO_SIZE_IP6; i++) {
        prefix.addr[i] = (uint8_t)(addr.addr[i] & nm.addr[i]);
    }
    
    return prefix;
}

/* RFC 4861: sending router advertisements */
int pico_icmp6_router_advertisement(struct pico_device *dev, struct pico_ip6 *dst)
{
    struct pico_frame *adv = NULL;
    struct pico_ip6 prefix_addr = {{ 0x00 }};
    struct pico_icmp6_hdr *icmp6_hdr = NULL;
    struct pico_icmp6_opt_lladdr *lladdr;
    struct pico_icmp6_opt_prefix *prefix;
    struct pico_ipv6_link *global = NULL;
    uint16_t len = 0;
    struct pico_ip6 dst_mcast = {{ 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }};
    uint8_t *nxt_opt;
#ifdef PICO_SUPPORT_SIXLOWPAN
    struct pico_ieee_addr *slp = NULL;
#endif

    len = PICO_ICMP6HDR_ROUTER_ADV_SIZE + PICO_ICMP6_OPT_LLADDR_SIZE + sizeof(struct pico_icmp6_opt_prefix);

#ifdef PICO_SUPPORT_SIXLOWPAN
    /* Always send router sollicitations for 6LoWPAN with Extended link-layer address (EUI-64) */
    if (LL_MODE_SIXLOWPAN == dev->mode) {
        len = (uint16_t)(len + 8);
    }
#endif /* PICO_SUPPORT_SIXLOWPAN */
    
    adv = pico_proto_ipv6.alloc(&pico_proto_ipv6, len);
    if (!adv) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    adv->payload = adv->transport_hdr + len;
    adv->payload_len = 0;
    adv->dev = dev;

    icmp6_hdr = (struct pico_icmp6_hdr *)adv->transport_hdr;
    icmp6_hdr->type = PICO_ICMP6_ROUTER_ADV;
    icmp6_hdr->code = 0;
    icmp6_hdr->msg.info.router_adv.life_time = short_be(45);
    icmp6_hdr->msg.info.router_adv.hop = 64;
    nxt_opt = (uint8_t *)&icmp6_hdr->msg.info.router_adv + sizeof(struct router_adv_s);

    prefix =  (struct pico_icmp6_opt_prefix *)nxt_opt;
    prefix->type = PICO_ND_OPT_PREFIX;
    prefix->len = sizeof(struct pico_icmp6_opt_prefix) >> 3;
    prefix->prefix_len = 64; /* Only /64 are forwarded */
    prefix->aac = 1;
    prefix->onlink = 1;
    prefix->val_lifetime = PICO_RADV_VAL_LIFETIME;
    prefix->pref_lifetime = PICO_RADV_PREF_LIFETIME;
    if (dev->mode != LL_MODE_SIXLOWPAN) {
        memcpy(&prefix->prefix, dst, sizeof(struct pico_ip6));
    } else {
        /* Find the globally routable prefix of the router-interface */
        if ((global = pico_ipv6_global_get(dev))) {
            prefix_addr = pico_icmp6_address_to_prefix(global->address, global->netmask);
            memcpy(&prefix->prefix, &prefix_addr, sizeof(struct pico_ip6));
        }
    }

    nxt_opt += (sizeof (struct pico_icmp6_opt_prefix));
    lladdr = (struct pico_icmp6_opt_lladdr *)nxt_opt;
    
    
    lladdr->type = PICO_ND_OPT_LLADDR_SRC;
    
    if (!dev->mode && dev->eth) {
        lladdr->len = 1;
        memcpy(lladdr->addr.mac.addr, dev->eth->mac.addr, PICO_SIZE_ETH);
    }
#ifdef PICO_SUPPORT_SIXLOWPAN
    else if (LL_MODE_SIXLOWPAN == dev->mode) {
        slp = (struct pico_ieee_addr *)dev->eth;
        lladdr->len = 2;
        memcpy(lladdr->addr._ext.addr, slp->_ext.addr, PICO_SIZE_IEEE_EXT);
        memset(lladdr->addr._ext.addr + PICO_SIZE_IEEE_EXT, 0x00, 6);
    }
#endif /* PICO_SUPPORT_SIXLOWPAN */
    else {
        
        return -1;
    }
    
    icmp6_hdr->crc = 0;
    icmp6_hdr->crc = short_be(pico_icmp6_checksum(adv));
    /* f->src is set in frame_push, checksum calculated there */
    if (dev->mode != LL_MODE_SIXLOWPAN)
        pico_ipv6_frame_push(adv, NULL, &dst_mcast, PICO_PROTO_ICMP6, 0);
    else
        pico_ipv6_frame_push(adv, NULL, dst, PICO_PROTO_ICMP6, 0);
    return 0;
}

/***********************/
/* Ping implementation */
/***********************/

#ifdef PICO_SUPPORT_PING
struct pico_icmp6_ping_cookie
{
    uint16_t id;
    uint16_t seq;
    uint16_t size;
    uint16_t err;
    int count;
    int interval;
    int timeout;
    pico_time timestamp;
    struct pico_ip6 dst;
    struct pico_device *dev;
    void (*cb)(struct pico_icmp6_stats*);
};

static int icmp6_cookie_compare(void *ka, void *kb)
{
    struct pico_icmp6_ping_cookie *a = ka, *b = kb;
    if (a->id < b->id)
        return -1;

    if (a->id > b->id)
        return 1;

    return (a->seq - b->seq);
}
PICO_TREE_DECLARE(IPV6Pings, icmp6_cookie_compare);

static int pico_icmp6_send_echo(struct pico_icmp6_ping_cookie *cookie)
{
    struct pico_frame *echo = NULL;
    struct pico_icmp6_hdr *hdr = NULL;

    echo = pico_proto_ipv6.alloc(&pico_proto_ipv6, (uint16_t)(PICO_ICMP6HDR_ECHO_REQUEST_SIZE + cookie->size));
    if (!echo) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    echo->payload = echo->transport_hdr + PICO_ICMP6HDR_ECHO_REQUEST_SIZE;
    echo->payload_len = cookie->size;

    hdr = (struct pico_icmp6_hdr *)echo->transport_hdr;
    hdr->type = PICO_ICMP6_ECHO_REQUEST;
    hdr->code = 0;
    hdr->msg.info.echo_request.id = short_be(cookie->id);
    hdr->msg.info.echo_request.seq = short_be(cookie->seq);
    /* XXX: Fill payload */
    hdr->crc = 0;
    hdr->crc = short_be(pico_icmp6_checksum(echo));
    echo->dev = cookie->dev;
    pico_ipv6_frame_push(echo, NULL, &cookie->dst, PICO_PROTO_ICMP6, 0);
    return 0;
}


static void pico_icmp6_ping_timeout(pico_time now, void *arg)
{
    struct pico_icmp6_ping_cookie *cookie = NULL;

    IGNORE_PARAMETER(now);

    cookie = (struct pico_icmp6_ping_cookie *)arg;
    if (pico_tree_findKey(&IPV6Pings, cookie)) {
        if (cookie->err == PICO_PING6_ERR_PENDING) {
            struct pico_icmp6_stats stats = {
                0
            };
            stats.dst = cookie->dst;
            stats.seq = cookie->seq;
            stats.time = 0;
            stats.size = cookie->size;
            stats.err = PICO_PING6_ERR_TIMEOUT;
            dbg(" ---- Ping6 timeout!!!\n");
            if (cookie->cb)
                cookie->cb(&stats);
        }

        pico_tree_delete(&IPV6Pings, cookie);
        PICO_FREE(cookie);
    }
}

static void pico_icmp6_next_ping(pico_time now, void *arg);
static inline void pico_icmp6_send_ping(struct pico_icmp6_ping_cookie *cookie)
{
    pico_icmp6_send_echo(cookie);
    cookie->timestamp = pico_tick;
    pico_timer_add((pico_time)(cookie->interval), pico_icmp6_next_ping, cookie);
    pico_timer_add((pico_time)(cookie->timeout), pico_icmp6_ping_timeout, cookie);
}

static void pico_icmp6_next_ping(pico_time now, void *arg)
{
    struct pico_icmp6_ping_cookie *cookie = NULL, *new = NULL;

    IGNORE_PARAMETER(now);

    cookie = (struct pico_icmp6_ping_cookie *)arg;
    if (pico_tree_findKey(&IPV6Pings, cookie)) {
        if (cookie->err == PICO_PING6_ERR_ABORTED)
            return;

        if (cookie->seq < (uint16_t)cookie->count) {
            new = PICO_ZALLOC(sizeof(struct pico_icmp6_ping_cookie));
            if (!new) {
                pico_err = PICO_ERR_ENOMEM;
                return;
            }

            memcpy(new, cookie, sizeof(struct pico_icmp6_ping_cookie));
            new->seq++;

            pico_tree_insert(&IPV6Pings, new);
            pico_icmp6_send_ping(new);
        }
    }
}

static void pico_icmp6_ping_recv_reply(struct pico_frame *f)
{
    struct pico_icmp6_ping_cookie *cookie = NULL, test = {
        0
    };
    struct pico_icmp6_hdr *hdr = NULL;

    hdr = (struct pico_icmp6_hdr *)f->transport_hdr;
    test.id  = short_be(hdr->msg.info.echo_reply.id);
    test.seq = short_be(hdr->msg.info.echo_reply.seq);
    cookie = pico_tree_findKey(&IPV6Pings, &test);
    if (cookie) {
        struct pico_icmp6_stats stats = {
            0
        };
        if (cookie->err == PICO_PING6_ERR_ABORTED)
            return;

        cookie->err = PICO_PING6_ERR_REPLIED;
        stats.dst = cookie->dst;
        stats.seq = cookie->seq;
        stats.size = cookie->size;
        stats.time = pico_tick - cookie->timestamp;
        stats.err = cookie->err;
        stats.ttl = ((struct pico_ipv6_hdr *)f->net_hdr)->hop;
        if(cookie->cb)
            cookie->cb(&stats);
    } else {
        dbg("Reply for seq=%d, not found.\n", test.seq);
    }
}

int pico_icmp6_ping(char *dst, int count, int interval, int timeout, int size, void (*cb)(struct pico_icmp6_stats *), struct pico_device *dev)
{
    static uint16_t next_id = 0x91c0;
    struct pico_icmp6_ping_cookie *cookie = NULL;

    if(!dst || !count || !interval || !timeout) {
        pico_err = PICO_ERR_EINVAL;
        return -1;
    }

    cookie = PICO_ZALLOC(sizeof(struct pico_icmp6_ping_cookie));
    if (!cookie) {
        pico_err = PICO_ERR_ENOMEM;
        return -1;
    }

    if (pico_string_to_ipv6(dst, cookie->dst.addr) < 0) {
        pico_err = PICO_ERR_EINVAL;
        PICO_FREE(cookie);
        return -1;
    }

    cookie->seq = 1;
    cookie->id = next_id++;
    cookie->err = PICO_PING6_ERR_PENDING;
    cookie->size = (uint16_t)size;
    cookie->interval = interval;
    cookie->timeout = timeout;
    cookie->cb = cb;
    cookie->count = count;
    cookie->dev = dev;

    pico_tree_insert(&IPV6Pings, cookie);
    pico_icmp6_send_ping(cookie);
    return (int)cookie->id;
}

int pico_icmp6_ping_abort(int id)
{
    struct pico_tree_node *node;
    int found = 0;
    pico_tree_foreach(node, &IPV6Pings)
    {
        struct pico_icmp6_ping_cookie *ck =
            (struct pico_icmp6_ping_cookie *) node->keyValue;
        if (ck->id == (uint16_t)id) {
            ck->err = PICO_PING6_ERR_ABORTED;
            found++;
        }
    }
    if (found > 0)
        return 0; /* OK if at least one pending ping has been canceled */

    pico_err = PICO_ERR_ENOENT;
    return -1;
}

#endif
