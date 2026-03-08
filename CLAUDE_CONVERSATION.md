# CLAUDE_CONVERSATION.md

Full conversation log including thinking steps.

---

> **Note:** This file captures the session beginning 2026-03-07. Earlier sessions (2026-03-01 through 2026-03-06) are summarised in the "Previous Session Summary" section below because that portion of the conversation was compacted before this session began.

---

## Previous Session Summary

The following is a summary of all prior work before this session's conversation began.

### Sessions 1–2 (2026-03-01): Project setup and CLAUDE.md

- Created `CLAUDE.md` describing build commands and high-level architecture.
- Analysed the codebase: `minivtun` is a UDP-based VPN tunneller with server and client modes, AES/DES/RC4 encryption via OpenSSL, and a custom `minivtun_msg` wire protocol.

### Sessions 3–5 (2026-03-02 to 2026-03-04): Userspace NAT server rewrite

- Rewrote `server.c` completely: replaced the TUN-based server with a userspace NAT implementation requiring no TUN interface, no iptables, no kernel forwarding.
- New files: `src/pktbuf.h/c` (portable IP/TCP/UDP/ICMP header structs and packet builders), `src/natmap.h/c` (VPN peer table + TCP/UDP/ICMP NAT connection tracking with hash tables).
- `run_server()` signature changed to drop `tunfd` parameter; `minivtun.c` updated to skip `tun_alloc()` in server mode.
- TCP NAT: async `connect()` + synthesised SYN-ACK with random ISN, full seq/ack tracking, FIN/RST handling, MSS clamping.
- UDP NAT: connected `SOCK_DGRAM` sockets per 5-tuple.
- ICMP NAT: `SOCK_DGRAM` ICMP (with `SOCK_RAW` fallback); ID rewriting and checksum fixup.
- Fixed two TCP correctness bugs (duplicate data on retransmit, wrong SYN-ACK sequence number).
- Fixed two ICMP parallel-ping bugs (missing reply-ID filter on `SOCK_RAW`, unique `mapped_id` from per-process counter instead of broken `getsockname()`).
- Implemented client receive-window tracking (`clt_ack`/`clt_wnd`) and a drain loop capped to the window to avoid stalling >64 KB connections.
- Added `-i`/`--random-iv` option for per-message random IV (new wire format; both peers must use it).
- Fixed TCP MTU overflow and macOS default-route detection.
- Added DEBUG packet dump statements.

### Session 6 (2026-03-06): Full test suite + SIGPIPE fix + idle-reconnect investigation

**Tests run:** ICMP 4-parallel (0% loss), UDP DNS 16/16, TCP HTTP/H1/H2.

**SIGPIPE crash discovered and fixed:**
- 8-parallel H2 test caused all connections to return `000` and the server process died.
- Root cause: `send()` on a broken TCP socket delivers `SIGPIPE` (default: kill process).
- First attempt used `MSG_NOSIGNAL` on both `send()` calls, but user asked about macOS compatibility — `MSG_NOSIGNAL` is Linux-only.
- Final fix: `signal(SIGPIPE, SIG_IGN)` in `run_server()` before the main loop — portable for both Linux and macOS.
- Verified: 8-parallel and 12-parallel H2 tests all succeed with server surviving.

**Cloudflare 000 in mixed-protocol storm:** confirmed as Cloudflare server-side bandwidth throttling (not a VPN bug). Non-CF targets (GitHub, YouTube, Apple) all returned 200.

**Committed:** all changes as commit `3ae92a8` on branch `userspace-nat-server`.

**Idle-reconnect investigation began:**
- User reported: `curl https://www.google.com` after 10 minutes of VPN idle consistently fails on the first attempt, then succeeds on runs 2–5.
- IPv6/Happy Eyeballs ruled out: `curl --ipv4` also fails on first run after idle.
- Root cause hypothesis at end of session: client reconnects because server never sends keepalives back → `last_recv` goes stale → after `reconnect_timeo` (60s) client closes old UDP socket and opens a new one.

---

## Session 7 (2026-03-07): Fix idle-reconnect race condition

---

### User — message 1

_(Session continues from previous context. The last task was investigating the "first connection after 10-minute idle fails" issue.)_

---

### Assistant — analysis and fix

**Root cause analysis:**

Reading `server.c` and `client.c` in full:

1. `peer_walk()` in `natmap.c` sends keepalives to a peer only when `now - p->last_active > keepalive_timeo (13s)`. But the client sends keepalives every 13s, keeping `p->last_active` perpetually fresh. So the condition is never satisfied and the server **never sends keepalives back to the client**.

