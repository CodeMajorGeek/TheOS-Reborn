#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <syscall.h>
#include <unistd.h>

#include <UAPI/Net.h>

#include "dhcp_proto.h"

static uint16_t dhcpd_to_be16(uint16_t value)
{
    return (uint16_t) ((value << 8) | (value >> 8));
}

static uint32_t dhcpd_to_be32(uint32_t value)
{
    return ((value & 0x000000FFUL) << 24) |
           ((value & 0x0000FF00UL) << 8) |
           ((value & 0x00FF0000UL) >> 8) |
           ((value & 0xFF000000UL) >> 24);
}

static uint16_t dhcpd_from_be16(uint16_t value)
{
    return dhcpd_to_be16(value);
}

static uint32_t dhcpd_from_be32(uint32_t value)
{
    return dhcpd_to_be32(value);
}

static uint16_t dhcpd_checksum_finalize(uint32_t sum)
{
    while ((sum >> 16) != 0U)
        sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t) (~sum & 0xFFFFU);
}

static uint16_t dhcpd_checksum_bytes(const uint8_t* data, size_t len)
{
    if (!data)
        return 0;

    uint32_t sum = 0;
    size_t index = 0U;
    while ((index + 1U) < len)
    {
        uint16_t word = (uint16_t) (((uint16_t) data[index] << 8) | (uint16_t) data[index + 1U]);
        sum += word;
        index += 2U;
    }

    if (index < len)
        sum += (uint16_t) ((uint16_t) data[index] << 8);

    return dhcpd_checksum_finalize(sum);
}

static uint16_t dhcpd_udp_checksum(uint32_t src_addr_be,
                                   uint32_t dst_addr_be,
                                   const uint8_t* udp_segment,
                                   size_t udp_len)
{
    if (!udp_segment || udp_len == 0U)
        return 0;

    uint32_t sum = 0;
    sum += (src_addr_be >> 16) & 0xFFFFU;
    sum += src_addr_be & 0xFFFFU;
    sum += (dst_addr_be >> 16) & 0xFFFFU;
    sum += dst_addr_be & 0xFFFFU;
    sum += (uint32_t) DHCPD_IP_PROTO_UDP;
    sum += (uint32_t) udp_len;

    size_t index = 0U;
    while ((index + 1U) < udp_len)
    {
        uint16_t word = (uint16_t) (((uint16_t) udp_segment[index] << 8) |
                                    (uint16_t) udp_segment[index + 1U]);
        sum += word;
        index += 2U;
    }
    if (index < udp_len)
        sum += (uint16_t) ((uint16_t) udp_segment[index] << 8);

    uint16_t checksum = dhcpd_checksum_finalize(sum);
    return (checksum == 0U) ? 0xFFFFU : checksum;
}

static void dhcpd_format_ipv4(uint32_t ip_be, char out[16])
{
    if (!out)
        return;

    uint32_t ip = dhcpd_from_be32(ip_be);
    (void) snprintf(out,
                    16,
                    "%u.%u.%u.%u",
                    (unsigned int) ((ip >> 24) & 0xFFU),
                    (unsigned int) ((ip >> 16) & 0xFFU),
                    (unsigned int) ((ip >> 8) & 0xFFU),
                    (unsigned int) (ip & 0xFFU));
}

