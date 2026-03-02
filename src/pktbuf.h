/*
 * pktbuf.h - Portable IP/TCP/UDP/ICMP packet building and checksum utilities
 *
 * Provides own header struct definitions to avoid platform divergence between
 * Linux (linux/ip.h) and macOS (netinet/ip.h).
 */

#ifndef __PKTBUF_H
#define __PKTBUF_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

/* -----------------------------------------------------------------------
 * Portable header structs
 * ----------------------------------------------------------------------- */

struct pb_iphdr {
	uint8_t  ihl_ver;      /* version(4) | IHL(4), IHL in 32-bit words */
	uint8_t  tos;
	uint16_t tot_len;      /* total packet length, network byte order */
	uint16_t id;
	uint16_t frag_off;     /* fragment flags + offset */
	uint8_t  ttl;
	uint8_t  protocol;     /* IPPROTO_TCP / IPPROTO_UDP / IPPROTO_ICMP */
	uint16_t check;        /* header checksum */
	uint32_t saddr;        /* source address, network byte order */
	uint32_t daddr;        /* destination address, network byte order */
} __attribute__((packed));

struct pb_ip6hdr {
	uint32_t ver_tc_flow;  /* version(4) | TC(8) | flow label(20) */
	uint16_t payload_len;  /* payload length (NOT including this header) */
	uint8_t  nexthdr;      /* IPPROTO_TCP / IPPROTO_UDP / IPPROTO_ICMPV6 */
	uint8_t  hop_limit;
	uint8_t  saddr[16];
	uint8_t  daddr[16];
} __attribute__((packed));

struct pb_tcphdr {
	uint16_t sport;
	uint16_t dport;
	uint32_t seq;
	uint32_t ack_seq;
	uint8_t  doff_res;     /* data offset(4) | reserved(4), doff in 32-bit words */
	uint8_t  flags;
	uint16_t window;
	uint16_t check;
	uint16_t urg_ptr;
} __attribute__((packed));

struct pb_udphdr {
	uint16_t sport;
	uint16_t dport;
	uint16_t len;          /* UDP header + data length */
	uint16_t check;
} __attribute__((packed));

struct pb_icmphdr {
	uint8_t  type;
	uint8_t  code;
	uint16_t check;
	uint16_t id;
	uint16_t seq;
} __attribute__((packed));

struct pb_icmp6hdr {
	uint8_t  type;
	uint8_t  code;
	uint16_t check;
	uint16_t id;
	uint16_t seq;
} __attribute__((packed));

/* TCP flag bits */
#define PB_TH_FIN  0x01
#define PB_TH_SYN  0x02
#define PB_TH_RST  0x04
#define PB_TH_PSH  0x08
#define PB_TH_ACK  0x10
#define PB_TH_URG  0x20

/* ICMP types */
#define PB_ICMP_ECHO_REPLY    0
#define PB_ICMP_ECHO_REQUEST  8

/* ICMPv6 types */
#define PB_ICMPV6_ECHO_REQUEST  128
#define PB_ICMPV6_ECHO_REPLY    129

/* IPv4 header length when no options are present */
#define PB_IPV4_HDR_LEN   20
/* IPv6 header length (fixed) */
#define PB_IPV6_HDR_LEN   40
#define PB_TCP_HDR_LEN    20   /* without options */
#define PB_UDP_HDR_LEN     8
#define PB_ICMP_HDR_LEN    8

/* Default TTL for synthesized packets */
#define PB_DEFAULT_TTL    64

/* MSS we advertise in SYN-ACK: 1300 (VPN MTU) - 20 (IP) - 20 (TCP) */
#define PB_TCP_MSS        1260

/* Maximum output buffer we ever need:
 * 40 (IPv6) + 24 (TCP+MSS opt) + NM_PI_BUFFER_SIZE payload */
#define PB_MAX_PKT        (8192 + 64)

/* -----------------------------------------------------------------------
 * Checksum functions
 * ----------------------------------------------------------------------- */

