/*
 * server.c - Userspace NAT/routing server for minivtun
 *
 * Replaces the old TUN-based server.  All packet forwarding and NAT is
 * done entirely in userspace using regular OS sockets.  No TUN interface,
 * no kernel IP forwarding, no iptables required.
 *
 * Features:
 *   - Multi-client: arbitrary number of simultaneous VPN clients
 *   - Inter-client routing: packets between clients are forwarded directly
 *   - External NAT (IPv4 and IPv6): TCP, UDP, ICMP relayed via real sockets
 *   - Linux and macOS support
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __linux__
#  include <netinet/icmp6.h>
#  include <netinet/ip6.h>
#endif

#include <openssl/rand.h>

#include "list.h"

/* -----------------------------------------------------------------------
 * Debug packet dump helpers (enabled only when compiled with -DDEBUG=1)
 * ----------------------------------------------------------------------- */
#if DEBUG
static void dbg_print_ip4(const char *tag,
                           const uint8_t *pkt, size_t len)
{
	char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];
	uint8_t proto;
	uint16_t sport = 0, dport = 0;

	if (len < 20) { fprintf(stderr, "[DBG] %s <short IPv4 %zu>\n", tag, len); return; }
	inet_ntop(AF_INET, pkt + 12, src, sizeof(src));
	inet_ntop(AF_INET, pkt + 16, dst, sizeof(dst));
	proto = pkt[9];
	uint8_t ihl = (pkt[0] & 0x0f) * 4;
	if ((size_t)ihl + 4 <= len) {
		sport = (uint16_t)((pkt[ihl] << 8) | pkt[ihl+1]);
		dport = (uint16_t)((pkt[ihl+2] << 8) | pkt[ihl+3]);
	}
	const char *pname = (proto == 6) ? "TCP" : (proto == 17) ? "UDP" :
	                    (proto == 1) ? "ICMP" : "?";
	fprintf(stderr, "[DBG] %s IPv4 %s %s:%u -> %s:%u len=%zu\n",
	        tag, pname, src, sport, dst, dport, len);
}

static void dbg_print_ip6(const char *tag,
                           const uint8_t *pkt, size_t len)
{
	char src[INET6_ADDRSTRLEN], dst[INET6_ADDRSTRLEN];
	uint8_t proto;
	uint16_t sport = 0, dport = 0;

	if (len < 40) { fprintf(stderr, "[DBG] %s <short IPv6 %zu>\n", tag, len); return; }
	inet_ntop(AF_INET6, pkt + 8,  src, sizeof(src));
	inet_ntop(AF_INET6, pkt + 24, dst, sizeof(dst));
	proto = pkt[6];
	if (40 + 4 <= len) {
		sport = (uint16_t)((pkt[40] << 8) | pkt[41]);
		dport = (uint16_t)((pkt[42] << 8) | pkt[43]);
	}
	const char *pname = (proto == 6) ? "TCP" : (proto == 17) ? "UDP" :
	                    (proto == 58) ? "ICMPv6" : "?";
	fprintf(stderr, "[DBG] %s IPv6 %s [%s]:%u -> [%s]:%u len=%zu\n",
	        tag, pname, src, sport, dst, dport, len);
}
#endif /* DEBUG */
#include "jhash.h"
#include "minivtun.h"
#include "pktbuf.h"
#include "natmap.h"

/* Maximum number of poll() entries (1 VPN socket + NAT sockets) */
#define MAX_POLL_FDS  4096

/* -----------------------------------------------------------------------
 * Virtual route table (reused from old server; used for subnet routing)
 * ----------------------------------------------------------------------- */

struct vt_route {
	struct in_addr network;
	struct in_addr netmask;
	struct in_addr gateway;
};

#define VIRTUAL_ROUTE_MAX  32
static struct vt_route *vt_routes[VIRTUAL_ROUTE_MAX];
static unsigned vt_routes_len = 0;

int vt_route_add(struct in_addr *network, unsigned prefix,
                 struct in_addr *gateway)
{
	struct vt_route *rt;
	uint32_t mask;

	if (prefix == 0) {
		mask = 0;
	} else if (prefix > 32) {
		return -1;
	} else {
		mask = ~((1U << (32 - prefix)) - 1) & 0xffffffff;
	}

	if (vt_routes_len >= VIRTUAL_ROUTE_MAX) {
		fprintf(stderr, "*** Virtual route table is full.\n");
		return -1;
	}

	rt = malloc(sizeof(*rt));
	if (!rt) return -1;
	rt->netmask.s_addr = htonl(mask);
	rt->network.s_addr = network->s_addr & rt->netmask.s_addr;
	rt->gateway        = *gateway;
	vt_routes[vt_routes_len++] = rt;
	return 0;
}

static struct in_addr *vt_route_lookup(const struct in_addr *addr)
{
	unsigned i;
	for (i = 0; i < vt_routes_len; i++) {
		struct vt_route *rt = vt_routes[i];
		if ((addr->s_addr & rt->netmask.s_addr) == rt->network.s_addr)
			return &rt->gateway;
	}
	return NULL;
}

/* -----------------------------------------------------------------------
 * Helper: send an inner IP packet back to a VPN client
 * ----------------------------------------------------------------------- */

static void send_inner_to_peer(int sockfd, struct vpn_peer *peer,
                                uint16_t eth_proto,
                                const void *ip_pkt, size_t ip_len)
{
	char crypt_buffer[NM_CRYPTO_BUF_SIZE];
	struct minivtun_msg nmsg;
	void *out_data;
	size_t out_dlen;
	socklen_t alen;

	if (ip_len > sizeof(nmsg.ipdata.data))
		return;

	nmsg.hdr.opcode = MINIVTUN_MSG_IPDATA;
	memset(nmsg.hdr.rsv, 0, sizeof(nmsg.hdr.rsv));
	memcpy(nmsg.hdr.auth_key, config.crypto_key, sizeof(nmsg.hdr.auth_key));
	nmsg.ipdata.proto  = htons(eth_proto);
	nmsg.ipdata.ip_dlen = htons((uint16_t)ip_len);
	memcpy(nmsg.ipdata.data, ip_pkt, ip_len);

	out_data = crypt_buffer;
	out_dlen = MINIVTUN_MSG_IPDATA_OFFSET + ip_len;
	local_to_netmsg(&nmsg, &out_data, &out_dlen);

	alen = (peer->udp_addr.ss_family == AF_INET6)
	       ? sizeof(struct sockaddr_in6)
	       : sizeof(struct sockaddr_in);
#if DEBUG
	if (eth_proto == ETH_P_IP)
		dbg_print_ip4("SERVER->CLIENT", (const uint8_t *)ip_pkt, ip_len);
	else
		dbg_print_ip6("SERVER->CLIENT", (const uint8_t *)ip_pkt, ip_len);
#endif
	sendto(sockfd, out_data, out_dlen, 0,
	       (struct sockaddr *)&peer->udp_addr, alen);
}

