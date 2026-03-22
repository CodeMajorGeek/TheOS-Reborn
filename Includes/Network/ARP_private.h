#ifndef _ARP_PRIVATE_H
#define _ARP_PRIVATE_H

#include <Network/ARP.h>

static inline uint16_t ARP_read_be16(const uint8_t* bytes);
static inline uint32_t ARP_read_be32(const uint8_t* bytes);
static inline void ARP_write_be32(uint8_t* bytes, uint32_t value);
static inline bool ARP_mac_is_zero(const uint8_t mac[ARP_MAC_LEN]);
static inline bool ARP_ipv4_is_zero(uint32_t ipv4_addr_be);

static uint64_t ARP_now_ticks(void);
static uint64_t ARP_ms_to_ticks(uint32_t ms);
static bool ARP_deadline_passed(uint64_t now_ticks, uint64_t deadline_ticks);
static void ARP_cleanup_expired_locked(uint64_t now_ticks);
static int32_t ARP_find_entry_index_locked(uint32_t ipv4_addr_be);
static int32_t ARP_find_replace_slot_locked(uint64_t now_ticks);
static bool ARP_upsert_locked(uint32_t ipv4_addr_be,
                              const uint8_t mac[ARP_MAC_LEN],
                              uint16_t flags,
                              bool permanent,
                              uint64_t now_ticks);
static bool ARP_parse_request_ipv4(const sys_arpreq_t* request, uint32_t* out_ipv4_addr_be);
static bool ARP_parse_hwaddr(const sys_arpreq_t* request, uint8_t out_mac[ARP_MAC_LEN]);
static void ARP_fill_reply(uint32_t ipv4_addr_be, const arp_entry_t* entry, sys_arpreq_t* out_reply);

#endif
