# Userspace NAT Server for minivtun

## Context
The current server writes decrypted inner packets to a TUN interface and relies on Linux kernel IP forwarding + iptables MASQUERADE for NAT. The new server must do all of this in userspace: receive VPN packets, NAT or route them using real OS sockets, synthesize IP responses, and re-encrypt for clients. No TUN, no kernel forwarding, no iptables.

---

## New Files

### `src/pktbuf.h` + `src/pktbuf.c`
Portable IP/TCP/UDP/ICMP packet building and checksum utilities.

**Headers (own definitions, avoid platform divergence):**
```c
struct pb_iphdr  { uint8_t ihl_ver, tos; uint16_t tot_len,id,frag_off; uint8_t ttl,protocol; uint16_t check; uint32_t saddr,daddr; } __attribute__((packed));
struct pb_ip6hdr { uint32_t ver_tc_flow; uint16_t payload_len; uint8_t nexthdr,hop_limit; uint8_t saddr[16],daddr[16]; } __attribute__((packed));
struct pb_tcphdr { uint16_t sport,dport; uint32_t seq,ack_seq; uint8_t doff_res,flags; uint16_t window,check,urg_ptr; } __attribute__((packed));
struct pb_udphdr { uint16_t sport,dport,len,check; } __attribute__((packed));
struct pb_icmphdr { uint8_t type,code; uint16_t check,id,seq; } __attribute__((packed));
// TCP flags
#define TH_FIN 0x01  #define TH_SYN 0x02  #define TH_RST 0x04
#define TH_PSH 0x08  #define TH_ACK 0x10
```

**Functions:**
```c
uint16_t pb_ip_checksum(const void *data, int len);
uint16_t pb_tcp_checksum4(uint32_t saddr, uint32_t daddr,
                          const void *tcp_seg, uint16_t tcp_len);
uint16_t pb_tcp_checksum6(const uint8_t *saddr, const uint8_t *daddr,
                          const void *tcp_seg, uint16_t tcp_len);
uint16_t pb_udp_checksum4(uint32_t saddr, uint32_t daddr,
                          const void *udp_seg, uint16_t udp_len);
uint16_t pb_udp_checksum6(const uint8_t *saddr, const uint8_t *daddr,
                          const void *udp_seg, uint16_t udp_len);
uint16_t pb_icmp_checksum(const void *data, int len);

/* Build complete IPv4 TCP segment into buf[]. Returns total length or -1.
 * opt_data/opt_len for TCP options (e.g. MSS); pass NULL/0 if none. */
int pb_build_tcp4(uint8_t *buf, size_t bufsz,
                  uint32_t saddr, uint32_t daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq,
                  uint8_t flags, uint16_t window,
                  const void *opt_data, uint8_t opt_len,
                  const void *payload, size_t payload_len);

/* Same for IPv6 TCP */
int pb_build_tcp6(uint8_t *buf, size_t bufsz,
                  const uint8_t *saddr, const uint8_t *daddr,
                  uint16_t sport, uint16_t dport,
                  uint32_t seq, uint32_t ack_seq,
                  uint8_t flags, uint16_t window,
                  const void *opt_data, uint8_t opt_len,
                  const void *payload, size_t payload_len);

int pb_build_udp4(uint8_t *buf, size_t bufsz,
                  uint32_t saddr, uint32_t daddr,
                  uint16_t sport, uint16_t dport,
                  const void *payload, size_t payload_len);

int pb_build_udp6(uint8_t *buf, size_t bufsz,
                  const uint8_t *saddr, const uint8_t *daddr,
                  uint16_t sport, uint16_t dport,
                  const void *payload, size_t payload_len);

/* ICMP echo reply (type=0) */
int pb_build_icmp4_reply(uint8_t *buf, size_t bufsz,
                         uint32_t saddr, uint32_t daddr,
                         uint16_t id, uint16_t seq,
                         const void *payload, size_t payload_len);

int pb_build_icmpv6_reply(uint8_t *buf, size_t bufsz,
                          const uint8_t *saddr, const uint8_t *daddr,
                          uint16_t id, uint16_t seq,
                          const void *payload, size_t payload_len);
```

**MSS option helper:**
```c
/* Fills 4-byte MSS option: kind=2, len=4, mss=htons(mss_val) */
void pb_mss_option(uint8_t opt[4], uint16_t mss_val);
```

---

### `src/natmap.h` + `src/natmap.c`
NAT connection tracking and VPN peer table.

