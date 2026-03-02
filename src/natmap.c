/*
 * natmap.c - NAT connection tracking and VPN peer table
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <openssl/rand.h>

#include "jhash.h"
#include "minivtun.h"
#include "natmap.h"

/* -----------------------------------------------------------------------
 * Hash table sizes (must be powers of 2)
 * ----------------------------------------------------------------------- */

#define PEER_HASH_SIZE   16
#define TCP_HASH_SIZE    32
#define UDP_HASH_SIZE    32
#define ICMP_HASH_SIZE   16

static struct list_head peer_table[PEER_HASH_SIZE];
static struct list_head tcp_table[TCP_HASH_SIZE];
static struct list_head udp_table[UDP_HASH_SIZE];
static struct list_head icmp_table[ICMP_HASH_SIZE];

static uint32_t hash_seed;

/* -----------------------------------------------------------------------
 * Initialization
 * ----------------------------------------------------------------------- */

void natmap_init(void)
{
	int i;
	for (i = 0; i < PEER_HASH_SIZE; i++) INIT_LIST_HEAD(&peer_table[i]);
	for (i = 0; i < TCP_HASH_SIZE;  i++) INIT_LIST_HEAD(&tcp_table[i]);
	for (i = 0; i < UDP_HASH_SIZE;  i++) INIT_LIST_HEAD(&udp_table[i]);
	for (i = 0; i < ICMP_HASH_SIZE; i++) INIT_LIST_HEAD(&icmp_table[i]);
	if (RAND_bytes((unsigned char *)&hash_seed, sizeof(hash_seed)) != 1)
		hash_seed = (uint32_t)time(NULL);  /* fallback on RAND failure */
}

/* -----------------------------------------------------------------------
 * Address comparison helpers
 * ----------------------------------------------------------------------- */

static inline int vip_equal(int af, const void *a, const void *b)
{
	if (af == AF_INET)
		return ((const struct in_addr *)a)->s_addr ==
		       ((const struct in_addr *)b)->s_addr;
	else {
		const uint32_t *x = (const uint32_t *)a;
		const uint32_t *y = (const uint32_t *)b;
		return x[0]==y[0] && x[1]==y[1] && x[2]==y[2] && x[3]==y[3];
	}
}

/* -----------------------------------------------------------------------
 * VPN peer table
 * ----------------------------------------------------------------------- */

static uint32_t peer_hash4(const struct in_addr *vip)
{
	return jhash_1word(vip->s_addr, hash_seed);
}

static uint32_t peer_hash6(const struct in6_addr *vip)
{
	const uint32_t *a = (const uint32_t *)vip;
	return jhash_3words(a[0] ^ a[1], a[2], a[3], hash_seed);
}

struct vpn_peer *peer_find_by_vip4(const struct in_addr *vip)
{
	struct list_head *chain = &peer_table[peer_hash4(vip) & (PEER_HASH_SIZE-1)];
	struct vpn_peer *p;

	list_for_each_entry(p, chain, node) {
		if (p->af == AF_INET && p->vip4.s_addr == vip->s_addr)
			return p;
	}
	return NULL;
}

struct vpn_peer *peer_find_by_vip6(const struct in6_addr *vip)
{
	struct list_head *chain = &peer_table[peer_hash6(vip) & (PEER_HASH_SIZE-1)];
	struct vpn_peer *p;

	list_for_each_entry(p, chain, node) {
		if (p->af == AF_INET6 && vip_equal(AF_INET6, &p->vip6, vip))
			return p;
	}
	return NULL;
}

struct vpn_peer *peer_get_or_create(int af, const void *vip,
                                    const struct sockaddr_storage *udp_addr)
{
	struct vpn_peer *p;
	struct list_head *chain;
	char s[50];

	if (af == AF_INET) {
		p = peer_find_by_vip4((const struct in_addr *)vip);
		chain = &peer_table[peer_hash4((const struct in_addr *)vip) & (PEER_HASH_SIZE-1)];
	} else {
		p = peer_find_by_vip6((const struct in6_addr *)vip);
		chain = &peer_table[peer_hash6((const struct in6_addr *)vip) & (PEER_HASH_SIZE-1)];
	}

	if (p) {
		/* Update real endpoint if client roamed */
		if (memcmp(&p->udp_addr, udp_addr, sizeof(*udp_addr)) != 0)
			memcpy(&p->udp_addr, udp_addr, sizeof(*udp_addr));
		return p;
	}

	p = malloc(sizeof(*p));
	if (!p) return NULL;

	memset(p, 0, sizeof(*p));
	p->af = af;
	if (af == AF_INET)
		p->vip4 = *(const struct in_addr *)vip;
	else
		p->vip6 = *(const struct in6_addr *)vip;
	memcpy(&p->udp_addr, udp_addr, sizeof(*udp_addr));
	p->last_active = time(NULL);
	list_add_tail(&p->node, chain);

	inet_ntop(af, vip, s, sizeof(s));
	printf("New VPN peer [%s]\n", s);

	return p;
}

