#include <Network/ARP.h>
#include <Network/ARP_private.h>

#include <CPU/ISR.h>

#include <string.h>

static arp_runtime_state_t ARP_state;

static inline uint16_t ARP_read_be16(const uint8_t* bytes)
{
    if (!bytes)
        return 0;

    return (uint16_t) (((uint16_t) bytes[0] << 8) | (uint16_t) bytes[1]);
}

static inline uint32_t ARP_read_be32(const uint8_t* bytes)
{
    if (!bytes)
        return 0;

    return ((uint32_t) bytes[0] << 24) |
           ((uint32_t) bytes[1] << 16) |
           ((uint32_t) bytes[2] << 8) |
           (uint32_t) bytes[3];
}

static inline void ARP_write_be32(uint8_t* bytes, uint32_t value)
{
    if (!bytes)
        return;

    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}

static inline bool ARP_mac_is_zero(const uint8_t mac[ARP_MAC_LEN])
{
    if (!mac)
        return true;

    for (size_t i = 0; i < ARP_MAC_LEN; i++)
    {
        if (mac[i] != 0U)
            return false;
    }
    return true;
}

static inline bool ARP_ipv4_is_zero(uint32_t ipv4_addr_be)
{
    return ipv4_addr_be == 0U;
}

static uint64_t ARP_now_ticks(void)
{
    return ISR_get_timer_ticks();
}

static uint64_t ARP_ms_to_ticks(uint32_t ms)
{
    uint32_t hz = ISR_get_tick_hz();
    if (hz == 0U)
        hz = 100U;

    uint64_t ticks = (((uint64_t) ms * (uint64_t) hz) + 999ULL) / 1000ULL;
    if (ticks == 0ULL)
        ticks = 1ULL;
    return ticks;
}

static bool ARP_deadline_passed(uint64_t now_ticks, uint64_t deadline_ticks)
{
    return (int64_t) (now_ticks - deadline_ticks) >= 0;
}

static void ARP_cleanup_expired_locked(uint64_t now_ticks)
{
    for (uint32_t i = 0; i < ARP_TABLE_CAPACITY; i++)
    {
        arp_entry_t* entry = &ARP_state.entries[i];
        if (!entry->used || entry->permanent)
            continue;
        if (!ARP_deadline_passed(now_ticks, entry->expires_ticks))
            continue;

        memset(entry, 0, sizeof(*entry));
        ARP_state.expired_entries++;
    }
}

static int32_t ARP_find_entry_index_locked(uint32_t ipv4_addr_be)
{
    for (uint32_t i = 0; i < ARP_TABLE_CAPACITY; i++)
    {
        const arp_entry_t* entry = &ARP_state.entries[i];
        if (!entry->used)
            continue;
        if (entry->ipv4_addr_be == ipv4_addr_be)
            return (int32_t) i;
    }

    return -1;
}

static int32_t ARP_find_replace_slot_locked(uint64_t now_ticks)
{
    (void) now_ticks;

    for (uint32_t i = 0; i < ARP_TABLE_CAPACITY; i++)
    {
        if (!ARP_state.entries[i].used)
            return (int32_t) i;
    }

    int32_t oldest_dynamic_slot = -1;
    uint64_t oldest_dynamic_ticks = UINT64_MAX;
    for (uint32_t i = 0; i < ARP_TABLE_CAPACITY; i++)
    {
        const arp_entry_t* entry = &ARP_state.entries[i];
        if (entry->permanent)
            continue;

        if (entry->updated_ticks <= oldest_dynamic_ticks)
        {
            oldest_dynamic_ticks = entry->updated_ticks;
            oldest_dynamic_slot = (int32_t) i;
        }
    }

    return oldest_dynamic_slot;
}

static bool ARP_upsert_locked(uint32_t ipv4_addr_be,
                              const uint8_t mac[ARP_MAC_LEN],
                              uint16_t flags,
                              bool permanent,
                              uint64_t now_ticks)
{
    if (ARP_ipv4_is_zero(ipv4_addr_be) || ARP_mac_is_zero(mac))
        return false;

    int32_t slot = ARP_find_entry_index_locked(ipv4_addr_be);
    if (slot < 0)
    {
        slot = ARP_find_replace_slot_locked(now_ticks);
        if (slot < 0)
            return false;

        if (ARP_state.entries[(uint32_t) slot].used)
            ARP_state.replaced_entries++;
    }

    arp_entry_t* entry = &ARP_state.entries[(uint32_t) slot];
    entry->used = true;
    entry->permanent = permanent;
    entry->flags = (uint16_t) (flags | ATF_COM);
    if (permanent)
        entry->flags |= ATF_PERM;
    else
        entry->flags &= (uint16_t) ~ATF_PERM;
    entry->ipv4_addr_be = ipv4_addr_be;
    memcpy(entry->mac, mac, ARP_MAC_LEN);
    entry->updated_ticks = now_ticks;
    if (entry->permanent)
    {
        entry->expires_ticks = UINT64_MAX;
    }
    else
    {
        uint64_t ttl_ticks = ARP_ms_to_ticks(ARP_DYNAMIC_ENTRY_TTL_MS);
        if (now_ticks > UINT64_MAX - ttl_ticks)
            entry->expires_ticks = UINT64_MAX;
        else
            entry->expires_ticks = now_ticks + ttl_ticks;
    }

    ARP_state.learned_entries++;
    return true;
}