2. The client's `last_recv` (updated in `_network_data_handler` before the switch, line 53 of `client.c`) only advances when the client actually receives a packet from the server. Since the server never sends anything during idle, `last_recv` drifts.

3. After `reconnect_timeo = 60s` without receiving from the server, the client closes the old UDP socket and opens a new one with a different source port (`try_resolve_and_connect` returns a fresh socket).

4. **The race:** The reconnect check (`current_ts - last_recv > 60`) runs on every poll loop iteration, **before** processing the TUN data. Scenario:
   - Loop iteration N: `current_ts - last_recv = 59s` → no reconnect; TUN has data (curl SYN) → `tunnel_receiving()` sends IPDATA(SYN) from socket port N.
   - Loop iteration N+1 (up to 2s later): `current_ts - last_recv = 61s` → **RECONNECT** fires. Socket port N is closed; new socket port N+1 opened.
   - Server already sent SYN-ACK to port N → dropped (socket closed).
   - Curl gets no SYN-ACK, retransmits SYN → server ignores it (state is `TCP_SYN_ACK_SENT`, not `TCP_CONNECTING`) → TCP handshake never completes → 15s timeout.

5. After the reconnect, `last_recv = current_ts` is reset, so runs 2–5 have a fresh 60s window with no reconnect → they all succeed.

**The fix** (added to `server.c`):

Added `send_keepalive_to_peer()` helper function that builds and sends a KEEPALIVE VPN packet to a peer's UDP address. Called from the KEEPALIVE handler in `network_receiving()` immediately after updating `peer->last_active`.

```c
/* Send a VPN keepalive packet to a peer (used to acknowledge client keepalives
 * so that the client's last_recv stays fresh and it doesn't reconnect) */
static void send_keepalive_to_peer(int sockfd, struct vpn_peer *peer)
{
    char in_data[64], crypt_buffer[128];
    struct minivtun_msg *nmsg = (struct minivtun_msg *)in_data;
    void *out_msg;
    size_t out_len;
    socklen_t alen;

    nmsg->hdr.opcode = MINIVTUN_MSG_KEEPALIVE;
    memset(nmsg->hdr.rsv, 0, sizeof(nmsg->hdr.rsv));
    memcpy(nmsg->hdr.auth_key, config.crypto_key, sizeof(nmsg->hdr.auth_key));
    nmsg->keepalive.loc_tun_in  = config.local_tun_in;
    nmsg->keepalive.loc_tun_in6 = config.local_tun_in6;

    out_msg = crypt_buffer;
    out_len = MINIVTUN_MSG_BASIC_HLEN + sizeof(nmsg->keepalive);
    local_to_netmsg(nmsg, &out_msg, &out_len);

    alen = (peer->udp_addr.ss_family == AF_INET6)
           ? sizeof(struct sockaddr_in6)
           : sizeof(struct sockaddr_in);
    sendto(sockfd, out_msg, out_len, 0,
           (struct sockaddr *)&peer->udp_addr, alen);
}
```

KEEPALIVE handler updated:

```c
if (is_valid_unicast_in(&ka_in4)) {
    peer = peer_get_or_create(AF_INET, &ka_in4, &real_peer);
    if (peer) {
        peer->last_active = time(NULL);
        /* Echo keepalive back so client's last_recv stays
         * fresh and it doesn't close/reopen its UDP socket */
        send_keepalive_to_peer(sockfd, peer);
    }
}
if (is_valid_unicast_in6(&ka_in6)) {
    peer = peer_get_or_create(AF_INET6, &ka_in6, &real_peer);
    if (peer) {
        peer->last_active = time(NULL);
        send_keepalive_to_peer(sockfd, peer);
    }
}
```

**Build and deploy:**

```
cd src && make          # clean build on macOS
rsync src/ t1.freecoder.org:~/minivtun-src/
ssh t1.freecoder.org "cd ~/minivtun-src && make"
ssh t1.freecoder.org "pkill minivtun; cd ~/minivtun-src && \
    ./minivtun -l 0.0.0.0:1194 -a 10.5.5.1/24 -e Hello -k 13 -d"
```

**75-second verification (sub-test):**

```
Waiting 75 seconds...   # old behavior: reconnect cycle guaranteed at 61s
Run 1: 302              # would have failed without the fix
Run 2: 302
Run 3: 302
Run 4: 302
Run 5: 302
Client log: (empty — no reconnects)
```

**Committed:** `665c6bd` — "Fix idle reconnect race: server echoes keepalive back to client"

---

### User — message 2

> Rerun the previous test, i.e. run 5 curl after 10 minutes

