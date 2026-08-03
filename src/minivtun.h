/*
 * Copyright (c) 2015 Justin Liu
 * Author: Justin Liu <rssnsj@gmail.com>
 * https://github.com/rssnsj/minivtun
 */

#ifndef __MINIVTUN_H
#define __MINIVTUN_H

#include "library.h"

#include <assert.h>
#include <net/if.h>

extern struct minivtun_config config;

struct minivtun_config {
	unsigned reconnect_timeo;
	unsigned keepalive_timeo;
	char devname[40];
	unsigned tun_mtu;
	const char *crypto_passwd;
	const char *pid_file;
	bool in_background;
	bool wait_dns;

	char crypto_key[CRYPTO_MAX_KEY_SIZE];
	const void *crypto_type;
	struct in_addr local_tun_in;
	struct in6_addr local_tun_in6;

	int send_all_traffic;
	char bind_to_addr[64];
	char bind_if[IFNAMSIZ];

    // v2: using tcp instead of udp
    bool use_tcp;
};

enum {
	MINIVTUN_MSG_KEEPALIVE,
	MINIVTUN_MSG_IPDATA,
	MINIVTUN_MSG_DISCONNECT,
};

#define NM_PI_BUFFER_SIZE  ((1024 * 8) + CRYPTO_MAX_KEY_SIZE)

// NOTE: A 32 bytes (potential) IV will be prepend to encrypted minivtun_msg on the wire.
struct minivtun_msg {
	struct {
		__u8 opcode;
		__u8 rsv[3];
		__u8 auth_key[16];
	}  __attribute__((packed)) hdr;

	union {
		struct {
			__be16 proto;   /* ETH_P_IP or ETH_P_IPV6 */ /* in network byte order! */
			__be16 ip_dlen; /* Total length of IP/IPv6 data */ 
			char data[NM_PI_BUFFER_SIZE];
		} __attribute__((packed)) ipdata;
		struct {
			struct in_addr loc_tun_in;
			struct in6_addr loc_tun_in6;
		} __attribute__((packed)) keepalive;
	};
} __attribute__((packed));

#define MINIVTUN_MSG_BASIC_HLEN  (sizeof(((struct minivtun_msg *)0)->hdr))
#define MINIVTUN_MSG_IPDATA_OFFSET  (offsetof(struct minivtun_msg, ipdata.data))

#define enabled_encryption()  (config.crypto_passwd[0])

// Convert data to what would be sent out to the transport
//
// @param in. The input buffer
// @param in_buffer_len. The whole input buffer langth, >= data len
// @param in_data_len. The input data len <= in_buffer_len
// @param out. The out buffer
// @param out_buffer_len. The whole output buffer length. 
// @param out_data_len. The data length in output buffer. <= out_buffer_len. This ths the output param.
//
// The returned @param out and @param out_data_len contains the IV in the head.
void local_to_netmsg(bool enabled_encryption, const char * crypto_key, const void * crypto_type, void *in, size_t in_buffer_len, size_t in_data_len, 
                     void *out, size_t out_buffer_len, size_t *out_data_len);

// The input data @param in include pretended IV
// The output @param out and @param out_data_len didn't include IV.
void netmsg_to_local(bool enabled_encryption, const char * crypto_key, const void * crypto_type, void *in, size_t in_buffer_len, size_t in_data_len, 
                     void *out, size_t out_buffer_len, size_t *out_data_len);

int run_client(int tunfd, const char *peer_addr_pair);
int run_server(int tunfd, const char *loc_addr_pair);
int vt_route_add(struct in_addr *network, unsigned prefix, struct in_addr *gateway);

#if DEBUG
static inline void dump_nmsg(struct minivtun_msg * nmsg)
{
	int i;

	printf("nmsg.hdr: opcode = 0x%x(%s), rsv = 0x%x, 0x%x, 0x%x, auth_key = ", 
	       nmsg->hdr.opcode, 
		   nmsg->hdr.opcode == MINIVTUN_MSG_IPDATA ? "IPDATA" : (nmsg->hdr.opcode == MINIVTUN_MSG_KEEPALIVE ? "Keepalive" : "Unknown"), 
		   nmsg->hdr.rsv[0], nmsg->hdr.rsv[1], nmsg->hdr.rsv[2]);

	for ( i = 0; i < 16; i++ )
	    printf("0x%02x, ", nmsg->hdr.auth_key[i]);
	printf("\n");

    if ( nmsg->hdr.opcode == MINIVTUN_MSG_KEEPALIVE ) {

	   char in_addr_string[256];
	   char in6_addr_string[256];

	   inet_ntop(AF_INET, &(nmsg->keepalive.loc_tun_in), in_addr_string, 256);
	   inet_ntop(AF_INET6, &(nmsg->keepalive.loc_tun_in6), in6_addr_string, 256);
	   
	   printf("nmsg.keepalive: in %s, in6: %s\n", in_addr_string, in6_addr_string);

	}

	if ( nmsg->hdr.opcode == MINIVTUN_MSG_IPDATA ) {

	   printf("nmsg.ipdata: proto = 0x%x(0x%x), ip_dlen = %d(%d), data = ", 
	          nmsg->ipdata.proto, ntohs(nmsg->ipdata.proto), nmsg->ipdata.ip_dlen, ntohs(nmsg->ipdata.ip_dlen));

	   for ( i = 0; i < ntohs(nmsg->ipdata.ip_dlen); i++ )
	       printf("0x%02x, ", nmsg->ipdata.data[i]);
	   printf("\n");

	}
}

#endif // DEBUG


struct minivtun_msg * _network_data_handler(char * data_buffer, size_t data_buffer_len, size_t data_len, 
                                            void * out_buffer, size_t out_buffer_len, struct tun_pi * ppi);

void _tunnel_data_handler(void * data_buffer, size_t data_len, uint16_t proto, void * out_data, 
                          size_t out_buffer_len, size_t * out_dlen);

void _keepalive_make(void * out_msg, size_t out_buffer_len, size_t * out_data_len);

void set_config_params(const char * crypto_key);


#endif /* __MINIVTUN_H */