void peer_update_addr(struct vpn_peer *p,
                      const struct sockaddr_storage *udp_addr)
{
	memcpy(&p->udp_addr, udp_addr, sizeof(*udp_addr));
}

/* Remove all TCP/UDP/ICMP NAT entries that reference the given peer.
 * Must be called before free()'ing a vpn_peer to avoid dangling pointers. */
static void remove_nat_entries_for_peer(struct vpn_peer *p)
{
	int i;
	struct tcp_conn  *c,  *ctmp;
	struct udp_flow  *uf, *utmp;
	struct icmp_flow *ic, *itmp;

	for (i = 0; i < TCP_HASH_SIZE; i++)
		list_for_each_entry_safe(c, ctmp, &tcp_table[i], node)
			if (c->peer == p) tcp_conn_remove(c);

	for (i = 0; i < UDP_HASH_SIZE; i++)
		list_for_each_entry_safe(uf, utmp, &udp_table[i], node)
			if (uf->peer == p) udp_flow_remove(uf);

	for (i = 0; i < ICMP_HASH_SIZE; i++)
		list_for_each_entry_safe(ic, itmp, &icmp_table[i], node)
			if (ic->peer == p) icmp_flow_remove(ic);
}

void peer_walk(int sockfd, time_t now, unsigned keepalive_timeo,
               unsigned peer_timeo)
{
	int i;
	char in_data[64], crypt_buffer[128];
	struct minivtun_msg *nmsg = (struct minivtun_msg *)in_data;
	void *out_msg;
	size_t out_len;

	/* Build keepalive payload once */
	nmsg->hdr.opcode = MINIVTUN_MSG_KEEPALIVE;
	memset(nmsg->hdr.rsv, 0, sizeof(nmsg->hdr.rsv));
	memcpy(nmsg->hdr.auth_key, config.crypto_key, sizeof(nmsg->hdr.auth_key));
	nmsg->keepalive.loc_tun_in  = config.local_tun_in;
	nmsg->keepalive.loc_tun_in6 = config.local_tun_in6;

	out_msg = crypt_buffer;
	out_len = MINIVTUN_MSG_BASIC_HLEN + sizeof(nmsg->keepalive);
	local_to_netmsg(nmsg, &out_msg, &out_len);

	for (i = 0; i < PEER_HASH_SIZE; i++) {
		struct vpn_peer *p, *tmp;

		list_for_each_entry_safe(p, tmp, &peer_table[i], node) {
			char s[50];

			if (now - p->last_active > (time_t)peer_timeo) {
				inet_ntop(p->af,
				          p->af == AF_INET ? (void *)&p->vip4 : (void *)&p->vip6,
				          s, sizeof(s));
				printf("Peer [%s] timed out, removing.\n", s);
				/* Remove all NAT entries that reference this peer
				 * before freeing it to prevent use-after-free. */
				remove_nat_entries_for_peer(p);
				list_del(&p->node);
				free(p);
				continue;
			}

			/* Send keepalive */
			if (now - p->last_active > (time_t)keepalive_timeo) {
				socklen_t alen = (p->udp_addr.ss_family == AF_INET6)
				                 ? sizeof(struct sockaddr_in6)
				                 : sizeof(struct sockaddr_in);
				sendto(sockfd, out_msg, out_len, 0,
				       (struct sockaddr *)&p->udp_addr, alen);
			}
		}
	}
}

/* -----------------------------------------------------------------------
 * TCP connection table
 * ----------------------------------------------------------------------- */

static uint32_t tcp_hash4(uint32_t clt_vip, uint16_t clt_port,
                           uint32_t dst_ip, uint16_t dst_port)
{
	return jhash_3words(clt_vip,
	                    ((uint32_t)clt_port << 16) | dst_port,
	                    dst_ip, hash_seed);
}

static uint32_t tcp_hash6(const uint8_t *clt_vip, uint16_t clt_port,
                           const uint8_t *dst_ip, uint16_t dst_port)
{
	const uint32_t *c = (const uint32_t *)clt_vip;
	const uint32_t *d = (const uint32_t *)dst_ip;
	return jhash_3words(c[0] ^ c[1] ^ c[2] ^ c[3],
	                    d[0] ^ d[1] ^ d[2] ^ d[3],
	                    ((uint32_t)clt_port << 16) | dst_port,
	                    hash_seed);
}

