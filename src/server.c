/*
 * Copyright (c) 2015 Justin Liu
 * Author: Justin Liu <rssnsj@gmail.com>
 * https://github.com/rssnsj/minivtun
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "list.h"
#include "jhash.h"
#include "minivtun.h"

static inline bool bool_equal(bool b1, bool b2) { return (b1 && b2) || !(b1 || b2); }


/* Timestamp for each loop. */
static time_t current_ts = 0;
static uint32_t hash_initval = 0;

/* tcp clients 
#define TCP_CLIENT_MAX     256

static int tcp_clients[TCP_CLIENT_MAX];
static unsigned tcp_client_count = 0;

// We are in non-blocking mode, so we need spaces to store patial read-in data
static uint8_t tcp_client_data_buffer[TCP_CLIENT_MAX][NM_PI_BUFFER_SIZE]; // 256 * 8K = 2M
static unsigned tcp_client_data_buffer_length[TCP_CLIENT_MAX];
*/

/**
 * Pseudo route table for binding client side subnets
 * to corresponding connected virtual addresses.
 */
struct vt_route {
	struct in_addr network;
	struct in_addr netmask;
	struct in_addr gateway;
};
#define VIRTUAL_ROUTE_MAX  (32)
static struct vt_route *vt_routes[VIRTUAL_ROUTE_MAX];
static unsigned vt_routes_len = 0; 

int vt_route_add(struct in_addr *network, unsigned prefix, struct in_addr *gateway)
{
	struct vt_route *rt;
	uint32_t mask;

	if (prefix == 0) {
		mask = 0;
	} else {
		mask = ~((1U << (32 - prefix)) - 1) & 0xffffffff;
	}

	if (vt_routes_len >= VIRTUAL_ROUTE_MAX) {
		fprintf(stderr, "*** Virtual route table is full.\n");
		return -1;
	}

	rt = malloc(sizeof(struct vt_route));
	rt->netmask.s_addr = htonl(mask);
	rt->network.s_addr = network->s_addr & rt->netmask.s_addr;
	rt->gateway = *gateway;
	vt_routes[vt_routes_len++] = rt;

	return 0;
}

static struct in_addr *vt_route_lookup(const struct in_addr *addr)
{
	unsigned i;

	for (i = 0; i < vt_routes_len; i++) {
		struct vt_route *rt = vt_routes[i];
		
		printf("0x%08x,0x%08x,0x%08x\n", addr->s_addr, rt->netmask.s_addr, rt->network.s_addr);
		if ((addr->s_addr & rt->netmask.s_addr) == rt->network.s_addr)
			return &rt->gateway;
	}

	return NULL;
}

/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- */

// ra means real addr. Real address means the address of the client transport
struct ra_entry {

	struct list_head list; // link inside the ra_set_hbase hash table entry.
	struct sockaddr_inx real_addr; // The real address of the client
	time_t last_recv;
	time_t last_xmit;
    int refs; // Maintains the reference count of current usage. ra_get_or_create will increase it and ra_put_no_free will decrease it

    // TCP transport only
    bool accepted_only;
    int client_fd; // >= 0 tcp. -1 udp
    bool is_tcp; // client_fd may be closed and set to -1. but we may still need to distinguish tcp and udp. assigned in ra_entry creation
    uint8_t tcp_read_buffer[NM_PI_BUFFER_SIZE];
    size_t tcp_read_buffer_len;
    uint32_t tcp_msg_len; // == 0 if we should read msg_len, otherwise we already have it

    // Field for nonblocking writing.
    struct list_head tcp_write_list;
};

struct tcp_write_buffer {
    uint8_t * buffer;
    size_t buffer_len;
    size_t offset;
    struct list_head list;
};


/* Hash table for dedicated clients (real addresses). */
#define RA_SET_HASH_SIZE  (1 << 3)
#define RA_SET_LIMIT_EACH_WALK  (10)
static struct list_head ra_set_hbase[RA_SET_HASH_SIZE];  // 8 entries. hash key: real address.
static unsigned ra_set_len;

// A standalone list to store accepted but no data incoming so we don't know the vaddr.
// static struct list_head tun_clients_accepted_only; // list of tun_client
// static unsigned tun_clients_accepted_only_len = 0;

static struct list_head ra_entries_accepted_only; // list of ra_entry
static unsigned ra_entries_accepted_only_len = 0;

// #define RA_ENTRY_LIST_HEAD(_raddr_, _base_)  &((_base_)[real_addr_hash(_raddr_) & (RA_SET_HASH_SIZE - 1)])

static inline uint32_t real_addr_hash(const struct sockaddr_inx *sa, bool is_tcp)
{
    uint32_t hash = 0;

	if (sa->sa.sa_family == AF_INET6) {
		// return jhash_2words(sa->sa.sa_family, sa->in6.sin6_port,
		// 	jhash2((uint32_t *)&sa->in6.sin6_addr, 4, hash_initval));
		hash = jhash_2words(sa->sa.sa_family, sa->in6.sin6_port,
			jhash2((uint32_t *)&sa->in6.sin6_addr, 4, hash_initval));
	} else {
		// return jhash_3words(sa->sa.sa_family, sa->in.sin_port,
		// 	sa->in.sin_addr.s_addr, hash_initval);
		hash = jhash_3words(sa->sa.sa_family, sa->in.sin_port,
			sa->in.sin_addr.s_addr, hash_initval);
	}

    // Since the client's tcp and udp can have same address and port, we should put the is_tcp flag into hash
    return jhash_2words(hash, is_tcp ? 1 : 0, hash_initval);
}

static struct ra_entry * ra_create_accepted_only(const struct sockaddr_inx * sa, int client_fd)
{
    struct ra_entry * re = NULL;

	if ((re = malloc(sizeof(*re))) == NULL) {
		fprintf(stderr, "*** [%s] malloc(): %s.\n", __FUNCTION__, strerror(errno));
		return NULL;
	}

