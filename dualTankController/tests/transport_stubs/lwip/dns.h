#pragma once
#include <WiFiClientSecure.h>

using err_t = int;
constexpr int ERR_OK = 0, ERR_INPROGRESS = -1, LWIP_DNS_ADDRTYPE_IPV4 = 0;
struct ip_addr_t { uint32_t addr; };
inline bool IP_IS_V4(const ip_addr_t *) { return true; }
inline const ip_addr_t *ip_2_ip4(const ip_addr_t *address) { return address; }
inline uint32_t ip4_addr_get_u32(const ip_addr_t *address) { return address->addr; }
using DnsCallback = void (*)(const char *, const ip_addr_t *, void *);
inline DnsCallback dnsCallback = nullptr;
inline void *dnsArgument = nullptr;
inline err_t dns_gethostbyname_addrtype(
    const char *, ip_addr_t *address, DnsCallback callback, void *argument, int) {
  dnsCallback = callback;
  dnsArgument = argument;
  address->addr = 123;
  return network.dnsReady ? ERR_OK : ERR_INPROGRESS;
}