struct tcp_conn *tcp_conn_find(int af,
                               const void *clt_vip, uint16_t clt_port,
                               const void *dst_ip,  uint16_t dst_port)
{
	uint32_t h;
	struct list_head *chain;
	struct tcp_conn *c;

	if (af == AF_INET) {
		h = tcp_hash4(((const struct in_addr *)clt_vip)->s_addr, clt_port,
		              ((const struct in_addr *)dst_ip)->s_addr, dst_port);
	} else {
		h = tcp_hash6((const uint8_t *)clt_vip, clt_port,
		              (const uint8_t *)dst_ip, dst_port);
	}
	chain = &tcp_table[h & (TCP_HASH_SIZE-1)];

	list_for_each_entry(c, chain, node) {
		if (c->af != af || c->clt_port != clt_port || c->dst_port != dst_port)
			continue;
		if (!vip_equal(af, af == AF_INET ? (void *)&c->clt_vip4 : (void *)&c->clt_vip6, clt_vip))
			continue;
		if (!vip_equal(af, af == AF_INET ? (void *)&c->dst_ip4 : (void *)&c->dst_ip6, dst_ip))
			continue;
		return c;
	}
	return NULL;
}

struct tcp_conn *tcp_conn_create(int af,
                                 const void *clt_vip, uint16_t clt_port,
                                 const void *dst_ip,  uint16_t dst_port,
                                 struct vpn_peer *peer)
{
	uint32_t h;
	struct list_head *chain;
	struct tcp_conn *c;

	c = malloc(sizeof(*c));
	if (!c) return NULL;
	memset(c, 0, sizeof(*c));

	c->af       = af;
	c->clt_port = clt_port;
	c->dst_port = dst_port;
	c->peer     = peer;
	c->fd       = -1;
	c->state    = TCP_CONNECTING;
	c->last_active = time(NULL);

	if (af == AF_INET) {
		c->clt_vip4 = *(const struct in_addr *)clt_vip;
		c->dst_ip4  = *(const struct in_addr *)dst_ip;
		h = tcp_hash4(c->clt_vip4.s_addr, clt_port,
		              c->dst_ip4.s_addr, dst_port);
	} else {
		c->clt_vip6 = *(const struct in6_addr *)clt_vip;
		c->dst_ip6  = *(const struct in6_addr *)dst_ip;
		h = tcp_hash6((const uint8_t *)&c->clt_vip6, clt_port,
		              (const uint8_t *)&c->dst_ip6, dst_port);
	}
	chain = &tcp_table[h & (TCP_HASH_SIZE-1)];
	list_add_tail(&c->node, chain);

	return c;
}

void tcp_conn_remove(struct tcp_conn *c)
{
	if (c->fd >= 0) close(c->fd);
	list_del(&c->node);
	free(c);
}

struct tcp_conn *tcp_conn_find_by_fd(int fd)
{
	int i;
	struct tcp_conn *c;

	for (i = 0; i < TCP_HASH_SIZE; i++)
		list_for_each_entry(c, &tcp_table[i], node)
			if (c->fd == fd) return c;
	return NULL;
}

/* -----------------------------------------------------------------------
 * UDP flow table
 * ----------------------------------------------------------------------- */

static uint32_t udp_hash4(uint32_t clt_vip, uint16_t clt_port,
                           uint32_t dst_ip, uint16_t dst_port)
{
	return jhash_3words(clt_vip,
	                    ((uint32_t)clt_port << 16) | dst_port,
	                    dst_ip, hash_seed);
}

static uint32_t udp_hash6(const uint8_t *clt_vip, uint16_t clt_port,
                           const uint8_t *dst_ip, uint16_t dst_port)
{
	const uint32_t *c = (const uint32_t *)clt_vip;
	const uint32_t *d = (const uint32_t *)dst_ip;
	return jhash_3words(c[0] ^ c[1] ^ c[2] ^ c[3],
	                    d[0] ^ d[1] ^ d[2] ^ d[3],
	                    ((uint32_t)clt_port << 16) | dst_port,
	                    hash_seed);
}