/* -----------------------------------------------------------------------
 * Inter-client routing: forward inner packet to another VPN peer
 * ----------------------------------------------------------------------- */

static void forward_to_peer(int sockfd, struct vpn_peer *dst_peer,
                             uint16_t eth_proto,
                             const void *ip_pkt, size_t ip_len)
{
	send_inner_to_peer(sockfd, dst_peer, eth_proto, ip_pkt, ip_len);
}

/* -----------------------------------------------------------------------
 * Resolve destination VPN peer for an inner IPv4 packet
 * (direct peer match, then subnet route table)
 * ----------------------------------------------------------------------- */

static struct vpn_peer *resolve_peer_ipv4(const struct in_addr *dst)
{
	struct vpn_peer *p = peer_find_by_vip4(dst);
	if (p) return p;

	/* Try the virtual route table (subnet → gateway peer) */
	if (vt_routes_len > 0) {
		struct in_addr *gw = vt_route_lookup(dst);
		if (gw) return peer_find_by_vip4(gw);
	}
	return NULL;
}

static struct vpn_peer *resolve_peer_ipv6(const struct in6_addr *dst)
{
	return peer_find_by_vip6(dst);
}

/* -----------------------------------------------------------------------
 * TCP NAT handlers
 * ----------------------------------------------------------------------- */

/* Send a synthesized IPv4 TCP segment to the VPN client */
static void tcp4_to_client(int sockfd, struct tcp_conn *conn,
                            uint8_t flags,
                            const uint8_t *opt, uint8_t opt_len,
                            const void *data, size_t data_len)
{
	uint8_t pkt[PB_MAX_PKT];
	int len;

	len = pb_build_tcp4(pkt, sizeof(pkt),
	                    conn->dst_ip4.s_addr, conn->clt_vip4.s_addr,
	                    conn->dst_port, conn->clt_port,
	                    conn->srv_seq, conn->clt_seq,
	                    flags, 65535,
	                    opt, opt_len,
	                    data, data_len);
	if (len > 0)
		send_inner_to_peer(sockfd, conn->peer, ETH_P_IP, pkt, (size_t)len);
}

static void tcp6_to_client(int sockfd, struct tcp_conn *conn,
                            uint8_t flags,
                            const uint8_t *opt, uint8_t opt_len,
                            const void *data, size_t data_len)
{
	uint8_t pkt[PB_MAX_PKT];
	int len;

	len = pb_build_tcp6(pkt, sizeof(pkt),
	                    conn->dst_ip6.s6_addr, conn->clt_vip6.s6_addr,
	                    conn->dst_port, conn->clt_port,
	                    conn->srv_seq, conn->clt_seq,
	                    flags, 65535,
	                    opt, opt_len,
	                    data, data_len);
	if (len > 0)
		send_inner_to_peer(sockfd, conn->peer, ETH_P_IPV6, pkt, (size_t)len);
}

/* Process an incoming TCP packet from a VPN client (IPv4) */
static void handle_client_tcp4(int sockfd,
                                const struct pb_iphdr *iph, size_t ip_len,
                                struct vpn_peer *peer)
{
	uint8_t ihl = (iph->ihl_ver & 0x0f) * 4;
	const struct pb_tcphdr *tcph;
	uint16_t clt_port, dst_port;
	uint16_t tcp_hdr_len;
	uint32_t seq;
	uint8_t flags;
	const uint8_t *payload;
	size_t payload_len;
	struct tcp_conn *conn;
	struct sockaddr_in dst_addr;

	if (ihl < PB_IPV4_HDR_LEN)
		return;

	if (ip_len < (size_t)ihl + PB_TCP_HDR_LEN)
		return;

	tcph      = (const struct pb_tcphdr *)((const uint8_t *)iph + ihl);
	clt_port  = ntohs(tcph->sport);
	dst_port  = ntohs(tcph->dport);
	seq       = ntohl(tcph->seq);
	flags     = tcph->flags;
	tcp_hdr_len = (uint16_t)((tcph->doff_res >> 4) * 4);

	if (tcp_hdr_len < PB_TCP_HDR_LEN || ip_len < (size_t)ihl + tcp_hdr_len)
		return;

	payload     = (const uint8_t *)tcph + tcp_hdr_len;
	payload_len = ip_len - ihl - tcp_hdr_len;

	conn = tcp_conn_find(AF_INET, &iph->saddr, clt_port,
	                     &iph->daddr, dst_port);

	/* RST from client: tear down */
	if (flags & PB_TH_RST) {
		if (conn)
			tcp_conn_remove(conn);
		return;
	}

	/* New SYN: create connection entry and begin async connect */
	if ((flags & PB_TH_SYN) && !(flags & PB_TH_ACK)) {
		if (conn) {
			/* Retransmitted SYN while still connecting: ignore */
			return;
		}

		conn = tcp_conn_create(AF_INET, &iph->saddr, clt_port,
		                       &iph->daddr, dst_port, peer);
		if (!conn) return;

		conn->clt_iss = seq;
		conn->clt_seq = seq + 1;
		/* Pick a cryptographically random ISN for the server side */
		if (RAND_bytes((unsigned char *)&conn->srv_iss, sizeof(conn->srv_iss)) != 1)
			conn->srv_iss = (uint32_t)(time(NULL) ^ (uintptr_t)conn);
		conn->srv_seq = conn->srv_iss; /* SYN-ACK SEQ = ISN; advances to ISN+1 after handshake */

		conn->fd = socket(AF_INET, SOCK_STREAM, 0);
		if (conn->fd < 0) {
			tcp_conn_remove(conn);
			return;
		}
		set_nonblock(conn->fd);

		memset(&dst_addr, 0, sizeof(dst_addr));
		dst_addr.sin_family      = AF_INET;
		dst_addr.sin_addr.s_addr = iph->daddr;
		dst_addr.sin_port        = htons(dst_port);

		if (connect(conn->fd, (struct sockaddr *)&dst_addr,
		            sizeof(dst_addr)) < 0 && errno != EINPROGRESS) {
			tcp_conn_remove(conn);
			return;
		}
		conn->state = TCP_CONNECTING;
#if DEBUG
		{
			char s_src[INET_ADDRSTRLEN], s_dst[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &iph->saddr, s_src, sizeof(s_src));
			inet_ntop(AF_INET, &iph->daddr, s_dst, sizeof(s_dst));
			fprintf(stderr, "[DBG] TCP4 SYN %s:%u -> %s:%u (connecting)\n",
			        s_src, clt_port, s_dst, dst_port);
		}
#endif
		return;
	}

