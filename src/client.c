/*
 * Copyright (c) 2015 Justin Liu
 * Author: Justin Liu <rssnsj@gmail.com>
 * https://github.com/rssnsj/minivtun
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "minivtun.h"

#if !defined(__APPLE_NETWORK_EXTENSION__) && !defined(__ANDROID_VPN_SERVICE__)
  #include "client_route.h"
  #include "list.h"
#endif

static time_t last_recv = 0, last_keepalive = 0, current_ts = 0;

// This would be called by both network_receiving() and NE codes 
struct minivtun_msg * _network_data_handler(char * data_buffer, size_t data_len, void * out_buffer, struct tun_pi * ppi)
{
	void *out_data;
	size_t ip_dlen, out_dlen;
	struct minivtun_msg * nmsg;
	
	out_data = out_buffer;
	// out_dlen = (size_t)rc;
	out_dlen = data_len;
	// netmsg_to_local(read_buffer, &out_data, &out_dlen);
	netmsg_to_local(data_buffer, &out_data, &out_dlen);
	nmsg = out_data;

	if (out_dlen < MINIVTUN_MSG_BASIC_HLEN)
		return 0;

	/* Verify password. */
	if (memcmp(nmsg->hdr.auth_key, config.crypto_key, 
		sizeof(nmsg->hdr.auth_key)) != 0)
		return 0;

	last_recv = current_ts;

	switch (nmsg->hdr.opcode) {

	case MINIVTUN_MSG_KEEPALIVE:
		break;

	case MINIVTUN_MSG_IPDATA:
		if (nmsg->ipdata.proto == htons(ETH_P_IP)) {
			/* No packet is shorter than a 20-byte IPv4 header. */
			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 20)
				return 0;
		} else if (nmsg->ipdata.proto == htons(ETH_P_IPV6)) {
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


		// pi.flags = 0;
		// pi.proto = nmsg->ipdata.proto;
		// osx_ether_to_af(&pi.proto);
		
		// set_pi_with_ether_proto(&pi, ntohs(nmsg->ipdata.proto));
		set_pi_with_ether_proto(ppi, ntohs(nmsg->ipdata.proto));
		/*
		iov[0].iov_base = &pi;
		iov[0].iov_len = sizeof(pi);
		iov[1].iov_base = (char *)nmsg + MINIVTUN_MSG_IPDATA_OFFSET;
		iov[1].iov_len = ip_dlen;
		rc = writev(tunfd, iov, 2);
		break;
		*/
		return nmsg;
	}

	return 0;
}


#if !defined(__APPLE_NETWORK_EXTENSION__) || !defined(__ANDROID_VPN_SERVICE__)