**Structures:**
```c
/* VPN peer (connected client) */
struct vpn_peer {
    int              af;          /* AF_INET or AF_INET6 virtual IP */
    struct in_addr   vip4;
    struct in6_addr  vip6;
    struct sockaddr_storage udp_addr; /* client's real UDP endpoint */
    time_t           last_active;
    struct list_head node;
};

enum tcp_state {
    TCP_CONNECTING,     /* async connect() in flight */
    TCP_SYN_ACK_SENT,   /* SYN-ACK sent to client, waiting for ACK */
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,       /* client sent FIN */
    TCP_CLOSE_WAIT,     /* real server closed */
    TCP_CLOSED,
};

struct tcp_conn {
    int              af;          /* AF_INET or AF_INET6 */
    /* 5-tuple (client side) */
    struct in_addr   clt_vip4;   struct in6_addr clt_vip6;
    uint16_t         clt_port;
    struct in_addr   dst_ip4;    struct in6_addr dst_ip6;
    uint16_t         dst_port;
    int              fd;          /* real TCP socket, non-blocking */
    enum tcp_state   state;
    /* TCP sequence tracking */
    uint32_t clt_iss;   /* client's ISN (from SYN) */
    uint32_t clt_seq;   /* next expected seq from client */
    uint32_t srv_iss;   /* our ISN (chosen at SYN-ACK time) */
    uint32_t srv_seq;   /* next seq we send to client */
    struct vpn_peer *peer;
    time_t           last_active;
    struct list_head node;
};

struct udp_flow {
    int              af;
    struct in_addr   clt_vip4;   struct in6_addr clt_vip6;
    uint16_t         clt_port;
    struct in_addr   dst_ip4;    struct in6_addr dst_ip6;
    uint16_t         dst_port;
    int              fd;          /* connected UDP socket */
    struct vpn_peer *peer;
    time_t           last_active;
    struct list_head node;
};

struct icmp_flow {
    int              af;
    struct in_addr   clt_vip4;   struct in6_addr clt_vip6;
    struct in_addr   dst_ip4;    struct in6_addr dst_ip6;
    uint16_t         orig_id;    /* ICMP ID in client's packet */
    uint16_t         mapped_id;  /* bound port (= ICMP ID used on wire) */
    int              fd;         /* SOCK_DGRAM ICMP/ICMPv6 socket */
    struct vpn_peer *peer;
    time_t           last_active;
    struct list_head node;
};
```

**API:**
```c
void natmap_init(void);

/* Peer management */
struct vpn_peer *peer_get_or_create(int af, const void *vip,
                                    const struct sockaddr_storage *udp_addr);
struct vpn_peer *peer_find_by_vip4(const struct in_addr *vip);
struct vpn_peer *peer_find_by_vip6(const struct in6_addr *vip);

/* TCP */
struct tcp_conn *tcp_conn_find(int af, const void *clt_vip, uint16_t clt_port,
                               const void *dst_ip, uint16_t dst_port);
struct tcp_conn *tcp_conn_create(int af, const void *clt_vip, uint16_t clt_port,
                                 const void *dst_ip, uint16_t dst_port,
                                 struct vpn_peer *peer);
void tcp_conn_remove(struct tcp_conn *c);
struct tcp_conn *tcp_conn_find_by_fd(int fd);

/* UDP */
struct udp_flow *udp_flow_find(int af, const void *clt_vip, uint16_t clt_port,
                               const void *dst_ip, uint16_t dst_port);
struct udp_flow *udp_flow_create(int af, const void *clt_vip, uint16_t clt_port,
                                 const void *dst_ip, uint16_t dst_port,
                                 struct vpn_peer *peer);
void udp_flow_remove(struct udp_flow *f);
struct udp_flow *udp_flow_find_by_fd(int fd);

/* ICMP */
struct icmp_flow *icmp_flow_find(int af, const void *clt_vip,
                                 const void *dst_ip, uint16_t orig_id);
struct icmp_flow *icmp_flow_create(int af, const void *clt_vip,
                                   const void *dst_ip, uint16_t orig_id,
                                   struct vpn_peer *peer);
void icmp_flow_remove(struct icmp_flow *f);
struct icmp_flow *icmp_flow_find_by_fd(int fd);

/* Poll array building: appends fds/events for all NAT entries */
int natmap_build_pollfd(struct pollfd *pfds, int max_fds, int flags_tcp_connecting);

/* Timeout cleanup (call every ~5s) */
void natmap_expire(time_t now, unsigned tcp_timeo, unsigned udp_timeo,
                   unsigned icmp_timeo, unsigned peer_timeo);
```