	if (!conn)
		return;

	conn->last_active = time(NULL);

	switch (conn->state) {

	case TCP_CONNECTING:
		/* Data or ACK before connect completes: ignore */
		break;

	case TCP_SYN_ACK_SENT:
		/* Expect the ACK of our SYN-ACK */
		if ((flags & PB_TH_ACK) && ntohl(tcph->ack_seq) == conn->srv_iss + 1) {
			conn->state = TCP_ESTABLISHED;
		}
		/* Fall through: if there's also payload, process it */
		if (!(flags & PB_TH_PSH) || payload_len == 0)
			break;
		/* FALLTHROUGH */

	case TCP_ESTABLISHED:
	case TCP_FIN_WAIT:
		/* Forward payload to real server */
		if (payload_len > 0) {
			ssize_t n = send(conn->fd, payload, payload_len, 0);
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				/* Real server gone: send RST to client */
				conn->srv_seq = ntohl(tcph->ack_seq);
				tcp4_to_client(sockfd, conn,
				               PB_TH_RST | PB_TH_ACK,
				               NULL, 0, NULL, 0);
				tcp_conn_remove(conn);
				return;
			}
			if (n > 0) {
				if ((size_t)n < payload_len) {
					/* Partial write: out-of-sync, RST the connection */
					conn->srv_seq = ntohl(tcph->ack_seq);
					tcp4_to_client(sockfd, conn,
					               PB_TH_RST | PB_TH_ACK,
					               NULL, 0, NULL, 0);
					tcp_conn_remove(conn);
					return;
				}
				conn->clt_seq = seq + (uint32_t)payload_len;
				/* Send ACK back to client */
				tcp4_to_client(sockfd, conn,
				               PB_TH_ACK,
				               NULL, 0, NULL, 0);
			}
		}

		/* Client closing */
		if ((flags & PB_TH_FIN) && conn->state == TCP_ESTABLISHED) {
			conn->clt_seq++;  /* FIN counts as one byte */
			conn->state = TCP_FIN_WAIT;
			shutdown(conn->fd, SHUT_WR);
			/* ACK the FIN */
			tcp4_to_client(sockfd, conn,
			               PB_TH_ACK,
			               NULL, 0, NULL, 0);
		}
		break;

	default:
		break;
	}
}

/* Process an incoming TCP packet from a VPN client (IPv6) */
static void handle_client_tcp6(int sockfd,
                                const struct pb_ip6hdr *ip6h, size_t ip_len,
                                struct vpn_peer *peer)
{
	const struct pb_tcphdr *tcph;
	uint16_t clt_port, dst_port;
	uint16_t tcp_hdr_len;
	uint32_t seq;
	uint8_t flags;
	const uint8_t *payload;
	size_t payload_len;
	struct tcp_conn *conn;
	struct sockaddr_in6 dst_addr;

	if (ip_len < PB_IPV6_HDR_LEN + PB_TCP_HDR_LEN)
		return;

	tcph        = (const struct pb_tcphdr *)((const uint8_t *)ip6h + PB_IPV6_HDR_LEN);
	clt_port    = ntohs(tcph->sport);
	dst_port    = ntohs(tcph->dport);
	seq         = ntohl(tcph->seq);
	flags       = tcph->flags;
	tcp_hdr_len = (uint16_t)((tcph->doff_res >> 4) * 4);

	if (tcp_hdr_len < PB_TCP_HDR_LEN || ip_len < PB_IPV6_HDR_LEN + tcp_hdr_len)
		return;

	payload     = (const uint8_t *)tcph + tcp_hdr_len;
	payload_len = ip_len - PB_IPV6_HDR_LEN - tcp_hdr_len;

	conn = tcp_conn_find(AF_INET6, ip6h->saddr, clt_port,
	                     ip6h->daddr, dst_port);

	if (flags & PB_TH_RST) {
		if (conn) tcp_conn_remove(conn);
		return;
	}

	if ((flags & PB_TH_SYN) && !(flags & PB_TH_ACK)) {
		if (conn) return;

		conn = tcp_conn_create(AF_INET6, ip6h->saddr, clt_port,
		                       ip6h->daddr, dst_port, peer);
		if (!conn) return;

		conn->clt_iss = seq;
		conn->clt_seq = seq + 1;
		if (RAND_bytes((unsigned char *)&conn->srv_iss, sizeof(conn->srv_iss)) != 1)
			conn->srv_iss = (uint32_t)(time(NULL) ^ (uintptr_t)conn);
		conn->srv_seq = conn->srv_iss; /* SYN-ACK SEQ = ISN; advances to ISN+1 after handshake */

		conn->fd = socket(AF_INET6, SOCK_STREAM, 0);
		if (conn->fd < 0) { tcp_conn_remove(conn); return; }
		set_nonblock(conn->fd);

		memset(&dst_addr, 0, sizeof(dst_addr));
		dst_addr.sin6_family = AF_INET6;
		memcpy(&dst_addr.sin6_addr, ip6h->daddr, 16);
		dst_addr.sin6_port = htons(dst_port);

		if (connect(conn->fd, (struct sockaddr *)&dst_addr,
		            sizeof(dst_addr)) < 0 && errno != EINPROGRESS) {
			tcp_conn_remove(conn); return;
		}
		conn->state = TCP_CONNECTING;
#if DEBUG
		{
			char s_src[INET6_ADDRSTRLEN], s_dst[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, ip6h->saddr, s_src, sizeof(s_src));
			inet_ntop(AF_INET6, ip6h->daddr, s_dst, sizeof(s_dst));
			fprintf(stderr, "[DBG] TCP6 SYN [%s]:%u -> [%s]:%u (connecting)\n",
			        s_src, clt_port, s_dst, dst_port);
		}
#endif
		return;
	}

	if (!conn) return;
	conn->last_active = time(NULL);