struct udp_flow *udp_flow_find(int af,
                               const void *clt_vip, uint16_t clt_port,
                               const void *dst_ip,  uint16_t dst_port)
{
	uint32_t h;
	struct list_head *chain;
	struct udp_flow *f;

	if (af == AF_INET) {
		h = udp_hash4(((const struct in_addr *)clt_vip)->s_addr, clt_port,
		              ((const struct in_addr *)dst_ip)->s_addr, dst_port);
	} else {
		h = udp_hash6((const uint8_t *)clt_vip, clt_port,
		              (const uint8_t *)dst_ip, dst_port);
	}
	chain = &udp_table[h & (UDP_HASH_SIZE-1)];

	list_for_each_entry(f, chain, node) {
		if (f->af != af || f->clt_port != clt_port || f->dst_port != dst_port)
			continue;
		if (!vip_equal(af, af == AF_INET ? (void *)&f->clt_vip4 : (void *)&f->clt_vip6, clt_vip))
			continue;
		if (!vip_equal(af, af == AF_INET ? (void *)&f->dst_ip4 : (void *)&f->dst_ip6, dst_ip))
			continue;
		return f;
	}
	return NULL;
}

struct udp_flow *udp_flow_create(int af,
                                 const void *clt_vip, uint16_t clt_port,
                                 const void *dst_ip,  uint16_t dst_port,
                                 struct vpn_peer *peer)
{
	uint32_t h;
	struct list_head *chain;
	struct udp_flow *f;

	f = malloc(sizeof(*f));
	if (!f) return NULL;
	memset(f, 0, sizeof(*f));

	f->af       = af;
	f->clt_port = clt_port;
	f->dst_port = dst_port;
	f->peer     = peer;
	f->fd       = -1;
	f->last_active = time(NULL);

	if (af == AF_INET) {
		f->clt_vip4 = *(const struct in_addr *)clt_vip;
		f->dst_ip4  = *(const struct in_addr *)dst_ip;
		h = udp_hash4(f->clt_vip4.s_addr, clt_port,
		              f->dst_ip4.s_addr, dst_port);
	} else {
		f->clt_vip6 = *(const struct in6_addr *)clt_vip;
		f->dst_ip6  = *(const struct in6_addr *)dst_ip;
		h = udp_hash6((const uint8_t *)&f->clt_vip6, clt_port,
		              (const uint8_t *)&f->dst_ip6, dst_port);
	}
	chain = &udp_table[h & (UDP_HASH_SIZE-1)];
	list_add_tail(&f->node, chain);

	return f;
}

void udp_flow_remove(struct udp_flow *f)
{
	if (f->fd >= 0) close(f->fd);
	list_del(&f->node);
	free(f);
}

struct udp_flow *udp_flow_find_by_fd(int fd)
{
	int i;
	struct udp_flow *f;

	for (i = 0; i < UDP_HASH_SIZE; i++)
		list_for_each_entry(f, &udp_table[i], node)
			if (f->fd == fd) return f;
	return NULL;
}

/* -----------------------------------------------------------------------
 * ICMP flow table
 * ----------------------------------------------------------------------- */

static uint32_t icmp_hash4(uint32_t clt_vip, uint32_t dst_ip, uint16_t orig_id)
{
	return jhash_3words(clt_vip, dst_ip, orig_id, hash_seed);
}

static uint32_t icmp_hash6(const uint8_t *clt_vip, const uint8_t *dst_ip,
                            uint16_t orig_id)
{
	const uint32_t *c = (const uint32_t *)clt_vip;
	const uint32_t *d = (const uint32_t *)dst_ip;
	return jhash_3words(c[0] ^ c[1] ^ c[2] ^ c[3],
	                    d[0] ^ d[1] ^ d[2] ^ d[3],
	                    orig_id, hash_seed);
}

struct icmp_flow *icmp_flow_find(int af,
                                 const void *clt_vip,
                                 const void *dst_ip,
                                 uint16_t    orig_id)
{
	uint32_t h;
	struct list_head *chain;
	struct icmp_flow *f;

	if (af == AF_INET) {
		h = icmp_hash4(((const struct in_addr *)clt_vip)->s_addr,
		               ((const struct in_addr *)dst_ip)->s_addr, orig_id);
	} else {
		h = icmp_hash6((const uint8_t *)clt_vip, (const uint8_t *)dst_ip,
		               orig_id);
	}
	chain = &icmp_table[h & (ICMP_HASH_SIZE-1)];

	list_for_each_entry(f, chain, node) {
		if (f->af != af || f->orig_id != orig_id)
			continue;
		if (!vip_equal(af, af == AF_INET ? (void *)&f->clt_vip4 : (void *)&f->clt_vip6, clt_vip))
			continue;
		if (!vip_equal(af, af == AF_INET ? (void *)&f->dst_ip4 : (void *)&f->dst_ip6, dst_ip))
			continue;
		return f;
	}
	return NULL;
}

