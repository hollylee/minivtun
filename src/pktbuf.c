/*
 * pktbuf.c - IP/TCP/UDP/ICMP packet building and checksum implementation
 */

#include <string.h>
#include <arpa/inet.h>
#include "pktbuf.h"

/* -----------------------------------------------------------------------
 * Checksum helpers
 * ----------------------------------------------------------------------- */

uint16_t pb_ip_checksum(const void *data, int len)
{
	const uint16_t *p = data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *p++;
		len -= 2;
	}
	if (len == 1)
		sum += *(const uint8_t *)p;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return (uint16_t)(~sum);
}

uint16_t pb_tcpudp_checksum4(uint32_t saddr, uint32_t daddr, uint8_t proto,
                              const void *seg_data, uint16_t seg_len)
{
	/* IPv4 pseudo-header: src(4) dst(4) zero(1) proto(1) len(2) */
	struct {
		uint32_t src;
		uint32_t dst;
		uint8_t  zero;
		uint8_t  proto;
		uint16_t len;
	} __attribute__((packed)) pseudo;
	uint32_t sum = 0;
	const uint16_t *p;
	int len;

	pseudo.src   = saddr;
	pseudo.dst   = daddr;
	pseudo.zero  = 0;
	pseudo.proto = proto;
	pseudo.len   = htons(seg_len);

	p = (const uint16_t *)&pseudo;
	len = sizeof(pseudo);
	while (len > 0) { sum += *p++; len -= 2; }

	p = seg_data;
	len = seg_len;
	while (len > 1) { sum += *p++; len -= 2; }
	if (len == 1) sum += *(const uint8_t *)p;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return (uint16_t)(~sum);
}

uint16_t pb_tcpudp_checksum6(const uint8_t *saddr, const uint8_t *daddr,
                              uint8_t proto,
                              const void *seg_data, uint16_t seg_len)
{
	/* IPv6 pseudo-header: src(16) dst(16) len(4) zero(3) nexthdr(1) */
	struct {
		uint8_t  src[16];
		uint8_t  dst[16];
		uint32_t len;
		uint8_t  zero[3];
		uint8_t  nexthdr;
	} __attribute__((packed)) pseudo;
	uint32_t sum = 0;
	const uint16_t *p;
	int len;

	memcpy(pseudo.src, saddr, 16);
	memcpy(pseudo.dst, daddr, 16);
	pseudo.len     = htonl(seg_len);
	memset(pseudo.zero, 0, 3);
	pseudo.nexthdr = proto;

	p = (const uint16_t *)&pseudo;
	len = sizeof(pseudo);
	while (len > 0) { sum += *p++; len -= 2; }

	p = seg_data;
	len = seg_len;
	while (len > 1) { sum += *p++; len -= 2; }
	if (len == 1) sum += *(const uint8_t *)p;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return (uint16_t)(~sum);
}

uint16_t pb_icmp_checksum(const void *data, int len)
{
	return pb_ip_checksum(data, len);
}

uint16_t pb_icmpv6_checksum(const uint8_t *saddr, const uint8_t *daddr,
                             const void *data, int len)
{
	return pb_tcpudp_checksum6(saddr, daddr, IPPROTO_ICMPV6, data, (uint16_t)len);
}

/* -----------------------------------------------------------------------
 * Option helpers
 * ----------------------------------------------------------------------- */

void pb_make_mss_option(uint8_t opt[4], uint16_t mss)
{
	opt[0] = 2;           /* kind = MSS */
	opt[1] = 4;           /* length = 4 */
	opt[2] = (mss >> 8) & 0xff;
	opt[3] =  mss        & 0xff;
}

/* -----------------------------------------------------------------------
 * IPv4 TCP
 * ----------------------------------------------------------------------- */