	switch (conn->state) {
	case TCP_CONNECTING:
		break;

	case TCP_SYN_ACK_SENT:
		if ((flags & PB_TH_ACK) && ntohl(tcph->ack_seq) == conn->srv_iss + 1)
			conn->state = TCP_ESTABLISHED;
		if (!(flags & PB_TH_PSH) || payload_len == 0)
			break;
		/* FALLTHROUGH */

	case TCP_ESTABLISHED:
	case TCP_FIN_WAIT:
		if (payload_len > 0) {
			ssize_t n = send(conn->fd, payload, payload_len, 0);
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				conn->srv_seq = ntohl(tcph->ack_seq);
				tcp6_to_client(sockfd, conn,
				               PB_TH_RST | PB_TH_ACK,
				               NULL, 0, NULL, 0);
				tcp_conn_remove(conn);
				return;
			}
			if (n > 0) {
				if ((size_t)n < payload_len) {
					conn->srv_seq = ntohl(tcph->ack_seq);
					tcp6_to_client(sockfd, conn,
					               PB_TH_RST | PB_TH_ACK,
					               NULL, 0, NULL, 0);
					tcp_conn_remove(conn);
					return;
				}
				conn->clt_seq = seq + (uint32_t)payload_len;
				tcp6_to_client(sockfd, conn,
				               PB_TH_ACK, NULL, 0, NULL, 0);
			}
		}
		if ((flags & PB_TH_FIN) && conn->state == TCP_ESTABLISHED) {
			conn->clt_seq++;
			conn->state = TCP_FIN_WAIT;
			shutdown(conn->fd, SHUT_WR);
			tcp6_to_client(sockfd, conn, PB_TH_ACK, NULL, 0, NULL, 0);
		}
		break;

	default:
		break;
	}
}

/* Called when a TCP NAT socket has an event */
static void handle_tcp_nat_event(int sockfd, struct tcp_conn *conn,
                                 short revents)
{
	/* Connect completion */
	if (revents & POLLOUT) {
		if (conn->state == TCP_CONNECTING) {
			int err = 0;
			socklen_t elen = sizeof(err);
			getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &elen);
			if (err != 0) {
				/* Connection refused / unreachable */
				if (conn->af == AF_INET) {
					uint8_t pkt[PB_MAX_PKT];
					int len = pb_build_rst4(
					    pkt, sizeof(pkt),
					    conn->dst_ip4.s_addr, conn->clt_vip4.s_addr,
					    conn->dst_port, conn->clt_port,
					    conn->srv_iss, conn->clt_seq,
					    PB_TH_RST | PB_TH_ACK);
					if (len > 0)
						send_inner_to_peer(sockfd, conn->peer,
						                   ETH_P_IP, pkt, (size_t)len);
				} else {
					uint8_t pkt[PB_MAX_PKT];
					int len = pb_build_rst6(
					    pkt, sizeof(pkt),
					    conn->dst_ip6.s6_addr, conn->clt_vip6.s6_addr,
					    conn->dst_port, conn->clt_port,
					    conn->srv_iss, conn->clt_seq,
					    PB_TH_RST | PB_TH_ACK);
					if (len > 0)
						send_inner_to_peer(sockfd, conn->peer,
						                   ETH_P_IPV6, pkt, (size_t)len);
				}
				tcp_conn_remove(conn);
				return;
			}

			/* Connected: send SYN-ACK to client */
			{
				uint8_t mss_opt[4];
				pb_make_mss_option(mss_opt, PB_TCP_MSS);

				if (conn->af == AF_INET) {
					tcp4_to_client(sockfd, conn,
					               PB_TH_SYN | PB_TH_ACK,
					               mss_opt, 4, NULL, 0);
				} else {
					tcp6_to_client(sockfd, conn,
					               PB_TH_SYN | PB_TH_ACK,
					               mss_opt, 4, NULL, 0);
				}
				/* SYN consumes one sequence number */
				conn->srv_seq++;
				conn->state = TCP_SYN_ACK_SENT;
			}
		}
		return;
	}

	/* Incoming data or connection close from real server */
	if (revents & (POLLIN | POLLHUP)) {
		uint8_t buf[NM_PI_BUFFER_SIZE];
		ssize_t n;

		n = read(conn->fd, buf, sizeof(buf));

		if (n <= 0) {
			/* Real server closed or error: send FIN to client */
			if (conn->af == AF_INET) {
				tcp4_to_client(sockfd, conn,
				               PB_TH_FIN | PB_TH_ACK,
				               NULL, 0, NULL, 0);
			} else {
				tcp6_to_client(sockfd, conn,
				               PB_TH_FIN | PB_TH_ACK,
				               NULL, 0, NULL, 0);
			}
			tcp_conn_remove(conn);
			return;
		}

		/* Forward data to client */
		if (conn->af == AF_INET) {
			tcp4_to_client(sockfd, conn,
			               PB_TH_PSH | PB_TH_ACK,
			               NULL, 0, buf, (size_t)n);
		} else {
			tcp6_to_client(sockfd, conn,
			               PB_TH_PSH | PB_TH_ACK,
			               NULL, 0, buf, (size_t)n);
		}
		conn->srv_seq += (uint32_t)n;
		conn->last_active = time(NULL);
	}
}

/* -----------------------------------------------------------------------
 * UDP NAT handlers
 * ----------------------------------------------------------------------- */

static void handle_client_udp4(int sockfd,
                                const struct pb_iphdr *iph, size_t ip_len,
                                struct vpn_peer *peer)
{
	uint8_t ihl = (iph->ihl_ver & 0x0f) * 4;
	const struct pb_udphdr *udph;
	uint16_t clt_port, dst_port;
	const void *payload;
	size_t payload_len;
	struct udp_flow *flow;
	struct sockaddr_in dst_addr;

	if (ihl < PB_IPV4_HDR_LEN)
		return;

	if (ip_len < (size_t)ihl + PB_UDP_HDR_LEN)
		return;

	udph        = (const struct pb_udphdr *)((const uint8_t *)iph + ihl);
	clt_port    = ntohs(udph->sport);
	dst_port    = ntohs(udph->dport);
	payload     = (const uint8_t *)udph + PB_UDP_HDR_LEN;
	payload_len = ip_len - ihl - PB_UDP_HDR_LEN;

	flow = udp_flow_find(AF_INET, &iph->saddr, clt_port,
	                     &iph->daddr, dst_port);
	if (!flow) {
		flow = udp_flow_create(AF_INET, &iph->saddr, clt_port,
		                       &iph->daddr, dst_port, peer);
		if (!flow) return;

		flow->fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (flow->fd < 0) { udp_flow_remove(flow); return; }
		set_nonblock(flow->fd);

		memset(&dst_addr, 0, sizeof(dst_addr));
		dst_addr.sin_family      = AF_INET;
		dst_addr.sin_addr.s_addr = iph->daddr;
		dst_addr.sin_port        = htons(dst_port);
		/* Connected socket: recv will get responses from this peer */
		if (connect(flow->fd, (struct sockaddr *)&dst_addr,
		            sizeof(dst_addr)) < 0) {
			udp_flow_remove(flow);
			return;
		}
#if DEBUG
		{
			char s_src[INET_ADDRSTRLEN], s_dst[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &iph->saddr, s_src, sizeof(s_src));
			inet_ntop(AF_INET, &iph->daddr, s_dst, sizeof(s_dst));
			fprintf(stderr, "[DBG] UDP4 NEW %s:%u -> %s:%u\n",
			        s_src, clt_port, s_dst, dst_port);
		}
#endif
	}