	re->real_addr = *sa;
	re->refs = 1;
    re->accepted_only = true;
    re->is_tcp = true;
    re->client_fd = client_fd;
    re->tcp_read_buffer_len = 0;
    re->tcp_msg_len = 0;
    INIT_LIST_HEAD(&(re->tcp_write_list));

	list_add_tail(&re->list, &ra_entries_accepted_only);
	ra_entries_accepted_only_len++;

    char s_real_addr[INET6_ADDRSTRLEN + 1] = { 0 };
	inet_ntop(re->real_addr.sa.sa_family, addr_of_sockaddr(&re->real_addr),
			  s_real_addr, sizeof(s_real_addr));

	printf("New client [%s:%u]\n", s_real_addr, ntohs(port_of_sockaddr(&re->real_addr)));

	return re;
}


static struct ra_entry *ra_get_or_create(const struct sockaddr_inx *sa, bool is_tcp, int client_fd)
{
	struct list_head *chain = &ra_set_hbase[real_addr_hash(sa, is_tcp) & (RA_SET_HASH_SIZE - 1)];
	struct ra_entry *re;
	char s_real_addr[50];

    // Got existing entry
	list_for_each_entry (re, chain, list) {
		if (is_sockaddr_equal(&re->real_addr, sa) && bool_equal(re->is_tcp, is_tcp)) {
			re->refs++;
			return re;
		}
	}

    // Create new
	if ((re = malloc(sizeof(*re))) == NULL) {
		fprintf(stderr, "*** [%s] malloc(): %s.\n", __FUNCTION__,
				strerror(errno));
		return NULL;
	}

	re->real_addr = *sa;
	re->refs = 1;

    re->accepted_only = false;
    re->client_fd = client_fd;
    re->is_tcp = is_tcp;
    re->tcp_read_buffer_len = 0;
    re->tcp_msg_len = 0;

    INIT_LIST_HEAD(&(re->tcp_write_list));

	list_add_tail(&re->list, chain);
	ra_set_len++;

	inet_ntop(re->real_addr.sa.sa_family, addr_of_sockaddr(&re->real_addr),
			  s_real_addr, sizeof(s_real_addr));
	printf("New client [%s:%u]\n", s_real_addr, ntohs(port_of_sockaddr(&re->real_addr)));

	return re;
}

static inline void ra_put_no_free(struct ra_entry *re)
{
    assert(re != NULL);
	assert(re->refs > 0);
	re->refs--;
}

static inline void ra_entry_release_accepted_only(struct ra_entry *re)
{
	char s_real_addr[50];

    assert(re != NULL);
    assert(re->accepted_only);

    ra_put_no_free(re);

	list_del(&re->list);
	ra_entries_accepted_only_len--;

	inet_ntop(re->real_addr.sa.sa_family, addr_of_sockaddr(&re->real_addr), s_real_addr, sizeof(s_real_addr));
	printf("Released client [%s:%u] from accepted only.\n", s_real_addr, ntohs(port_of_sockaddr(&re->real_addr)));

	free(re);
}

static inline void ra_entry_release(struct ra_entry *re)
{
	char s_real_addr[50];

    assert(re != NULL);
	assert(re->refs == 0);
    assert(re->client_fd < 0);

	list_del(&re->list);
	ra_set_len--;

	inet_ntop(re->real_addr.sa.sa_family, addr_of_sockaddr(&re->real_addr),
			  s_real_addr, sizeof(s_real_addr));
	printf("Recycled client [%s:%u]. %s\n", s_real_addr, ntohs(port_of_sockaddr(&re->real_addr)), re->is_tcp ? "tcp" : "udp");

	free(re);
}

// address of the tun interface. Since different clients must have different addresses, this is enough
// to distinguish the clients.
struct tun_addr {
	unsigned short af;
	union {
		struct in_addr in;
		struct in6_addr in6;
	};
};

// Identify a client 
struct tun_client {
	struct list_head list;      // link inside the list inside va_map_hbase.
	struct tun_addr virt_addr;  // client's address (tun address in client)
	struct ra_entry *ra;        // Point to a real address entry (one to one). NULL if this is a client just acceoted
	time_t last_recv;           // The time received last packet from the client
	time_t last_xmit;           // The time we sent last packet to the client

    // New fields for tcp transport only. - Moved to ra_entry
    // bool accepted_only;
    // bool is_tcp;
    // int client_fd;
    // uint8_t tcp_read_buffer[NM_PI_BUFFER_SIZE];
    // size_t tcp_read_buffer_len;
};

/* Hash table of virtual address in tunnel. */
#define VA_MAP_HASH_SIZE  (1 << 4)
#define VA_MAP_LIMIT_EACH_WALK  (10)
static struct list_head va_map_hbase[VA_MAP_HASH_SIZE]; // 16 entries hash table. hash key is client's virtual address
static unsigned va_map_len; // Total vaddrs in va_map_hbase[]



// 
static inline void init_va_ra_maps(void)
{
	int i;

	for (i = 0; i < VA_MAP_HASH_SIZE; i++)
		INIT_LIST_HEAD(&va_map_hbase[i]);
	va_map_len = 0;

	for (i = 0; i < RA_SET_HASH_SIZE; i++)
		INIT_LIST_HEAD(&ra_set_hbase[i]);
	ra_set_len = 0;

    INIT_LIST_HEAD(&ra_entries_accepted_only);
    ra_entries_accepted_only_len = 0;
}



static inline uint32_t tun_addr_hash(const struct tun_addr *addr)
{
	if (addr->af == AF_INET) {
		return jhash_2words(addr->af, addr->in.s_addr, hash_initval);
	} else if (addr->af == AF_INET6) {
		const __be32 *aa = (void *)&addr->in6;
		return jhash_2words(aa[2], aa[3],
			jhash_3words(addr->af, aa[0], aa[1], hash_initval));
	} else {
		abort();
		return 0;
	}
}

// Compare 2 tun addrs. Must be same address family and addres value
static inline int tun_addr_comp(
		const struct tun_addr *a1, const struct tun_addr *a2)
{
	if (a1->af != a2->af)
		return 1;

	if (a1->af == AF_INET) {
		if (a1->in.s_addr == a2->in.s_addr) {
			return 0;
		} else {
			return 1;
		}
	} else if (a1->af == AF_INET6) {
		if (is_in6_equal(&a1->in6, &a2->in6)) {
			return 0;
		} else {
			return 1;
		}
	} else {
		abort();
		return 0;
	}
}

