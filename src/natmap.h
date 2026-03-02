/*
 * natmap.h - VPN peer table and NAT connection tracking
 *
 * Tracks:
 *   vpn_peer   - connected VPN clients (virtual IP → real UDP endpoint)
 *   tcp_conn   - TCP NAT entries with minimal state machine
 *   udp_flow   - UDP NAT entries
 *   icmp_flow  - ICMP NAT entries
 */

#ifndef __NATMAP_H
#define __NATMAP_H

#include <stdint.h>
#include <time.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "list.h"

/* -----------------------------------------------------------------------
 * VPN peer (a connected client)
 * ----------------------------------------------------------------------- */

struct vpn_peer {
	int             af;           /* AF_INET or AF_INET6 (virtual IP family) */
	struct in_addr  vip4;         /* virtual IPv4 address */
	struct in6_addr vip6;         /* virtual IPv6 address */
	struct sockaddr_storage udp_addr; /* client's real UDP endpoint */
	time_t          last_active;
	struct list_head node;
};

/* -----------------------------------------------------------------------
 * TCP NAT entry
 * ----------------------------------------------------------------------- */

enum tcp_state {
	TCP_CONNECTING,      /* non-blocking connect() in flight */
	TCP_SYN_ACK_SENT,    /* SYN-ACK sent to client, awaiting ACK */
	TCP_ESTABLISHED,
	TCP_FIN_WAIT,        /* client has sent FIN */
	TCP_CLOSE_WAIT,      /* real server has closed */
	TCP_CLOSED,
};

struct tcp_conn {
	int              af;           /* AF_INET or AF_INET6 */
	/* key: client virtual IP, client port, destination IP/port */
	struct in_addr   clt_vip4;
	struct in6_addr  clt_vip6;
	uint16_t         clt_port;    /* host byte order */
	struct in_addr   dst_ip4;
	struct in6_addr  dst_ip6;
	uint16_t         dst_port;    /* host byte order */

	int              fd;           /* real TCP socket (non-blocking) */
	enum tcp_state   state;

	/* TCP sequence number tracking */
	uint32_t clt_iss;   /* client's Initial Sequence Number (from SYN) */
	uint32_t clt_seq;   /* next expected seq from client (= our ACK number) */
	uint32_t srv_iss;   /* our Initial Sequence Number (chosen at SYN-ACK) */
	uint32_t srv_seq;   /* next seq we send to client */

	struct vpn_peer *peer;
	time_t           last_active;
	struct list_head node;
};

/* -----------------------------------------------------------------------
 * UDP flow entry
 * ----------------------------------------------------------------------- */

struct udp_flow {
	int              af;
	struct in_addr   clt_vip4;
	struct in6_addr  clt_vip6;
	uint16_t         clt_port;    /* host byte order */
	struct in_addr   dst_ip4;
	struct in6_addr  dst_ip6;
	uint16_t         dst_port;    /* host byte order */

	int              fd;           /* connected UDP socket */

	struct vpn_peer *peer;
	time_t           last_active;
	struct list_head node;
};

/* -----------------------------------------------------------------------
 * ICMP flow entry
 * ----------------------------------------------------------------------- */

struct icmp_flow {
	int              af;
	struct in_addr   clt_vip4;
	struct in6_addr  clt_vip6;
	struct in_addr   dst_ip4;
	struct in6_addr  dst_ip6;
	uint16_t         orig_id;     /* ICMP ID in the client's packet */
	uint16_t         mapped_id;   /* ICMP ID we use on the wire */

	int              fd;           /* SOCK_DGRAM or SOCK_RAW ICMP/ICMPv6 socket */
	int              sock_type;    /* SOCK_DGRAM or SOCK_RAW (affects recv format) */

	struct vpn_peer *peer;
	time_t           last_active;
	struct list_head node;
};

/* -----------------------------------------------------------------------
 * Timeout constants (seconds)
 * ----------------------------------------------------------------------- */

#define NAT_TCP_TIMEOUT   300
#define NAT_UDP_TIMEOUT    60
#define NAT_ICMP_TIMEOUT   30

/* -----------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------------- */

void natmap_init(void);

/* --- VPN peers --- */

struct vpn_peer *peer_get_or_create(int af, const void *vip,
                                    const struct sockaddr_storage *udp_addr);
/* vip is struct in_addr* for AF_INET, struct in6_addr* for AF_INET6 */
struct vpn_peer *peer_find_by_vip4(const struct in_addr *vip);
struct vpn_peer *peer_find_by_vip6(const struct in6_addr *vip);

/* Update peer's real UDP address (client roamed) */
void peer_update_addr(struct vpn_peer *p,
                      const struct sockaddr_storage *udp_addr);

/* Send a keepalive to all peers whose last_xmit is stale.
 * Remove peers whose last_active > peer_timeo. */
void peer_walk(int sockfd, time_t now, unsigned keepalive_timeo,
               unsigned peer_timeo);

/* --- TCP connections --- */

struct tcp_conn *tcp_conn_find(int af,
                               const void *clt_vip, uint16_t clt_port,
                               const void *dst_ip,  uint16_t dst_port);
struct tcp_conn *tcp_conn_create(int af,
                                 const void *clt_vip, uint16_t clt_port,
                                 const void *dst_ip,  uint16_t dst_port,
                                 struct vpn_peer *peer);
void             tcp_conn_remove(struct tcp_conn *c);
struct tcp_conn *tcp_conn_find_by_fd(int fd);

/* --- UDP flows --- */

struct udp_flow *udp_flow_find(int af,
                               const void *clt_vip, uint16_t clt_port,
                               const void *dst_ip,  uint16_t dst_port);
struct udp_flow *udp_flow_create(int af,
                                 const void *clt_vip, uint16_t clt_port,
                                 const void *dst_ip,  uint16_t dst_port,
                                 struct vpn_peer *peer);
void             udp_flow_remove(struct udp_flow *f);
struct udp_flow *udp_flow_find_by_fd(int fd);

/* --- ICMP flows --- */

struct icmp_flow *icmp_flow_find(int af,
                                 const void *clt_vip,
                                 const void *dst_ip,
                                 uint16_t    orig_id);
struct icmp_flow *icmp_flow_create(int af,
                                   const void *clt_vip,
                                   const void *dst_ip,
                                   uint16_t    orig_id,
                                   struct vpn_peer *peer);
void              icmp_flow_remove(struct icmp_flow *f);
struct icmp_flow *icmp_flow_find_by_fd(int fd);

/*
 * Append poll entries for all NAT sockets into pfds[].
 * Returns number of entries appended, or -1 if pfds is too small.
 * TCP_CONNECTING sockets are added with POLLOUT; all others with POLLIN.
 */
int natmap_build_pollfd(struct pollfd *pfds, int max_entries);

/*
 * Remove expired NAT entries.  Call periodically (every ~5 s).
 */
void natmap_expire(time_t now);

#endif /* __NATMAP_H */