int pb_build_tcp4(uint8_t *buf, size_t bufsz,
                  uint32_t saddr, uint32_t daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq,
                  uint8_t flags, uint16_t window,
                  const uint8_t *opt_data, uint8_t opt_len,
                  const void *payload, size_t payload_len)
{
	uint8_t tcp_doff;
	size_t tcp_hdr_len, total;
	struct pb_iphdr *iph;
	struct pb_tcphdr *tcph;
	uint8_t *p;

	/* opt_len must be a multiple of 4 */
	tcp_hdr_len = PB_TCP_HDR_LEN + opt_len;
	tcp_doff    = (uint8_t)(tcp_hdr_len / 4);
	total       = PB_IPV4_HDR_LEN + tcp_hdr_len + payload_len;

	if (total > bufsz)
		return -1;

	/* IP header */
	iph = (struct pb_iphdr *)buf;
	memset(iph, 0, PB_IPV4_HDR_LEN);
	iph->ihl_ver  = 0x45;
	iph->tos      = 0;
	iph->tot_len  = htons((uint16_t)total);
	iph->id       = 0;
	iph->frag_off = 0;
	iph->ttl      = PB_DEFAULT_TTL;
	iph->protocol = IPPROTO_TCP;
	iph->saddr    = saddr;
	iph->daddr    = daddr;
	iph->check    = pb_ip_checksum(iph, PB_IPV4_HDR_LEN);

	/* TCP header */
	p = buf + PB_IPV4_HDR_LEN;
	tcph = (struct pb_tcphdr *)p;
	memset(tcph, 0, PB_TCP_HDR_LEN);
	tcph->sport   = htons(sport);
	tcph->dport   = htons(dport);
	tcph->seq     = htonl(seq);
	tcph->ack_seq = htonl(ack_seq);
	tcph->doff_res = (tcp_doff << 4);
	tcph->flags   = flags;
	tcph->window  = htons(window);

	/* Options */
	if (opt_len && opt_data)
		memcpy(p + PB_TCP_HDR_LEN, opt_data, opt_len);

	/* Payload */
	if (payload_len && payload)
		memcpy(p + tcp_hdr_len, payload, payload_len);

	/* TCP checksum */
	tcph->check = pb_tcpudp_checksum4(saddr, daddr, IPPROTO_TCP,
	                                   p, (uint16_t)(tcp_hdr_len + payload_len));

	return (int)total;
}

/* -----------------------------------------------------------------------
 * IPv6 TCP
 * ----------------------------------------------------------------------- */

int pb_build_tcp6(uint8_t *buf, size_t bufsz,
                  const uint8_t *saddr, const uint8_t *daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq,
                  uint8_t flags, uint16_t window,
                  const uint8_t *opt_data, uint8_t opt_len,
                  const void *payload, size_t payload_len)
{
	uint8_t tcp_doff;
	size_t tcp_hdr_len, total, tcp_total;
	struct pb_ip6hdr *ip6h;
	struct pb_tcphdr *tcph;
	uint8_t *p;

	tcp_hdr_len = PB_TCP_HDR_LEN + opt_len;
	tcp_doff    = (uint8_t)(tcp_hdr_len / 4);
	tcp_total   = tcp_hdr_len + payload_len;
	total       = PB_IPV6_HDR_LEN + tcp_total;

	if (total > bufsz)
		return -1;

	ip6h = (struct pb_ip6hdr *)buf;
	memset(ip6h, 0, PB_IPV6_HDR_LEN);
	ip6h->ver_tc_flow = htonl(0x60000000);
	ip6h->payload_len = htons((uint16_t)tcp_total);
	ip6h->nexthdr     = IPPROTO_TCP;
	ip6h->hop_limit   = PB_DEFAULT_TTL;
	memcpy(ip6h->saddr, saddr, 16);
	memcpy(ip6h->daddr, daddr, 16);

	p = buf + PB_IPV6_HDR_LEN;
	tcph = (struct pb_tcphdr *)p;
	memset(tcph, 0, PB_TCP_HDR_LEN);
	tcph->sport    = htons(sport);
	tcph->dport    = htons(dport);
	tcph->seq      = htonl(seq);
	tcph->ack_seq  = htonl(ack_seq);
	tcph->doff_res = (tcp_doff << 4);
	tcph->flags    = flags;
	tcph->window   = htons(window);

	if (opt_len && opt_data)
		memcpy(p + PB_TCP_HDR_LEN, opt_data, opt_len);
	if (payload_len && payload)
		memcpy(p + tcp_hdr_len, payload, payload_len);

	tcph->check = pb_tcpudp_checksum6(saddr, daddr, IPPROTO_TCP,
	                                   p, (uint16_t)tcp_total);

	return (int)total;
}