static void dhcpd_log_line(int log_fd, bool console_echo, const char* fmt, ...)
{
    char line[DHCPD_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int text_len = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (text_len < 0)
        return;

    size_t used = (size_t) text_len;
    if (used >= sizeof(line))
        used = sizeof(line) - 1U;

    if (used < (sizeof(line) - 1U))
    {
        line[used++] = '\n';
        line[used] = '\0';
    }
    else
    {
        line[sizeof(line) - 2U] = '\n';
        line[sizeof(line) - 1U] = '\0';
        used = sizeof(line) - 1U;
    }

    if (log_fd >= 0)
        (void) write(log_fd, line, used);
    if (console_echo)
        (void) write(STDOUT_FILENO, line, used);
}

static uint32_t dhcpd_seed_xid(const uint8_t mac[6])
{
    uint32_t seed = (uint32_t) sys_tick_get();
    if (!mac)
        return seed ? seed : 0x4D595DF4UL;

    seed ^= ((uint32_t) mac[0] << 24);
    seed ^= ((uint32_t) mac[1] << 16);
    seed ^= ((uint32_t) mac[2] << 8);
    seed ^= (uint32_t) mac[3];
    seed ^= ((uint32_t) mac[4] << 7);
    seed ^= ((uint32_t) mac[5] << 13);
    if (seed == 0U)
        seed = 0x4D595DF4UL;
    return seed;
}

static size_t dhcpd_build_discover(uint8_t* frame_out,
                                   size_t frame_cap,
                                   const uint8_t mac[6],
                                   uint32_t xid)
{
    if (!frame_out || !mac || frame_cap < DHCPD_ETH_HEADER_LEN)
        return 0;

    uint8_t options[64];
    size_t options_len = 0U;

    options[options_len++] = DHCPD_OPT_MSG_TYPE;
    options[options_len++] = 1U;
    options[options_len++] = DHCPD_MSG_DISCOVER;

    options[options_len++] = DHCPD_OPT_CLIENT_ID;
    options[options_len++] = 7U;
    options[options_len++] = DHCPD_BOOTP_HTYPE_ETHERNET;
    memcpy(&options[options_len], mac, 6U);
    options_len += 6U;

    options[options_len++] = DHCPD_OPT_PARAM_REQ_LIST;
    options[options_len++] = 4U;
    options[options_len++] = DHCPD_OPT_SUBNET_MASK;
    options[options_len++] = DHCPD_OPT_ROUTER;
    options[options_len++] = DHCPD_OPT_DNS;
    options[options_len++] = DHCPD_OPT_DOMAIN;

    options[options_len++] = DHCPD_OPT_HOSTNAME;
    options[options_len++] = 5U;
    memcpy(&options[options_len], "theos", 5U);
    options_len += 5U;

    options[options_len++] = DHCPD_OPT_END;

    size_t bootp_len = sizeof(dhcpd_bootp_header_t) + sizeof(uint32_t) + options_len;
    size_t udp_len = sizeof(dhcpd_udp_header_t) + bootp_len;
    size_t ip_len = sizeof(dhcpd_ipv4_header_t) + udp_len;
    size_t frame_len = sizeof(dhcpd_eth_header_t) + ip_len;
    if (frame_len > frame_cap || udp_len > 0xFFFFU || ip_len > 0xFFFFU)
        return 0;

    memset(frame_out, 0, frame_len);

    dhcpd_eth_header_t* eth = (dhcpd_eth_header_t*) frame_out;
    memset(eth->dst, DHCPD_ETH_BROADCAST_BYTE, sizeof(eth->dst));
    memcpy(eth->src, mac, sizeof(eth->src));
    eth->ethertype_be = dhcpd_to_be16((uint16_t) ETH_P_IP);

    uint8_t* ip_ptr = frame_out + sizeof(dhcpd_eth_header_t);
    dhcpd_ipv4_header_t* ip = (dhcpd_ipv4_header_t*) ip_ptr;
    ip->version_ihl = DHCPD_IP_VERSION_IHL;
    ip->dscp_ecn = 0;
    ip->total_len_be = dhcpd_to_be16((uint16_t) ip_len);
    ip->identification_be = dhcpd_to_be16((uint16_t) (xid & 0xFFFFU));
    ip->flags_frag_be = dhcpd_to_be16(DHCPD_IP_FLAGS_DF);
    ip->ttl = DHCPD_IP_TTL_DEFAULT;
    ip->protocol = DHCPD_IP_PROTO_UDP;
    ip->header_checksum_be = 0;
    ip->src_addr_be = 0;
    ip->dst_addr_be = dhcpd_to_be32(0xFFFFFFFFUL);
    ip->header_checksum_be = dhcpd_checksum_bytes(ip_ptr, sizeof(dhcpd_ipv4_header_t));

    uint8_t* udp_ptr = ip_ptr + sizeof(dhcpd_ipv4_header_t);
    dhcpd_udp_header_t* udp = (dhcpd_udp_header_t*) udp_ptr;
    udp->src_port_be = dhcpd_to_be16(DHCPD_CLIENT_PORT);
    udp->dst_port_be = dhcpd_to_be16(DHCPD_SERVER_PORT);
    udp->length_be = dhcpd_to_be16((uint16_t) udp_len);
    udp->checksum_be = 0;

    uint8_t* bootp_ptr = udp_ptr + sizeof(dhcpd_udp_header_t);
    dhcpd_bootp_header_t* bootp = (dhcpd_bootp_header_t*) bootp_ptr;
    bootp->op = DHCPD_BOOTP_OP_REQUEST;
    bootp->htype = DHCPD_BOOTP_HTYPE_ETHERNET;
    bootp->hlen = DHCPD_BOOTP_HLEN_ETHERNET;
    bootp->hops = 0;
    bootp->xid_be = dhcpd_to_be32(xid);
    bootp->secs_be = 0;
    bootp->flags_be = dhcpd_to_be16(DHCPD_BOOTP_FLAGS_BROADCAST);
    bootp->ciaddr_be = 0;
    bootp->yiaddr_be = 0;
    bootp->siaddr_be = 0;
    bootp->giaddr_be = 0;
    memcpy(bootp->chaddr, mac, 6U);

    uint8_t* magic_ptr = bootp_ptr + sizeof(dhcpd_bootp_header_t);
    uint32_t magic_cookie_be = dhcpd_to_be32(DHCPD_DHCP_MAGIC_COOKIE);
    memcpy(magic_ptr, &magic_cookie_be, sizeof(magic_cookie_be));
    memcpy(magic_ptr + sizeof(magic_cookie_be), options, options_len);

    udp->checksum_be = dhcpd_udp_checksum(ip->src_addr_be, ip->dst_addr_be, udp_ptr, udp_len);
    return frame_len;
}

static void dhcpd_parse_options(const uint8_t* options,
                                size_t options_len,
                                uint8_t* out_msg_type,
                                uint32_t* out_server_id_be)
{
    if (out_msg_type)
        *out_msg_type = 0U;
    if (out_server_id_be)
        *out_server_id_be = 0U;
    if (!options)
        return;

    size_t index = 0U;
    while (index < options_len)
    {
        uint8_t opt = options[index++];
        if (opt == DHCPD_OPT_PAD)
            continue;
        if (opt == DHCPD_OPT_END)
            break;
        if (index >= options_len)
            break;

        uint8_t opt_len = options[index++];
        if ((size_t) opt_len > (options_len - index))
            break;

        const uint8_t* data = &options[index];
        if (opt == DHCPD_OPT_MSG_TYPE && opt_len >= 1U && out_msg_type)
            *out_msg_type = data[0];
        else if (opt == DHCPD_OPT_SERVER_ID && opt_len >= 4U && out_server_id_be)
        {
            uint32_t server_id_be = ((uint32_t) data[0] << 24) |
                                    ((uint32_t) data[1] << 16) |
                                    ((uint32_t) data[2] << 8) |
                                    (uint32_t) data[3];
            *out_server_id_be = server_id_be;
        }

        index += (size_t) opt_len;
    }
}

static bool dhcpd_parse_reply(const uint8_t* frame,
                              size_t frame_len,
                              const uint8_t mac[6],
                              uint32_t expected_xid,
                              uint8_t* out_msg_type,
                              uint32_t* out_yiaddr_be,
                              uint32_t* out_server_id_be)
{
    if (out_msg_type)
        *out_msg_type = 0U;
    if (out_yiaddr_be)
        *out_yiaddr_be = 0U;
    if (out_server_id_be)
        *out_server_id_be = 0U;

    if (!frame || !mac || frame_len < (DHCPD_ETH_HEADER_LEN + DHCPD_IPV4_MIN_HEADER_LEN + DHCPD_UDP_HEADER_LEN))
        return false;

    const dhcpd_eth_header_t* eth = (const dhcpd_eth_header_t*) frame;
    if (dhcpd_from_be16(eth->ethertype_be) != ETH_P_IP)
        return false;

    const uint8_t* ip_ptr = frame + sizeof(dhcpd_eth_header_t);
    const dhcpd_ipv4_header_t* ip = (const dhcpd_ipv4_header_t*) ip_ptr;
    uint8_t version = (uint8_t) (ip->version_ihl >> 4);
    uint8_t ihl = (uint8_t) (ip->version_ihl & 0x0FU);
    if (version != 4U || ihl < 5U)
        return false;

    size_t ip_header_len = (size_t) ihl * 4U;
    if (frame_len < sizeof(dhcpd_eth_header_t) + ip_header_len + sizeof(dhcpd_udp_header_t))
        return false;
    if (ip->protocol != DHCPD_IP_PROTO_UDP)
        return false;

    uint16_t ip_total_len = dhcpd_from_be16(ip->total_len_be);
    if (ip_total_len < (uint16_t) (ip_header_len + sizeof(dhcpd_udp_header_t)))
        return false;
    if ((size_t) ip_total_len > (frame_len - sizeof(dhcpd_eth_header_t)))
        return false;

    const uint8_t* udp_ptr = ip_ptr + ip_header_len;
    const dhcpd_udp_header_t* udp = (const dhcpd_udp_header_t*) udp_ptr;
    if (dhcpd_from_be16(udp->src_port_be) != DHCPD_SERVER_PORT ||
        dhcpd_from_be16(udp->dst_port_be) != DHCPD_CLIENT_PORT)
    {
        return false;
    }

    uint16_t udp_len = dhcpd_from_be16(udp->length_be);
    if (udp_len < sizeof(dhcpd_udp_header_t))
        return false;
    if ((size_t) udp_len > ((size_t) ip_total_len - ip_header_len))
        return false;

    const uint8_t* bootp_ptr = udp_ptr + sizeof(dhcpd_udp_header_t);
    size_t bootp_len = (size_t) udp_len - sizeof(dhcpd_udp_header_t);
    if (bootp_len < (sizeof(dhcpd_bootp_header_t) + sizeof(uint32_t)))
        return false;

    const dhcpd_bootp_header_t* bootp = (const dhcpd_bootp_header_t*) bootp_ptr;
    if (bootp->op != DHCPD_BOOTP_OP_REPLY ||
        bootp->htype != DHCPD_BOOTP_HTYPE_ETHERNET ||
        bootp->hlen != DHCPD_BOOTP_HLEN_ETHERNET)
    {
        return false;
    }
    if (dhcpd_from_be32(bootp->xid_be) != expected_xid)
        return false;
    if (memcmp(bootp->chaddr, mac, 6U) != 0)
        return false;

    const uint8_t* magic_ptr = bootp_ptr + sizeof(dhcpd_bootp_header_t);
    uint32_t magic_cookie_be = 0U;
    memcpy(&magic_cookie_be, magic_ptr, sizeof(magic_cookie_be));
    if (dhcpd_from_be32(magic_cookie_be) != DHCPD_DHCP_MAGIC_COOKIE)
        return false;

    uint8_t msg_type = 0U;
    uint32_t server_id_be = 0U;
    const uint8_t* options = magic_ptr + sizeof(uint32_t);
    size_t options_len = bootp_len - sizeof(dhcpd_bootp_header_t) - sizeof(uint32_t);
    dhcpd_parse_options(options, options_len, &msg_type, &server_id_be);
    if (msg_type == 0U)
        return false;

    if (out_msg_type)
        *out_msg_type = msg_type;
    if (out_yiaddr_be)
        *out_yiaddr_be = bootp->yiaddr_be;
    if (out_server_id_be)
        *out_server_id_be = server_id_be;
    return true;
}

static bool dhcpd_mac_is_zero(const uint8_t mac[6])
{
    if (!mac)
        return true;
    for (size_t i = 0; i < 6U; i++)
    {
        if (mac[i] != 0U)
            return false;
    }
    return true;
}

static bool dhcpd_parse_args(int argc, char** argv, bool* out_daemonize, bool* out_console_echo)
{
    if (!out_daemonize || !out_console_echo)
        return false;

    bool daemonize = false;
    bool console_echo = true;
    for (int i = 1; i < argc; i++)
    {
        const char* arg = argv[i];
        if (!arg)
            continue;

        if (strcmp(arg, "--daemon") == 0 || strcmp(arg, "--daemonize") == 0)
        {
            daemonize = true;
            console_echo = false;
            continue;
        }
        if (strcmp(arg, "--foreground") == 0)
        {
            daemonize = false;
            console_echo = true;
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            printf("Usage: %s [--foreground|--daemon]\n", (argc > 0 && argv && argv[0]) ? argv[0] : "TheDHCPd");
            return false;
        }
    }

    *out_daemonize = daemonize;
    *out_console_echo = console_echo;
    return true;
}

int main(int argc, char** argv)
{
    bool daemonize = false;
    bool console_echo = true;
    if (!dhcpd_parse_args(argc, argv, &daemonize, &console_echo))
        return 0;

    if (daemonize && daemon(0, 0) != 0)
    {
        printf("[TheDHCPd] daemonize failed errno=%d\n", errno);
        return 1;
    }

    (void) mkdir("/var", 0755U);
    (void) mkdir("/var/log", 0755U);
    (void) mkdir("/var/run", 0755U);

    int log_fd = open("/var/log/dhcpd.log", O_CREAT | O_WRONLY | O_TRUNC);
    if (log_fd < 0)
        log_fd = -1;

    uint8_t mac[6];
    mac[0] = 0x02U;
    mac[1] = 0x00U;
    mac[2] = 0x00U;
    mac[3] = 0x00U;
    mac[4] = 0x00U;
    mac[5] = 0x01U;
    uint32_t xid = dhcpd_seed_xid(mac);

    uint8_t tx_frame[DHCPD_TX_FRAME_MAX];
    uint8_t rx_frame[DHCPD_RX_FRAME_MAX];
    uint32_t discover_elapsed_ms = DHCPD_DISCOVER_PERIOD_MS;
    uint32_t discover_attempt = 0U;
    uint32_t retry_elapsed_ms = DHCPD_NET_RETRY_MS;
    int net_fd = -1;
    bool waiting_net_logged = false;
    bool net_ready_logged = false;

    for (;;)
    {
        if (net_fd < 0 && retry_elapsed_ms >= DHCPD_NET_RETRY_MS)
        {
            net_fd = open(DHCPD_NET_NODE_PATH, O_RDWR | O_NONBLOCK);
            retry_elapsed_ms = 0U;
            if (net_fd < 0)
            {
                if (!waiting_net_logged)
                {
                    dhcpd_log_line(log_fd,
                                   console_echo,
                                   "[TheDHCPd] waiting for %s (open failed errno=%d)",
                                   DHCPD_NET_NODE_PATH,
                                   errno);
                    waiting_net_logged = true;
                }
            }
            else
            {
                sys_net_raw_stats_t stats;
                memset(&stats, 0, sizeof(stats));
                if (ioctl(net_fd, NET_RAW_IOCTL_GET_STATS, &stats) == 0 && !dhcpd_mac_is_zero(stats.mac))
                {
                    memcpy(mac, stats.mac, sizeof(mac));
                    xid = dhcpd_seed_xid(mac);
                }

                if (!net_ready_logged)
                {
                    dhcpd_log_line(log_fd,
                                   console_echo,
                                   "[TheDHCPd] started net=%s mac=%02X:%02X:%02X:%02X:%02X:%02X xid=0x%08X",
                                   DHCPD_NET_NODE_PATH,
                                   (unsigned int) mac[0],
                                   (unsigned int) mac[1],
                                   (unsigned int) mac[2],
                                   (unsigned int) mac[3],
                                   (unsigned int) mac[4],
                                   (unsigned int) mac[5],
                                   (unsigned int) xid);
                    net_ready_logged = true;
                }

                waiting_net_logged = false;
                discover_elapsed_ms = DHCPD_DISCOVER_PERIOD_MS;
                discover_attempt = 0U;
            }
        }

        if (net_fd < 0)
        {
            (void) usleep(DHCPD_LOOP_SLEEP_MS * 1000U);
            retry_elapsed_ms += DHCPD_LOOP_SLEEP_MS;
            continue;
        }

        if (discover_elapsed_ms >= DHCPD_DISCOVER_PERIOD_MS)
        {
            discover_attempt++;
            size_t tx_len = dhcpd_build_discover(tx_frame, sizeof(tx_frame), mac, xid);
            if (tx_len == 0U)
            {
                dhcpd_log_line(log_fd, console_echo, "[TheDHCPd] failed to build DHCPDISCOVER");
            }
            else
            {
                int write_rc = (int) write(net_fd, tx_frame, tx_len);
                if (write_rc == (int) tx_len)
                {
                    if (discover_attempt == 1U || (discover_attempt % DHCPD_DISCOVER_LOG_EVERY) == 0U)
                    {
                        dhcpd_log_line(log_fd,
                                       console_echo,
                                       "[TheDHCPd] DHCPDISCOVER sent bytes=%u attempt=%u period_ms=%u",
                                       (unsigned int) tx_len,
                                       (unsigned int) discover_attempt,
                                       (unsigned int) DHCPD_DISCOVER_PERIOD_MS);
                    }
                }
                else
                {
                    dhcpd_log_line(log_fd,
                                   console_echo,
                                   "[TheDHCPd] DHCPDISCOVER send failed rc=%d errno=%d",
                                   write_rc,
                                   errno);
                    (void) close(net_fd);
                    net_fd = -1;
                    waiting_net_logged = false;
                    net_ready_logged = false;
                    discover_attempt = 0U;
                }
            }
            discover_elapsed_ms = 0U;
        }

        for (;;)
        {
            int read_rc = (int) read(net_fd, rx_frame, sizeof(rx_frame));
            if (read_rc <= 0)
                break;

            uint8_t msg_type = 0U;
            uint32_t yiaddr_be = 0U;
            uint32_t server_id_be = 0U;
            if (!dhcpd_parse_reply(rx_frame,
                                   (size_t) read_rc,
                                   mac,
                                   xid,
                                   &msg_type,
                                   &yiaddr_be,
                                   &server_id_be))
            {
                continue;
            }

            char yiaddr_text[16];
            char server_text[16];
            dhcpd_format_ipv4(yiaddr_be, yiaddr_text);
            dhcpd_format_ipv4(server_id_be, server_text);

            if (msg_type == DHCPD_MSG_OFFER)
            {
                dhcpd_log_line(log_fd,
                               console_echo,
                               "[TheDHCPd] DHCPOFFER yiaddr=%s server=%s (REQUEST path not implemented yet)",
                               yiaddr_text,
                               server_text);
            }
            else if (msg_type == DHCPD_MSG_ACK)
            {
                dhcpd_log_line(log_fd,
                               console_echo,
                               "[TheDHCPd] DHCPACK yiaddr=%s server=%s",
                               yiaddr_text,
                               server_text);
            }
            else if (msg_type == DHCPD_MSG_NAK)
            {
                dhcpd_log_line(log_fd, console_echo, "[TheDHCPd] DHCPNAK from server=%s", server_text);
            }
        }

        (void) usleep(DHCPD_LOOP_SLEEP_MS * 1000U);
        discover_elapsed_ms += DHCPD_LOOP_SLEEP_MS;
        retry_elapsed_ms += DHCPD_LOOP_SLEEP_MS;
    }
}