**Timeouts:** TCP: 300s, UDP: 60s, ICMP: 30s, Peer: config.reconnect_timeo

**Hash tables:** Use jhash (already in tree). 4 hash tables: peers (by vip), tcp_conns, udp_flows, icmp_flows. Size: 16 buckets each with list_head chains.

---

## Rewritten: `src/server.c`

**Signature change:** `int run_server(const char *loc_addr_pair)` — no tunfd.

**Virtual route table:** Kept from old server.c — `vt_route_add()`, `vt_route_lookup()`.

**Main loop:**
```
run_server(loc_addr_pair):
    resolve addr, bind UDP socket, init natmap
    if in_background: do_daemonize()

    loop:
        build poll array:
            pfds[0] = {sockfd, POLLIN}
            natmap_build_pollfd(pfds+1, MAX_FDS-1, ...)

        poll(pfds, nfds, 2000ms)
        current_ts = time(NULL)

        if pfds[0].revents & POLLIN:
            handle_vpn_packet(sockfd)

        for i in 1..nfds-1:
            find NAT entry by pfds[i].fd:
                tcp_conn: handle_tcp_nat(entry, pfds[i].revents, sockfd)
                udp_flow: handle_udp_nat(entry, sockfd)
                icmp_flow: handle_icmp_nat(entry, sockfd)

        if now - last_walk >= 5:
            natmap_expire(...)
            peer_keepalive_walk(sockfd)
            last_walk = now
```

**`handle_vpn_packet(sockfd)`:**
```
recvfrom → read_buffer, real_peer_addr
decrypt (netmsg_to_local)
verify auth_key

switch opcode:
  KEEPALIVE:
    peer_get_or_create(vip, real_peer_addr)->last_active = now
    send keepalive reply

  IPDATA:
    parse proto (ETH_P_IP → AF_INET, ETH_P_IPV6 → AF_INET6)
    extract inner IP packet (nmsg->ipdata.data, ip_dlen)
    update/get peer entry by (src_vip, real_peer_addr)

    determine dst_ip from inner packet:
      if dst matches another vpn_peer:
        forward_to_peer(inner_pkt, ip_dlen, proto, dst_peer, sockfd)
      else:
        dispatch_to_nat(inner_pkt, ip_dlen, af, src_peer, sockfd)
```

**`dispatch_to_nat(pkt, len, af, peer, sockfd)`:**
```
if af == AF_INET:
    iph = (pb_iphdr*)pkt
    protocol = iph->protocol
    if protocol == IPPROTO_TCP: handle_client_tcp4(iph, peer, sockfd)
    if protocol == IPPROTO_UDP: handle_client_udp4(iph, peer, sockfd)
    if protocol == IPPROTO_ICMP: handle_client_icmp4(iph, peer, sockfd)
else (AF_INET6):
    ip6h = (pb_ip6hdr*)pkt
    nexthdr = ip6h->nexthdr
    similar dispatch for TCP/UDP/ICMPv6
```

**`handle_client_tcp4(iph, peer, sockfd)`:**
```
tcph = (pb_tcphdr*)((uint8_t*)iph + ihl)
clt_port = tcph->sport, dst_port = tcph->dport
clt_vip = iph->saddr, dst_ip = iph->daddr

conn = tcp_conn_find(AF_INET, &clt_vip, clt_port, &dst_ip, dst_port)

if SYN and no conn:
    conn = tcp_conn_create(...)
    conn->clt_iss = ntohl(tcph->seq)
    conn->clt_seq = conn->clt_iss + 1
    conn->srv_iss = (uint32_t)random()
    conn->srv_seq = conn->srv_iss + 1  // after SYN-ACK is sent
    fd = socket(AF_INET, SOCK_STREAM, 0); set_nonblock(fd)
    connect(fd, dst_addr, ...) -- EINPROGRESS is OK
    conn->fd = fd, conn->state = TCP_CONNECTING
    return

if RST:
    if conn: synthesize RST to client, tcp_conn_remove(conn)
    return

if not conn: return  // unknown flow, drop

update conn->last_active

switch conn->state:
  TCP_CONNECTING: ignore (shouldn't get data before connect)
  TCP_SYN_ACK_SENT:
    if ACK flag and ack_seq == conn->srv_iss+1:
        conn->state = TCP_ESTABLISHED
  TCP_ESTABLISHED:
    payload = (uint8_t*)tcph + (tcph->doff_res>>4)*4
    payload_len = ntohs(iph->tot_len) - ihl - tcp_hdr_len
    if payload_len > 0:
        n = send(conn->fd, payload, payload_len, 0)
        if n <= 0: handle error, RST
        conn->clt_seq = ntohl(tcph->seq) + payload_len
        send_ack_to_client(conn, sockfd)  // ACK with srv_seq, ack=clt_seq
    if FIN flag:
        conn->state = TCP_FIN_WAIT
        shutdown(conn->fd, SHUT_WR)
        send_ack_to_client(conn, sockfd)  // ACK the FIN
```

