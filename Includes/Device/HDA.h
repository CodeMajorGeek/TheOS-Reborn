#ifndef _HDA_H
#define _HDA_H

#include <Debug/Spinlock.h>
#include <UAPI/Sound.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HDA_VENDOR_INTEL                0x8086U

#define HDA_DSP_NODE_PATH               "/dev/dsp"
#define HDA_AUDIO_NODE_PATH             "/dev/audio"

#define HDA_MMIO_REG_WINDOW_SIZE        0x4000U
#define HDA_STREAM_BDL_ENTRIES          16U
#define HDA_STREAM_PAGE_BYTES           0x1000U
#define HDA_STREAM_BUFFER_BYTES         (HDA_STREAM_BDL_ENTRIES * HDA_STREAM_PAGE_BYTES)
#define HDA_STREAM_TAG_DEFAULT          1U
#define HDA_DEFAULT_SAMPLE_RATE         11025U
#define HDA_DEFAULT_CHANNELS            2U
#define HDA_DEFAULT_OSS_FORMAT          AFMT_S16_LE
#define HDA_DEFAULT_FRAGMENT_SHIFT      8U
#define HDA_DEFAULT_FRAGMENT_COUNT      4U
#define HDA_MIN_FRAGMENT_SHIFT          8U
#define HDA_MAX_FRAGMENT_SHIFT          16U

#define HDA_REG_GCAP                    0x00U
#define HDA_REG_GCTL                    0x08U
#define HDA_REG_STATESTS                0x0EU
#define HDA_REG_INTCTL                  0x20U
#define HDA_REG_INTSTS                  0x24U
#define HDA_REG_ICOI                    0x60U
#define HDA_REG_ICII                    0x64U
#define HDA_REG_ICIS                    0x68U

#define HDA_GCTL_CRST                   (1U << 0)

#define HDA_ICIS_ICB                    (1U << 0)
#define HDA_ICIS_IRV                    (1U << 1)

#define HDA_GCAP_ISS_SHIFT              8U
#define HDA_GCAP_ISS_MASK               0x0FU
#define HDA_GCAP_OSS_SHIFT              12U
#define HDA_GCAP_OSS_MASK               0x0FU

#define HDA_STREAM_DESC_BASE            0x80U
#define HDA_STREAM_DESC_STRIDE          0x20U

#define HDA_SD_REG_CTL0                 0x00U
#define HDA_SD_REG_CTL2                 0x02U
#define HDA_SD_REG_STS                  0x03U
#define HDA_SD_REG_LPIB                 0x04U
#define HDA_SD_REG_CBL                  0x08U
#define HDA_SD_REG_LVI                  0x0CU
#define HDA_SD_REG_FMT                  0x12U
#define HDA_SD_REG_BDLPL                0x18U
#define HDA_SD_REG_BDLPU                0x1CU

#define HDA_SD_CTL_SRST                 (1U << 0)
#define HDA_SD_CTL_RUN                  (1U << 1)
#define HDA_SD_STS_BCIS                 (1U << 2)
#define HDA_SD_STS_FIFOE                (1U << 3)
#define HDA_SD_STS_DESE                 (1U << 4)

#define HDA_VERB_GET_PARAMETER          0xF00U
#define HDA_VERB_SET_STREAM_FORMAT      0x200U
#define HDA_VERB_SET_AMP_GAIN_MUTE      0x300U
#define HDA_VERB_SET_CONNECT_SEL        0x701U
#define HDA_VERB_SET_POWER_STATE        0x705U
#define HDA_VERB_SET_CHANNEL_STREAMID   0x706U
#define HDA_VERB_SET_PIN_WIDGET_CONTROL 0x707U
#define HDA_VERB_SET_EAPD_BTLENABLE     0x70CU

#define HDA_PARAM_NODE_COUNT            0x04U
#define HDA_PARAM_FUNCTION_TYPE         0x05U
#define HDA_PARAM_AUDIO_WIDGET_CAP      0x09U
#define HDA_PARAM_PIN_CAP               0x0CU

#define HDA_FN_GROUP_AUDIO              0x01U
#define HDA_WIDGET_TYPE_SHIFT           20U
#define HDA_WIDGET_TYPE_MASK            0x0FU
#define HDA_WIDGET_TYPE_OUTPUT          0x0U
#define HDA_WIDGET_TYPE_PIN             0x4U

#define HDA_PINCAP_OUT                  (1U << 4)

#define HDA_PIN_WIDGET_CTL_OUT_EN       0x40U
#define HDA_EAPD_ENABLE                 0x02U

#define HDA_POWER_STATE_D0              0x00U

#define HDA_AMP_SET_OUTPUT              0x8000U
#define HDA_AMP_SET_LEFT                0x2000U
#define HDA_AMP_SET_RIGHT               0x1000U
#define HDA_AMP_GAIN_MAX                0x007FU

#define HDA_FMT_BASE_44K                (1U << 14)
#define HDA_FMT_MULT_SHIFT              11U
#define HDA_FMT_DIV_SHIFT               8U
#define HDA_FMT_BITS_SHIFT              4U
#define HDA_FMT_CHANNELS_MASK           0x0FU

#define HDA_FMT_BITS_8                  0x0U
#define HDA_FMT_BITS_16                 0x1U

#define HDA_FMT_STEREO                  2U
#define HDA_FMT_MONO                    1U

#define HDA_FMT_48K                     0x0000U
#define HDA_FMT_44K1                    0x4000U
#define HDA_FMT_32K                     0x0A00U
#define HDA_FMT_22K05                   0x4100U
#define HDA_FMT_16K                     0x0200U
#define HDA_FMT_11K025                  0x4300U
#define HDA_FMT_8K                      0x0500U

#define HDA_TIMEOUT_SHORT               100000U
#define HDA_TIMEOUT_LONG                1000000U
#define HDA_WRITE_STALL_LIMIT           5000U

typedef struct hda_bdl_entry
{
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;
    uint32_t ioc;
} __attribute__((packed)) hda_bdl_entry_t;

typedef struct hda_runtime_state
{
    bool available;
    bool controller_ready;
    bool route_ready;
    bool stream_running;
    bool lock_ready;
    bool dsp_open;
    uint32_t dsp_owner_pid;
    uint32_t dsp_open_count;

    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_function;
    uint8_t codec_addr;
    uint8_t afg_nid;
    uint8_t output_nid;
    uint8_t pin_nid;
    uint8_t stream_tag;
    uint8_t stream_index;

    uint16_t stream_sd_offset;
    uint16_t stream_format;
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t oss_format;
    uint16_t fragment_shift;
    uint16_t fragment_count;
    uint32_t fragment_bytes;
    uint32_t queue_limit_bytes;

    uintptr_t mmio_phys;
    uintptr_t mmio_virt;

    uintptr_t bdl_phys;
    hda_bdl_entry_t* bdl_virt;
    uintptr_t stream_page_phys[HDA_STREAM_BDL_ENTRIES];
    uint8_t* stream_page_virt[HDA_STREAM_BDL_ENTRIES];

    uint32_t stream_write_pos;
    uint32_t stream_hw_pos;
    uint32_t stream_buffered_bytes;

    spinlock_t lock;
} hda_runtime_state_t;

void HDA_try_setup_device(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
bool HDA_is_available(void);
bool HDA_dsp_open(uint32_t owner_pid);
void HDA_dsp_close(uint32_t owner_pid);
size_t HDA_dsp_write(const void* data, size_t len);
bool HDA_dsp_ioctl(unsigned long request, int32_t* inout_value);

#endif