	send(flow->fd, payload, payload_len, 0);
	flow->last_active = time(NULL);
	(void)sockfd;
}

static void handle_client_udp6(int sockfd,
                                const struct pb_ip6hdr *ip6h, size_t ip_len,
                                struct vpn_peer *peer)
{
	const struct pb_udphdr *udph;
	uint16_t clt_port, dst_port;
	const void *payload;
	size_t payload_len;
	struct udp_flow *flow;
	struct sockaddr_in6 dst_addr;

	if (ip_len < PB_IPV6_HDR_LEN + PB_UDP_HDR_LEN)
		return;

	udph        = (const struct pb_udphdr *)((const uint8_t *)ip6h + PB_IPV6_HDR_LEN);
	clt_port    = ntohs(udph->sport);
	dst_port    = ntohs(udph->dport);
	payload     = (const uint8_t *)udph + PB_UDP_HDR_LEN;
	payload_len = ip_len - PB_IPV6_HDR_LEN - PB_UDP_HDR_LEN;

	flow = udp_flow_find(AF_INET6, ip6h->saddr, clt_port,
	                     ip6h->daddr, dst_port);
	if (!flow) {
		flow = udp_flow_create(AF_INET6, ip6h->saddr, clt_port,
		                       ip6h->daddr, dst_port, peer);
		if (!flow) return;

		flow->fd = socket(AF_INET6, SOCK_DGRAM, 0);
		if (flow->fd < 0) { udp_flow_remove(flow); return; }
		set_nonblock(flow->fd);

		memset(&dst_addr, 0, sizeof(dst_addr));
		dst_addr.sin6_family = AF_INET6;
		memcpy(&dst_addr.sin6_addr, ip6h->daddr, 16);
		dst_addr.sin6_port = htons(dst_port);
		if (connect(flow->fd, (struct sockaddr *)&dst_addr,
		            sizeof(dst_addr)) < 0) {
			udp_flow_remove(flow); return;
		}
#if DEBUG
		{
			char s_src[INET6_ADDRSTRLEN], s_dst[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, ip6h->saddr, s_src, sizeof(s_src));
			inet_ntop(AF_INET6, ip6h->daddr, s_dst, sizeof(s_dst));
			fprintf(stderr, "[DBG] UDP6 NEW [%s]:%u -> [%s]:%u\n",
			        s_src, clt_port, s_dst, dst_port);
		}
#endif
	}

	send(flow->fd, payload, payload_len, 0);
	flow->last_active = time(NULL);
	(void)sockfd;
}

/* Called when a UDP NAT socket is readable */
static void handle_udp_nat_event(int sockfd, struct udp_flow *flow)
{
	uint8_t buf[NM_PI_BUFFER_SIZE];
	ssize_t n;
	uint8_t pkt[PB_MAX_PKT];
	int pkt_len;

	n = recv(flow->fd, buf, sizeof(buf), 0);
	if (n <= 0) return;

	if (flow->af == AF_INET) {
		pkt_len = pb_build_udp4(pkt, sizeof(pkt),
		                        flow->dst_ip4.s_addr, flow->clt_vip4.s_addr,
		                        flow->dst_port, flow->clt_port,
		                        buf, (size_t)n);
		if (pkt_len > 0)
			send_inner_to_peer(sockfd, flow->peer,
			                   ETH_P_IP, pkt, (size_t)pkt_len);
	} else {
		pkt_len = pb_build_udp6(pkt, sizeof(pkt),
		                        flow->dst_ip6.s6_addr, flow->clt_vip6.s6_addr,
		                        flow->dst_port, flow->clt_port,
		                        buf, (size_t)n);
		if (pkt_len > 0)
			send_inner_to_peer(sockfd, flow->peer,
			                   ETH_P_IPV6, pkt, (size_t)pkt_len);
	}

	flow->last_active = time(NULL);
}

/* -----------------------------------------------------------------------
 * ICMP NAT handlers
 * ----------------------------------------------------------------------- */

/*
 * Create an ICMP socket for a flow.
 * Try SOCK_DGRAM first (no root needed on Linux 3.x+); fall back to
 * SOCK_RAW if it's not permitted.
 * Returns the socket fd on success, or -1 on failure.
 * *mapped_id is set to the ICMP ID the OS assigned to this socket.
 */
static int icmp_socket_create(int af, uint16_t *mapped_id, int *p_sock_type)
{
	int fd;
	int proto = (af == AF_INET6) ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

	/* Try unprivileged SOCK_DGRAM first */
	fd = socket(af, SOCK_DGRAM, proto);
	if (fd >= 0) {
		*p_sock_type = SOCK_DGRAM;
	} else {
		fd = socket(af, SOCK_RAW, proto);
		if (fd >= 0)
			*p_sock_type = SOCK_RAW;
	}
	if (fd < 0)
		return -1;

	set_nonblock(fd);

	/* On SOCK_DGRAM ICMP the OS assigns the ID via the ephemeral port.
	 * On SOCK_RAW we own the whole ICMP datagram and choose the ID
	 * ourselves; for simplicity we still read back the local port
	 * via getsockname as a unique ID generator. */
	if (af == AF_INET) {
		struct sockaddr_in sin;
		socklen_t slen = sizeof(sin);
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = INADDR_ANY;
		sin.sin_port = 0;
		bind(fd, (struct sockaddr *)&sin, sizeof(sin));
		slen = sizeof(sin);
		getsockname(fd, (struct sockaddr *)&sin, &slen);
		*mapped_id = ntohs(sin.sin_port);
	} else {
		struct sockaddr_in6 sin6;
		socklen_t slen = sizeof(sin6);
		memset(&sin6, 0, sizeof(sin6));
		sin6.sin6_family = AF_INET6;
		sin6.sin6_port = 0;
		bind(fd, (struct sockaddr *)&sin6, sizeof(sin6));
		slen = sizeof(sin6);
		getsockname(fd, (struct sockaddr *)&sin6, &slen);
		*mapped_id = ntohs(sin6.sin6_port);
	}

#ifdef __linux__
	/* For ICMPv6 raw sockets, filter to only echo replies */
	if (af == AF_INET6) {
		struct icmp6_filter filt;
		ICMP6_FILTER_SETBLOCKALL(&filt);
		ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filt);
		setsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filt, sizeof(filt));
	}
#endif

	return fd;
}