**`handle_tcp_nat(conn, revents, sockfd)`:**
```
if revents & POLLOUT and state == TCP_CONNECTING:
    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err)
    if err != 0:
        synthesize RST to client; tcp_conn_remove; return
    // Send SYN-ACK
    opt[4] = MSS option (1260)
    buf = pb_build_tcp4(dst_ip, clt_vip, dst_port, clt_port,
                        conn->srv_iss, conn->clt_iss+1,
                        TH_SYN|TH_ACK, 65535, opt, 4, NULL, 0)
    send_inner_to_client(conn->peer, ETH_P_IP, buf, len, sockfd)
    conn->state = TCP_SYN_ACK_SENT
    return

if revents & (POLLIN|POLLHUP) and state >= TCP_ESTABLISHED:
    n = read(conn->fd, buf, sizeof(buf))
    if n == 0 or (n < 0 and errno != EAGAIN):
        // Server closed
        pkt = pb_build_tcp4(dst_ip, clt_vip, dst_port, clt_port,
                            conn->srv_seq, conn->clt_seq,
                            TH_FIN|TH_ACK, 65535, NULL, 0, NULL, 0)
        send_inner_to_client(conn->peer, ETH_P_IP, pkt, len, sockfd)
        conn->srv_seq++
        tcp_conn_remove(conn)
        return
    // Data from server
    pkt = pb_build_tcp4(dst_ip, clt_vip, dst_port, clt_port,
                        conn->srv_seq, conn->clt_seq,
                        TH_PSH|TH_ACK, 65535, NULL, 0, buf, n)
    send_inner_to_client(conn->peer, ETH_P_IP, pkt, pkt_len, sockfd)
    conn->srv_seq += n
```

**`handle_client_udp4(iph, peer, sockfd)`:**
```
udph = payload pointer
flow = udp_flow_find(AF_INET, &clt_vip, udph->sport, &dst_ip, udph->dport)
if not flow:
    flow = udp_flow_create(...)
    fd = socket(AF_INET, SOCK_DGRAM, 0)
    connect(fd, &dst_addr)  // connected UDP socket
    flow->fd = fd
send(flow->fd, udp_payload, payload_len, 0)
flow->last_active = now
```

**`handle_udp_nat(flow, sockfd)`:**
```
n = recv(flow->fd, buf, sizeof(buf), 0)
pkt = pb_build_udp4(dst_ip4, clt_vip4, dst_port, clt_port, buf, n)
send_inner_to_client(flow->peer, ETH_P_IP, pkt, pkt_len, sockfd)
flow->last_active = now
```

**`handle_client_icmp4(iph, peer, sockfd)`:**
```
icmph = (pb_icmphdr*)((uint8_t*)iph + ihl)
if icmph->type != 8 (ECHO REQUEST): return
orig_id = ntohs(icmph->id)
flow = icmp_flow_find(AF_INET, &clt_vip, &dst_ip, orig_id)
if not flow:
    flow = icmp_flow_create(...)
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)
    // If SOCK_DGRAM fails (needs root), retry with SOCK_RAW
    bind to port 0 → get mapped_id from getsockname
    flow->fd = fd, flow->mapped_id = mapped_id
// Rewrite ICMP ID to mapped_id, recompute checksum
modified_icmph = *icmph; modified_icmph.id = htons(flow->mapped_id)
modified_icmph.check = 0
modified_icmph.check = pb_icmp_checksum(&modified_icmph + rest...)
sendto(flow->fd, &modified_icmph + rest, len, 0, &dst_addr)
flow->last_active = now
```

**`handle_icmp_nat(flow, sockfd)`:**
```
n = recv(flow->fd, buf, sizeof(buf), 0)  // kernel strips IP header for SOCK_DGRAM
// buf contains ICMP header (type=0, code=0, id=mapped_id, seq, data)
// Restore orig_id
icmph = (pb_icmphdr*)buf
icmph->id = htons(flow->orig_id)
icmph->check = 0; icmph->check = pb_icmp_checksum(buf, n)
pkt = pb_build_icmp4_reply(dst_ip4, clt_vip4, flow->orig_id, ntohs(icmph->seq),
                           icmph_data, data_len)
send_inner_to_client(flow->peer, ETH_P_IP, pkt, pkt_len, sockfd)
flow->last_active = now
```