// return read size in this call. < 0 if error (including EOF). *read_size returned total read size in this batch.
static 
int read_tcp_data_nonblocking(int fd, char * buffer, size_t * read_size, size_t total_size)
{
    assert(*read_size < total_size);

    size_t size_to_read = total_size - *read_size;

    ssize_t read_bytes = 0;
    do {
       read_bytes = read(fd, buffer, size_to_read);
    } while ( read_bytes < 0 && errno == EINTR );

    if ( read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
       return 0;

    if ( read_bytes < 0 ) {
#if DEBUG
       fprintf(stderr, "Error reading from fd %d, error %s\n", fd, strerror(errno));
#endif        
       return -1;
    }

    // EOF
    if ( read_bytes == 0 ) {
#if DEBUG
       fprintf(stderr, "EOF reading from fd %d\n", fd);
#endif        
       return -1;
    }

    *read_size += read_bytes;
    return read_bytes;
}

// Write buffer queue for nonblocking writing
static __LIST_HEAD(write_buffer_list);

struct write_buffer_entry {
    struct list_head list;
    char * buffer;
    size_t buffer_len;
    size_t offset;
};

// Do real socket writing. Return < 0 if writing error.
static
int write_tcp_data_nonblocking(int fd)
{
    // Get buffer in list head. It is OK if nothing to write
    if ( list_empty(&write_buffer_list) )
        return 0;

    struct write_buffer_entry * entry = list_first_entry(&write_buffer_list, struct write_buffer_entry, list);
    assert(entry->offset < entry->buffer_len - 1);

    // Write it.
    ssize_t written_size = 0;
    do {
        written_size = write(fd, entry->buffer + entry->offset, entry->buffer_len - entry->offset);
    } while ( written_size < 0 && errno == EINTR );

    if ( written_size < 0 ) {
       if ( errno == EAGAIN || errno == EWOULDBLOCK ) {
          return 0;          
       }
#if DEBUG
       fprintf(stderr, "write to fd %d failed: %s\n", fd, strerror(errno));
#endif     
       
       return -1;   
    }

    // EOF
    if ( written_size == 0 ) {
#if DEBUG
       fprintf(stderr, "write to fd %d EOF\n", fd);
#endif     
       return -1;   
    }

    // Success
    entry->offset += written_size;
    assert(entry->offset <= entry->buffer_len);

    if ( entry->offset == entry->buffer_len ) {
        list_del(&(entry->list));
        free(entry);
    }

    return written_size;
}

// Queue a buffer for writing if ready.
static
int queue_writing_data(char * write_buffer, size_t write_len)
{
    // Allocate
    struct write_buffer_entry * entry = 
           (struct write_buffer_entry *)malloc(sizeof(struct write_buffer_entry) + write_len);

    if ( entry == NULL ) {
       return -1;        
    }

    // Fill
    entry->buffer = (char *)(entry + 1);
    memcpy(entry->buffer, write_buffer, write_len);
    entry->buffer_len = write_len;
    entry->offset = 0;

    // Add
    list_add_tail(&(entry->list), &write_buffer_list);
    return 0;
}

// Clear whole writeing queue
static
void clear_writing_queue()
{
    struct write_buffer_entry * entry, *temp;

    list_for_each_entry_safe(entry, temp, &write_buffer_list, list) {
        list_del(&(entry->list));
        free(entry);
    }

    assert(list_empty(&write_buffer_list));
}

// Handling packets received from Internet.
//
// < 0 error. Should reconnect.
static int network_receiving(int tunfd, int sockfd)
{
	char read_buffer[NM_PI_BUFFER_SIZE], crypt_buffer[NM_PI_BUFFER_SIZE];
	struct minivtun_msg *nmsg;
	struct tun_pi pi;
	// void *out_data;
	// size_t ip_dlen, out_dlen;
	struct sockaddr_in real_peer;
	socklen_t real_peer_alen;
	struct iovec iov[2];
	int rc;

    // For non-blokcing tcp reading
    static char tcp_read_buffer[NM_PI_BUFFER_SIZE] = { 0 };
    static size_t tcp_read_buffer_len = 0;
    static size_t tcp_msg_len = 0; // == 0 means we should read msg_len, otherwise read net_msg data

    if ( config.use_tcp ) {

        if ( tcp_msg_len == 0 ) {
        
           int rv = read_tcp_data_nonblocking(sockfd, tcp_read_buffer, &tcp_read_buffer_len, sizeof(uint32_t));

           if ( rv < 0 ) {
#if DEBUG
              printf("Read from tcp fd %d error. Should close and reconnect\n", sockfd);
#endif            
              return -1;
           }

           // Not fully read in
           if ( tcp_read_buffer_len < sizeof(uint32_t) )
              return 0;

           // Got msg_len
           tcp_msg_len = ntohl(*(uint32_t *)tcp_read_buffer);
           tcp_read_buffer_len = 0;

#if DEBUG
           fprintf(stderr, "Read msg_len %zu from sockfd %d\n", tcp_msg_len, sockfd);
#endif 

           return 0;

        } // read msg_len

        // Read ata
        int rv = read_tcp_data_nonblocking(sockfd, tcp_read_buffer, &tcp_read_buffer_len, tcp_msg_len);
        if ( rv < 0 ) {
#if DEBUG
           printf("Read tcp data (len %zu) from fd %d failed\n", tcp_msg_len, sockfd);           
#endif            
           return -1;
        }

        // Not fully read
        if ( tcp_read_buffer_len < tcp_msg_len ) 
           return 0;

        // Got all 
#if DEBUG
        fprintf(stderr, "Read net_msg len %zu from sockfd %d\n", tcp_msg_len, sockfd);
#endif 
        memcpy(read_buffer, tcp_read_buffer, tcp_read_buffer_len);
        rc = tcp_msg_len;

        // Reset
        tcp_read_buffer_len = 0;
        tcp_msg_len = 0;
    }
    else {
	    real_peer_alen = sizeof(real_peer);
	    rc = (int)recvfrom(sockfd, &read_buffer, NM_PI_BUFFER_SIZE, 0, (struct sockaddr *)&real_peer, &real_peer_alen);
    }

#if DEBUG	
    printf("network receiving: Read %d bytes from network\n", rc);
    hexdump(read_buffer, rc);
#endif

	if (rc <= 0)
		return 0;

    // nmsg is not null for real IP data only.
	nmsg = _network_data_handler(read_buffer, rc, crypt_buffer, &pi);

#if DEBUG
    if ( nmsg == 0 )
	   printf("nmsg is NULL or keepalive\n");
	else
	   dump_nmsg(nmsg);
#endif	

    if ( nmsg != 0 ) {
  	    iov[0].iov_base = &pi;
        iov[0].iov_len = sizeof(pi);
    	iov[1].iov_base = (char *)nmsg + MINIVTUN_MSG_IPDATA_OFFSET;
		iov[1].iov_len = ntohs(nmsg->ipdata.ip_dlen); // ip_dlen;
		rc = (int)writev(tunfd, iov, 2);
#if DEBUG
        printf("write to tunnel. return %d\n", rc);
		if ( rc < 0 )
		   perror("writev");
#endif		
	}

	return 0;
//	out_data = crypt_buffer;
//	out_dlen = (size_t)rc;
//	netmsg_to_local(read_buffer, &out_data, &out_dlen);
//	nmsg = out_data;

//	if (out_dlen < MINIVTUN_MSG_BASIC_HLEN)
//		return 0;
 
	/* Verify password. */
//	if (memcmp(nmsg->hdr.auth_key, config.crypto_key, 
//		sizeof(nmsg->hdr.auth_key)) != 0)
//		return 0;

//	last_recv = current_ts;

//	switch (nmsg->hdr.opcode) {
//	case MINIVTUN_MSG_KEEPALIVE:
//		break;
//	case MINIVTUN_MSG_IPDATA:
//		if (nmsg->ipdata.proto == htons(ETH_P_IP)) {
			/* No packet is shorter than a 20-byte IPv4 header. */
//			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 20)
//				return 0;
//		} else if (nmsg->ipdata.proto == htons(ETH_P_IPV6)) {
//			if (out_dlen < MINIVTUN_MSG_IPDATA_OFFSET + 40)
//				return 0;
//		} else {
//			fprintf(stderr, "*** Invalid protocol: 0x%x.\n", ntohs(nmsg->ipdata.proto));
//			return 0;
//		}

//		ip_dlen = ntohs(nmsg->ipdata.ip_dlen);
		/* Drop incomplete IP packets. */
//		if (out_dlen - MINIVTUN_MSG_IPDATA_OFFSET < ip_dlen)
//			return 0;

		// pi.flags = 0;
		// pi.proto = nmsg->ipdata.proto;
		// osx_ether_to_af(&pi.proto);
//		set_pi_with_ether_proto(&pi, ntohs(nmsg->ipdata.proto));
//		iov[0].iov_base = &pi;
//		iov[0].iov_len = sizeof(pi);
//		iov[1].iov_base = (char *)nmsg + MINIVTUN_MSG_IPDATA_OFFSET;
//		iov[1].iov_len = ip_dlen;
//		rc = writev(tunfd, iov, 2);
//		break;
//	}

//	return 0;
}

#endif // !__APPLE_NETWORK_EXTENSION__


void _tunnel_data_handler(void * data_buffer, size_t data_len, uint16_t proto, void ** out_data, size_t * out_dlen)
{
	// char crypt_buffer[NM_PI_BUFFER_SIZE];
	// void *out_data;
	// size_t ip_dlen, out_dlen;
	struct minivtun_msg nmsg;

	nmsg.hdr.opcode = MINIVTUN_MSG_IPDATA;
	memset(nmsg.hdr.rsv, 0x0, sizeof(nmsg.hdr.rsv));
	memcpy(nmsg.hdr.auth_key, config.crypto_key, sizeof(nmsg.hdr.auth_key));
	nmsg.ipdata.proto = htons(proto); // htons(get_ether_proto_from_pi(pi)); // pi->proto;
	nmsg.ipdata.ip_dlen = htons(data_len); // htons(ip_dlen);
	memcpy(nmsg.ipdata.data, data_buffer, data_len);
	// nmsg.ipdata.ip_dlen = htons(ip_dlen);
	// memcpy(nmsg.ipdata.data, pi + 1, ip_dlen);

	/* Do encryption. */
	// out_data = crypt_buffer;
	// out_dlen = MINIVTUN_MSG_IPDATA_OFFSET + ip_dlen;
	// local_to_netmsg(&nmsg, &out_data, &out_dlen);	
	*out_dlen = MINIVTUN_MSG_IPDATA_OFFSET + data_len;
	local_to_netmsg(&nmsg, out_data, out_dlen);
}


#ifndef __APPLE_NETWORK_EXTENSION__

// Handling packets received from tunnel. That is, local applications send them to
// outside.
static int tunnel_receiving(int tunfd, int sockfd)
{
	char read_buffer[NM_PI_BUFFER_SIZE], crypt_buffer[NM_PI_BUFFER_SIZE];
	struct tun_pi *pi = (void *)read_buffer;
	// struct minivtun_msg nmsg;
	void *out_data;
	size_t ip_dlen, out_dlen;
	int rc;

	rc = (int)read(tunfd, pi, NM_PI_BUFFER_SIZE);
	if (rc < sizeof(struct tun_pi))
		return -1;

    //
	ip_dlen = (size_t)rc - sizeof(struct tun_pi);

	/* We only accept IPv4 or IPv6 frames. */
	uint16_t proto = get_ether_proto_from_pi(pi);

	if ( proto == ETH_P_IP ) {
		if (ip_dlen < 20)
			return 0;
	} else if ( proto == ETH_P_IPV6 ) {
		if (ip_dlen < 40)
			return 0;
	} else {
		fprintf(stderr, "*** Invalid protocol: 0x%x.\n", proto);
		return 0;
	}

//	nmsg.hdr.opcode = MINIVTUN_MSG_IPDATA;
//	memset(nmsg.hdr.rsv, 0x0, sizeof(nmsg.hdr.rsv));
//	memcpy(nmsg.hdr.auth_key, config.crypto_key, sizeof(nmsg.hdr.auth_key));
//	nmsg.ipdata.proto = htons(get_ether_proto_from_pi(pi)); // pi->proto;
//	nmsg.ipdata.ip_dlen = htons(ip_dlen);
//	memcpy(nmsg.ipdata.data, pi + 1, ip_dlen);

	/* Do encryption. */
//	out_data = crypt_buffer;
//	out_dlen = MINIVTUN_MSG_IPDATA_OFFSET + ip_dlen;
//	local_to_netmsg(&nmsg, &out_data, &out_dlen);

    out_data = crypt_buffer;

#if DEBUG
    if ( proto == ETH_P_IP ) {

        // IP v4: source addr: offset 12. destination addr: offset 16
        char from_addr[INET_ADDRSTRLEN + 1] = { 0 };
        uint32_t from_ip = *(uint32_t *)((uint8_t *)(pi + 1) + 12);
        inet_ntop(AF_INET, &from_ip, from_addr, INET_ADDRSTRLEN + 1);

        char to_addr[INET_ADDRSTRLEN + 1] = { 0 };
        uint32_t to_ip = *(uint32_t *)((uint8_t *)(pi + 1) + 16);
        inet_ntop(AF_INET, &to_ip, to_addr, INET_ADDRSTRLEN + 1);

        printf("Read %d bytes from tunnel. from %s to %s\n", rc, from_addr, to_addr);

    }
    else if ( proto == ETH_P_IPV6 ) {

        // IP v6: source addr: offset 8. destination addr: offset 24
        char from_addr[INET6_ADDRSTRLEN + 1] = { 0 };
        uint8_t * from_ip = (uint8_t *)(pi + 1) + 8;
        inet_ntop(AF_INET6, from_ip, from_addr, INET6_ADDRSTRLEN + 1);

        char to_addr[INET6_ADDRSTRLEN + 1] = { 0 };
        uint8_t * to_ip = (uint8_t *)(pi + 1) + 24;
        inet_ntop(AF_INET6, to_ip, to_addr, INET6_ADDRSTRLEN + 1);

        printf("Read %d bytes from tunnel. from %s to %s\n", rc, from_addr, to_addr);

    }
#endif

    _tunnel_data_handler(pi+1, ip_dlen, proto, &out_data, &out_dlen);

    if ( config.use_tcp ) {
        uint32_t msg_len = htonl(out_dlen);
        /*
        struct iovec iov[2];
        iov[0].iov_base = &msg_len;
        iov[0].iov_len = sizeof(uint32_t);
        iov[1].iov_base = out_data;
        iov[1].iov_len = out_dlen;

        rc = writev(sockfd, iov, sizeof(iov) / sizeof(struct iovec));
        */
        queue_writing_data((char *)&msg_len, sizeof(uint32_t));
        queue_writing_data((char *)out_data, out_dlen);

        rc = 1;

#if DEBUG
        printf("queue write msg_len %zu\n", out_dlen);
#endif

    }
    else {
	    rc = (int)send(sockfd, out_data, out_dlen, 0);
    }

#if DEBUG
    printf("tunnel -> network: %zu bytes\n", out_dlen);
    hexdump(out_data, out_dlen);
#endif	


	/**
	 * NOTICE: Don't update this on each tunnel packet
	 * transmit. We always need to keep the local virtual IP
	 * (-a local/...) alive.
	 */
	/* last_keepalive = current_ts; */

	return 0;
}

#endif // __APPLE_NETWORK_EXTENSION__

void _keepalive_make(void ** out_msg, size_t * out_len)
{
	char in_data[64]; //, crypt_buffer[64];
	struct minivtun_msg *nmsg = (struct minivtun_msg *)in_data;

	nmsg->hdr.opcode = MINIVTUN_MSG_KEEPALIVE;
	memset(nmsg->hdr.rsv, 0x0, sizeof(nmsg->hdr.rsv));
	memcpy(nmsg->hdr.auth_key, config.crypto_key, sizeof(nmsg->hdr.auth_key));
	nmsg->keepalive.loc_tun_in = config.local_tun_in;
	nmsg->keepalive.loc_tun_in6 = config.local_tun_in6;

	// out_msg = crypt_buffer;
	*out_len = MINIVTUN_MSG_BASIC_HLEN + sizeof(nmsg->keepalive);
	// local_to_netmsg(nmsg, &out_msg, &out_len);
	local_to_netmsg(nmsg, out_msg, out_len);
}

#ifndef __APPLE_NETWORK_EXTENSION__

// Send keep-alive packet out
static int peer_keepalive(int sockfd)
{
	// char in_data[64], crypt_buffer[64];
	char crypt_buffer[64];
	// struct minivtun_msg *nmsg = (struct minivtun_msg *)in_data;
	void *out_msg;
	size_t out_len;
	int rc;

	// nmsg->hdr.opcode = MINIVTUN_MSG_KEEPALIVE;
	// memset(nmsg->hdr.rsv, 0x0, sizeof(nmsg->hdr.rsv));
	// memcpy(nmsg->hdr.auth_key, config.crypto_key, sizeof(nmsg->hdr.auth_key));
	// nmsg->keepalive.loc_tun_in = config.local_tun_in;
	// nmsg->keepalive.loc_tun_in6 = config.local_tun_in6;

	out_msg = crypt_buffer;
	// out_len = MINIVTUN_MSG_BASIC_HLEN + sizeof(nmsg->keepalive);
	// local_to_netmsg(nmsg, &out_msg, &out_len);

    _keepalive_make(&out_msg, &out_len);

	if ( config.use_tcp ) {
        uint32_t msg_len = htonl(out_len);
        /*
        struct iovec iov[2];
        iov[0].iov_len = sizeof(uint32_t);
        iov[0].iov_base = &msg_len;
        iov[1].iov_len = out_len;
        iov[1].iov_base = out_msg;

        rc = writev(sockfd, iov, sizeof(iov) / sizeof(struct iovec));
        */
        queue_writing_data((char *)&msg_len, sizeof(uint32_t));
        queue_writing_data((char *)out_msg, out_len);

        rc = 1;
    }
    else {
	    rc = (int)send(sockfd, out_msg, out_len, 0);
    }

	/* Update 'last_keepalive' only when it's really sent out. */
	if (rc > 0) {
		last_keepalive = current_ts;
	}

	return rc;
}

// This function would be called each time that we need to re-establish virtual connecion
static int try_resolve_and_connect(const char *peer_addr_pair, struct sockaddr_inx *peer_addr)
{
	int sockfd, rc;

	if ((rc = get_sockaddr_inx_pair(peer_addr_pair, peer_addr)) < 0)
		return rc;

    if ( config.use_tcp ) {
        sockfd = socket(peer_addr->sa.sa_family, SOCK_STREAM, IPPROTO_TCP);
        if ( sockfd < 0 ) {
		   fprintf(stderr, "*** socket(SOCK_STREAM) failed: %s.\n", strerror(errno));
		   return -1;
        }
    }
    else {
	    if ((sockfd = socket(peer_addr->sa.sa_family, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		    fprintf(stderr, "*** socket(SOCK_DGRAM) failed: %s.\n", strerror(errno));
		    return -1;
	    }
    }

    // Here, we should detect possible default interface/ip address changes and bind to 
	// new address.
	struct sockaddr_in bind_addr_in;
	if ( config.bind_if[0] != 0 && get_ip_addr_of_interface(config.bind_if, &bind_addr_in) == 0 ) {
	   char * bind_addr_string = inet_ntoa(bind_addr_in.sin_addr);
	   if ( bind_addr_string != 0 )
	      strncpy(config.bind_to_addr, bind_addr_string, sizeof(config.bind_to_addr) - 1);
	   else
	      config.bind_to_addr[0] = 0;
	}
	
	//
	if ( config.bind_to_addr[0] != 0 ) {
	   struct sockaddr_in bind_in;
#ifdef __APPLE__ // linux doesn't have this field.	   
	   bind_in.sin_len = sizeof(struct sockaddr_in);
#endif	   
	   bind_in.sin_family = AF_INET;
	   bind_in.sin_port = 0;
	   inet_aton(config.bind_to_addr, &bind_in.sin_addr);
	   memset(bind_in.sin_zero, 0, 8);

	   if ( bind(sockfd, (struct sockaddr *)&bind_in, sizeof(struct sockaddr_in)) < 0 ) {
		  close(sockfd);
		  return -EADDRNOTAVAIL;
	   }
	}
	// 

	if (connect(sockfd, (struct sockaddr *)peer_addr, sizeof_sockaddr(peer_addr)) < 0) {
		close(sockfd);
		return -EAGAIN;
	}
	set_nonblock(sockfd);

	return sockfd;
}


static
int _reconnect(int sockfd, const char * peer_addr_pair, struct sockaddr_inx * peer_addr)
{
	char s_peer_addr[50];

			if (sockfd >= 0)
				close(sockfd);

			do {
				if ((sockfd = try_resolve_and_connect(peer_addr_pair, peer_addr)) < 0) {
					fprintf(stderr, "Unable to connect to '%s', retrying.\n", peer_addr_pair);
					sleep(5);
				}
			} while (sockfd < 0);

			last_keepalive = 0;
			last_recv = current_ts;

			inet_ntop(peer_addr->sa.sa_family, addr_of_sockaddr(peer_addr), s_peer_addr,
					  sizeof(s_peer_addr));
			printf("Reconnected to %s:%u. socket %d\n", s_peer_addr, ntohs(port_of_sockaddr(peer_addr)), sockfd);

    return sockfd;
}

// The entry. Called from main() in minivtun.c directly after ifconfig interfaces
//
// @param peer_addr_pair:  <remote-host>:<port>
int run_client(int tunfd, const char *peer_addr_pair)
{
	struct timeval timeo;
	int sockfd = -1, rc;
	fd_set rset, wset;
	char s_peer_addr[50];
	struct sockaddr_inx peer_addr;

	if ((sockfd = try_resolve_and_connect(peer_addr_pair, &peer_addr)) >= 0) {

		/* DNS resolve OK, start service normally. */
		last_recv = time(NULL);
		inet_ntop(peer_addr.sa.sa_family, addr_of_sockaddr(&peer_addr),
				  s_peer_addr, sizeof(s_peer_addr));
		printf("Mini virtual tunnelling client to %s:%u, interface: %s, bind to address %s\n",
				s_peer_addr, ntohs(port_of_sockaddr(&peer_addr)), config.devname, config.bind_to_addr);

	} else if (sockfd == -EAGAIN && config.wait_dns) {

		/* Resolve later (last_recv = 0). */
		last_recv = 0;
		printf("Mini virtual tunnelling client, interface: %s. \n", config.devname);
		printf("WARNING: Connection to '%s' temporarily unavailable, "
			   "to be tried later.\n", peer_addr_pair);

	} else if (sockfd == -EINVAL) {

		fprintf(stderr, "*** Invalid address pair '%s'.\n", peer_addr_pair);
		return -1;

	}

	else if ( sockfd == -EADDRNOTAVAIL ) {

		fprintf(stderr, "*** Cannot bind to address '%s'.\n", config.bind_to_addr);
		return -1;

	} else {
		fprintf(stderr, "*** Unable to connect to '%s'.\n", peer_addr_pair);
		return -1;
	}

    printf("Connected to server %s. sockfd %d\n", peer_addr_pair, sockfd);

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

	/* For triggering the first keep-alive packet to be sent. */
	last_keepalive = 0;

	for (;;) {

		FD_ZERO(&rset);
		FD_SET(tunfd, &rset);
		if (sockfd >= 0)
			FD_SET(sockfd, &rset);

        FD_ZERO(&wset);
        if ( sockfd >= 0 )
           FD_SET(sockfd, &wset);

		timeo.tv_sec = 2;
		timeo.tv_usec = 0;

		rc = select((tunfd > sockfd ? tunfd : sockfd) + 1, &rset, &wset, NULL, &timeo);
		if (rc < 0) {
			fprintf(stderr, "*** select(): %s.\n", strerror(errno));
			return -1;
		}

		current_ts = time(NULL);

		if (last_recv > current_ts)
			last_recv = current_ts;
		if (last_keepalive > current_ts)
			last_keepalive = current_ts;

		/* Packet transmission timed out, send keep-alive packet. */
		if (current_ts - last_keepalive > config.keepalive_timeo) {
			if (sockfd >= 0) {                
				peer_keepalive(sockfd);
            }

		}

		/* Connection timed out, try reconnecting. */
		if (current_ts - last_recv > config.reconnect_timeo) {

reconnect:
			/* Reopen the socket for a different local port. */
/*
			if (sockfd >= 0)
				close(sockfd);

			do {
				if ((sockfd = try_resolve_and_connect(peer_addr_pair, &peer_addr)) < 0) {
					fprintf(stderr, "Unable to connect to '%s', retrying.\n", peer_addr_pair);
					sleep(5);
				}
			} while (sockfd < 0);

			last_keepalive = 0;
			last_recv = current_ts;

			inet_ntop(peer_addr.sa.sa_family, addr_of_sockaddr(&peer_addr), s_peer_addr,
					  sizeof(s_peer_addr));
			printf("Reconnected to %s:%u. socket %d\n", s_peer_addr, ntohs(port_of_sockaddr(&peer_addr)), sockfd);
*/            
            sockfd = _reconnect(sockfd, peer_addr_pair, &peer_addr);
			continue;
		}

		/* No result from select(), do nothing. */
		if (rc == 0)
			continue;

		if (sockfd >= 0 && FD_ISSET(sockfd, &rset)) {
			rc = network_receiving(tunfd, sockfd);
			if (rc < 0) {
				fprintf(stderr, "Connection went bad. About to reconnect.\n");
				goto reconnect;
			}
		}

		if (FD_ISSET(tunfd, &rset)) {
			rc = tunnel_receiving(tunfd, sockfd);
			assert(rc == 0);
		}

        // socket is writable
        if ( sockfd >= 0 && FD_ISSET(sockfd, &wset) ) {
            int rv = write_tcp_data_nonblocking(sockfd);
#if DEBUG
            if ( rv > 0 )
               fprintf(stderr, "Write %d bytes to fd %d\n", rv, sockfd);
            else if ( rv == 0 ) {
               // fprintf(stderr, "Nothing to write to fd %d?\n", sockfd);
            }
            else 
               fprintf(stderr, "Write to fd %d failed.\n", rv);
#endif
            // If writing failed, should we reconnect it or wait for rset?
            if ( rv < 0 ) {
				fprintf(stderr, "Connection went bad due to writing failure. About to reconnect.\n");
                clear_writing_queue();
				goto reconnect;                
            }
        } // if writable

	} // for (;;)

	return 0;
}

#endif // ! __APPLE_NETWORK_EXTENSION__