static bool ARP_parse_request_ipv4(const sys_arpreq_t* request, uint32_t* out_ipv4_addr_be)
{
    if (!request || !out_ipv4_addr_be)
        return false;

    if (request->arp_pa.sa_family != AF_INET)
        return false;

    char dev_name[IFNAMSIZ + 1U];
    memset(dev_name, 0, sizeof(dev_name));
    memcpy(dev_name, request->arp_dev, IFNAMSIZ);
    if (dev_name[0] != '\0' && strcmp(dev_name, ARP_DEVICE_NAME) != 0)
        return false;

    uint32_t ipv4_addr_be = ARP_read_be32(&request->arp_pa.sa_data[ARP_SOCKADDR_DATA_IPV4_OFFSET]);
    if (ARP_ipv4_is_zero(ipv4_addr_be))
        return false;

    *out_ipv4_addr_be = ipv4_addr_be;
    return true;
}

static bool ARP_parse_hwaddr(const sys_arpreq_t* request, uint8_t out_mac[ARP_MAC_LEN])
{
    if (!request || !out_mac)
        return false;

    if (request->arp_ha.sa_family != 0U &&
        request->arp_ha.sa_family != AF_UNSPEC &&
        request->arp_ha.sa_family != ARPHRD_ETHER)
    {
        return false;
    }

    memcpy(out_mac, request->arp_ha.sa_data, ARP_MAC_LEN);
    return !ARP_mac_is_zero(out_mac);
}

static void ARP_fill_reply(uint32_t ipv4_addr_be, const arp_entry_t* entry, sys_arpreq_t* out_reply)
{
    if (!entry || !out_reply)
        return;

    memset(out_reply, 0, sizeof(*out_reply));
    out_reply->arp_pa.sa_family = AF_INET;
    ARP_write_be32(&out_reply->arp_pa.sa_data[ARP_SOCKADDR_DATA_IPV4_OFFSET], ipv4_addr_be);

    out_reply->arp_ha.sa_family = ARPHRD_ETHER;
    memcpy(out_reply->arp_ha.sa_data, entry->mac, ARP_MAC_LEN);
    out_reply->arp_flags = (int32_t) entry->flags;

    size_t dev_len = strlen(ARP_DEVICE_NAME);
    if (dev_len > IFNAMSIZ - 1U)
        dev_len = IFNAMSIZ - 1U;
    memcpy(out_reply->arp_dev, ARP_DEVICE_NAME, dev_len);
    out_reply->arp_dev[dev_len] = '\0';
}

void ARP_init(void)
{
    if (!ARP_state.lock_ready)
    {
        spinlock_init(&ARP_state.lock);
        ARP_state.lock_ready = true;
    }

    spin_lock(&ARP_state.lock);
    if (!ARP_state.initialized)
    {
        memset(ARP_state.entries, 0, sizeof(ARP_state.entries));
        ARP_state.rx_arp_frames = 0;
        ARP_state.learned_entries = 0;
        ARP_state.replaced_entries = 0;
        ARP_state.expired_entries = 0;
        ARP_state.lookup_hits = 0;
        ARP_state.lookup_misses = 0;
        ARP_state.initialized = true;
    }
    spin_unlock(&ARP_state.lock);
}

bool ARP_is_ready(void)
{
    return ARP_state.initialized;
}

void ARP_note_ethernet_frame(const uint8_t* frame, size_t frame_len)
{
    if (!frame || frame_len < ARP_FRAME_MIN_LEN)
        return;

    uint16_t ethertype = ARP_read_be16(&frame[12]);
    if (ethertype != ETH_P_ARP)
        return;

    const arp_packet_ipv4_t* packet = (const arp_packet_ipv4_t*) (frame + ARP_ETH_HEADER_LEN);
    if (ARP_read_be16((const uint8_t*) &packet->htype_be) != ARPHRD_ETHER ||
        ARP_read_be16((const uint8_t*) &packet->ptype_be) != ETH_P_IP ||
        packet->hlen != ARP_MAC_LEN ||
        packet->plen != 4U)
    {
        return;
    }

    uint16_t operation = ARP_read_be16((const uint8_t*) &packet->oper_be);
    if (operation != ARPOP_REQUEST && operation != ARPOP_REPLY)
        return;

    uint32_t sender_ipv4_be = ARP_read_be32(packet->spa);
    if (ARP_ipv4_is_zero(sender_ipv4_be) || ARP_mac_is_zero(packet->sha))
        return;

    if (!__atomic_load_n(&ARP_state.initialized, __ATOMIC_ACQUIRE))
        ARP_init();
    spin_lock(&ARP_state.lock);
    uint64_t now_ticks = ARP_now_ticks();
    ARP_cleanup_expired_locked(now_ticks);
    ARP_state.rx_arp_frames++;
    (void) ARP_upsert_locked(sender_ipv4_be, packet->sha, ATF_COM, false, now_ticks);
    spin_unlock(&ARP_state.lock);
}

