#ifndef _ARP_H
#define _ARP_H

#include <Debug/Spinlock.h>
#include <UAPI/Net.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARP_TABLE_CAPACITY                 128U
#define ARP_DEVICE_NAME                    "net0"

#define ARP_ETH_HEADER_LEN                 14U
#define ARP_PACKET_IPV4_LEN                28U
#define ARP_FRAME_MIN_LEN                  (ARP_ETH_HEADER_LEN + ARP_PACKET_IPV4_LEN)

#define ARP_DYNAMIC_ENTRY_TTL_MS           300000U

#define ARP_SOCKADDR_DATA_IPV4_OFFSET      2U
#define ARP_MAC_LEN                        6U

typedef struct arp_entry
{
    bool used;
    bool permanent;
    uint16_t flags;
    uint32_t ipv4_addr_be;
    uint8_t mac[ARP_MAC_LEN];
    uint64_t updated_ticks;
    uint64_t expires_ticks;
} arp_entry_t;

typedef struct arp_runtime_state
{
    bool lock_ready;
    bool initialized;
    spinlock_t lock;
    arp_entry_t entries[ARP_TABLE_CAPACITY];
    uint64_t rx_arp_frames;
    uint64_t learned_entries;
    uint64_t replaced_entries;
    uint64_t expired_entries;
    uint64_t lookup_hits;
    uint64_t lookup_misses;
} arp_runtime_state_t;

typedef struct arp_packet_ipv4
{
    uint16_t htype_be;
    uint16_t ptype_be;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper_be;
    uint8_t sha[ARP_MAC_LEN];
    uint8_t spa[4];
    uint8_t tha[ARP_MAC_LEN];
    uint8_t tpa[4];
} __attribute__((packed)) arp_packet_ipv4_t;

void ARP_init(void);
bool ARP_is_ready(void);
void ARP_note_ethernet_frame(const uint8_t* frame, size_t frame_len);
bool ARP_lookup_ipv4(uint32_t ipv4_addr_be, uint8_t out_mac[ARP_MAC_LEN], uint16_t* out_flags);
bool ARP_set_ipv4(uint32_t ipv4_addr_be, const uint8_t mac[ARP_MAC_LEN], uint16_t flags);
bool ARP_delete_ipv4(uint32_t ipv4_addr_be);
bool ARP_ioctl_get(const sys_arpreq_t* request, sys_arpreq_t* out_reply);
bool ARP_ioctl_set(const sys_arpreq_t* request);
bool ARP_ioctl_delete(const sys_arpreq_t* request);

#endif