static void handle_client_icmp4(int sockfd,
                                 const struct pb_iphdr *iph, size_t ip_len,
                                 struct vpn_peer *peer)
{
	uint8_t ihl = (iph->ihl_ver & 0x0f) * 4;
	const struct pb_icmphdr *icmph;
	uint16_t orig_id, orig_seq;
	struct icmp_flow *flow;
	struct sockaddr_in dst_addr;
	/* Buffer for ICMP header + data to send */
	uint8_t icmp_buf[NM_PI_BUFFER_SIZE];
	size_t icmp_data_len;

	if (ihl < PB_IPV4_HDR_LEN)
		return;

	if (ip_len < (size_t)ihl + PB_ICMP_HDR_LEN)
		return;

	icmph = (const struct pb_icmphdr *)((const uint8_t *)iph + ihl);

	/* Only handle echo requests */
	if (icmph->type != PB_ICMP_ECHO_REQUEST)
		return;

	orig_id  = ntohs(icmph->id);
	orig_seq = ntohs(icmph->seq);
	icmp_data_len = ip_len - ihl - PB_ICMP_HDR_LEN;

	/* Guard against stack overflow: icmp_buf is NM_PI_BUFFER_SIZE bytes */
	if (icmp_data_len > sizeof(icmp_buf) - PB_ICMP_HDR_LEN)
		return;

	flow = icmp_flow_find(AF_INET, &iph->saddr, &iph->daddr, orig_id);
	if (!flow) {
		uint16_t mapped_id = 0;
		int sock_type = SOCK_DGRAM;
		int fd = icmp_socket_create(AF_INET, &mapped_id, &sock_type);
		if (fd < 0) return;

		flow = icmp_flow_create(AF_INET, &iph->saddr, &iph->daddr,
		                        orig_id, peer);
		if (!flow) { close(fd); return; }
		flow->fd        = fd;
		flow->mapped_id = mapped_id;
		flow->sock_type = sock_type;
	}

	/* Rewrite the ICMP header with mapped_id */
	memset(&icmp_buf, 0, PB_ICMP_HDR_LEN);
	{
		struct pb_icmphdr *out = (struct pb_icmphdr *)icmp_buf;
		out->type  = PB_ICMP_ECHO_REQUEST;
		out->code  = 0;
		out->id    = htons(flow->mapped_id);
		out->seq   = htons(orig_seq);
		/* Copy ICMP data after the header */
		if (icmp_data_len > 0)
			memcpy(icmp_buf + PB_ICMP_HDR_LEN,
			       (const uint8_t *)icmph + PB_ICMP_HDR_LEN,
			       icmp_data_len);
		out->check = 0;
		out->check = pb_icmp_checksum(icmp_buf,
		                              (int)(PB_ICMP_HDR_LEN + icmp_data_len));
	}

	memset(&dst_addr, 0, sizeof(dst_addr));
	dst_addr.sin_family      = AF_INET;
	dst_addr.sin_addr.s_addr = iph->daddr;
	dst_addr.sin_port        = htons(flow->mapped_id); /* id for SOCK_DGRAM */

	sendto(flow->fd, icmp_buf, PB_ICMP_HDR_LEN + icmp_data_len, 0,
	       (struct sockaddr *)&dst_addr, sizeof(dst_addr));
#if DEBUG
	{
		char s_dst[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &iph->daddr, s_dst, sizeof(s_dst));
		fprintf(stderr, "[DBG] ICMP4 ECHO id=%u seq=%u -> %s (mapped_id=%u)\n",
		        orig_id, orig_seq, s_dst, flow->mapped_id);
	}
#endif
	flow->last_active = time(NULL);
	(void)sockfd;
}

static void handle_client_icmpv6(int sockfd,
                                  const struct pb_ip6hdr *ip6h, size_t ip_len,
                                  struct vpn_peer *peer)
{
	const struct pb_icmp6hdr *icmph;
	uint16_t orig_id, orig_seq;
	struct icmp_flow *flow;
	struct sockaddr_in6 dst_addr;
	uint8_t icmp_buf[NM_PI_BUFFER_SIZE];
	size_t icmp_data_len;

	if (ip_len < PB_IPV6_HDR_LEN + PB_ICMP_HDR_LEN)
		return;

	icmph = (const struct pb_icmp6hdr *)((const uint8_t *)ip6h + PB_IPV6_HDR_LEN);

	if (icmph->type != PB_ICMPV6_ECHO_REQUEST)
		return;

	orig_id  = ntohs(icmph->id);
	orig_seq = ntohs(icmph->seq);
	icmp_data_len = ip_len - PB_IPV6_HDR_LEN - PB_ICMP_HDR_LEN;

	/* Guard against stack overflow: icmp_buf is NM_PI_BUFFER_SIZE bytes */
	if (icmp_data_len > sizeof(icmp_buf) - PB_ICMP_HDR_LEN)
		return;

	flow = icmp_flow_find(AF_INET6, ip6h->saddr, ip6h->daddr, orig_id);
	if (!flow) {
		uint16_t mapped_id = 0;
		int sock_type = SOCK_DGRAM;
		int fd = icmp_socket_create(AF_INET6, &mapped_id, &sock_type);
		if (fd < 0) return;

		flow = icmp_flow_create(AF_INET6, ip6h->saddr, ip6h->daddr,
		                        orig_id, peer);
		if (!flow) { close(fd); return; }
		flow->fd        = fd;
		flow->mapped_id = mapped_id;
		flow->sock_type = sock_type;
	}

	{
		struct pb_icmp6hdr *out = (struct pb_icmp6hdr *)icmp_buf;
		out->type  = PB_ICMPV6_ECHO_REQUEST;
		out->code  = 0;
		out->id    = htons(flow->mapped_id);
		out->seq   = htons(orig_seq);
		if (icmp_data_len > 0)
			memcpy(icmp_buf + PB_ICMP_HDR_LEN,
			       (const uint8_t *)icmph + PB_ICMP_HDR_LEN,
			       icmp_data_len);
		out->check = 0;
		out->check = pb_icmpv6_checksum(ip6h->saddr, ip6h->daddr,
		                                icmp_buf,
		                                (int)(PB_ICMP_HDR_LEN + icmp_data_len));
	}

	memset(&dst_addr, 0, sizeof(dst_addr));
	dst_addr.sin6_family = AF_INET6;
	memcpy(&dst_addr.sin6_addr, ip6h->daddr, 16);
	dst_addr.sin6_port = htons(flow->mapped_id);