#if 0
static inline void tun_client_dump(struct tun_client *ce)
{
	char s_virt_addr[50] = "", s_real_addr[50] = "";

	inet_ntop(ce->virt_addr.af, &ce->virt_addr.in, s_virt_addr,
			  sizeof(s_virt_addr));
	inet_ntop(ce->ra->real_addr.sa.sa_family, addr_of_sockaddr(&ce->ra->real_addr),
			  s_real_addr, sizeof(s_real_addr));
	printf("[%s] (%s:%u), last_recv: %lu, last_xmit: %lu\n", s_virt_addr,
			s_real_addr, ntohs(port_of_sockaddr(&ce->ra->real_addr)),
			(unsigned long)ce->last_recv, (unsigned long)ce->last_xmit);
}
#endif

static inline void tun_client_release(struct tun_client *ce)
{
	char s_virt_addr[50] = { 0}, s_real_addr[50] = { 0 };

	inet_ntop(ce->virt_addr.af, &ce->virt_addr.in, s_virt_addr,
			  sizeof(s_virt_addr));

    inet_ntop(ce->ra->real_addr.sa.sa_family, addr_of_sockaddr(&ce->ra->real_addr),
	    	  s_real_addr, sizeof(s_real_addr));

   	printf("Recycled virtual address [%s] at [%s:%u]. %s\n", s_virt_addr, s_real_addr,
			ntohs(port_of_sockaddr(&ce->ra->real_addr)), ce->ra->is_tcp ? "tcp" : "udp");

    ra_put_no_free(ce->ra);

	list_del(&ce->list);
	va_map_len--;

	free(ce);
}

// Release all tun_clients assigned to the re.
static inline void tun_client_release_for_re(struct ra_entry * re)
{
    size_t va_index = 0, va_count = 0;

    do {
        struct tun_client * ce, * __ce;

 	    list_for_each_entry_safe (ce, __ce, &va_map_hbase[va_index], list) {

            if ( ce->ra == re )
               tun_client_release(ce);

			va_count++;
		}

		va_index = (va_index + 1) & (VA_MAP_HASH_SIZE - 1);

	} while (va_count < va_map_len && va_index != 0);

} // tun_client_release_for_re

/** Try to get tun_client from the virtual address */
static struct tun_client *tun_client_try_get(const struct tun_addr *vaddr)
{
	struct list_head *chain = &va_map_hbase[
		tun_addr_hash(vaddr) & (VA_MAP_HASH_SIZE - 1)];
	struct tun_client *ce;

	list_for_each_entry (ce, chain, list) {
		if (tun_addr_comp(&ce->virt_addr, vaddr) == 0)
			return ce;
	}
	return NULL;
}

/** 
 *  Try to get, create if not exist. Note this function requires a real address argument, 
 *  so it can handle real address change case. 
 * 
 *  tcp_client_fd < 0 means it is a udp client.
 * 
 *  The tcp client is created in connection acceptance stage. At that time we don't know what vaddr is
 */
static struct tun_client *tun_client_get_or_create(
		const struct tun_addr *vaddr, const struct sockaddr_inx *raddr, int tcp_client_fd)
{
	struct list_head *chain = &va_map_hbase[
		tun_addr_hash(vaddr) & (VA_MAP_HASH_SIZE - 1)];
	struct tun_client *ce, *__ce;
	char s_virt_addr[50], s_real_addr[50];

    assert(raddr != NULL);

    // Iterate the list indicated by chain. chain points to a list (an entry) in hash map
	list_for_each_entry_safe (ce, __ce, chain, list) {

        // Have this virtual address.
		if (tun_addr_comp(&ce->virt_addr, vaddr) == 0) {

			if ( !is_sockaddr_equal(&ce->ra->real_addr, raddr) || !bool_equal(ce->ra->is_tcp >= 0, tcp_client_fd >= 0) ){
				/* Real address changed, reassign a new entry for it. */
				ra_put_no_free(ce->ra);
				if ((ce->ra = ra_get_or_create(raddr, tcp_client_fd >= 0, tcp_client_fd)) == NULL) {
					tun_client_release(ce);
					return NULL;
				}
			}

            // else ce->ra->read_addr == raddr. Nothing to do.
			return ce;
		}

	} // Iterate the list

	/* Not found, always create new entry. */
	if ((ce = malloc(sizeof(*ce))) == NULL) {
		fprintf(stderr, "*** [%s] malloc(): %s.\n", __FUNCTION__,
				strerror(errno));
		return NULL;
	}
    
	ce->virt_addr = *vaddr;

    /* Get real_addr entry before adding to list. */
    if ((ce->ra = ra_get_or_create(raddr, tcp_client_fd >= 0, tcp_client_fd)) == NULL) {
	    free(ce);
	    return NULL;
    }

	list_add_tail(&ce->list, chain);
	va_map_len++;

	inet_ntop(ce->virt_addr.af, &ce->virt_addr.in, s_virt_addr, sizeof(s_virt_addr));

	inet_ntop(ce->ra->real_addr.sa.sa_family, addr_of_sockaddr(&ce->ra->real_addr),
	    	  s_real_addr, sizeof(s_real_addr));

	printf("New virtual address [%s] at [%s:%u]. %s\n", s_virt_addr, s_real_addr,
	    	ntohs(port_of_sockaddr(&ce->ra->real_addr)), ce->ra->is_tcp ? "tcp" : "udp");

	return ce;
}

static
void close_tcp_client(struct ra_entry * re)
{
     assert(re->client_fd >= 0 && re->is_tcp);
     close(re->client_fd);
     re->client_fd = -1;
}