---

### Assistant

Checked client still running (`pid 3082`), then:

```
Starting 10-minute idle wait at Fri Mar  6 22:10:23 CST 2026
Done waiting at Fri Mar  6 22:20:23 CST 2026

Client log (reconnects during idle): (empty)

Run 1: 302
Run 2: 302
Run 3: 302
Run 4: 302
Run 5: 302
```

All 5 succeed. Zero reconnects logged during the 10-minute idle period.

---

### User — message 3

> Try run 5 times curl https://www.google.com after 10 minutes from the container, to see whether it works or not.

---

### Assistant

Verified client still running (same pid 3082), waited another 10 minutes:

```
Starting 10-minute idle wait at Fri Mar  6 23:06:57 CST 2026
Done at Fri Mar  6 23:16:57 CST 2026

Client log: (empty)

Run 1: 302
Run 2: 302
Run 3: 302
Run 4: 302
Run 5: 302
```

Fix confirmed stable across multiple 10-minute idle cycles.

---

### User — message 4

> Does current client code behave as the original code? Can you use original client to connect to current server?

---

### Assistant — compatibility analysis

**Wire protocol compatibility:**

Inspected `git show 7421f92 -- src/library.c` (the rewrite commit). The random-IV change in `library.c` is **conditional** on `config.random_iv` (set only by `-i`/`--random-iv`):

```c
// library.c lines 80–88
if (config.random_iv) {
    /* Generate a fresh random IV for every message and prepend it. */
    ...
} else {
    /* Legacy mode: fixed IV, no IV prefix in the wire packet. */
    memcpy(iv, crypto_ivec_initdata, iv_len);
}
```

Without `-i`, the wire format is identical to the original (`crypto_ivec_initdata` fixed IV, ciphertext at byte 0). The current server is running without `-i`.

**Changes to `client.c` since original** (from `git show 7421f92`):

| Change | Nature |
|--------|--------|
| Buffer sizes `NM_PI_BUFFER_SIZE` → `NM_CRYPTO_BUF_SIZE` | Internal only, no wire change |
| `recvfrom` size arg: `NM_PI_BUFFER_SIZE` → `sizeof(read_buffer)` | Bug fix |
| Guard `rc <= 0` before `sizeof()` comparison | Bug fix (unsigned wrap) |
| `peer_keepalive` `crypt_buffer`: 64 → 128 bytes | IV headroom; original 64-byte buffer still handles ≤48-byte keepalive |
| TUN read error triggers reconnect instead of `assert()` | Behaviour improvement |

**Practical verification:**

1. Extracted original source tree from git commit `67b9b42` (pre-rewrite).
2. Copied to container (aarch64), built with `make`.
3. Stopped modified client, started original:

```
nohup /tmp/orig-client/minivtun -r 43.167.168.193:1194 \
    -a 10.5.5.2/10.5.5.1 -e vpnpassword -n tun0 &
ping -c 3 8.8.8.8   → 0% loss, ~152 ms
```

4. 10-minute idle test with original client:

```
Starting 10-minute idle wait at Fri Mar  6 23:36:34 CST 2026
Done at Fri Mar  6 23:46:34 CST 2026

Client log: (empty — no reconnects)

Run 1: 302
Run 2: 302
Run 3: 302
Run 4: 302
Run 5: 302
```

**Conclusion:** The original unmodified client works correctly with the current server. The server's keepalive-echo fix is fully backward compatible — the original client accepts server keepalives (updates `last_recv`) and benefits from the fix identically to the modified client.

---

### User — message 5

> Now write whole conversation content to a file CLAUDE_CONVERSATION.md in the project directory, Overwrite it if there is an old one.

---

### Assistant

Wrote this file (`CLAUDE_CONVERSATION.md`).

---

## Files changed this session

| File | Change |
|------|--------|
| `src/server.c` | Added `send_keepalive_to_peer()` helper; server now echoes keepalive back to client on every received keepalive |

## Commits this session

| Hash | Message |
|------|---------|
| `665c6bd` | Fix idle reconnect race: server echoes keepalive back to client |

## Deployment state

- **Server binary:** `~/minivtun-src/minivtun` on `t1.freecoder.org` (port 1194, daemon mode)
- **Start command:** `cd ~/minivtun-src && ./minivtun -l 0.0.0.0:1194 -a 10.5.5.1/24 -e Hello -k 13 -d`
- **Test client:** Docker container `ubuntu-ssh-1` (aarch64), using either `/tmp/minivtun-src/src/minivtun` (modified) or `/tmp/orig-client/minivtun` (original) — both work identically with the current server
