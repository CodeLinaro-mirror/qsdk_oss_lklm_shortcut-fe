/*
 * sfe_ipv6_tunipip6.c
 *	Shortcut forwarding engine file for IPv6 TUNIPIP6
 *
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/version.h>
#include <net/ip6_checksum.h>
#include <net/protocol.h>

#include "sfe_debug.h"
#include "sfe_api.h"
#include "sfe.h"
#include "sfe_flow_cookie.h"
#include "sfe_ipv6.h"

/*
 * sfe_ipv6_recv_tunipip6()
 *	Handle TUNIPIP6 packet receives and forwarding.
 */
int sfe_ipv6_recv_tunipip6(struct sfe_ipv6 *si, struct sk_buff *skb, struct net_device *dev,
			     unsigned int len, struct ipv6hdr *iph, unsigned int ihl,
				 bool sync_on_find, struct sfe_l2_info *l2_info, bool tun_outer)
{
	struct sfe_ipv6_addr *src_ip;
	struct sfe_ipv6_addr *dest_ip;
	__be16 src_port = 0;
	__be16 dest_port = 0;
	struct sfe_ipv6_connection_match *cm;

	DEBUG_TRACE("%px: sfe: sfe_ipv6_recv_tunipip6 called.\n", skb);

	/*
	 * Read the IP address information.  Read the IP header data first
	 * because we've almost certainly got that in the cache.
	 */
	src_ip = (struct sfe_ipv6_addr *)iph->saddr.s6_addr32;
	dest_ip = (struct sfe_ipv6_addr *)iph->daddr.s6_addr32;

	rcu_read_lock();

	/*
	 * Look for a connection match.
	 */
#ifdef CONFIG_NF_FLOW_COOKIE
	cm = si->sfe_flow_cookie_table[skb->flow_cookie & SFE_FLOW_COOKIE_MASK].match;
	if (unlikely(!cm)) {
		cm = sfe_ipv6_find_connection_match_rcu(si, dev, IPPROTO_IPIP, src_ip, src_port, dest_ip, dest_port);
	}
#else
	cm = sfe_ipv6_find_connection_match_rcu(si, dev, IPPROTO_IPIP, src_ip, src_port, dest_ip, dest_port);
#endif
	if (unlikely(!cm)) {
		rcu_read_unlock();
		sfe_ipv6_exception_stats_inc(si, SFE_IPV6_EXCEPTION_EVENT_TUNIPIP6_NO_CONNECTION);
		DEBUG_TRACE("%px: no connection found\n", skb);
		return 0;
	}

	/*
	 * If our packet has been marked as "sync on find" we will sync the status
	 * and forward it to slowpath.
	 */
	if (unlikely(sync_on_find)) {
		sfe_ipv6_sync_status(si, cm->connection, SFE_SYNC_REASON_STATS);
		rcu_read_unlock();
		sfe_ipv6_exception_stats_inc(si, SFE_IPV6_EXCEPTION_EVENT_TUNIPIP6_SYNC_ON_FIND);
		DEBUG_TRACE("%px: Sync on find\n", skb);

		return 0;
	}

	/*
	 * If cm->proto is set, it means the decap path.
	 * Otherwise we forward the packet in encap path.
	 */
	if(cm->proto) {
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0))
		const struct inet6_protocol *ipprot = cm->proto;
#else
		struct inet6_protocol *ipprot = cm->proto;
#endif
		skb_pull(skb, ihl);
		skb_reset_transport_header(skb);

		/*
		 * ipprot->handler(skb) will always return 0;
		 * There is no way to tell whether the packet is dropped later in linux or not.
		 * Hence here inc the byte/packet count always.
		 */
		atomic_inc(&cm->rx_packet_count);
		atomic_add(len, &cm->rx_byte_count);
		this_cpu_inc(si->stats_pcpu->packets_forwarded64);
		rcu_read_unlock();
		DEBUG_TRACE("%px: %s decap done \n",skb, __func__);
		ipprot->handler(skb);
		return 1;

	}

	/*
	 * If our packet is larger than the MTU of the transmit interface then
	 * we can't forward it easily.
	 */
	if (unlikely(len > cm->xmit_dev_mtu)) {
		sfe_ipv6_sync_status(si, cm->connection, SFE_SYNC_REASON_STATS);
		rcu_read_unlock();

		sfe_ipv6_exception_stats_inc(si, SFE_IPV6_EXCEPTION_EVENT_TUNIPIP6_NEEDS_FRAGMENTATION);
		DEBUG_TRACE("%px: Larger than mtu\n", skb);
		return 0;
	}

	/*
	 * Update DSCP
	 */
	if (unlikely(cm->flags & SFE_IPV6_CONNECTION_MATCH_FLAG_DSCP_REMARK)) {
		sfe_ipv6_change_dsfield(iph, cm->dscp);
	}

	/*
	 * Update traffic stats.
	 */
	atomic_inc(&cm->rx_packet_count);
	atomic_add(len, &cm->rx_byte_count);

	skb->dev = cm->xmit_dev;

	/*
	 * Check to see if we need to write a header.
	 */
	if (likely(cm->flags & SFE_IPV6_CONNECTION_MATCH_FLAG_WRITE_L2_HDR)) {
		if (unlikely(!(cm->flags & SFE_IPV6_CONNECTION_MATCH_FLAG_WRITE_FAST_ETH_HDR))) {
			dev_hard_header(skb, cm->xmit_dev, ETH_P_IPV6,
					cm->xmit_dest_mac, cm->xmit_src_mac, len);
		} else {
			struct ethhdr *eth = (struct ethhdr *)__skb_push(skb, ETH_HLEN);
			eth->h_proto = htons(ETH_P_IPV6);
			ether_addr_copy((u8 *)eth->h_dest, (u8 *)cm->xmit_dest_mac);
			ether_addr_copy((u8 *)eth->h_source, (u8 *)cm->xmit_src_mac);
		}
	}

	/*
	 * Update priority of skb.
	 */
	if (unlikely(cm->flags & SFE_IPV6_CONNECTION_MATCH_FLAG_PRIORITY_REMARK)) {
		skb->priority = cm->priority;
	}

	/*
	 * Mark outgoing packet.
	 */
	if (unlikely(cm->flags & SFE_IPV6_CONNECTION_MATCH_FLAG_MARK)) {
		skb->mark = cm->mark;
	}

	rcu_read_unlock();

	this_cpu_inc(si->stats_pcpu->packets_forwarded64);

	/*
	 * We're going to check for GSO flags when we transmit the packet so
	 * start fetching the necessary cache line now.
	 */
	prefetch(skb_shinfo(skb));

	/*
	 * Mark that this packet has been fast forwarded and send it on its way.
	 */
	skb->fast_forwarded = 1;
	dev_queue_xmit(skb);

	return 1;
}