**`send_inner_to_client(peer, proto, ip_pkt, ip_len, sockfd)`:**
```
// Wrap ip_pkt in minivtun_msg IPDATA and send encrypted to peer
struct minivtun_msg nmsg
nmsg.hdr.opcode = MINIVTUN_MSG_IPDATA
memcpy(nmsg.hdr.auth_key, config.crypto_key, ...)
nmsg.ipdata.proto = htons(proto)  // ETH_P_IP or ETH_P_IPV6
nmsg.ipdata.ip_dlen = htons(ip_len)
memcpy(nmsg.ipdata.data, ip_pkt, ip_len)
out_data = crypt_buffer; out_dlen = MINIVTUN_MSG_IPDATA_OFFSET + ip_len
local_to_netmsg(&nmsg, &out_data, &out_dlen)
sendto(sockfd, out_data, out_dlen, 0, &peer->udp_addr, sizeof_sockaddr_storage(&peer->udp_addr))
```

**`forward_to_peer(pkt, len, proto, dst_peer, sockfd)`:**
```
// Direct client-to-client forwarding — same as send_inner_to_client
send_inner_to_client(dst_peer, proto, pkt, len, sockfd)
```

**Inter-client destination lookup:**
```
dst_peer = peer_find_by_vip4(&dst_ip)
if not found and vt_routes: gw = vt_route_lookup(&dst_ip) → peer_find_by_vip4(gw)
```

---

## Modified: `src/minivtun.c`

Change server mode path to skip TUN:
```c
if (loc_addr_pair) {
    /* Server mode: no tun needed */
    if (enabled_encryption()) { ... }
    run_server(loc_addr_pair);   // no tunfd
} else if (peer_addr_pair) {
    if ((tunfd = tun_alloc(...)) < 0) { ... }
    /* configure tun, setup encryption */
    run_client(tunfd, peer_addr_pair);
}
```

Remove the `tun_alloc()` call from the server path and the `ifconfig` / `route` calls (they're only needed for client mode).

---

## Modified: `src/minivtun.h`

```c
// Change:
int run_server(int tunfd, const char *loc_addr_pair);
// To:
int run_server(const char *loc_addr_pair);
```

---

## Modified: `src/Makefile`

```makefile
minivtun: minivtun.o library.o server.o client.o client_route.o pktbuf.o natmap.o
    $(CC) $(LDFLAGS) -o $@ $^ -lcrypto
```

---

## Platform Notes

**ICMP sockets (Linux):** `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)` works without root since kernel 3.x. The bound port is used as ICMP echo ID. Check `/proc/sys/net/ipv4/ping_group_range`.

**ICMP sockets (macOS):** `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)` requires root (or entitlements). Fall back to `SOCK_RAW, IPPROTO_ICMP` if needed.

**ICMPv6 (both):** `socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6)` or `SOCK_RAW`. Need `ICMP6_FILTER` to limit received types to echo replies.

**`poll()` vs `select()`:** Use `poll()` throughout — no fd_set size limits, better API.

**IPv6 TCP/UDP sockets:** `socket(AF_INET6, SOCK_STREAM/DGRAM, ...)` with `sockaddr_in6` for external IPv6 connections.

---

## Simplifications / Known Limitations
- No TCP flow control (fixed advertise window 65535, no window scaling)
- No SACK, no timestamp option
- TCP retransmits from client: detected by `seq < conn->clt_seq`, silently dropped
- No IP fragmentation handling (VPN MTU of 1300 prevents it in practice)
- UDP: connected sockets — works for most protocols but not for protocols that reply from a different address
- Poll array rebuilt from scratch every iteration (fine for typical load)
- SOCK_RAW ICMP fallback tried if SOCK_DGRAM ICMP fails

---

## Verification
1. `cd src && make` — must compile clean
2. Server: `sudo ./minivtun -l 0.0.0.0:1414 -a 10.7.0.1/24 -e test`
3. Client: `sudo ./minivtun -r <server>:1414 -a 10.7.0.2/24 -e test`
4. From client: `ping 8.8.8.8` — ICMP NAT test
5. From client: `curl http://example.com` — TCP NAT test
6. From client: `nslookup google.com 8.8.8.8` — UDP NAT test
7. Two clients: ping between client virtual IPs — inter-client routing test
