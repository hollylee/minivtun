// ------------------------------------------------------------------------------
// client_route.c
//
// Functions to manipulate routing table, addresses and interfaces
//
// Holly Lee <holly.lee@gmail.com> 2017 @ Shanghai, China
// ------------------------------------------------------------------------------
#include <sys/types.h>
#include <net/route.h>
#include <net/if.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>

#include "client_route.h"

#ifdef __linux__
  #include <linux/netlink.h>
  #include <linux/rtnetlink.h>
  #include <linux/if_link.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif // __linux__


/* alignment constraint for routing socket */
#define ROUNDUP(a) \
       ((a) > 0 ? (1 + (((a) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))


#ifdef __APPLE__

// Check whether a dest/mask pair is a default route
static int is_default_route(struct sockaddr * dest_addr, struct sockaddr * mask_addr)
{
       struct sockaddr_in * dest_in = (struct sockaddr_in *)dest_addr;
       struct sockaddr_in * mask_in = (struct sockaddr_in *)mask_addr;
#if DEBUG
       printf("is_default_route: dest %s, mask %s (0x%x)\n", 
              inet_ntoa(dest_in->sin_addr), 
              (mask_addr != 0 ? inet_ntoa(mask_in->sin_addr): ""), 
              (mask_addr != 0 ? mask_in->sin_addr.s_addr : 0))  ;
#endif       

       return (dest_in->sin_addr.s_addr == INADDR_ANY) && 
              (mask_in == 0 || mask_in->sin_addr.s_addr == 0 || mask_in->sin_len == 0);
}


// Get addresses from rt_msghdr2 struct
static void get_addrs_from_rtm(struct rt_msghdr2 * rtm, struct sockaddr * rti_info[RTAX_MAX])
{
       struct sockaddr * sa = (struct sockaddr *)(rtm + 1);

       for ( int i = 0; i < RTAX_MAX; i++ ) {

           if ( rtm->rtm_addrs & (1 << i) ) {
              rti_info[i] = sa;
              sa = (struct sockaddr *)((char *)sa + ROUNDUP(sa->sa_len));
           }
           else {
              rti_info[i] = 0;
           }
         
       } // for 0..RTAX_MAX - 1
}

#endif // __APPLE__

#ifdef __linux__

static void get_attrs_from_rtmsg(struct nlmsghdr * hdr, struct rtmsg * rtm, struct rtattr * attrs[RTA_MAX])
{
       memset(attrs, 0, RTA_MAX * sizeof(struct rtattr *));

       int attrs_len = RTM_PAYLOAD(hdr);
       for ( struct rtattr * attr = RTM_RTA(rtm); RTA_OK(attr, attrs_len); attr = RTA_NEXT(attr, attrs_len) ) {
           attrs[attr->rta_type] = attr;
       }
}

#endif

// Get IPv4 address for the interface
int get_ip_addr_of_interface(const char * ifname, struct sockaddr_in * addr_in)
{
       struct ifaddrs * ifaddrs = 0;

       int rv = getifaddrs(&ifaddrs);
       if ( rv < 0 )
          return rv;

       rv = -ENOENT;

       for ( struct ifaddrs * ifa = ifaddrs; ifa != 0; ifa = ifa->ifa_next ) {
           if ( strcmp(ifa->ifa_name, ifname) == 0 && ifa->ifa_addr != 0 && ifa->ifa_addr->sa_family == AF_INET ) {
              *addr_in = *(struct sockaddr_in *)(ifa->ifa_addr);
              rv = 0;
              break;
           }
       }

       freeifaddrs(ifaddrs);

       return rv;
}

// Get interface name by IPv4 address. 0 if got the name,
int get_interface_of_ip_addr(struct sockaddr_in * addr_in, char * ifname, size_t ifname_len)
{
    struct ifaddrs * ifaddrs = 0;

    int rv = getifaddrs(&ifaddrs);
    if ( rv < 0 )
       return rv;

    rv = -ENOENT;

    for ( struct ifaddrs * ifa = ifaddrs; ifa != 0; ifa = ifa->ifa_next ) {

        if ( ifa->ifa_addr != 0 && ifa->ifa_addr->sa_family == AF_INET && 
             ((struct sockaddr_in *)(ifa->ifa_addr))->sin_addr.s_addr == addr_in->sin_addr.s_addr ) {

           strncpy(ifname, ifa->ifa_name, ifname_len);
           rv = 0;
           break;

        }

    }

    freeifaddrs(ifaddrs);

    return rv;
}

#ifdef __APPLE__

// Get route table. Return 0 if failed. Caller must free() returned buffer
static struct rt_msghdr2 * get_route_table(size_t * size)
{
    // Get route table
    int mib[6] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_DUMP2, 0 };
    size_t space_required = 0;

    // Get required space
    int rv = sysctl(mib, 6, NULL, &space_required, NULL, 0);
    if ( rv < 0 )
       return 0;

    struct rt_msghdr2 * buf = (struct rt_msghdr2 *)malloc(space_required);

    rv = sysctl(mib, 6, buf, &space_required, NULL, 0);
    if ( rv < 0 ) {
       free(buf);
       return 0;
    }

    *size = space_required;
    return buf;    
}

#endif // __APPLE__

#ifdef __linux__

struct route_table_req {
    struct nlmsghdr nmh;
    struct rtmsg    rtm;
};

// The return value must ve released by free()
static struct nlmsghdr * get_route_table(size_t * size)
{
        int s = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if ( s < 0 ) {
           return 0;
        }
    
        struct route_table_req req;
        memset(&req, 0, sizeof(struct route_table_req));
    
        req.nmh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
        req.nmh.nlmsg_type = RTM_GETROUTE;
        req.nmh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    
        req.rtm.rtm_family = AF_INET;
        req.rtm.rtm_table = RT_TABLE_MAIN;
    
        // Send the request
        int rv = send(s, &req, sizeof(struct route_table_req), 0);
        if ( rv < 0 ) {
           close(s);
           return 0;
        }
    
        // Read result
        size_t received_bufsize = 1024 * 64;
        size_t received_size = 0;
        char * received_buf = (char *)malloc(received_bufsize);
        char * p = received_buf;
    
        while ( received_size < received_bufsize ) {
    
              // receive
              rv = recv(s, p, received_bufsize - received_size, 0);
              if ( rv < 0 ) {
                 close(s);
                 return 0;
              }
    
              // Check 
              struct nlmsghdr * msghdr = (struct nlmsghdr *)p;
              if ( msghdr->nlmsg_type == NLMSG_DONE )
                 break;
    
              // next receive
              p += rv;
              received_size += rv;
    
        } // while

        close(s);

        *size = received_size;
        return (struct nlmsghdr *)received_buf;             
}

#endif // __linux__

// Get default route ip address. Any failure returns negative value.
int get_default_route_ip_addr(struct sockaddr_in * default_route_interface_ip_addr)
{
    char ifname[IFNAMSIZ] = { 0 };

    int rv = get_default_route_interface(ifname, IFNAMSIZ);
    if ( rv < 0 )
       return rv;

    return get_ip_addr_of_interface(ifname, default_route_interface_ip_addr);
}

#ifdef __APPLE__

// Get default routine interface name
int get_default_route_interface(char * ifname, size_t ifname_len)
{
    // Get route table
    size_t space_required = 0;
    struct rt_msghdr2 * buf = get_route_table(&space_required);
    if ( buf == 0 )
       return -ENOENT; 

    // Now route table is in buf
    int has_default_route = 0;

    for ( struct rt_msghdr2 * rtm = buf; rtm < (struct rt_msghdr2 *)((char *)buf + space_required); 
          rtm = (struct rt_msghdr2 *)((char *)rtm + rtm->rtm_msglen) ) {

        struct sockaddr * sa = (struct sockaddr *)(rtm + 1);

        // So far we handle IPv4 only
        if ( sa->sa_family != AF_INET )
           continue;

        // Get destination and mask part
        struct sockaddr * rti_info[RTAX_MAX];

        get_addrs_from_rtm(rtm, rti_info);

        if ( rti_info[RTAX_DST] == 0 )
           continue;

        // Check default route
        if ( is_default_route(rti_info[RTAX_DST], rti_info[RTAX_NETMASK]) ) {

           // more than one default route...
           if ( has_default_route ) {
              free(buf);
              return -EEXIST;
           }

           // Get interface name of the default route
           char if_name[IFNAMSIZ] = { 0 };
           if ( if_indextoname(rtm->rtm_index, if_name) == 0 )
              continue;

           strncpy(ifname, if_name, ifname_len);

           //
           has_default_route = 1;
           
        } // is_default_route()

    } // for rt_msghdr2

    free(buf);

    if ( !has_default_route )
       return -ENOENT;

    return 0;   
}


// Check whether an ip is assigned to the interface of default route.
// Return 0 if not in, otherwise non-zero.
static int is_ip_in_default_route(struct sockaddr_in * addr_in)
{
    // Get route table
    size_t space_required = 0;
    struct rt_msghdr2 * buf = get_route_table(&space_required);
    if ( buf == 0 )
       return 0; 

    // Now route table is in buf
    for ( struct rt_msghdr2 * rtm = buf; rtm < (struct rt_msghdr2 *)((char *)buf + space_required); 
          rtm = (struct rt_msghdr2 *)((char *)rtm + rtm->rtm_msglen) ) {

        struct sockaddr * sa = (struct sockaddr *)(rtm + 1);

        // So far we handle IPv4 only
        if ( sa->sa_family != AF_INET )
           continue;

        // Get destination and mask part
        struct sockaddr * rti_info[RTAX_MAX];

        get_addrs_from_rtm(rtm, rti_info);

        if ( rti_info[RTAX_DST] == 0 )
           continue;

        // Check default route
        if ( is_default_route(rti_info[RTAX_DST], rti_info[RTAX_NETMASK]) ) {

           // Get interface name of the default route
           char ifname[IFNAMSIZ] = { 0 };
           if ( if_indextoname(rtm->rtm_index, ifname) == 0 )
              continue;

           // Get ip address for the interface
           struct sockaddr_in if_addr_in;
           int rv = get_ip_addr_of_interface(ifname, &if_addr_in);
           if ( rv < 0 )
              continue;

           //
           if ( if_addr_in.sin_addr.s_addr == addr_in->sin_addr.s_addr ) {
              free(buf);
              return 1;
           }
           
        } // is_default_route()

    } // for rt_msghdr2
    
    free(buf);
    return 0;   

} // is_ip_in_default_route()

#endif // __APPLE__

#ifdef __linux__

int get_default_route_interface(char * ifname, size_t ifname_len)
{
    // Get route table
    size_t route_table_size = 0;
    struct nlmsghdr * buf = get_route_table(&route_table_size);
    if ( buf == 0 )
       return -ENOENT; 

    // Now route table is in buf
    int has_default_route = 0;

    for ( struct nlmsghdr * hdr = buf; NLMSG_OK(hdr, route_table_size); hdr = NLMSG_NEXT(hdr, route_table_size) ) {

        struct rtmsg * rtmsg = (struct rtmsg *)NLMSG_DATA(hdr);

        // So far we handle IPv4 only
        if ( rtmsg->rtm_family != AF_INET )
           continue;

        // Get attributes        
        struct rtattr * attrs[RTA_MAX];
        get_attrs_from_rtmsg(hdr, rtmsg, attrs);

        // Linux has a RTA_GATEWAY attribute without dest addr to identify the default route
        if ( attrs[RTA_DST] == 0 && attrs[RTA_GATEWAY] != 0 ) {

           // more than one default route...
           if ( has_default_route ) {
              free(buf);
              return -EEXIST;
           }

           // Get interface name of the default route
           char if_name[IFNAMSIZ] = { 0 };
           if ( if_indextoname(*(unsigned int *)RTA_DATA(attrs[RTA_OIF]), if_name) == 0 )
              continue;

           strncpy(ifname, if_name, ifname_len);

           //
           has_default_route = 1;
           
        } // is_default_route()

    } // for rt_msghdr2

    free(buf);

    if ( !has_default_route )
       return -ENOENT;

    return 0;   

}


int is_ip_in_default_route(struct sockaddr_in * addr_in)
{
    // Get route table
    size_t route_table_size = 0;
    struct nlmsghdr * buf = get_route_table(&route_table_size);
    if ( buf == 0 )
       return 0; 

    // Now route table is in buf
    for ( struct nlmsghdr * hdr = buf; NLMSG_OK(hdr, route_table_size); hdr = NLMSG_NEXT(hdr, route_table_size) ) {

        struct rtmsg * rtmsg = (struct rtmsg *)NLMSG_DATA(hdr);

        // So far we handle IPv4 only
        if ( rtmsg->rtm_family != AF_INET )
           continue;

        // Get destination and mask part
        struct rtattr * attrs[RTA_MAX];

        get_attrs_from_rtmsg(hdr, rtmsg, attrs);


        if ( attrs[RTA_DST] == 0 && attrs[RTA_GATEWAY] != 0 ) {
        
           // Get interface name of the default route
           char ifname[IFNAMSIZ] = { 0 };
           if ( if_indextoname(*(unsigned int *)RTA_DATA(attrs[RTA_OIF]), ifname) == 0 )
              continue;

           // Get ip address for the interface
           struct sockaddr_in if_addr_in;
           int rv = get_ip_addr_of_interface(ifname, &if_addr_in);
           if ( rv < 0 )
              continue;

           //
           if ( if_addr_in.sin_addr.s_addr == addr_in->sin_addr.s_addr ) {
              free(buf);
              return 1;
           }
           
        } // is_default_route()

    } // for rt_msghdr2
    
    free(buf);
    return 0;   

}


#endif // __linux__

// Return 0 if succeed, otherwise return negative number
int validate_and_setup_bind_addr(char * bind_addr, size_t bind_addr_len, char * bind_if, size_t bind_if_len)
{
    // printf("validate_and_setup_bind_addr: bind_to_addr = %s\n", bind_addr);

    // If no bind addr specified...
    if ( bind_addr == 0 || *bind_addr == 0 ) {

       struct sockaddr_in if_ip_to_default_route;

       int rv = get_default_route_interface(bind_if, bind_if_len);
       if ( rv < 0 )
          return rv;

       rv = get_ip_addr_of_interface(bind_if, &if_ip_to_default_route);
       if ( rv < 0 )
          return rv;

       // Convert to char *
       char * addr_string = inet_ntoa(if_ip_to_default_route.sin_addr);
       if ( addr_string == 0 )
          return -EINVAL;

       strncpy(bind_addr, addr_string, bind_addr_len);
       return 0;
    }

    // If bind_addr specified, verify it...
    else {
       struct sockaddr_in addr_in = { 
           .sin_family = AF_INET, 
#ifdef __APPLE__           
           .sin_len = sizeof(struct sockaddr_in) 
#endif           
       };

       if ( !inet_aton(bind_addr, &(addr_in.sin_addr)) )
          return -EINVAL;

       // Get interface of the ip
       char ifname[IFNAMSIZ] = { 0 };

       int rv = get_interface_of_ip_addr(&addr_in, ifname, IFNAMSIZ);
       if ( rv < 0 )
          return rv;

       return is_ip_in_default_route(&addr_in) ? 0 : -ENOENT;
    }

    
}