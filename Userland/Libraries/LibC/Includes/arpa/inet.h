#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef INADDR_NONE
#define INADDR_NONE ((in_addr_t) 0xFFFFFFFFU)
#endif

/* Byte-order helpers are provided as inline operations (no runtime backend). */
static inline uint16_t __libc_bswap16(uint16_t value)
{
    return (uint16_t) ((value << 8) | (value >> 8));
}

static inline uint32_t __libc_bswap32(uint32_t value)
{
    return ((value & 0x000000FFU) << 24) |
           ((value & 0x0000FF00U) << 8) |
           ((value & 0x00FF0000U) >> 8) |
           ((value & 0xFF000000U) >> 24);
}

static inline uint16_t htons(uint16_t host16)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __libc_bswap16(host16);
#else
    return host16;
#endif
}

static inline uint16_t ntohs(uint16_t net16)
{
    return htons(net16);
}

static inline uint32_t htonl(uint32_t host32)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __libc_bswap32(host32);
#else
    return host32;
#endif
}

static inline uint32_t ntohl(uint32_t net32)
{
    return htonl(net32);
}

/* Network conversion routines are declared for source compatibility only. */
in_addr_t inet_addr(const char* cp);
char* inet_ntoa(struct in_addr in);
int inet_aton(const char* cp, struct in_addr* inp);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);
int inet_pton(int af, const char* src, void* dst);

#ifdef __cplusplus
}
#endif

#endif