// Write data to a nonblocking tcp fd.
// 
// Return <0 if error (including EOF) 
// Return 0 if nothing occur
// Return >0 means bytes written
static 
int write_queued_tcp_data_nonblocking(int fd, struct ra_entry * entry)
{
    // Pick a buffer from queue
    if ( list_empty(&(entry->tcp_write_list)) )
       return 0;

    struct tcp_write_buffer * write_buffer = list_first_entry(&(entry->tcp_write_list), struct tcp_write_buffer, list);
    assert(write_buffer->offset < write_buffer->buffer_len);

    ssize_t written_size = 0;
    do {
        written_size = write(fd, write_buffer->buffer + write_buffer->offset, write_buffer->buffer_len - write_buffer->offset);
    } while (written_size == -1 && errno == EINTR);

    if ( written_size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
       return 0;

    // Real error
    if ( written_size < 0 )
       return -1;

    // EOF
    if ( written_size == 0 )
       return -1;

    // Succeeded
    write_buffer->offset += written_size;
    assert(write_buffer->offset <= write_buffer->buffer_len);

    // Remove the completed buffer
    if ( write_buffer->offset == write_buffer->buffer_len ) {
       list_del(&(write_buffer->list));
       free(write_buffer);
    }

    return written_size;
}

// Queue data into writing queue. -1 if allocation error.
static
int queue_tcp_write_data(struct ra_entry * entry, void * buffer, size_t buffer_len)
{
     // Allocate a tcp_write_buffer struct and the buffer inside
     struct tcp_write_buffer * write_buffer = 
         (struct tcp_write_buffer *)malloc(sizeof(struct tcp_write_buffer) + buffer_len);

     if ( write_buffer == NULL )  
        return -1;

    // Fill the buffer
    write_buffer->buffer = (uint8_t *)(write_buffer + 1);
    memcpy(write_buffer->buffer, buffer, buffer_len);
    write_buffer->buffer_len = buffer_len;
    write_buffer->offset = 0;

    // link into the list in entry
    list_add_tail(&(write_buffer->list), &(entry->tcp_write_list));
    return 0;
}


/**
 * Send keep-alive packet to the corresponding client
 * with information stored in 're'.
 */
static int ra_entry_keepalive(struct ra_entry *re, int sockfd)
{
	char in_data[64], crypt_buffer[64];
	struct minivtun_msg *nmsg = (struct minivtun_msg *)in_data;
	void *out_msg;
	size_t out_len;
	int rc;

	nmsg->hdr.opcode = MINIVTUN_MSG_KEEPALIVE;
	memset(nmsg->hdr.rsv, 0x0, sizeof(nmsg->hdr.rsv));
	memcpy(nmsg->hdr.auth_key, config.crypto_key, sizeof(nmsg->hdr.auth_key));
	nmsg->keepalive.loc_tun_in = config.local_tun_in;
	nmsg->keepalive.loc_tun_in6 = config.local_tun_in6;

	out_msg = crypt_buffer;
	out_len = MINIVTUN_MSG_BASIC_HLEN + sizeof(nmsg->keepalive);
	local_to_netmsg(nmsg, &out_msg, &out_len);

    if ( re->is_tcp ) {

        uint32_t msg_len = htonl(out_len);
        /*
        struct iovec iov[2];
        iov[0].iov_base = &msg_len;
        iov[0].iov_len = sizeof(uint32_t);
        iov[1].iov_base = out_msg;
        iov[1].iov_len = out_len;
        rc = writev(re->client_fd, iov, sizeof(iov) / sizeof(struct iovec));
        */
        queue_tcp_write_data(re, &msg_len, sizeof(uint32_t));
        queue_tcp_write_data(re, out_msg, out_len);

        rc = 1;
#if DEBUG
        printf("ra_entry_keepalive queue %zu to tcp fd %d for writing.\n", out_len, re->client_fd);
#endif

    }
    else {
	   rc = (int)sendto(sockfd, out_msg, out_len, 0, (struct sockaddr *)&re->real_addr,
		     	        sizeof_sockaddr(&re->real_addr));
#if DEBUG
        printf("ra_entry_keepalive send %zu to udp fd %d. result %d\n", out_len, sockfd, rc);
#endif
    }

    /* Update 'last_xmit' only when it's really sent out. */
    if (rc > 0) {
	  re->last_xmit = current_ts;
    }

	return rc;
}

static void va_ra_walk_continue(int sockfd)
{
	static unsigned va_index = 0, ra_index = 0;
	unsigned va_walk_max = VA_MAP_LIMIT_EACH_WALK, va_count = 0;
	unsigned ra_walk_max = RA_SET_LIMIT_EACH_WALK, ra_count = 0;
	unsigned __va_index = va_index, __ra_index = ra_index;
	struct tun_client *ce, *__ce;
	struct ra_entry *re, *__re;

	if (va_walk_max > va_map_len)
		va_walk_max = va_map_len;
	if (ra_walk_max > ra_set_len)
		ra_walk_max = ra_set_len;

	/* Recycle timeout virtual address entries. */
	if (va_walk_max > 0) {
		do {
			list_for_each_entry_safe (ce, __ce, &va_map_hbase[va_index], list) {
				//tun_client_dump(ce);
				if (current_ts - ce->last_recv > config.reconnect_timeo) {
					tun_client_release(ce);
				}
				va_count++;
			}
			va_index = (va_index + 1) & (VA_MAP_HASH_SIZE - 1);
		} while (va_count < va_walk_max && va_index != __va_index);
	}

	/* Recycle or keep-alive real client addresses. */
	if (ra_walk_max > 0) {
		do {
			list_for_each_entry_safe (re, __re, &ra_set_hbase[ra_index], list) {
				if (current_ts - re->last_recv > config.reconnect_timeo) {
					if (re->refs == 0) {
                        // Indeed I think when re->refs is zero the client_fd should already be closed.
                        if ( re->is_tcp && re->client_fd >= 0 )
                           close_tcp_client(re);
						ra_entry_release(re);
					}
				} else if (current_ts - re->last_xmit > config.keepalive_timeo) {
					ra_entry_keepalive(re, sockfd);
				}
				ra_count++;
			}
			ra_index = (ra_index + 1) & (RA_SET_HASH_SIZE - 1);
		} while (ra_count < ra_walk_max && ra_index != __ra_index);
	}

	printf("Online clients: %u, addresses: %u, accept only: %u\n", ra_set_len, va_map_len, 
           ra_entries_accepted_only_len);
}

static inline void source_addr_of_ipdata(
		const void *data, unsigned char af, struct tun_addr *addr)
{
	addr->af = af;
	switch (af) {
	case AF_INET:
		memcpy(&addr->in, (char *)data + 12, 4);
		break;
	case AF_INET6:
		memcpy(&addr->in6, (char *)data + 8, 16);
		break;
	default:
		abort();
	}
}

static inline void dest_addr_of_ipdata(
		const void *data, unsigned char af, struct tun_addr *addr)
{
	addr->af = af;
	switch (af) {
	case AF_INET:
		memcpy(&addr->in, (char *)data + 16, 4);
		break;
	case AF_INET6:
		memcpy(&addr->in6, (char *)data + 24, 16);
		break;
	default:
		abort();
	}
}

// TCP read non-blocking. Return 1 if whole netmsg is read, 0 if not all read, -1 if error
static
int read_tcp_client_data(int client_fd, uint8_t * buffer, size_t * buffer_offset, size_t whole_len)
{
    ssize_t read_size = 0;

    size_t size_to_read = whole_len - *buffer_offset;
    if ( size_to_read == 0 )
       return 1;

    do {        
       read_size = read(client_fd, buffer + *buffer_offset, size_to_read);
    } while ( read_size == -1 && errno == EINTR );
    
    if ( read_size > 0 ) {
       *buffer_offset += read_size;
       if ( *buffer_offset >= whole_len ) {
          fprintf(stderr, "client fd %d read %zd bytes. offset %zu whole %zu. All read in\n", client_fd, read_size, *buffer_offset, whole_len);
          return 1;
       }
       else {
          fprintf(stderr, "client fd %d read %zd bytes. offset %zu whole %zu. more data\n", client_fd, read_size, *buffer_offset, whole_len);
          return 0;
       }
    }

    // EOF
    if ( read_size == 0 ) {
       fprintf(stderr, "read() on client fd %d EOF (peer disconnected)\n", client_fd);
       return -1;
    }

    // Something not arrived
    if ( read_size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ) {

       // Data not ready? in FIN?
       int socket_error = 0;
       socklen_t socket_error_len = sizeof(int);
       getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len);
       
       //
       fprintf(stderr, "read() on client fd %d read_size %zd (errno %d), getsockopt get error %d, more data?\n", 
               client_fd, read_size, errno, socket_error);       

       return 0;
    }

    // Other errors
    fprintf(stderr, "read() on client fd %d failed %s\n", client_fd, strerror(errno));
    return -1;
}

