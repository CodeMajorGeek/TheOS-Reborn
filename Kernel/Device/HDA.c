#include <Device/HDA.h>
#include <Device/HDA_private.h>

#include <CPU/PCI.h>
#include <CPU/ISR.h>
#include <Debug/KDebug.h>
#include <Memory/PMM.h>
#include <Memory/VMM.h>
#include <Task/Task.h>

#include <string.h>

static hda_runtime_state_t HDA_state = {
    .stream_tag = HDA_STREAM_TAG_DEFAULT,
    .sample_rate = HDA_DEFAULT_SAMPLE_RATE,
    .channels = HDA_DEFAULT_CHANNELS,
    .oss_format = HDA_DEFAULT_OSS_FORMAT,
    .fragment_shift = HDA_DEFAULT_FRAGMENT_SHIFT,
    .fragment_count = HDA_DEFAULT_FRAGMENT_COUNT
};
static bool HDA_log_open_once = false;
static bool HDA_log_write_once = false;
static bool HDA_log_stall_once = false;

typedef struct hda_rate_format
{
    uint32_t rate;
    uint16_t fmt;
} hda_rate_format_t;

static const hda_rate_format_t HDA_rate_formats[] = {
    { 8000U, HDA_FMT_8K },
    { 11025U, HDA_FMT_11K025 },
    { 16000U, HDA_FMT_16K },
    { 22050U, HDA_FMT_22K05 },
    { 32000U, HDA_FMT_32K },
    { 44100U, HDA_FMT_44K1 },
    { 48000U, HDA_FMT_48K }
};