	sendto(flow->fd, icmp_buf, PB_ICMP_HDR_LEN + icmp_data_len, 0,
	       (struct sockaddr *)&dst_addr, sizeof(dst_addr));
#if DEBUG
	{
		char s_dst[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, ip6h->daddr, s_dst, sizeof(s_dst));
		fprintf(stderr, "[DBG] ICMP6 ECHO id=%u seq=%u -> [%s] (mapped_id=%u)\n",
		        orig_id, orig_seq, s_dst, flow->mapped_id);
	}
#endif
	flow->last_active = time(NULL);
	(void)sockfd;
}

/* Called when an ICMP NAT socket is readable */
static void handle_icmp_nat_event(int sockfd, struct icmp_flow *flow)
{
	uint8_t buf[NM_PI_BUFFER_SIZE];
	ssize_t n;
	uint8_t pkt[PB_MAX_PKT];
	int pkt_len;

	n = recv(flow->fd, buf, sizeof(buf), 0);
	if (n < (ssize_t)PB_ICMP_HDR_LEN)
		return;

	/* buf contains the ICMP/ICMPv6 header (id=mapped_id) + data.
	 * SOCK_DGRAM: kernel strips the IP header; first byte is the ICMP type.
	 * SOCK_RAW: kernel delivers the full IP packet; skip the IP header. */
	{
		const uint8_t *icmp_start = buf;
		size_t icmp_total = (size_t)n;

		if (flow->af == AF_INET && flow->sock_type == SOCK_RAW) {
			uint8_t ihl = (buf[0] & 0x0f) * 4;
			if (ihl < PB_IPV4_HDR_LEN || (size_t)n <= ihl)
				return;
			icmp_start = buf + ihl;
			icmp_total = (size_t)n - ihl;
		}

		if (icmp_total < PB_ICMP_HDR_LEN)
			return;

		{
			const struct pb_icmphdr *rep = (const struct pb_icmphdr *)icmp_start;

			/* Validate that we received the expected reply type */
			if (flow->af == AF_INET && rep->type != PB_ICMP_ECHO_REPLY)
				return;
			if (flow->af == AF_INET6 &&
			    rep->type != (uint8_t)PB_ICMPV6_ECHO_REPLY)
				return;

			uint16_t rep_seq  = ntohs(rep->seq);
			const void *data  = icmp_start + PB_ICMP_HDR_LEN;
			size_t data_len   = icmp_total - PB_ICMP_HDR_LEN;

			if (flow->af == AF_INET) {
				pkt_len = pb_build_icmp4_reply(
				    pkt, sizeof(pkt),
				    flow->dst_ip4.s_addr, flow->clt_vip4.s_addr,
				    flow->orig_id, rep_seq,
				    data, data_len);
				if (pkt_len > 0)
					send_inner_to_peer(sockfd, flow->peer,
					                   ETH_P_IP, pkt, (size_t)pkt_len);
			} else {
				pkt_len = pb_build_icmpv6_reply(
				    pkt, sizeof(pkt),
				    flow->dst_ip6.s6_addr, flow->clt_vip6.s6_addr,
				    flow->orig_id, rep_seq,
				    data, data_len);
				if (pkt_len > 0)
					send_inner_to_peer(sockfd, flow->peer,
					                   ETH_P_IPV6, pkt, (size_t)pkt_len);
			}
		}
	}

	flow->last_active = time(NULL);
}

/* -----------------------------------------------------------------------
 * Main VPN packet dispatcher
 * ----------------------------------------------------------------------- */

static void dispatch_ipv4(int sockfd, const uint8_t *pkt, size_t len,
                           struct vpn_peer *peer)
{
	const struct pb_iphdr *iph = (const struct pb_iphdr *)pkt;
	uint8_t ihl;
	struct in_addr dst;

	if (len < PB_IPV4_HDR_LEN)
		return;

	ihl = (iph->ihl_ver & 0x0f) * 4;
	if (ihl < PB_IPV4_HDR_LEN || len < (size_t)ihl)
		return;

	dst.s_addr = iph->daddr;

	/* Check for inter-client destination */
	{
		struct vpn_peer *dp = resolve_peer_ipv4(&dst);
		if (dp && dp != peer) {
			forward_to_peer(sockfd, dp, ETH_P_IP, pkt, len);
			return;
		}
	}

	/* External NAT */
	switch (iph->protocol) {
	case IPPROTO_TCP:
		handle_client_tcp4(sockfd, iph, len, peer);
		break;
	case IPPROTO_UDP:
		handle_client_udp4(sockfd, iph, len, peer);
		break;
	case IPPROTO_ICMP:
		handle_client_icmp4(sockfd, iph, len, peer);
		break;
	default:
		break;
	}
}

static void dispatch_ipv6(int sockfd, const uint8_t *pkt, size_t len,
                           struct vpn_peer *peer)
{
	const struct pb_ip6hdr *ip6h = (const struct pb_ip6hdr *)pkt;
	struct in6_addr dst;

	if (len < PB_IPV6_HDR_LEN)
		return;

	memcpy(&dst, ip6h->daddr, 16);

	{
		struct vpn_peer *dp = resolve_peer_ipv6(&dst);
		if (dp && dp != peer) {
			forward_to_peer(sockfd, dp, ETH_P_IPV6, pkt, len);
			return;
		}
	}

	switch (ip6h->nexthdr) {
	case IPPROTO_TCP:
		handle_client_tcp6(sockfd, ip6h, len, peer);
		break;
	case IPPROTO_UDP:
		handle_client_udp6(sockfd, ip6h, len, peer);
		break;
	case IPPROTO_ICMPV6:
		handle_client_icmpv6(sockfd, ip6h, len, peer);
		break;
	default:
		break;
	}
}

/* -----------------------------------------------------------------------
 * Receive one VPN UDP packet
 * ----------------------------------------------------------------------- */