/* -----------------------------------------------------------------------
 * RST helpers (thin wrappers around the TCP builders)
 * ----------------------------------------------------------------------- */

int pb_build_rst4(uint8_t *buf, size_t bufsz,
                  uint32_t saddr, uint32_t daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq, uint8_t flags)
{
	return pb_build_tcp4(buf, bufsz, saddr, daddr, sport, dport,
	                     seq, ack_seq, flags, 0, NULL, 0, NULL, 0);
}

int pb_build_rst6(uint8_t *buf, size_t bufsz,
                  const uint8_t *saddr, const uint8_t *daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq, uint8_t flags)
{
	return pb_build_tcp6(buf, bufsz, saddr, daddr, sport, dport,
	                     seq, ack_seq, flags, 0, NULL, 0, NULL, 0);
}

/* -----------------------------------------------------------------------
 * IPv4 UDP
 * ----------------------------------------------------------------------- */

int pb_build_udp4(uint8_t *buf, size_t bufsz,
                  uint32_t saddr, uint32_t daddr,
                  uint16_t sport, uint16_t dport,
                  const void *payload, size_t payload_len)
{
	size_t udp_len = PB_UDP_HDR_LEN + payload_len;
	size_t total   = PB_IPV4_HDR_LEN + udp_len;
	struct pb_iphdr *iph;
	struct pb_udphdr *udph;
	uint8_t *p;

	if (total > bufsz)
		return -1;

	iph = (struct pb_iphdr *)buf;
	memset(iph, 0, PB_IPV4_HDR_LEN);
	iph->ihl_ver  = 0x45;
	iph->tot_len  = htons((uint16_t)total);
	iph->ttl      = PB_DEFAULT_TTL;
	iph->protocol = IPPROTO_UDP;
	iph->saddr    = saddr;
	iph->daddr    = daddr;
	iph->check    = pb_ip_checksum(iph, PB_IPV4_HDR_LEN);

	p = buf + PB_IPV4_HDR_LEN;
	udph = (struct pb_udphdr *)p;
	udph->sport = htons(sport);
	udph->dport = htons(dport);
	udph->len   = htons((uint16_t)udp_len);
	udph->check = 0;

	if (payload_len && payload)
		memcpy(p + PB_UDP_HDR_LEN, payload, payload_len);

	udph->check = pb_tcpudp_checksum4(saddr, daddr, IPPROTO_UDP,
	                                   p, (uint16_t)udp_len);
	/* Zero checksum means "not computed" in IPv4 UDP; keep as is */

	return (int)total;
}

/* -----------------------------------------------------------------------
 * IPv6 UDP
 * ----------------------------------------------------------------------- */

int pb_build_udp6(uint8_t *buf, size_t bufsz,
                  const uint8_t *saddr, const uint8_t *daddr,
                  uint16_t sport, uint16_t dport,
                  const void *payload, size_t payload_len)
{
	size_t udp_len = PB_UDP_HDR_LEN + payload_len;
	size_t total   = PB_IPV6_HDR_LEN + udp_len;
	struct pb_ip6hdr *ip6h;
	struct pb_udphdr *udph;
	uint8_t *p;

	if (total > bufsz)
		return -1;

	ip6h = (struct pb_ip6hdr *)buf;
	memset(ip6h, 0, PB_IPV6_HDR_LEN);
	ip6h->ver_tc_flow = htonl(0x60000000);
	ip6h->payload_len = htons((uint16_t)udp_len);
	ip6h->nexthdr     = IPPROTO_UDP;
	ip6h->hop_limit   = PB_DEFAULT_TTL;
	memcpy(ip6h->saddr, saddr, 16);
	memcpy(ip6h->daddr, daddr, 16);

	p = buf + PB_IPV6_HDR_LEN;
	udph = (struct pb_udphdr *)p;
	udph->sport = htons(sport);
	udph->dport = htons(dport);
	udph->len   = htons((uint16_t)udp_len);
	udph->check = 0;

	if (payload_len && payload)
		memcpy(p + PB_UDP_HDR_LEN, payload, payload_len);

	udph->check = pb_tcpudp_checksum6(saddr, daddr, IPPROTO_UDP,
	                                   p, (uint16_t)udp_len);

	return (int)total;
}