// This handles packet from the client. i.e. It should be a netmsg.
//
// re == NULL: udp
// re != NULL: tcp
static int network_receiving(int tunfd, int sockfd, struct ra_entry * re)
{
	char read_buffer[NM_PI_BUFFER_SIZE], crypt_buffer[NM_PI_BUFFER_SIZE];
	struct minivtun_msg *nmsg;
	struct tun_pi pi;
	void *out_data;
	size_t ip_dlen, out_dlen;
	unsigned short af = 0;
	struct tun_addr virt_addr;
	struct tun_client *ce;
	struct ra_entry *new_re;
	struct sockaddr_inx real_peer;
	socklen_t real_peer_alen;
	struct iovec iov[2];
	int rc;
    int client_fd = -1;

    // Receive the wrapped packet (netmsg)

    // TCP
    if ( re != NULL ) {
       
       struct ra_entry * entry = re;

       assert(entry->client_fd >= 0 && sockfd == entry->client_fd);

       // Read the length of netmsg (32bit BE)
       if ( re->tcp_msg_len == 0 ) {

          int read_result = read_tcp_client_data(sockfd, entry->tcp_read_buffer, &(entry->tcp_read_buffer_len), 
                                                 sizeof(uint32_t));

          // error
          if ( read_result < 0 ) {
#if DEBUG          
             fprintf(stderr, "tcp client fd %d closed due to read msg_len error\n", entry->client_fd);
#endif          
             close_tcp_client(entry);

             if ( entry->accepted_only )
                ra_entry_release_accepted_only(entry);
             else
                tun_client_release_for_re(entry);

             return -1;
          }

          // Next
          if ( read_result == 0 )
             return 0;

          // We got netmsg length
          entry->tcp_msg_len = ntohl(*(uint32_t *)entry->tcp_read_buffer);
          entry->tcp_read_buffer_len = 0; // reset read len       
       
#if DEBUG
          printf("network_receiving: received msg_len %d\n", entry->tcp_msg_len);
#endif

          return 0;

       } // read msg_len
       
       // Read net_msg
       int read_result = read_tcp_client_data(sockfd, entry->tcp_read_buffer, &entry->tcp_read_buffer_len, 
                                              entry->tcp_msg_len);
       if ( read_result < 0 ) {
#if DEBUG          
          fprintf(stderr, "tun tcp client fd %d closed due to read net_msg error\n", entry->client_fd);
#endif          
          close_tcp_client(entry);
          if ( entry->accepted_only )
             ra_entry_release_accepted_only(entry);
          else 
             tun_client_release_for_re(entry);
          return -1;
       }

       if ( read_result == 0 )
          return 0;

       // All read in.
       memcpy(read_buffer, entry->tcp_read_buffer, entry->tcp_read_buffer_len);
       rc = entry->tcp_read_buffer_len;
       entry->tcp_read_buffer_len = 0;
       entry->tcp_msg_len = 0;

       // set real_peer & real_peer_alen
       real_peer = entry->real_addr;
       real_peer_alen = entry->real_addr.sa.sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
       client_fd = entry->client_fd;

       // Release the entry in accepted list only. fd is alive
       if ( entry->accepted_only )
          ra_entry_release_accepted_only(entry);
    } 
    // UDP
    else {
	    real_peer_alen = sizeof(real_peer);
	    rc = (int)recvfrom(sockfd, &read_buffer, NM_PI_BUFFER_SIZE, 0,
		        	(struct sockaddr *)&real_peer, &real_peer_alen);
	    if (rc <= 0)
		    return 0;
    } // udp

#if DEBUG
    char real_peer_str[INET6_ADDRSTRLEN + 1] = { 0 };
    inet_ntop(real_peer.sa.sa_family, addr_of_sockaddr(&real_peer), real_peer_str, INET6_ADDRSTRLEN);
    printf("network_receiving: received %d bytes. [%s:%d]\n", rc, real_peer_str, port_of_sockaddr(&real_peer));
	hexdump(read_buffer, rc);
#endif

    // Decrypt payload. encrypted is in read_buffer, plain data is in nmsg, out_data(crypt_buffer) with length out_dlen
	out_data = crypt_buffer;
	out_dlen = (size_t)rc;
	netmsg_to_local(read_buffer, &out_data, &out_dlen);
	nmsg = out_data;

	if (out_dlen < MINIVTUN_MSG_BASIC_HLEN)
		return 0;
 
 #if DEBUG
    dump_nmsg(nmsg);
 #endif

	/* Verify password. */
	if (memcmp(nmsg->hdr.auth_key, config.crypto_key,
		sizeof(nmsg->hdr.auth_key)) != 0)
		return 0;

	switch (nmsg->hdr.opcode) {

		// Keepalive packet
	case MINIVTUN_MSG_KEEPALIVE:
		if ((new_re = ra_get_or_create(&real_peer, re != NULL, client_fd))) {
			new_re->last_recv = current_ts;
			ra_put_no_free(new_re);
		}
		if (out_dlen < MINIVTUN_MSG_BASIC_HLEN + sizeof(nmsg->keepalive))
			return 0;
        
        struct in_addr inaddr = nmsg->keepalive.loc_tun_in;
		if (is_valid_unicast_in(&inaddr)) {
			virt_addr.af = AF_INET;
			virt_addr.in = nmsg->keepalive.loc_tun_in;
			if ((ce = tun_client_get_or_create(&virt_addr, &real_peer, client_fd)))
				ce->last_recv = current_ts;
		}

        struct in6_addr inaddr6 = nmsg->keepalive.loc_tun_in6;
		if (is_valid_unicast_in6(&inaddr6)) {
			virt_addr.af = AF_INET6;
			virt_addr.in6 = nmsg->keepalive.loc_tun_in6;
			if ((ce = tun_client_get_or_create(&virt_addr, &real_peer, client_fd)))
				ce->last_recv = current_ts;
		}
		break;

		// data packet
	case MINIVTUN_MSG_IPDATA:
		if (nmsg->ipdata.proto == htons(ETH_P_IP)) {
			af = AF_INET;
			/* No packet is shorter than a 20-byte IPv4 header. */
			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 20)
				return 0;
		} else if (nmsg->ipdata.proto == htons(ETH_P_IPV6)) {
			af = AF_INET6;
			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 40)
				return 0;
		} else {
			fprintf(stderr, "*** Invalid protocol: 0x%x.\n", ntohs(nmsg->ipdata.proto));
			return 0;
		}

		ip_dlen = ntohs(nmsg->ipdata.ip_dlen);
		/* Drop incomplete IP packets. */
		if (out_dlen - MINIVTUN_MSG_IPDATA_OFFSET < ip_dlen)
			return 0;

        // Copy source address  in ip packet to virt_addr.in
		source_addr_of_ipdata(nmsg->ipdata.data, af, &virt_addr);
		if ((ce = tun_client_get_or_create(&virt_addr, &real_peer, client_fd)) == NULL)
			return 0;

		ce->last_recv = current_ts;
		ce->ra->last_recv = current_ts;

        // tun required plain data
		set_pi_with_ether_proto(&pi, ntohs(nmsg->ipdata.proto));
		iov[0].iov_base = &pi;
		iov[0].iov_len = sizeof(pi);
		iov[1].iov_base = (char *)nmsg + MINIVTUN_MSG_IPDATA_OFFSET;
		iov[1].iov_len = ip_dlen;
		rc = (int)writev(tunfd, iov, 2);