bool ARP_lookup_ipv4(uint32_t ipv4_addr_be, uint8_t out_mac[ARP_MAC_LEN], uint16_t* out_flags)
{
    if (ARP_ipv4_is_zero(ipv4_addr_be) || !out_mac)
        return false;

    if (!__atomic_load_n(&ARP_state.initialized, __ATOMIC_ACQUIRE))
        ARP_init();
    spin_lock(&ARP_state.lock);
    uint64_t now_ticks = ARP_now_ticks();
    ARP_cleanup_expired_locked(now_ticks);
    int32_t slot = ARP_find_entry_index_locked(ipv4_addr_be);
    if (slot < 0)
    {
        ARP_state.lookup_misses++;
        spin_unlock(&ARP_state.lock);
        return false;
    }

    const arp_entry_t* entry = &ARP_state.entries[(uint32_t) slot];
    memcpy(out_mac, entry->mac, ARP_MAC_LEN);
    if (out_flags)
        *out_flags = entry->flags;
    ARP_state.lookup_hits++;
    spin_unlock(&ARP_state.lock);
    return true;
}

bool ARP_set_ipv4(uint32_t ipv4_addr_be, const uint8_t mac[ARP_MAC_LEN], uint16_t flags)
{
    if (ARP_ipv4_is_zero(ipv4_addr_be) || ARP_mac_is_zero(mac))
        return false;

    if (!__atomic_load_n(&ARP_state.initialized, __ATOMIC_ACQUIRE))
        ARP_init();
    spin_lock(&ARP_state.lock);
    uint64_t now_ticks = ARP_now_ticks();
    ARP_cleanup_expired_locked(now_ticks);
    bool permanent = (flags & ATF_PERM) != 0U;
    bool ok = ARP_upsert_locked(ipv4_addr_be, mac, flags, permanent, now_ticks);
    spin_unlock(&ARP_state.lock);
    return ok;
}

bool ARP_delete_ipv4(uint32_t ipv4_addr_be)
{
    if (ARP_ipv4_is_zero(ipv4_addr_be))
        return false;

    if (!__atomic_load_n(&ARP_state.initialized, __ATOMIC_ACQUIRE))
        ARP_init();
    spin_lock(&ARP_state.lock);
    uint64_t now_ticks = ARP_now_ticks();
    ARP_cleanup_expired_locked(now_ticks);
    int32_t slot = ARP_find_entry_index_locked(ipv4_addr_be);
    if (slot < 0)
    {
        spin_unlock(&ARP_state.lock);
        return false;
    }

    memset(&ARP_state.entries[(uint32_t) slot], 0, sizeof(ARP_state.entries[(uint32_t) slot]));
    spin_unlock(&ARP_state.lock);
    return true;
}

bool ARP_ioctl_get(const sys_arpreq_t* request, sys_arpreq_t* out_reply)
{
    uint32_t ipv4_addr_be = 0;
    if (!ARP_parse_request_ipv4(request, &ipv4_addr_be) || !out_reply)
        return false;

    if (!__atomic_load_n(&ARP_state.initialized, __ATOMIC_ACQUIRE))
        ARP_init();
    spin_lock(&ARP_state.lock);
    uint64_t now_ticks = ARP_now_ticks();
    ARP_cleanup_expired_locked(now_ticks);
    int32_t slot = ARP_find_entry_index_locked(ipv4_addr_be);
    if (slot < 0)
    {
        ARP_state.lookup_misses++;
        spin_unlock(&ARP_state.lock);
        return false;
    }

    ARP_fill_reply(ipv4_addr_be, &ARP_state.entries[(uint32_t) slot], out_reply);
    ARP_state.lookup_hits++;
    spin_unlock(&ARP_state.lock);
    return true;
}

bool ARP_ioctl_set(const sys_arpreq_t* request)
{
    uint32_t ipv4_addr_be = 0;
    uint8_t mac[ARP_MAC_LEN];
    if (!ARP_parse_request_ipv4(request, &ipv4_addr_be))
        return false;
    if (!ARP_parse_hwaddr(request, mac))
        return false;

    uint16_t flags = (uint16_t) request->arp_flags;
    if ((flags & ATF_COM) == 0U)
        flags |= ATF_COM;
    return ARP_set_ipv4(ipv4_addr_be, mac, flags);
}

bool ARP_ioctl_delete(const sys_arpreq_t* request)
{
    uint32_t ipv4_addr_be = 0;
    if (!ARP_parse_request_ipv4(request, &ipv4_addr_be))
        return false;
    return ARP_delete_ipv4(ipv4_addr_be);
}
