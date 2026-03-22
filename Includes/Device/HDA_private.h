#ifndef _HDA_PRIVATE_H
#define _HDA_PRIVATE_H

#include <Device/HDA.h>

static inline uint8_t HDA_mmio_read8(uint16_t reg);
static inline uint16_t HDA_mmio_read16(uint16_t reg);
static inline uint32_t HDA_mmio_read32(uint16_t reg);
static inline void HDA_mmio_write8(uint16_t reg, uint8_t value);
static inline void HDA_mmio_write16(uint16_t reg, uint16_t value);
static inline void HDA_mmio_write32(uint16_t reg, uint32_t value);

static bool HDA_wait_mmio_bit16(uint16_t reg, uint16_t mask, bool set, uint32_t timeout_loops);
static bool HDA_wait_mmio_bit32(uint16_t reg, uint32_t mask, bool set, uint32_t timeout_loops);

static uint16_t HDA_clamp_fragment_shift(uint16_t shift);
static void HDA_stream_apply_fragment_config_locked(uint16_t shift, uint16_t count);
static void HDA_stream_set_default_fragments_locked(void);

static bool HDA_stream_alloc_buffers_locked(void);
static void HDA_stream_zero_region_locked(uint32_t start, uint32_t len);
static void HDA_stream_reset_buffer_locked(void);
static void HDA_stream_refresh_consumed_locked(void);
static bool HDA_stream_prepare_locked(void);
static bool HDA_stream_start_locked(void);
static void HDA_stream_stop_locked(void);

static uint16_t HDA_build_stream_format(uint32_t sample_rate, uint8_t channels, uint32_t oss_format);
static uint32_t HDA_pick_supported_rate(uint32_t requested);

static bool HDA_codec_send_verb_locked(uint8_t codec_addr, uint8_t nid, uint16_t verb, uint16_t payload, uint32_t* out_response);
static bool HDA_codec_get_param_locked(uint8_t codec_addr, uint8_t nid, uint8_t param_id, uint32_t* out_response);
static bool HDA_codec_discover_route_locked(void);
static bool HDA_codec_program_route_locked(void);

static bool HDA_controller_reset_locked(void);
static bool HDA_controller_init_locked(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);

#endif