#ifdef DEBUG
        printf("Write to tun: ");
		hexdump(iov[0].iov_base, iov[0].iov_len);
		hexdump(iov[1].iov_base, iov[1].iov_len);
#endif

		break;
	}

	return 0;
}

// When sth. readable from tun interface.
static int tunnel_receiving(int tunfd, int sockfd)
{
	char read_buffer[NM_PI_BUFFER_SIZE], crypt_buffer[NM_PI_BUFFER_SIZE];
	struct tun_pi *pi = (void *)read_buffer;
	struct minivtun_msg nmsg;
	void *out_data;
	size_t ip_dlen, out_dlen;
	unsigned short af = 0;
	struct tun_addr virt_addr;
	struct tun_client *ce;
	int rc;

	rc = (int)read(tunfd, pi, NM_PI_BUFFER_SIZE);
#if DEBUG	
	if ( rc < 0 ) {
	   perror("read");
	   abort();
	}

    printf("tunnel_receiving:\n");
	hexdump(read_buffer, rc);
#endif

	if (rc < sizeof(struct tun_pi))
		return 0;

	// osx_af_to_ether(&pi->proto);

	ip_dlen = (size_t)rc - sizeof(struct tun_pi);

	/* We only accept IPv4 or IPv6 frames. */
	/*
	if (pi->proto == htons(ETH_P_IP)) {
		af = AF_INET;
		if (ip_dlen < 20)
			return 0;
	} else if (pi->proto == htons(ETH_P_IPV6)) {
		af = AF_INET6;
		if (ip_dlen < 40)
			return 0;
	} else {
		fprintf(stderr, "*** Invalid protocol: 0x%x.\n", ntohs(pi->proto));
		return 0;
	}
	*/
	af = get_family_from_pi(pi);

	if ( af == AF_INET ) {
	   if ( ip_dlen < 20 )
	      return 0;
	}
	else if ( af == AF_INET6 ) {
	   if ( ip_dlen < 40 )
	      return 0;
	}
	else {   
		fprintf(stderr, "*** Invalid protocol: 0x%x.\n", get_ether_proto_from_pi(pi));
		return 0;
	}

    // Copy destination address in incoming ip packet to virt_addr.addr_in or addr_in6
	dest_addr_of_ipdata(pi + 1, af, &virt_addr);

	if ((ce = tun_client_try_get(&virt_addr)) == NULL) {

		/**
		 * Not an existing client address, lookup the pseudo
		 * route table for a destination to send.
		 */
		if (virt_addr.af == AF_INET) {
			struct in_addr *gw;
			struct tun_addr __virt_addr;

			/* Lookup the gateway virtual address first. */
			if ((gw = vt_route_lookup(&virt_addr.in)) == NULL)
				return 0;

			/* Then get the gateway client entry. */
			memset(&__virt_addr, 0x0, sizeof(__virt_addr));
			__virt_addr.af = AF_INET;
			__virt_addr.in = *gw;
			if ((ce = tun_client_try_get(&__virt_addr)) == NULL)
				return 0;

			/* Finally, create the client entry. Using gateway's real address */
			if ((ce = tun_client_get_or_create(&virt_addr, &ce->ra->real_addr, ce->ra->is_tcp ? ce->ra->client_fd : -1)) == NULL)
				return 0;
		} else {
			return 0;
		}
	}

    // Now ce contains the tun_client of the destination?

    // Make netmsg
	nmsg.hdr.opcode = MINIVTUN_MSG_IPDATA;
	memset(nmsg.hdr.rsv, 0x0, sizeof(nmsg.hdr.rsv));
	memcpy(nmsg.hdr.auth_key, config.crypto_key, sizeof(nmsg.hdr.auth_key));
	nmsg.ipdata.proto = htons(get_ether_proto_from_pi(pi)); // pi->proto;
	nmsg.ipdata.ip_dlen = htons(ip_dlen);
	memcpy(nmsg.ipdata.data, pi + 1, ip_dlen);

	/* Do encryption. */
	out_data = crypt_buffer;
	out_dlen = MINIVTUN_MSG_IPDATA_OFFSET + ip_dlen;
	local_to_netmsg(&nmsg, &out_data, &out_dlen);

#if DEBUG
    dump_nmsg(&nmsg);

	printf("out data:\n");
	hexdump(out_data, out_dlen);
#endif	

    if ( ce->ra->is_tcp ) {

        assert(ce->ra->client_fd >= 0);

        uint32_t msg_len = htonl(out_dlen);
        /*
        struct iovec iov[2];
        iov[0].iov_base = &msg_len;
        iov[0].iov_len = sizeof(uint32_t);
        iov[1].iov_base = out_data;
        iov[1].iov_len = out_dlen;
        rc = writev(ce->ra->client_fd, iov, sizeof(iov) / sizeof(struct iovec));
        */
        queue_tcp_write_data(ce->ra, &msg_len, sizeof(uint32_t));
        queue_tcp_write_data(ce->ra, out_data, out_dlen);        

    }
    else {
	    rc = (int)sendto(sockfd, out_data, out_dlen, 0,
		    		(struct sockaddr *)&ce->ra->real_addr,
			    	sizeof_sockaddr(&ce->ra->real_addr));
    }

	ce->last_xmit = current_ts;
	ce->ra->last_xmit = current_ts;

	return 0;
}

