# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

All source is under `src/`. Build from that directory:

```bash
cd src
make                  # compile minivtun binary
make DEBUG=1          # compile with debug symbols and DEBUG=1 macro
sudo make install     # install to /opt/local/sbin/ (macOS) or /usr/sbin/ (Linux)
make clean            # remove binary and object files
```

Dependencies: `libssl-dev` (OpenSSL). On macOS with MacPorts, `/opt/local/include` and `/opt/local/lib` are included automatically.

## Architecture

minivtun is a single-binary UDP-based VPN tunneller operating in two modes (server/client), selected by whether `-l` (listen) or `-r` (remote) is passed.

**Entry point:** `minivtun.c` — parses CLI args, configures the TUN interface, then calls either `run_server()` or `run_client()`.

**Protocol:** Custom UDP protocol. Each `minivtun_msg` has a 20-byte header (`opcode` + 3 reserved bytes + 16-byte MD5 auth key), followed by a payload union of either IP packet data or keepalive state. Three opcodes: `KEEPALIVE`, `IPDATA`, `DISCONNECT`. The auth key is derived by MD5-hashing the password.

**Server mode** (`server.c`): Maintains a hash table of connected clients (keyed by source address) and a virtual route table (`vt_route`) mapping subnets to client gateways. Routes packets between the TUN interface and UDP clients.

**Client mode** (`client.c`): Connects to a remote server, handles connection recovery (reconnects immediately on next received packet with no re-negotiation), and forwards traffic between TUN and UDP.

**Route management** (`client_route.c`): Linux-specific helpers to query `/proc/net/route`, detect the default gateway, and manage interface binding.

**Crypto** (`library.c`/`library.h`): Wraps OpenSSL EVP for AES-128-CBC (default), AES-256-CBC, DES-CBC, DES-X-CBC, and RC4. Password is MD5-padded to fill `crypto_key[32]`.

**Platform differences:**
- macOS: supports both native `utun` (4-byte protocol prefix) and old TUNTAP driver (struct `tun_pi`). Conditional on `__APPLE__` and `__APPLE_NETWORK_EXTENSION__`.
- Linux: uses `/dev/net/tun` with `IFF_TUN` flag and standard `tun_pi` struct.

**Supporting headers:** `list.h` — kernel-style intrusive doubly-linked list; `jhash.h` — Jenkins hash for client lookup tables.

**Default MTU** is 1300 to avoid fragmentation when tunnelling over another VPN.