static uint32_t HDA_abs_diff_u32(uint32_t a, uint32_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

static uint32_t HDA_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static uint64_t HDA_stream_bytes_per_second_from_params(uint32_t sample_rate, uint8_t channels, uint32_t oss_format)
{
    uint64_t bytes_per_sample = (oss_format == AFMT_U8) ? 1ULL : 2ULL;
    uint64_t stream_channels = (channels == 0U) ? 1ULL : (uint64_t) channels;
    return (uint64_t) sample_rate * bytes_per_sample * stream_channels;
}

static uint16_t HDA_clamp_fragment_shift(uint16_t shift)
{
    if (shift < HDA_MIN_FRAGMENT_SHIFT)
        return (uint16_t) HDA_MIN_FRAGMENT_SHIFT;
    if (shift > HDA_MAX_FRAGMENT_SHIFT)
        return (uint16_t) HDA_MAX_FRAGMENT_SHIFT;
    return shift;
}

static void HDA_stream_apply_fragment_config_locked(uint16_t shift, uint16_t count)
{
    uint16_t applied_shift = HDA_clamp_fragment_shift(shift);
    uint32_t fragment_bytes = (uint32_t) (1U << applied_shift);
    uint32_t max_count = HDA_STREAM_BUFFER_BYTES / fragment_bytes;
    uint32_t applied_count = (count == 0U) ? HDA_DEFAULT_FRAGMENT_COUNT : count;

    if (max_count == 0U)
        max_count = 1U;

    if (applied_count > max_count)
        applied_count = max_count;
    if (applied_count == 0U)
        applied_count = 1U;

    uint32_t queue_limit = fragment_bytes * applied_count;
    if (queue_limit >= HDA_STREAM_BUFFER_BYTES)
        queue_limit = HDA_STREAM_BUFFER_BYTES - 1U;
    if (queue_limit == 0U)
        queue_limit = fragment_bytes;
    if (queue_limit >= HDA_STREAM_BUFFER_BYTES)
        queue_limit = HDA_STREAM_BUFFER_BYTES - 1U;

    uint64_t stream_bps = HDA_stream_bytes_per_second_from_params(HDA_state.sample_rate,
                                                                   HDA_state.channels,
                                                                   HDA_state.oss_format);
    uint64_t queue_floor = (uint64_t) HDA_QUEUE_FLOOR_BYTES;
    if (stream_bps != 0ULL)
    {
        uint64_t queue_floor_from_ms =
            ((stream_bps * (uint64_t) HDA_QUEUE_FLOOR_MS) + 999ULL) / 1000ULL;
        if (queue_floor_from_ms > queue_floor)
            queue_floor = queue_floor_from_ms;
    }

    if (queue_floor >= (uint64_t) HDA_STREAM_BUFFER_BYTES)
        queue_floor = (uint64_t) HDA_STREAM_BUFFER_BYTES - 1ULL;

    if ((uint64_t) queue_limit < queue_floor)
    {
        uint64_t aligned_floor =
            ((queue_floor + (uint64_t) fragment_bytes - 1ULL) / (uint64_t) fragment_bytes) *
            (uint64_t) fragment_bytes;
        if (aligned_floor == 0ULL)
            aligned_floor = (uint64_t) fragment_bytes;
        if (aligned_floor >= (uint64_t) HDA_STREAM_BUFFER_BYTES)
            aligned_floor = (uint64_t) HDA_STREAM_BUFFER_BYTES - 1ULL;
        queue_limit = (uint32_t) aligned_floor;
    }

    HDA_state.fragment_shift = applied_shift;
    HDA_state.fragment_count = (uint16_t) applied_count;
    HDA_state.fragment_bytes = fragment_bytes;
    HDA_state.queue_limit_bytes = queue_limit;
}

static void HDA_stream_set_default_fragments_locked(void)
{
    HDA_stream_apply_fragment_config_locked(HDA_DEFAULT_FRAGMENT_SHIFT,
                                            HDA_DEFAULT_FRAGMENT_COUNT);
}

static uint32_t HDA_profile_tick_hz(void)
{
    uint32_t hz = ISR_get_tick_hz();
    return (hz == 0U) ? 100U : hz;
}

static uint64_t HDA_stream_bytes_per_second_locked(void)
{
    return HDA_stream_bytes_per_second_from_params(HDA_state.sample_rate,
                                                   HDA_state.channels,
                                                   HDA_state.oss_format);
}

static void HDA_profile_reset_window_locked(uint64_t now_ticks)
{
    HDA_state.prof_window_start_ticks = now_ticks;
    HDA_state.prof_write_calls = 0;
    HDA_state.prof_write_bytes = 0;
    HDA_state.prof_write_short_calls = 0;
    HDA_state.prof_write_zero_calls = 0;
    HDA_state.prof_stalled_calls = 0;
    HDA_state.prof_stall_sleeps = 0;
    HDA_state.prof_stall_sleeps_max = 0;
    HDA_state.prof_buffered_peak = 0;
    HDA_state.prof_sync_calls = 0;
    HDA_state.prof_sync_wait_rounds = 0;
    HDA_state.prof_sync_wait_rounds_max = 0;
}

static void HDA_profile_maybe_emit_locked(uint64_t now_ticks)
{
    if (HDA_state.prof_window_start_ticks == 0ULL)
    {
        HDA_profile_reset_window_locked(now_ticks);
        HDA_state.prof_last_report_ticks = now_ticks;
        return;
    }

    uint64_t hz = (uint64_t) HDA_profile_tick_hz();
    uint64_t period_ticks = hz;
    uint64_t elapsed_ticks = now_ticks - HDA_state.prof_window_start_ticks;
    if (elapsed_ticks < period_ticks)
        return;

    if (elapsed_ticks == 0ULL)
        elapsed_ticks = 1ULL;

    uint64_t write_calls = HDA_state.prof_write_calls;
    uint64_t write_bytes = HDA_state.prof_write_bytes;
    uint64_t bytes_per_sec = (write_bytes * hz) / elapsed_ticks;
    uint64_t calls_per_sec = (write_calls * hz) / elapsed_ticks;
    uint64_t avg_write = (write_calls == 0ULL) ? 0ULL : (write_bytes / write_calls);
    uint64_t avg_stall = (HDA_state.prof_stalled_calls == 0ULL)
                             ? 0ULL
                             : (HDA_state.prof_stall_sleeps / HDA_state.prof_stalled_calls);
    uint64_t stream_bps = HDA_stream_bytes_per_second_locked();
    uint64_t queue_ms = (stream_bps == 0ULL)
                            ? 0ULL
                            : (((uint64_t) HDA_state.queue_limit_bytes * 1000ULL) / stream_bps);
    uint64_t peak_ms = (stream_bps == 0ULL)
                           ? 0ULL
                           : (((uint64_t) HDA_state.prof_buffered_peak * 1000ULL) / stream_bps);

    if (write_calls != 0ULL ||
        HDA_state.prof_stall_sleeps != 0ULL ||
        HDA_state.prof_sync_calls != 0ULL)
    {
        kdebug_printf("[HDA-PROF] win_ticks=%llu calls=%llu calls_s=%llu bytes=%llu bytes_s=%llu avg_write=%llu short=%llu zero=%llu stalled_calls=%llu stall_total_ms=%llu stall_avg_ms=%llu stall_max_ms=%u sync_calls=%llu sync_total_ms=%llu sync_max_ms=%u buffered_peak=%u buffered_peak_ms=%llu queue_limit=%u queue_ms=%llu\n",
                      (unsigned long long) elapsed_ticks,
                      (unsigned long long) write_calls,
                      (unsigned long long) calls_per_sec,
                      (unsigned long long) write_bytes,
                      (unsigned long long) bytes_per_sec,
                      (unsigned long long) avg_write,
                      (unsigned long long) HDA_state.prof_write_short_calls,
                      (unsigned long long) HDA_state.prof_write_zero_calls,
                      (unsigned long long) HDA_state.prof_stalled_calls,
                      (unsigned long long) HDA_state.prof_stall_sleeps,
                      (unsigned long long) avg_stall,
                      (unsigned) HDA_state.prof_stall_sleeps_max,
                      (unsigned long long) HDA_state.prof_sync_calls,
                      (unsigned long long) HDA_state.prof_sync_wait_rounds,
                      (unsigned) HDA_state.prof_sync_wait_rounds_max,
                      (unsigned) HDA_state.prof_buffered_peak,
                      (unsigned long long) peak_ms,
                      (unsigned) HDA_state.queue_limit_bytes,
                      (unsigned long long) queue_ms);
    }

    HDA_profile_reset_window_locked(now_ticks);
    HDA_state.prof_last_report_ticks = now_ticks;
}

static void HDA_cpu_relax(void)
{
    __asm__ __volatile__("pause");
}

static inline uint8_t HDA_mmio_read8(uint16_t reg)
{
    volatile uint8_t* ptr = (volatile uint8_t*) (HDA_state.mmio_virt + reg);
    return *ptr;
}

static inline uint16_t HDA_mmio_read16(uint16_t reg)
{
    volatile uint16_t* ptr = (volatile uint16_t*) (HDA_state.mmio_virt + reg);
    return *ptr;
}

static inline uint32_t HDA_mmio_read32(uint16_t reg)
{
    volatile uint32_t* ptr = (volatile uint32_t*) (HDA_state.mmio_virt + reg);
    return *ptr;
}

static inline void HDA_mmio_write8(uint16_t reg, uint8_t value)
{
    volatile uint8_t* ptr = (volatile uint8_t*) (HDA_state.mmio_virt + reg);
    *ptr = value;
}

static inline void HDA_mmio_write16(uint16_t reg, uint16_t value)
{
    volatile uint16_t* ptr = (volatile uint16_t*) (HDA_state.mmio_virt + reg);
    *ptr = value;
}

static inline void HDA_mmio_write32(uint16_t reg, uint32_t value)
{
    volatile uint32_t* ptr = (volatile uint32_t*) (HDA_state.mmio_virt + reg);
    *ptr = value;
}

static bool HDA_wait_mmio_bit16(uint16_t reg, uint16_t mask, bool set, uint32_t timeout_loops)
{
    for (uint32_t i = 0; i < timeout_loops; i++)
    {
        bool bit_set = (HDA_mmio_read16(reg) & mask) != 0;
        if (bit_set == set)
            return true;
        HDA_cpu_relax();
    }

    return false;
}

static bool HDA_wait_mmio_bit32(uint16_t reg, uint32_t mask, bool set, uint32_t timeout_loops)
{
    for (uint32_t i = 0; i < timeout_loops; i++)
    {
        bool bit_set = (HDA_mmio_read32(reg) & mask) != 0;
        if (bit_set == set)
            return true;
        HDA_cpu_relax();
    }

    return false;
}

static void HDA_stream_copy_to_ring_locked(const uint8_t* src, uint32_t start, uint32_t len)
{
    uint32_t copied = 0;
    while (copied < len)
    {
        uint32_t pos = (start + copied) % HDA_STREAM_BUFFER_BYTES;
        uint32_t page_index = pos / HDA_STREAM_PAGE_BYTES;
        uint32_t page_offset = pos % HDA_STREAM_PAGE_BYTES;
        uint32_t chunk = HDA_min_u32(len - copied, HDA_STREAM_PAGE_BYTES - page_offset);

        memcpy(HDA_state.stream_page_virt[page_index] + page_offset,
               src + copied,
               chunk);
        copied += chunk;
    }
}

static void HDA_stream_zero_region_locked(uint32_t start, uint32_t len)
{
    if (len == 0)
        return;

    uint32_t zeroed = 0;
    while (zeroed < len)
    {
        uint32_t pos = (start + zeroed) % HDA_STREAM_BUFFER_BYTES;
        uint32_t page_index = pos / HDA_STREAM_PAGE_BYTES;
        uint32_t page_offset = pos % HDA_STREAM_PAGE_BYTES;
        uint32_t chunk = HDA_min_u32(len - zeroed, HDA_STREAM_PAGE_BYTES - page_offset);

        memset(HDA_state.stream_page_virt[page_index] + page_offset, 0, chunk);
        zeroed += chunk;
    }
}

static bool HDA_stream_alloc_buffers_locked(void)
{
    if (!HDA_state.bdl_virt)
    {
        HDA_state.bdl_phys = (uintptr_t) PMM_alloc_page();
        if (HDA_state.bdl_phys == 0)
            return false;

        HDA_state.bdl_virt = (hda_bdl_entry_t*) P2V(HDA_state.bdl_phys);
        memset(HDA_state.bdl_virt, 0, HDA_STREAM_PAGE_BYTES);
    }

    for (uint32_t i = 0; i < HDA_STREAM_BDL_ENTRIES; i++)
    {
        if (!HDA_state.stream_page_virt[i])
        {
            HDA_state.stream_page_phys[i] = (uintptr_t) PMM_alloc_page();
            if (HDA_state.stream_page_phys[i] == 0)
                return false;

            HDA_state.stream_page_virt[i] = (uint8_t*) P2V(HDA_state.stream_page_phys[i]);
            memset(HDA_state.stream_page_virt[i], 0, HDA_STREAM_PAGE_BYTES);
        }

        HDA_state.bdl_virt[i].addr_lo = ADDRLO(HDA_state.stream_page_phys[i]);
        HDA_state.bdl_virt[i].addr_hi = ADDRHI(HDA_state.stream_page_phys[i]);
        HDA_state.bdl_virt[i].length = HDA_STREAM_PAGE_BYTES;
        HDA_state.bdl_virt[i].ioc = 0;
    }

    HDA_state.stream_write_pos = 0;
    HDA_state.stream_hw_pos = 0;
    HDA_state.stream_buffered_bytes = 0;
    return true;
}

static void HDA_stream_reset_buffer_locked(void)
{
    for (uint32_t i = 0; i < HDA_STREAM_BDL_ENTRIES; i++)
    {
        if (HDA_state.stream_page_virt[i])
            memset(HDA_state.stream_page_virt[i], 0, HDA_STREAM_PAGE_BYTES);
    }

    HDA_state.stream_write_pos = 0;
    HDA_state.stream_hw_pos = 0;
    HDA_state.stream_buffered_bytes = 0;
}

static void HDA_stream_refresh_consumed_locked(void)
{
    if (!HDA_state.stream_running)
        return;

    uint32_t hw_pos = HDA_mmio_read32((uint16_t) (HDA_state.stream_sd_offset + HDA_SD_REG_LPIB));
    hw_pos %= HDA_STREAM_BUFFER_BYTES;

    uint32_t prev_hw_pos = HDA_state.stream_hw_pos;
    uint32_t consumed = (hw_pos >= prev_hw_pos)
                            ? (hw_pos - prev_hw_pos)
                            : (HDA_STREAM_BUFFER_BYTES - prev_hw_pos + hw_pos);

    if (consumed > HDA_state.stream_buffered_bytes)
        consumed = HDA_state.stream_buffered_bytes;

    if (consumed > 0)
    {
        HDA_stream_zero_region_locked(prev_hw_pos, consumed);
        HDA_state.stream_buffered_bytes -= consumed;
    }

    HDA_state.stream_hw_pos = hw_pos;
}

static uint16_t HDA_build_stream_format(uint32_t sample_rate, uint8_t channels, uint32_t oss_format)
{
    uint16_t rate_fmt = 0;
    bool rate_found = false;

    for (size_t i = 0; i < (sizeof(HDA_rate_formats) / sizeof(HDA_rate_formats[0])); i++)
    {
        if (HDA_rate_formats[i].rate == sample_rate)
        {
            rate_fmt = HDA_rate_formats[i].fmt;
            rate_found = true;
            break;
        }
    }

    if (!rate_found)
        return 0;

    uint16_t bits_fmt = 0;
    if (oss_format == AFMT_U8)
        bits_fmt = (uint16_t) (HDA_FMT_BITS_8 << HDA_FMT_BITS_SHIFT);
    else if (oss_format == AFMT_S16_LE)
        bits_fmt = (uint16_t) (HDA_FMT_BITS_16 << HDA_FMT_BITS_SHIFT);
    else
        return 0;

    if (channels < 1 || channels > 2)
        return 0;

    uint16_t chan_fmt = (uint16_t) ((channels - 1U) & HDA_FMT_CHANNELS_MASK);
    return (uint16_t) (rate_fmt | bits_fmt | chan_fmt);
}

static uint32_t HDA_pick_supported_rate(uint32_t requested)
{
    uint32_t best = HDA_rate_formats[0].rate;
    uint32_t best_diff = HDA_abs_diff_u32(best, requested);

    for (size_t i = 1; i < (sizeof(HDA_rate_formats) / sizeof(HDA_rate_formats[0])); i++)
    {
        uint32_t candidate = HDA_rate_formats[i].rate;
        uint32_t diff = HDA_abs_diff_u32(candidate, requested);
        if (diff < best_diff)
        {
            best = candidate;
            best_diff = diff;
        }
    }

    return best;
}

static bool HDA_codec_send_verb_locked(uint8_t codec_addr,
                                       uint8_t nid,
                                       uint16_t verb,
                                       uint16_t payload,
                                       uint32_t* out_response)
{
    if (!HDA_state.controller_ready)
        return false;

    HDA_mmio_write16(HDA_REG_ICIS, HDA_ICIS_IRV);
    if (!HDA_wait_mmio_bit16(HDA_REG_ICIS, HDA_ICIS_ICB, false, HDA_TIMEOUT_SHORT))
        return false;

    uint32_t command = (((uint32_t) codec_addr & 0x0FU) << 28) |
                       (((uint32_t) nid & 0xFFU) << 20) |
                       (((uint32_t) verb & 0xFFFU) << 8) |
                       ((uint32_t) payload & 0xFFFFU);

    HDA_mmio_write32(HDA_REG_ICOI, command);
    HDA_mmio_write16(HDA_REG_ICIS, HDA_ICIS_ICB);

    if (!HDA_wait_mmio_bit16(HDA_REG_ICIS, HDA_ICIS_IRV, true, HDA_TIMEOUT_SHORT))
        return false;

    if (out_response)
        *out_response = HDA_mmio_read32(HDA_REG_ICII);

    HDA_mmio_write16(HDA_REG_ICIS, HDA_ICIS_IRV);
    return true;
}

static bool HDA_codec_get_param_locked(uint8_t codec_addr,
                                       uint8_t nid,
                                       uint8_t param_id,
                                       uint32_t* out_response)
{
    return HDA_codec_send_verb_locked(codec_addr,
                                      nid,
                                      HDA_VERB_GET_PARAMETER,
                                      param_id,
                                      out_response);
}

static bool HDA_codec_discover_route_locked(void)
{
    uint32_t response = 0;
    if (!HDA_codec_get_param_locked(HDA_state.codec_addr, 0, HDA_PARAM_NODE_COUNT, &response))
        return false;

    uint8_t root_start = (uint8_t) ((response >> 16) & 0xFFU);
    uint8_t root_count = (uint8_t) (response & 0xFFU);

    uint8_t afg_nid = 0;
    for (uint8_t i = 0; i < root_count; i++)
    {
        uint8_t nid = (uint8_t) (root_start + i);
        if (!HDA_codec_get_param_locked(HDA_state.codec_addr, nid, HDA_PARAM_FUNCTION_TYPE, &response))
            continue;

        if ((response & 0xFFU) == HDA_FN_GROUP_AUDIO)
        {
            afg_nid = nid;
            break;
        }
    }

    if (afg_nid == 0)
        return false;

    if (!HDA_codec_get_param_locked(HDA_state.codec_addr, afg_nid, HDA_PARAM_NODE_COUNT, &response))
        return false;

    uint8_t widget_start = (uint8_t) ((response >> 16) & 0xFFU);
    uint8_t widget_count = (uint8_t) (response & 0xFFU);

    uint8_t output_nid = 0;
    uint8_t pin_nid = 0;

    for (uint8_t i = 0; i < widget_count; i++)
    {
        uint8_t nid = (uint8_t) (widget_start + i);
        if (!HDA_codec_get_param_locked(HDA_state.codec_addr, nid, HDA_PARAM_AUDIO_WIDGET_CAP, &response))
            continue;

        uint8_t widget_type = (uint8_t) ((response >> HDA_WIDGET_TYPE_SHIFT) & HDA_WIDGET_TYPE_MASK);
        if (widget_type == HDA_WIDGET_TYPE_OUTPUT && output_nid == 0)
            output_nid = nid;

        if (widget_type == HDA_WIDGET_TYPE_PIN && pin_nid == 0)
        {
            uint32_t pin_cap = 0;
            if (HDA_codec_get_param_locked(HDA_state.codec_addr, nid, HDA_PARAM_PIN_CAP, &pin_cap) &&
                (pin_cap & HDA_PINCAP_OUT) != 0)
            {
                pin_nid = nid;
            }
        }
    }

    if (output_nid == 0 || pin_nid == 0)
        return false;

    HDA_state.afg_nid = afg_nid;
    HDA_state.output_nid = output_nid;
    HDA_state.pin_nid = pin_nid;
    HDA_state.route_ready = true;
    return true;
}

static bool HDA_codec_program_route_locked(void)
{
    if (!HDA_state.route_ready)
        return false;

    uint8_t cad = HDA_state.codec_addr;
    uint8_t afg = HDA_state.afg_nid;
    uint8_t out = HDA_state.output_nid;
    uint8_t pin = HDA_state.pin_nid;

    uint16_t stream_chan = (uint16_t) (((uint16_t) HDA_state.stream_tag << 4) | 0U);

    if (!HDA_codec_send_verb_locked(cad, afg, HDA_VERB_SET_POWER_STATE, HDA_POWER_STATE_D0, NULL))
        return false;
    if (!HDA_codec_send_verb_locked(cad, out, HDA_VERB_SET_POWER_STATE, HDA_POWER_STATE_D0, NULL))
        return false;
    if (!HDA_codec_send_verb_locked(cad, pin, HDA_VERB_SET_POWER_STATE, HDA_POWER_STATE_D0, NULL))
        return false;

    (void) HDA_codec_send_verb_locked(cad,
                                      out,
                                      HDA_VERB_SET_AMP_GAIN_MUTE,
                                      (uint16_t) (HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | HDA_AMP_GAIN_MAX),
                                      NULL);
    (void) HDA_codec_send_verb_locked(cad,
                                      pin,
                                      HDA_VERB_SET_AMP_GAIN_MUTE,
                                      (uint16_t) (HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | HDA_AMP_GAIN_MAX),
                                      NULL);

    (void) HDA_codec_send_verb_locked(cad, pin, HDA_VERB_SET_CONNECT_SEL, 0, NULL);
    if (!HDA_codec_send_verb_locked(cad, pin, HDA_VERB_SET_PIN_WIDGET_CONTROL, HDA_PIN_WIDGET_CTL_OUT_EN, NULL))
        return false;
    (void) HDA_codec_send_verb_locked(cad, pin, HDA_VERB_SET_EAPD_BTLENABLE, HDA_EAPD_ENABLE, NULL);

    if (!HDA_codec_send_verb_locked(cad, out, HDA_VERB_SET_STREAM_FORMAT, HDA_state.stream_format, NULL))
        return false;
    if (!HDA_codec_send_verb_locked(cad, out, HDA_VERB_SET_CHANNEL_STREAMID, stream_chan, NULL))
        return false;

    return true;
}

static bool HDA_stream_prepare_locked(void)
{
    if (!HDA_state.controller_ready || !HDA_state.route_ready)
        return false;

    if (!HDA_stream_alloc_buffers_locked())
        return false;

    uint16_t sd = HDA_state.stream_sd_offset;

    uint8_t ctl0 = HDA_mmio_read8((uint16_t) (sd + HDA_SD_REG_CTL0));
    ctl0 &= (uint8_t) ~HDA_SD_CTL_RUN;
    HDA_mmio_write8((uint16_t) (sd + HDA_SD_REG_CTL0), ctl0);

    HDA_mmio_write8((uint16_t) (sd + HDA_SD_REG_STS),
                    (uint8_t) (HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE));

    HDA_mmio_write8((uint16_t) (sd + HDA_SD_REG_CTL0), (uint8_t) (ctl0 | HDA_SD_CTL_SRST));
    if (!HDA_wait_mmio_bit16((uint16_t) (sd + HDA_SD_REG_CTL0), HDA_SD_CTL_SRST, true, HDA_TIMEOUT_SHORT))
        return false;

    HDA_mmio_write8((uint16_t) (sd + HDA_SD_REG_CTL0), (uint8_t) (ctl0 & ~HDA_SD_CTL_SRST));
    if (!HDA_wait_mmio_bit16((uint16_t) (sd + HDA_SD_REG_CTL0), HDA_SD_CTL_SRST, false, HDA_TIMEOUT_SHORT))
        return false;

    HDA_mmio_write32((uint16_t) (sd + HDA_SD_REG_CBL), HDA_STREAM_BUFFER_BYTES);
    HDA_mmio_write16((uint16_t) (sd + HDA_SD_REG_LVI), (uint16_t) (HDA_STREAM_BDL_ENTRIES - 1U));
    HDA_mmio_write16((uint16_t) (sd + HDA_SD_REG_FMT), HDA_state.stream_format);
    HDA_mmio_write32((uint16_t) (sd + HDA_SD_REG_BDLPL), ADDRLO(HDA_state.bdl_phys));
    HDA_mmio_write32((uint16_t) (sd + HDA_SD_REG_BDLPU), ADDRHI(HDA_state.bdl_phys));

    uint8_t ctl2 = HDA_mmio_read8((uint16_t) (sd + HDA_SD_REG_CTL2));
    ctl2 &= 0x0FU;
    ctl2 |= (uint8_t) ((HDA_state.stream_tag & 0x0FU) << 4);
    HDA_mmio_write8((uint16_t) (sd + HDA_SD_REG_CTL2), ctl2);

    if (!HDA_codec_program_route_locked())
        return false;

    HDA_stream_reset_buffer_locked();
    HDA_state.stream_running = false;
    return true;
}

static bool HDA_stream_start_locked(void)
{
    if (HDA_state.stream_running)
        return true;

    if (!HDA_stream_prepare_locked())
        return false;

    uint16_t sd = HDA_state.stream_sd_offset;
    uint8_t ctl0 = HDA_mmio_read8((uint16_t) (sd + HDA_SD_REG_CTL0));
    ctl0 |= HDA_SD_CTL_RUN;
    HDA_mmio_write8((uint16_t) (sd + HDA_SD_REG_CTL0), ctl0);

    HDA_state.stream_running = true;
    HDA_state.stream_hw_pos = HDA_mmio_read32((uint16_t) (sd + HDA_SD_REG_LPIB)) % HDA_STREAM_BUFFER_BYTES;
    return true;
}

static void HDA_stream_stop_locked(void)
{
    if (!HDA_state.controller_ready)
        return;

    uint16_t sd = HDA_state.stream_sd_offset;
    uint8_t ctl0 = HDA_mmio_read8((uint16_t) (sd + HDA_SD_REG_CTL0));
    ctl0 &= (uint8_t) ~HDA_SD_CTL_RUN;
    HDA_mmio_write8((uint16_t) (sd + HDA_SD_REG_CTL0), ctl0);

    HDA_state.stream_running = false;
    HDA_stream_reset_buffer_locked();
}

static bool HDA_controller_reset_locked(void)
{
    uint32_t gctl = HDA_mmio_read32(HDA_REG_GCTL);
    HDA_mmio_write32(HDA_REG_GCTL, gctl & ~HDA_GCTL_CRST);
    if (!HDA_wait_mmio_bit32(HDA_REG_GCTL, HDA_GCTL_CRST, false, HDA_TIMEOUT_SHORT))
        return false;

    HDA_mmio_write32(HDA_REG_GCTL, (gctl | HDA_GCTL_CRST));
    if (!HDA_wait_mmio_bit32(HDA_REG_GCTL, HDA_GCTL_CRST, true, HDA_TIMEOUT_LONG))
        return false;

    return true;
}

static bool HDA_controller_init_locked(uint8_t bus,
                                       uint8_t slot,
                                       uint8_t function,
                                       uint16_t vendor,
                                       uint16_t device)
{
    uint32_t bar0 = PCI_config_read(bus, slot, function, PCI_BAR0_ADDR_REG);
    if (bar0 == 0 || bar0 == 0xFFFFFFFFU)
        return false;
    if ((bar0 & 0x1U) != 0)
        return false;

    uintptr_t mmio_phys = (uintptr_t) (bar0 & ~0xFULL);
    uintptr_t mmio_virt = VMM_MMIO_VIRT(mmio_phys);
    VMM_map_mmio_uc_pages(mmio_virt, mmio_phys, HDA_MMIO_REG_WINDOW_SIZE);

    uint16_t pci_command = PCI_config_readw(bus, slot, function, PCI_COMMAND_REG);
    pci_command |= (1U << 1);
    pci_command |= (1U << 2);
    PCI_config_writew(bus, slot, function, PCI_COMMAND_REG, pci_command);

    HDA_state.pci_bus = bus;
    HDA_state.pci_slot = slot;
    HDA_state.pci_function = function;
    HDA_state.mmio_phys = mmio_phys;
    HDA_state.mmio_virt = mmio_virt;

    HDA_mmio_write32(HDA_REG_INTCTL, 0);
    HDA_mmio_write32(HDA_REG_INTSTS, 0xFFFFFFFFU);

    if (!HDA_controller_reset_locked())
        return false;

    uint16_t gcap = HDA_mmio_read16(HDA_REG_GCAP);
    uint8_t iss = (uint8_t) ((gcap >> HDA_GCAP_ISS_SHIFT) & HDA_GCAP_ISS_MASK);
    uint8_t oss = (uint8_t) ((gcap >> HDA_GCAP_OSS_SHIFT) & HDA_GCAP_OSS_MASK);
    if (oss == 0)
        return false;

    HDA_state.stream_index = iss;
    HDA_state.stream_sd_offset = (uint16_t) (HDA_STREAM_DESC_BASE + ((uint16_t) HDA_state.stream_index * HDA_STREAM_DESC_STRIDE));
    if ((uint32_t) HDA_state.stream_sd_offset + HDA_STREAM_DESC_STRIDE > HDA_MMIO_REG_WINDOW_SIZE)
        return false;

    uint16_t statests = HDA_mmio_read16(HDA_REG_STATESTS);
    if (statests == 0)
    {
        for (uint32_t i = 0; i < HDA_TIMEOUT_SHORT; i++)
        {
            statests = HDA_mmio_read16(HDA_REG_STATESTS);
            if (statests != 0)
                break;
            HDA_cpu_relax();
        }
    }

    if (statests == 0)
        return false;

    uint8_t codec_addr = 0xFFU;
    for (uint8_t cad = 0; cad < 15; cad++)
    {
        if ((statests & (1U << cad)) != 0)
        {
            codec_addr = cad;
            break;
        }
    }

    if (codec_addr == 0xFFU)
        return false;

    HDA_state.codec_addr = codec_addr;
    HDA_state.controller_ready = true;

    HDA_state.sample_rate = HDA_pick_supported_rate(HDA_DEFAULT_SAMPLE_RATE);
    HDA_state.channels = HDA_DEFAULT_CHANNELS;
    HDA_state.oss_format = HDA_DEFAULT_OSS_FORMAT;
    HDA_stream_set_default_fragments_locked();
    HDA_state.stream_format = HDA_build_stream_format(HDA_state.sample_rate,
                                                      HDA_state.channels,
                                                      HDA_state.oss_format);
    if (HDA_state.stream_format == 0)
        return false;

    if (!HDA_stream_alloc_buffers_locked())
        return false;

    if (!HDA_codec_discover_route_locked())
    {
        /* QEMU fallback topology for hda-output codec. */
        HDA_state.afg_nid = 0x01U;
        HDA_state.output_nid = 0x02U;
        HDA_state.pin_nid = 0x05U;
        HDA_state.route_ready = true;
    }

    if (!HDA_stream_prepare_locked())
        return false;

    HDA_state.available = true;

    kdebug_printf("[HDA] initialized b=%u s=%u f=%u vid=0x%X did=0x%X cad=%u afg=0x%X out=0x%X pin=0x%X rate=%u ch=%u fmt=0x%X\n",
                  (unsigned) bus,
                  (unsigned) slot,
                  (unsigned) function,
                  (unsigned) vendor,
                  (unsigned) device,
                  (unsigned) HDA_state.codec_addr,
                  (unsigned) HDA_state.afg_nid,
                  (unsigned) HDA_state.output_nid,
                  (unsigned) HDA_state.pin_nid,
                  (unsigned) HDA_state.sample_rate,
                  (unsigned) HDA_state.channels,
                  (unsigned) HDA_state.oss_format);

    return true;
}

void HDA_try_setup_device(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device)
{
    if (!HDA_state.lock_ready)
    {
        spinlock_init(&HDA_state.lock);
        HDA_state.lock_ready = true;
    }

    spin_lock(&HDA_state.lock);
    if (!HDA_state.available)
    {
        if (!HDA_controller_init_locked(bus, slot, function, vendor, device))
        {
            HDA_state.available = false;
            HDA_state.controller_ready = false;
            HDA_state.route_ready = false;
        }
    }
    spin_unlock(&HDA_state.lock);
}

bool HDA_is_available(void)
{
    bool available = false;

    if (!HDA_state.lock_ready)
        return false;

    spin_lock(&HDA_state.lock);
    available = HDA_state.available;
    spin_unlock(&HDA_state.lock);
    return available;
}

bool HDA_dsp_open(uint32_t owner_pid)
{
    if (owner_pid == 0 || !HDA_state.lock_ready)
        return false;

    spin_lock(&HDA_state.lock);
    if (!HDA_state.available)
    {
        spin_unlock(&HDA_state.lock);
        return false;
    }

    if (!HDA_state.dsp_open)
    {
        HDA_state.dsp_open = true;
        HDA_state.dsp_owner_pid = owner_pid;
        HDA_state.dsp_open_count = 1;
        HDA_stream_set_default_fragments_locked();
        HDA_stream_stop_locked();
        (void) HDA_stream_prepare_locked();
        uint64_t now_ticks = ISR_get_timer_ticks();
        HDA_profile_reset_window_locked(now_ticks);
        HDA_state.prof_last_report_ticks = now_ticks;
        if (!HDA_log_open_once)
        {
            HDA_log_open_once = true;
            kdebug_printf("[HDA] /dev/dsp opened owner=%u rate=%u ch=%u fmt=0x%X frag=%u/%u queue=%u\n",
                          (unsigned) owner_pid,
                          (unsigned) HDA_state.sample_rate,
                          (unsigned) HDA_state.channels,
                          (unsigned) HDA_state.oss_format,
                          (unsigned) HDA_state.fragment_count,
                          (unsigned) HDA_state.fragment_bytes,
                          (unsigned) HDA_state.queue_limit_bytes);
        }
        spin_unlock(&HDA_state.lock);
        return true;
    }

    if (HDA_state.dsp_owner_pid != owner_pid)
    {
        spin_unlock(&HDA_state.lock);
        return false;
    }

    HDA_state.dsp_open_count++;
    spin_unlock(&HDA_state.lock);
    return true;
}

void HDA_dsp_close(uint32_t owner_pid)
{
    if (owner_pid == 0 || !HDA_state.lock_ready)
        return;

    spin_lock(&HDA_state.lock);
    if (!HDA_state.dsp_open || HDA_state.dsp_owner_pid != owner_pid)
    {
        spin_unlock(&HDA_state.lock);
        return;
    }

    if (HDA_state.dsp_open_count > 1)
    {
        HDA_state.dsp_open_count--;
        spin_unlock(&HDA_state.lock);
        return;
    }

    HDA_stream_stop_locked();
    HDA_state.dsp_open = false;
    HDA_state.dsp_open_count = 0;
    HDA_state.dsp_owner_pid = 0;
    spin_unlock(&HDA_state.lock);
}

size_t HDA_dsp_write(const void* data, size_t len)
{
    if (!data || len == 0 || !HDA_state.lock_ready)
        return 0;

    const uint8_t* src = (const uint8_t*) data;
    size_t requested = len;
    size_t written = 0;
    uint32_t stalls = 0;
    uint32_t stalls_this_call = 0;
    uint32_t buffered_peak_call = 0;

    while (written < len)
    {
        spin_lock(&HDA_state.lock);

        if (!HDA_state.available || !HDA_state.dsp_open)
        {
            spin_unlock(&HDA_state.lock);
            break;
        }

        if (!HDA_state.stream_running)
        {
            if (!HDA_stream_start_locked())
            {
                spin_unlock(&HDA_state.lock);
                break;
            }
        }

        HDA_stream_refresh_consumed_locked();
        if (HDA_state.stream_buffered_bytes > buffered_peak_call)
            buffered_peak_call = HDA_state.stream_buffered_bytes;

        uint32_t queue_limit = HDA_state.queue_limit_bytes;
        if (queue_limit == 0U || queue_limit >= HDA_STREAM_BUFFER_BYTES)
            queue_limit = HDA_STREAM_BUFFER_BYTES - 1U;

        uint32_t free_space = 0;
        if (HDA_state.stream_buffered_bytes < queue_limit)
            free_space = queue_limit - HDA_state.stream_buffered_bytes;

        if (free_space == 0)
        {
            spin_unlock(&HDA_state.lock);
            stalls++;
            stalls_this_call++;
            if (stalls >= HDA_WRITE_STALL_LIMIT && !HDA_log_stall_once)
            {
                HDA_log_stall_once = true;
                kdebug_printf("[HDA] stream stalled lpib=%u buffered=%u limit=%u write_pos=%u\n",
                              (unsigned) HDA_state.stream_hw_pos,
                              (unsigned) HDA_state.stream_buffered_bytes,
                              (unsigned) HDA_state.queue_limit_bytes,
                              (unsigned) HDA_state.stream_write_pos);
            }
            if (stalls >= HDA_WRITE_STALL_LIMIT)
                break;
            (void) task_sleep_ms(1);
            continue;
        }

        uint32_t chunk = (uint32_t) HDA_min_u32((uint32_t) (len - written), free_space);
        HDA_stream_copy_to_ring_locked(src + written, HDA_state.stream_write_pos, chunk);

        HDA_state.stream_write_pos = (HDA_state.stream_write_pos + chunk) % HDA_STREAM_BUFFER_BYTES;
        HDA_state.stream_buffered_bytes += chunk;
        if (HDA_state.stream_buffered_bytes > buffered_peak_call)
            buffered_peak_call = HDA_state.stream_buffered_bytes;

        spin_unlock(&HDA_state.lock);

        written += chunk;
        stalls = 0;
        if (!HDA_log_write_once)
        {
            HDA_log_write_once = true;
            kdebug_printf("[HDA] first pcm write bytes=%u\n", (unsigned) chunk);
        }
    }

    spin_lock(&HDA_state.lock);
    if (HDA_state.available && HDA_state.dsp_open)
    {
        HDA_state.prof_write_calls++;
        HDA_state.prof_write_bytes += (uint64_t) written;
        if (written < requested)
            HDA_state.prof_write_short_calls++;
        if (written == 0U)
            HDA_state.prof_write_zero_calls++;
        if (stalls_this_call != 0U)
        {
            HDA_state.prof_stalled_calls++;
            HDA_state.prof_stall_sleeps += (uint64_t) stalls_this_call;
            if (stalls_this_call > HDA_state.prof_stall_sleeps_max)
                HDA_state.prof_stall_sleeps_max = stalls_this_call;
        }
        if (buffered_peak_call > HDA_state.prof_buffered_peak)
            HDA_state.prof_buffered_peak = buffered_peak_call;

        HDA_profile_maybe_emit_locked(ISR_get_timer_ticks());
    }
    spin_unlock(&HDA_state.lock);

    return written;
}

bool HDA_dsp_ioctl(unsigned long request, int32_t* inout_value)
{
    if (!HDA_state.lock_ready)
        return false;

    spin_lock(&HDA_state.lock);
    if (!HDA_state.available || !HDA_state.dsp_open)
    {
        spin_unlock(&HDA_state.lock);
        return false;
    }

    switch (request)
    {
        case SNDCTL_DSP_RESET:
            HDA_stream_stop_locked();
            if (!HDA_stream_prepare_locked())
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }
            spin_unlock(&HDA_state.lock);
            return true;

        case SNDCTL_DSP_SYNC:
        {
            uint32_t wait_rounds = 0;
            while (HDA_state.stream_running)
            {
                HDA_stream_refresh_consumed_locked();
                if (HDA_state.stream_buffered_bytes == 0)
                    break;

                if (wait_rounds >= HDA_WRITE_STALL_LIMIT)
                    break;

                wait_rounds++;
                spin_unlock(&HDA_state.lock);
                (void) task_sleep_ms(1);
                spin_lock(&HDA_state.lock);
            }

            HDA_state.prof_sync_calls++;
            HDA_state.prof_sync_wait_rounds += (uint64_t) wait_rounds;
            if (wait_rounds > HDA_state.prof_sync_wait_rounds_max)
                HDA_state.prof_sync_wait_rounds_max = wait_rounds;
            HDA_profile_maybe_emit_locked(ISR_get_timer_ticks());

            HDA_stream_stop_locked();
            spin_unlock(&HDA_state.lock);
            return true;
        }

        case SNDCTL_DSP_GETFMTS:
            if (!inout_value)
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }
            *inout_value = (int32_t) (AFMT_U8 | AFMT_S16_LE);
            spin_unlock(&HDA_state.lock);
            return true;

        case SNDCTL_DSP_SETFRAGMENT:
        {
            if (!inout_value)
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            uint32_t packed = (uint32_t) *inout_value;
            uint16_t requested_shift = (uint16_t) (packed & 0xFFFFU);
            uint16_t requested_count = (uint16_t) ((packed >> 16) & 0xFFFFU);

            HDA_stream_apply_fragment_config_locked(requested_shift, requested_count);
            HDA_stream_stop_locked();
            if (!HDA_stream_prepare_locked())
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            *inout_value = (int32_t) ((((uint32_t) HDA_state.fragment_count) << 16) |
                                      ((uint32_t) HDA_state.fragment_shift));
            kdebug_printf("[HDA] ioctl SETFRAGMENT req_count=%u req_shift=%u -> count=%u bytes=%u limit=%u\n",
                          (unsigned) requested_count,
                          (unsigned) requested_shift,
                          (unsigned) HDA_state.fragment_count,
                          (unsigned) HDA_state.fragment_bytes,
                          (unsigned) HDA_state.queue_limit_bytes);
            spin_unlock(&HDA_state.lock);
            return true;
        }

        case SNDCTL_DSP_SPEED:
        {
            if (!inout_value || *inout_value <= 0)
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            uint32_t new_rate = HDA_pick_supported_rate((uint32_t) *inout_value);
            uint16_t new_fmt = HDA_build_stream_format(new_rate, HDA_state.channels, HDA_state.oss_format);
            if (new_fmt == 0)
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            HDA_state.sample_rate = new_rate;
            HDA_state.stream_format = new_fmt;
            HDA_stream_apply_fragment_config_locked(HDA_state.fragment_shift,
                                                    HDA_state.fragment_count);

            HDA_stream_stop_locked();
            if (!HDA_stream_prepare_locked())
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            *inout_value = (int32_t) HDA_state.sample_rate;
            kdebug_printf("[HDA] ioctl SPEED -> %u\n", (unsigned) HDA_state.sample_rate);
            spin_unlock(&HDA_state.lock);
            return true;
        }

        case SNDCTL_DSP_STEREO:
        {
            if (!inout_value)
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            uint8_t new_channels = (*inout_value != 0) ? HDA_FMT_STEREO : HDA_FMT_MONO;
            uint16_t new_fmt = HDA_build_stream_format(HDA_state.sample_rate, new_channels, HDA_state.oss_format);
            if (new_fmt == 0)
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            HDA_state.channels = new_channels;
            HDA_state.stream_format = new_fmt;
            HDA_stream_apply_fragment_config_locked(HDA_state.fragment_shift,
                                                    HDA_state.fragment_count);

            HDA_stream_stop_locked();
            if (!HDA_stream_prepare_locked())
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            *inout_value = (int32_t) ((HDA_state.channels == HDA_FMT_STEREO) ? 1 : 0);
            kdebug_printf("[HDA] ioctl STEREO -> %u\n", (unsigned) ((HDA_state.channels == HDA_FMT_STEREO) ? 1U : 0U));
            spin_unlock(&HDA_state.lock);
            return true;
        }

        case SNDCTL_DSP_SETFMT:
        {
            if (!inout_value)
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            if ((uint32_t) *inout_value == AFMT_QUERY)
            {
                *inout_value = (int32_t) HDA_state.oss_format;
                spin_unlock(&HDA_state.lock);
                return true;
            }

            uint32_t new_oss_fmt = (uint32_t) *inout_value;
            if (new_oss_fmt != AFMT_U8 && new_oss_fmt != AFMT_S16_LE)
            {
                *inout_value = (int32_t) HDA_state.oss_format;
                spin_unlock(&HDA_state.lock);
                return true;
            }

            uint16_t new_fmt = HDA_build_stream_format(HDA_state.sample_rate, HDA_state.channels, new_oss_fmt);
            if (new_fmt == 0)
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            HDA_state.oss_format = new_oss_fmt;
            HDA_state.stream_format = new_fmt;
            HDA_stream_apply_fragment_config_locked(HDA_state.fragment_shift,
                                                    HDA_state.fragment_count);

            HDA_stream_stop_locked();
            if (!HDA_stream_prepare_locked())
            {
                spin_unlock(&HDA_state.lock);
                return false;
            }

            *inout_value = (int32_t) HDA_state.oss_format;
            kdebug_printf("[HDA] ioctl SETFMT -> 0x%X\n", (unsigned) HDA_state.oss_format);
            spin_unlock(&HDA_state.lock);
            return true;
        }

        default:
            spin_unlock(&HDA_state.lock);
            return false;
    }
}