// Accept incoming TCP connection. Return -1 if error, otherwise the 
static 
int accept_connection(int listen_fd)
{
    struct sockaddr_inx addr_in; // real address
    socklen_t addr_len = sizeof(addr_in);   

    int rv = accept(listen_fd, (struct sockaddr *)&addr_in, &addr_len);
    if ( rv < 0 )
       return rv;

    set_nonblock(rv);

    // Cannot create.
    if ( ra_create_accepted_only(&addr_in, rv) == NULL ) {
       close(rv);
       return -1;
    }
    
    // return the client_fd;
    return rv;
}

// The server's entry function
int run_server(int tunfd, const char *loc_addr_pair)
{
	struct timeval timeo;
	int sockfd, rc;
	struct sockaddr_inx loc_addr;
	fd_set rset, wset;
	time_t last_walk;
	char s_loc_addr[50];

    int tcp_listen_fd = -1;

#if DEBUG
    assert(bool_equal(true, true));
    assert(bool_equal(false, false));
    assert(!bool_equal(true, false));
    assert(!bool_equal(false, true));
#endif

    // 
	if (get_sockaddr_inx_pair(loc_addr_pair, &loc_addr) < 0) {
		fprintf(stderr, "*** Cannot resolve address pair '%s'.\n", loc_addr_pair);
		return -1;
	}

	inet_ntop(loc_addr.sa.sa_family, addr_of_sockaddr(&loc_addr), s_loc_addr,
			  sizeof(s_loc_addr));
	printf("Mini virtual tunnelling server on %s:%u, interface: %s.\n",
			s_loc_addr, ntohs(port_of_sockaddr(&loc_addr)), config.devname);

	/* Initialize address map hash table. */
	init_va_ra_maps();
	hash_initval = (uint32_t)time(NULL);

    // bind to UDP socket
	if ((sockfd = socket(loc_addr.sa.sa_family, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		fprintf(stderr, "*** socket() failed: %s.\n", strerror(errno));
		exit(1);
	}
	if (bind(sockfd, (struct sockaddr *)&loc_addr, sizeof_sockaddr(&loc_addr)) < 0) {
		fprintf(stderr, "*** bind() failed: %s.\n", strerror(errno));
		exit(1);
	}
	set_nonblock(sockfd);

    // Listen on TCP socket
    tcp_listen_fd = socket(loc_addr.sa.sa_family, SOCK_STREAM, IPPROTO_TCP);
    if ( tcp_listen_fd < 0 ) {
        fprintf(stderr, "*** socket() to create tcp listening socket failed: %s\n", strerror(errno));
        exit(1);
    }

    // Bind to tcp listening socket
    if ( bind(tcp_listen_fd, (struct sockaddr *)&loc_addr, sizeof_sockaddr(&loc_addr)) < 0 ) {
        fprintf(stderr, "*** bind() to bind tcp listening socket failed: %s\n", strerror(errno));
        exit(1);
    }
    set_nonblock(tcp_listen_fd);

    if ( listen(tcp_listen_fd, 128) < 0 ) {
        fprintf(stderr, "*** listen() failed: %s\n", strerror(errno));
        exit(1);
    }

	/* Run in background. */
	if (config.in_background)
		do_daemonize();

	if (config.pid_file) {
		FILE *fp;
		if ((fp = fopen(config.pid_file, "w"))) {
			fprintf(fp, "%d\n", (int)getpid());
			fclose(fp);
		}
	}

	last_walk = time(NULL);

	for (;;) {

		FD_ZERO(&rset);
		FD_SET(tunfd, &rset);
		FD_SET(sockfd, &rset);
        FD_SET(tcp_listen_fd, &rset);

        FD_ZERO(&wset);

        int max_fd = max_of(tunfd, sockfd);
        max_fd = max_of(max_fd, tcp_listen_fd);

        // client fds in accepted only list
        {
            struct ra_entry *entry = NULL;
            list_for_each_entry(entry, &ra_entries_accepted_only, list) {
                FD_SET(entry->client_fd, &rset);
                FD_SET(entry->client_fd, &wset);
                max_fd = max_of(max_fd, entry->client_fd);
#if DEBUG            
                fprintf(stderr, "Accepted only: add fd %d to set. max fd is %d\n", entry->client_fd, max_fd);
#endif
            }
        }

        // tcp client connections in ra_hbase
        for ( int i = 0; i < RA_SET_HASH_SIZE; i++ ) {

            struct list_head *chain = &ra_set_hbase[i];
            struct ra_entry *re;

            list_for_each_entry (re, chain, list) {
                if (re->is_tcp && re->client_fd >= 0) {
                   FD_SET(re->client_fd, &rset);
                   FD_SET(re->client_fd, &wset);
                   max_fd = max_of(max_fd, re->client_fd);
#if DEBUG            
            fprintf(stderr, "Connected client: add fd %d to set. max fd is %d\n", re->client_fd, max_fd);
#endif
                }
            }
        } // list

		timeo.tv_sec = 2;
		timeo.tv_usec = 0;

		rc = select(max_fd + 1, &rset, NULL, NULL, &timeo);
		if ( rc < 0 && errno != EINTR ) {
			fprintf(stderr, "*** select(): %s.\n", strerror(errno));
			return -1;
		}

		current_ts = time(NULL);
#if DEBUG
        printf("current_ts = %ld\n", current_ts);
#endif        

		if (rc > 0) {

            // The udp socket to clients
			if (FD_ISSET(sockfd, &rset)) {
				rc = network_receiving(tunfd, sockfd, NULL);
			}
            // tun fd. The socket to receive outside packets
			if (FD_ISSET(tunfd, &rset)) {
				rc = tunnel_receiving(tunfd, sockfd); //
			}
            // tcp listen socket
            if (FD_ISSET(tcp_listen_fd, &rset)) {
                rc = accept_connection(tcp_listen_fd);
                if (rc < 0) {
                   fprintf(stderr, "accept() failed: %s\n", strerror(errno));
                }
                else {
#if DEBUG                   
                   fprintf(stderr, "accepted client connection fd = %d, accepted_only_len %u\n", 
                           rc, ra_entries_accepted_only_len);
#endif                           
                }
            }
            // tcp client connections in ra_set
            for ( int i = 0; i < RA_SET_HASH_SIZE; i++ ) {

                struct list_head *chain = &ra_set_hbase[i];
                struct ra_entry *re, *temp;

                list_for_each_entry_safe (re, temp, chain, list) {
                    if (re->is_tcp) {
                       if ( re->refs > 0 && FD_ISSET(re->client_fd, &rset) ) {
#if DEBUG
                          fprintf(stderr, "connected client fd %d ready to read\n", re->client_fd);
#endif                    
                          int client_fd = re->client_fd;
                          rc = network_receiving(tunfd, client_fd, re);
                          if ( rc < 0 ) {
#if DEBUG
                             fprintf(stderr, "connected client fd %d network receiving failed. Clear\n", client_fd);
#endif                    
                          }
                       }

                       // writable
                       if ( re->refs > 0 && FD_ISSET(re->client_fd, &wset) ) {
                          int client_fd = re->client_fd;
                          rc = write_queued_tcp_data_nonblocking(client_fd, re);
                          if ( rc < 0 ) {
#if DEBUG
                             fprintf(stderr, "connected client fd %d network wrting failed.\n", client_fd);
#endif                    
                          }
                       }
                    }
                } // list

            } // all real addresses in ra_set

            // tcp client connections in accepted only list
            struct ra_entry *entry, *temp;
            list_for_each_entry_safe(entry, temp, &ra_entries_accepted_only, list) {
                 if ( entry->refs > 0 && FD_ISSET(entry->client_fd, &rset) ) {
#if DEBUG
                    fprintf(stderr, "accepted client fd %d ready to read\n", entry->client_fd);
#endif                    
                    int client_fd = entry->client_fd;
                    rc = network_receiving(tunfd, entry->client_fd, entry); 
                    if ( rc < 0 ) {
#if DEBUG
                        fprintf(stderr, "accepted client fd %d network receiving failed.\n", client_fd);
#endif                    
                    }
                 }

                 // Writable
                 if ( entry->refs > 0 && FD_ISSET(entry->client_fd, &wset) ) {
                    int client_fd = entry->client_fd;
                    rc = write_queued_tcp_data_nonblocking(client_fd, entry);
                    if ( rc < 0 ) {
#if DEBUG
                        fprintf(stderr, "accepted client fd %d writing failed.\n", client_fd);
#endif                    
                    }
                 }
            } // all accepted only


        } // if select() > 0

		/* Check connection state at each chance. */
		if (current_ts - last_walk >= 3) {
			va_ra_walk_continue(sockfd);
			last_walk = current_ts;
		}
	}

	return 0;
}
