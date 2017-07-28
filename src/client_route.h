// ----------------------------------------------------------------------------------------
// client_route.h
//
// Functions about route table and interfaces. 
//
// Holly Lee <holly.lee@gmail.com> 2017 @ Shanghai, China
// ----------------------------------------------------------------------------------------
#ifndef __CLIENT_ROUTE_H__
#define __CLIENT_ROUTE_H__

#include <netinet/in.h>

int get_ip_addr_of_interface(const char * ifname, struct sockaddr_in * addr_in);

// Get interface name by IPv4 address. 0 if got the name.
int get_interface_of_ip_addr(struct sockaddr_in * addr_in, char * ifname, size_t ifname_len);

// Succeeded: 0, -ENOENT: get route table error or no default route. -EEXIST multiple default route exists.
int get_default_route_ip_addr(struct sockaddr_in * default_route_interface_ip_addr);

// Succeeded: 0. -ENOENT: No default route. -EEXIST multiple default routes
int get_default_route_interface(char * ifname, size_t ifname_len);

// Return 0 if succeed, otherwise return negative number
int validate_and_setup_bind_addr(char * bind_addr, size_t bind_addr_len, char * bind_if, size_t bind_if_len);

#endif // __CLIENT_ROUTE_H__
