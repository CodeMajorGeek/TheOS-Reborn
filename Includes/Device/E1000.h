#ifndef _E1000_H
#define _E1000_H

#include <Debug/Spinlock.h>
#include <Task/Task.h>
#include <UAPI/Net.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E1000_VENDOR_INTEL                 0x8086U
#define E1000_DEVICE_82574L                0x10D3U
#define E1000_NET_NODE_PATH                "/dev/net0"

#define E1000_IRQ_VECTOR                   0xD1U

#define E1000_IRQ_MODE_POLL                0U
#define E1000_IRQ_MODE_MSI                 1U
#define E1000_IRQ_MODE_MSIX                2U

#define E1000_MMIO_WINDOW_SIZE             0x20000U
#define E1000_RX_DESC_COUNT                64U
#define E1000_TX_DESC_COUNT                64U
#define E1000_RX_BUFFER_BYTES              2048U
#define E1000_TX_BUFFER_BYTES              2048U
#define E1000_RX_SW_QUEUE_LEN              128U

#define E1000_ETH_FRAME_MIN_BYTES          14U
#define E1000_ETH_FRAME_MAX_BYTES          1518U

#define E1000_REG_CTRL                     0x0000U
#define E1000_REG_STATUS                   0x0008U
#define E1000_REG_ICR                      0x00C0U
#define E1000_REG_IMS                      0x00D0U
#define E1000_REG_IMC                      0x00D8U
#define E1000_REG_RCTL                     0x0100U
#define E1000_REG_TCTL                     0x0400U
#define E1000_REG_TIPG                     0x0410U
#define E1000_REG_RDBAL                    0x2800U
#define E1000_REG_RDBAH                    0x2804U
#define E1000_REG_RDLEN                    0x2808U
#define E1000_REG_RDH                      0x2810U
#define E1000_REG_RDT                      0x2818U
#define E1000_REG_TDBAL                    0x3800U
#define E1000_REG_TDBAH                    0x3804U
#define E1000_REG_TDLEN                    0x3808U
#define E1000_REG_TDH                      0x3810U
#define E1000_REG_TDT                      0x3818U
#define E1000_REG_RAL0                     0x5400U
#define E1000_REG_RAH0                     0x5404U

#define E1000_CTRL_RST                     (1U << 26)
#define E1000_STATUS_LINK_UP               (1U << 1)
#define E1000_RAH_ADDR_VALID               (1U << 31)

#define E1000_RCTL_EN                      (1U << 1)
#define E1000_RCTL_BAM                     (1U << 15)
#define E1000_RCTL_SECRC                   (1U << 26)

#define E1000_TCTL_EN                      (1U << 1)
#define E1000_TCTL_PSP                     (1U << 3)
#define E1000_TCTL_CT_SHIFT                4U
#define E1000_TCTL_COLD_SHIFT              12U
#define E1000_TCTL_CT_DEFAULT              0x10U
#define E1000_TCTL_COLD_DEFAULT            0x40U
#define E1000_TIPG_DEFAULT                 0x0060200AU

#define E1000_TX_DESC_CMD_EOP              (1U << 0)
#define E1000_TX_DESC_CMD_IFCS             (1U << 1)
#define E1000_TX_DESC_CMD_RS               (1U << 3)
#define E1000_TX_DESC_STATUS_DD            (1U << 0)

#define E1000_RX_DESC_STATUS_DD            (1U << 0)
#define E1000_RX_DESC_STATUS_EOP           (1U << 1)

#define E1000_ICR_TXDW                     (1U << 0)
#define E1000_ICR_LSC                      (1U << 2)
#define E1000_ICR_RXDMT0                   (1U << 4)
#define E1000_ICR_RXO                      (1U << 6)
#define E1000_ICR_RXT0                     (1U << 7)
#define E1000_IRQ_MASK                     (E1000_ICR_TXDW | E1000_ICR_LSC | E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXT0)

#define E1000_RESET_TIMEOUT_LOOPS          1000000U
#define E1000_TX_WAIT_TIMEOUT_LOOPS        100000U

typedef struct e1000_rx_desc
{
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct e1000_tx_desc
{
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct e1000_frame_slot
{
    uint16_t len;
    uint8_t data[E1000_RX_BUFFER_BYTES];
} e1000_frame_slot_t;

typedef struct e1000_runtime_state
{
    bool available;
    bool controller_ready;
    bool lock_ready;
    bool waitq_ready;
    bool irq_vector_registered;
    bool link_up;
    uint8_t irq_mode;

    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_function;

    uint16_t vendor_id;
    uint16_t device_id;

    uintptr_t mmio_phys;
    uintptr_t mmio_virt;

    uint8_t mac[6];

    uintptr_t rx_desc_phys;
    e1000_rx_desc_t* rx_desc_virt;
    uintptr_t tx_desc_phys;
    e1000_tx_desc_t* tx_desc_virt;

    uintptr_t rx_buf_phys[E1000_RX_DESC_COUNT];
    uint8_t* rx_buf_virt[E1000_RX_DESC_COUNT];
    uintptr_t tx_buf_phys[E1000_TX_DESC_COUNT];
    uint8_t* tx_buf_virt[E1000_TX_DESC_COUNT];

    uint16_t rx_next_clean;
    uint16_t tx_next_use;

    uint16_t rx_sw_head;
    uint16_t rx_sw_tail;
    uint32_t rx_sw_count;
    e1000_frame_slot_t rx_sw_queue[E1000_RX_SW_QUEUE_LEN];

    uint64_t irq_count;
    uint64_t rx_packets;
    uint64_t rx_dropped;
    uint64_t tx_packets;
    uint64_t tx_dropped;

    task_wait_queue_t rx_waitq;
    spinlock_t lock;
} e1000_runtime_state_t;

void E1000_try_setup_device(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
bool E1000_is_available(void);
bool E1000_get_mac(uint8_t out_mac[6]);
bool E1000_link_is_up(void);
uint8_t E1000_get_irq_mode(void);
uint64_t E1000_get_irq_count(void);
const char* E1000_get_irq_mode_name(void);
bool E1000_get_stats(sys_net_raw_stats_t* out_stats);
bool E1000_get_pending_rx_bytes(size_t* out_bytes);
bool E1000_raw_read(uint8_t* out_frame, size_t out_cap, size_t* out_len, bool block);
bool E1000_raw_write(const uint8_t* frame, size_t len, size_t* out_written);

#endif