/* -----------------------------------------------------------------------
 * IPv4 ICMP echo reply
 * ----------------------------------------------------------------------- */

int pb_build_icmp4_reply(uint8_t *buf, size_t bufsz,
                         uint32_t saddr, uint32_t daddr,
                         uint16_t id, uint16_t seq,
                         const void *payload, size_t payload_len)
{
	size_t icmp_len = PB_ICMP_HDR_LEN + payload_len;
	size_t total    = PB_IPV4_HDR_LEN + icmp_len;
	struct pb_iphdr *iph;
	struct pb_icmphdr *icmph;
	uint8_t *p;

	if (total > bufsz)
		return -1;

	iph = (struct pb_iphdr *)buf;
	memset(iph, 0, PB_IPV4_HDR_LEN);
	iph->ihl_ver  = 0x45;
	iph->tot_len  = htons((uint16_t)total);
	iph->ttl      = PB_DEFAULT_TTL;
	iph->protocol = IPPROTO_ICMP;
	iph->saddr    = saddr;
	iph->daddr    = daddr;
	iph->check    = pb_ip_checksum(iph, PB_IPV4_HDR_LEN);

	p = buf + PB_IPV4_HDR_LEN;
	icmph = (struct pb_icmphdr *)p;
	icmph->type  = PB_ICMP_ECHO_REPLY;
	icmph->code  = 0;
	icmph->check = 0;
	icmph->id    = htons(id);
	icmph->seq   = htons(seq);

	if (payload_len && payload)
		memcpy(p + PB_ICMP_HDR_LEN, payload, payload_len);

	icmph->check = pb_icmp_checksum(p, (int)icmp_len);

	return (int)total;
}

/* -----------------------------------------------------------------------
 * IPv6 ICMPv6 echo reply
 * ----------------------------------------------------------------------- */

int pb_build_icmpv6_reply(uint8_t *buf, size_t bufsz,
                          const uint8_t *saddr, const uint8_t *daddr,
                          uint16_t id, uint16_t seq,
                          const void *payload, size_t payload_len)
{
	size_t icmp_len = PB_ICMP_HDR_LEN + payload_len;
	size_t total    = PB_IPV6_HDR_LEN + icmp_len;
	struct pb_ip6hdr *ip6h;
	struct pb_icmp6hdr *icmph;
	uint8_t *p;

	if (total > bufsz)
		return -1;

	ip6h = (struct pb_ip6hdr *)buf;
	memset(ip6h, 0, PB_IPV6_HDR_LEN);
	ip6h->ver_tc_flow = htonl(0x60000000);
	ip6h->payload_len = htons((uint16_t)icmp_len);
	ip6h->nexthdr     = IPPROTO_ICMPV6;
	ip6h->hop_limit   = PB_DEFAULT_TTL;
	memcpy(ip6h->saddr, saddr, 16);
	memcpy(ip6h->daddr, daddr, 16);

	p = buf + PB_IPV6_HDR_LEN;
	icmph = (struct pb_icmp6hdr *)p;
	icmph->type  = PB_ICMPV6_ECHO_REPLY;
	icmph->code  = 0;
	icmph->check = 0;
	icmph->id    = htons(id);
	icmph->seq   = htons(seq);

	if (payload_len && payload)
		memcpy(p + PB_ICMP_HDR_LEN, payload, payload_len);

	icmph->check = pb_icmpv6_checksum(saddr, daddr, p, (int)icmp_len);

	return (int)total;
}