/* Standard one's-complement IP checksum over 'len' bytes at 'data'. */
uint16_t pb_ip_checksum(const void *data, int len);

/* TCP/UDP checksum using IPv4 pseudo-header.
 * seg_data points to the TCP/UDP header + payload, seg_len is its length. */
uint16_t pb_tcpudp_checksum4(uint32_t saddr, uint32_t daddr, uint8_t proto,
                              const void *seg_data, uint16_t seg_len);

/* TCP/UDP checksum using IPv6 pseudo-header. */
uint16_t pb_tcpudp_checksum6(const uint8_t *saddr, const uint8_t *daddr,
                              uint8_t proto,
                              const void *seg_data, uint16_t seg_len);

/* Plain ICMP checksum (no pseudo-header). */
uint16_t pb_icmp_checksum(const void *data, int len);

/* ICMPv6 checksum using IPv6 pseudo-header. */
uint16_t pb_icmpv6_checksum(const uint8_t *saddr, const uint8_t *daddr,
                             const void *data, int len);

/* -----------------------------------------------------------------------
 * Packet building functions
 * All return the total packet length written into buf[], or -1 on error
 * (buffer too small).  bufsz must be >= PB_MAX_PKT to be safe.
 * ----------------------------------------------------------------------- */

/*
 * Build a complete IPv4/TCP packet.
 * opt_data/opt_len: TCP options bytes (must be multiple of 4); pass NULL/0
 * for no options.  payload/payload_len: TCP segment data.
 */
int pb_build_tcp4(uint8_t *buf, size_t bufsz,
                  uint32_t saddr, uint32_t daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq,
                  uint8_t flags, uint16_t window,
                  const uint8_t *opt_data, uint8_t opt_len,
                  const void *payload, size_t payload_len);

/* Build a complete IPv6/TCP packet. */
int pb_build_tcp6(uint8_t *buf, size_t bufsz,
                  const uint8_t *saddr, const uint8_t *daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq,
                  uint8_t flags, uint16_t window,
                  const uint8_t *opt_data, uint8_t opt_len,
                  const void *payload, size_t payload_len);

/* Build a complete IPv4/UDP packet. */
int pb_build_udp4(uint8_t *buf, size_t bufsz,
                  uint32_t saddr, uint32_t daddr,
                  uint16_t sport, uint16_t dport,
                  const void *payload, size_t payload_len);

/* Build a complete IPv6/UDP packet. */
int pb_build_udp6(uint8_t *buf, size_t bufsz,
                  const uint8_t *saddr, const uint8_t *daddr,
                  uint16_t sport, uint16_t dport,
                  const void *payload, size_t payload_len);

/*
 * Build a complete IPv4/ICMP echo-reply packet.
 * payload/payload_len is the ICMP data after the 8-byte ICMP header.
 */
int pb_build_icmp4_reply(uint8_t *buf, size_t bufsz,
                         uint32_t saddr, uint32_t daddr,
                         uint16_t id, uint16_t seq,
                         const void *payload, size_t payload_len);

/* Build a complete IPv6/ICMPv6 echo-reply packet. */
int pb_build_icmpv6_reply(uint8_t *buf, size_t bufsz,
                          const uint8_t *saddr, const uint8_t *daddr,
                          uint16_t id, uint16_t seq,
                          const void *payload, size_t payload_len);

/*
 * Build a 4-byte MSS TCP option: kind=2, len=4, mss (network byte order).
 * Write into opt[4].
 */
void pb_make_mss_option(uint8_t opt[4], uint16_t mss);

/*
 * Build an IPv4/TCP RST packet.
 */
int pb_build_rst4(uint8_t *buf, size_t bufsz,
                  uint32_t saddr, uint32_t daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq, uint8_t flags);

/* IPv6/TCP RST */
int pb_build_rst6(uint8_t *buf, size_t bufsz,
                  const uint8_t *saddr, const uint8_t *daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq, uint8_t flags);

#endif /* __PKTBUF_H */