struct icmp_flow *icmp_flow_create(int af,
                                   const void *clt_vip,
                                   const void *dst_ip,
                                   uint16_t    orig_id,
                                   struct vpn_peer *peer)
{
	uint32_t h;
	struct list_head *chain;
	struct icmp_flow *f;

	f = malloc(sizeof(*f));
	if (!f) return NULL;
	memset(f, 0, sizeof(*f));

	f->af      = af;
	f->orig_id = orig_id;
	f->peer    = peer;
	f->fd      = -1;
	f->last_active = time(NULL);

	if (af == AF_INET) {
		f->clt_vip4 = *(const struct in_addr *)clt_vip;
		f->dst_ip4  = *(const struct in_addr *)dst_ip;
		h = icmp_hash4(f->clt_vip4.s_addr, f->dst_ip4.s_addr, orig_id);
	} else {
		f->clt_vip6 = *(const struct in6_addr *)clt_vip;
		f->dst_ip6  = *(const struct in6_addr *)dst_ip;
		h = icmp_hash6((const uint8_t *)&f->clt_vip6,
		               (const uint8_t *)&f->dst_ip6, orig_id);
	}
	chain = &icmp_table[h & (ICMP_HASH_SIZE-1)];
	list_add_tail(&f->node, chain);

	return f;
}

void icmp_flow_remove(struct icmp_flow *f)
{
	if (f->fd >= 0) close(f->fd);
	list_del(&f->node);
	free(f);
}

struct icmp_flow *icmp_flow_find_by_fd(int fd)
{
	int i;
	struct icmp_flow *f;

	for (i = 0; i < ICMP_HASH_SIZE; i++)
		list_for_each_entry(f, &icmp_table[i], node)
			if (f->fd == fd) return f;
	return NULL;
}

/* -----------------------------------------------------------------------
 * Poll array builder
 * ----------------------------------------------------------------------- */

int natmap_build_pollfd(struct pollfd *pfds, int max_entries)
{
	int n = 0, i;
	struct tcp_conn  *c;
	struct udp_flow  *uf;
	struct icmp_flow *ic;

	for (i = 0; i < TCP_HASH_SIZE; i++) {
		list_for_each_entry(c, &tcp_table[i], node) {
			if (n >= max_entries) return -1;
			pfds[n].fd = c->fd;
			if (c->state == TCP_CONNECTING)
				pfds[n].events = POLLOUT;
			else
				pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			n++;
		}
	}

	for (i = 0; i < UDP_HASH_SIZE; i++) {
		list_for_each_entry(uf, &udp_table[i], node) {
			if (n >= max_entries) return -1;
			pfds[n].fd = uf->fd;
			pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			n++;
		}
	}

	for (i = 0; i < ICMP_HASH_SIZE; i++) {
		list_for_each_entry(ic, &icmp_table[i], node) {
			if (n >= max_entries) return -1;
			pfds[n].fd = ic->fd;
			pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			n++;
		}
	}

	return n;
}

/* -----------------------------------------------------------------------
 * Expiry / garbage collection
 * ----------------------------------------------------------------------- */

void natmap_expire(time_t now)
{
	int i;
	struct tcp_conn  *c,  *ctmp;
	struct udp_flow  *uf, *utmp;
	struct icmp_flow *ic, *itmp;

	for (i = 0; i < TCP_HASH_SIZE; i++) {
		list_for_each_entry_safe(c, ctmp, &tcp_table[i], node) {
			if (now - c->last_active > NAT_TCP_TIMEOUT) {
				char s[50];
				inet_ntop(c->af,
				          c->af == AF_INET ? (void *)&c->clt_vip4 : (void *)&c->clt_vip6,
				          s, sizeof(s));
				printf("TCP conn [%s]:%u expired\n", s, c->clt_port);
				tcp_conn_remove(c);
			}
		}
	}

	for (i = 0; i < UDP_HASH_SIZE; i++) {
		list_for_each_entry_safe(uf, utmp, &udp_table[i], node) {
			if (now - uf->last_active > NAT_UDP_TIMEOUT) {
				char s[50];
				inet_ntop(uf->af,
				          uf->af == AF_INET ? (void *)&uf->clt_vip4 : (void *)&uf->clt_vip6,
				          s, sizeof(s));
				printf("UDP flow [%s]:%u expired\n", s, uf->clt_port);
				udp_flow_remove(uf);
			}
		}
	}

	for (i = 0; i < ICMP_HASH_SIZE; i++) {
		list_for_each_entry_safe(ic, itmp, &icmp_table[i], node) {
			if (now - ic->last_active > NAT_ICMP_TIMEOUT) {
				icmp_flow_remove(ic);
			}
		}
	}
}