static void network_receiving(int sockfd)
{
	char read_buf[NM_CRYPTO_BUF_SIZE], crypt_buf[NM_CRYPTO_BUF_SIZE];
	struct minivtun_msg *nmsg;
	struct sockaddr_storage real_peer;
	socklen_t real_peer_len = sizeof(real_peer);
	void *out_data;
	size_t out_dlen;
	int rc;
	unsigned short af;
	struct vpn_peer *peer;

	rc = (int)recvfrom(sockfd, read_buf, sizeof(read_buf), 0,
	                   (struct sockaddr *)&real_peer, &real_peer_len);
	if (rc <= 0)
		return;

	out_data = crypt_buf;
	out_dlen = (size_t)rc;
	netmsg_to_local(read_buf, &out_data, &out_dlen);
	nmsg = out_data;

	if (out_dlen < MINIVTUN_MSG_BASIC_HLEN)
		return;

	if (memcmp(nmsg->hdr.auth_key, config.crypto_key,
	           sizeof(nmsg->hdr.auth_key)) != 0)
		return;

	switch (nmsg->hdr.opcode) {

	case MINIVTUN_MSG_KEEPALIVE: {
		/* Copy out of packed struct to avoid unaligned pointer warnings */
		struct in_addr ka_in4;
		struct in6_addr ka_in6;
		if (out_dlen < MINIVTUN_MSG_BASIC_HLEN + sizeof(nmsg->keepalive))
			break;
		memcpy(&ka_in4, &nmsg->keepalive.loc_tun_in,  sizeof(ka_in4));
		memcpy(&ka_in6, &nmsg->keepalive.loc_tun_in6, sizeof(ka_in6));
#if DEBUG
		{
			char s4[INET_ADDRSTRLEN], s6[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET,  &ka_in4, s4, sizeof(s4));
			inet_ntop(AF_INET6, &ka_in6, s6, sizeof(s6));
			fprintf(stderr, "[DBG] KEEPALIVE from peer vip4=%s vip6=%s\n", s4, s6);
		}
#endif
		if (is_valid_unicast_in(&ka_in4)) {
			peer = peer_get_or_create(AF_INET, &ka_in4, &real_peer);
			if (peer) peer->last_active = time(NULL);
		}
		if (is_valid_unicast_in6(&ka_in6)) {
			peer = peer_get_or_create(AF_INET6, &ka_in6, &real_peer);
			if (peer) peer->last_active = time(NULL);
		}
		break;
	}

	case MINIVTUN_MSG_IPDATA: {
		size_t ip_dlen;
		const uint8_t *ip_pkt;

		if (nmsg->ipdata.proto == htons(ETH_P_IP)) {
			af = AF_INET;
			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 20)
				break;
		} else if (nmsg->ipdata.proto == htons(ETH_P_IPV6)) {
			af = AF_INET6;
			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 40)
				break;
		} else {
			break;
		}

		ip_dlen = ntohs(nmsg->ipdata.ip_dlen);
		if (out_dlen - MINIVTUN_MSG_IPDATA_OFFSET < ip_dlen)
			break;

		ip_pkt = (const uint8_t *)nmsg + MINIVTUN_MSG_IPDATA_OFFSET;

		/* Identify / update the sending peer */
		if (af == AF_INET) {
			/* Source IP is at offset 12 in IPv4 header */
			struct in_addr src;
			memcpy(&src, ip_pkt + 12, 4);
			peer = peer_get_or_create(AF_INET, &src, &real_peer);
		} else {
			/* Source IP is at offset 8 in IPv6 header */
			struct in6_addr src;
			memcpy(&src, ip_pkt + 8, 16);
			peer = peer_get_or_create(AF_INET6, &src, &real_peer);
		}
		if (!peer)
			break;

		peer->last_active = time(NULL);

#if DEBUG
		if (af == AF_INET)
			dbg_print_ip4("CLIENT->SERVER", ip_pkt, ip_dlen);
		else
			dbg_print_ip6("CLIENT->SERVER", ip_pkt, ip_dlen);
#endif

		if (af == AF_INET)
			dispatch_ipv4(sockfd, ip_pkt, ip_dlen, peer);
		else
			dispatch_ipv6(sockfd, ip_pkt, ip_dlen, peer);
		break;
	}

	default:
		break;
	}
}

/* -----------------------------------------------------------------------
 * Main server entry point
 * ----------------------------------------------------------------------- */

int run_server(const char *loc_addr_pair)
{
	struct sockaddr_inx loc_addr;
	int sockfd;
	char s_loc_addr[50];
	time_t last_walk;
	static struct pollfd pfds[MAX_POLL_FDS];

	if (get_sockaddr_inx_pair(loc_addr_pair, &loc_addr) < 0) {
		fprintf(stderr, "*** Cannot resolve address pair '%s'.\n", loc_addr_pair);
		return -1;
	}

	inet_ntop(loc_addr.sa.sa_family, addr_of_sockaddr(&loc_addr),
	          s_loc_addr, sizeof(s_loc_addr));
	printf("Userspace NAT server on %s:%u\n",
	       s_loc_addr, ntohs(port_of_sockaddr(&loc_addr)));

	natmap_init();

	sockfd = socket(loc_addr.sa.sa_family, SOCK_DGRAM, IPPROTO_UDP);
	if (sockfd < 0) {
		fprintf(stderr, "*** socket(): %s\n", strerror(errno));
		return -1;
	}
	if (bind(sockfd, (struct sockaddr *)&loc_addr,
	         sizeof_sockaddr(&loc_addr)) < 0) {
		fprintf(stderr, "*** bind(): %s\n", strerror(errno));
		close(sockfd);
		return -1;
	}
	set_nonblock(sockfd);

	if (config.in_background)
		do_daemonize();
	if (config.pid_file) {
		FILE *fp = fopen(config.pid_file, "w");
		if (fp) { fprintf(fp, "%d\n", (int)getpid()); fclose(fp); }
	}

	last_walk = time(NULL);

	for (;;) {
		int nfds, nat_fds, rc, i;
		time_t now;

		/* [0] always the VPN UDP socket */
		pfds[0].fd      = sockfd;
		pfds[0].events  = POLLIN;
		pfds[0].revents = 0;

		nat_fds = natmap_build_pollfd(pfds + 1, MAX_POLL_FDS - 1);
		if (nat_fds < 0) nat_fds = 0;
		nfds = 1 + nat_fds;

		rc = poll(pfds, (nfds_t)nfds, 2000);
		if (rc < 0 && errno != EINTR) {
			fprintf(stderr, "*** poll(): %s\n", strerror(errno));
			break;
		}

		now = time(NULL);

		if (rc > 0) {
			if (pfds[0].revents & POLLIN)
				network_receiving(sockfd);

			for (i = 1; i < nfds; i++) {
				short rev = pfds[i].revents;
				if (!rev) continue;
				int fd = pfds[i].fd;

				/* Identify NAT entry by fd */
				{
					struct tcp_conn *tc = tcp_conn_find_by_fd(fd);
					if (tc) {
						handle_tcp_nat_event(sockfd, tc, rev);
						continue;
					}
				}
				{
					struct udp_flow *uf = udp_flow_find_by_fd(fd);
					if (uf) {
						handle_udp_nat_event(sockfd, uf);
						continue;
					}
				}
				{
					struct icmp_flow *ic = icmp_flow_find_by_fd(fd);
					if (ic) {
						handle_icmp_nat_event(sockfd, ic);
						continue;
					}
				}
			}
		}

		if (now - last_walk >= 5) {
			natmap_expire(now);
			peer_walk(sockfd, now, config.keepalive_timeo,
			          config.reconnect_timeo);
			last_walk = now;
		}
	}

	close(sockfd);
	return 0;
}
