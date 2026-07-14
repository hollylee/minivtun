# What's new in v2

## Support TCP transport

The server listens on tcp and udp simulteanously, in the same port specified in command line option. It accepts incoming data through either udp or tcp. Note one client can use one kind of transport only.

The client specifies the transport type in command line. if '-T' is specified, the tcp transport is used. Otherwise the udp transport is used as before.

