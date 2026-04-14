#include <Device/Mouse.h>
#include <Device/Mouse_private.h>
#include <Device/Keyboard.h>
#include <CPU/IO.h>
#include <CPU/ISR.h>
#include <Debug/Spinlock.h>

#include <stdbool.h>
#include <stdint.h>

#define PS2_STATUS_OUT_FULL        0x01U
#define PS2_STATUS_IN_FULL         0x02U
#define PS2_STATUS_AUX_DATA        0x20U
#define PS2_PORT_STATUS            0x64U
#define PS2_PORT_DATA              0x60U

#define PS2_CMD_ENABLE_AUX         0xA8U
#define PS2_CMD_ENABLE_KBD         0xAEU
#define PS2_CMD_READ_CMD_BYTE      0x20U
#define PS2_CMD_WRITE_CMD_BYTE     0x60U
#define PS2_CMD_WRITE_AUX          0xD4U

#define PS2_MOUSE_CMD_SET_DEFAULTS 0xF6U
#define PS2_MOUSE_CMD_SET_SAMPLE_RATE 0xF3U
#define PS2_MOUSE_CMD_SET_RESOLUTION 0xE8U
#define PS2_MOUSE_CMD_READ_DATA    0xEBU
#define PS2_MOUSE_CMD_ENABLE_DATA  0xF4U
#define PS2_MOUSE_ACK              0xFAU
#define PS2_MOUSE_RESEND           0xFEU
#define MOUSE_POLL_DRAIN_BUDGET    8U
#define MOUSE_IRQ_DRAIN_BUDGET     64U
#define PS2_MOUSE_SAMPLE_RATE_FAST 100U
#define PS2_MOUSE_RESOLUTION_8CMM  2U
#define MOUSE_FORCED_REQUEST_MIN_TICKS 6ULL

static Mouse_runtime_state_t Mouse_state;

static bool Mouse_handle_controller_byte_locked(uint8_t status, uint8_t byte, uint8_t source);
static uint32_t Mouse_poll_aux_bytes_locked(uint32_t budget);
static bool Mouse_force_read_data_locked(void);

static inline uint64_t Mouse_lock_irqsave(void)
{
    return spin_lock_irqsave(&Mouse_state.lock);
}

static inline void Mouse_unlock_irqrestore(uint64_t flags)
{
    spin_unlock_irqrestore(&Mouse_state.lock, flags);
}

static inline void Mouse_lock_noirq(void)
{
    spin_lock(&Mouse_state.lock);
}

static inline void Mouse_unlock_noirq(void)
{
    spin_unlock(&Mouse_state.lock);
}

static bool Mouse_wait_input_clear(uint32_t spin_count)
{
    for (uint32_t i = 0; i < spin_count; i++)
    {
        if ((IO_inb(PS2_PORT_STATUS) & PS2_STATUS_IN_FULL) == 0U)
            return true;
        __asm__ __volatile__("pause");
    }

    return false;
}

static bool Mouse_wait_output_full(uint32_t spin_count)
{
    for (uint32_t i = 0; i < spin_count; i++)
    {
        if ((IO_inb(PS2_PORT_STATUS) & PS2_STATUS_OUT_FULL) != 0U)
            return true;
        __asm__ __volatile__("pause");
    }

    return false;
}

static bool Mouse_read_aux_data(uint8_t* out_data, uint32_t spin_count)
{
    if (!out_data)
        return false;

    for (uint32_t i = 0; i < spin_count; i++)
    {
        uint8_t status = IO_inb(PS2_PORT_STATUS);
        if ((status & PS2_STATUS_OUT_FULL) == 0U)
        {
            __asm__ __volatile__("pause");
            continue;
        }

        uint8_t data = IO_inb(PS2_PORT_DATA);
        if ((status & PS2_STATUS_AUX_DATA) == 0U)
            continue;

        *out_data = data;
        return true;
    }

    return false;
}

static bool Mouse_write_aux_command(uint8_t command)
{
    for (uint32_t attempt = 0U; attempt < 3U; attempt++)
    {
        if (!Mouse_wait_input_clear(2000000U))
            return false;
        IO_outb(PS2_PORT_STATUS, PS2_CMD_WRITE_AUX);

        if (!Mouse_wait_input_clear(2000000U))
            return false;
        IO_outb(PS2_PORT_DATA, command);

        uint8_t ack = 0U;
        if (!Mouse_read_aux_data(&ack, 2000000U))
            return false;
        if (ack == PS2_MOUSE_ACK)
            return true;
        if (ack != PS2_MOUSE_RESEND)
            return false;
    }

    return false;
}

static void Mouse_push_event_locked(const syscall_mouse_event_t* event)
{
    if (!event)
        return;

    if (Mouse_state.event_count >= MOUSE_EVENT_BUFFER_SIZE)
    {
        Mouse_state.events_dropped_full++;
        return;
    }

    Mouse_state.events[Mouse_state.write_pos] = *event;
    Mouse_state.write_pos++;
    if (Mouse_state.write_pos >= MOUSE_EVENT_BUFFER_SIZE)
        Mouse_state.write_pos = 0U;
    Mouse_state.event_count++;
    Mouse_state.events_pushed++;
}

static uint32_t Mouse_poll_aux_bytes_locked(uint32_t budget)
{
    uint32_t drained = 0U;

    while (drained < budget)
    {
        uint8_t status = IO_inb(PS2_PORT_STATUS);
        if ((status & PS2_STATUS_OUT_FULL) == 0U)
            break;

        uint8_t byte = IO_inb(PS2_PORT_DATA);
        (void) Mouse_handle_controller_byte_locked(status, byte, MOUSE_BYTE_SOURCE_POLL);
        if ((status & PS2_STATUS_AUX_DATA) == 0U)
            Keyboard_enqueue_scancode_from_poll(byte);
        drained++;
    }

    Mouse_state.poll_cycles++;
    return drained;
}

static bool Mouse_force_read_data_locked(void)
{
    Mouse_state.forced_request_attempts++;

    if (!Mouse_wait_input_clear(2000U))
    {
        Mouse_state.forced_request_fail++;
        return false;
    }
    IO_outb(PS2_PORT_STATUS, PS2_CMD_WRITE_AUX);

    if (!Mouse_wait_input_clear(2000U))
    {
        Mouse_state.forced_request_fail++;
        return false;
    }
    IO_outb(PS2_PORT_DATA, PS2_MOUSE_CMD_READ_DATA);

    uint8_t ack = 0U;
    if (!Mouse_read_aux_data(&ack, 2000U) || ack != PS2_MOUSE_ACK)
    {
        Mouse_state.forced_request_fail++;
        return false;
    }

    for (uint32_t i = 0U; i < 3U; i++)
    {
        uint8_t byte = 0U;
        if (!Mouse_read_aux_data(&byte, 2000U))
        {
            Mouse_state.forced_request_fail++;
            return false;
        }
        (void) Mouse_handle_controller_byte_locked((uint8_t) (PS2_STATUS_OUT_FULL | PS2_STATUS_AUX_DATA),
                                                   byte,
                                                   MOUSE_BYTE_SOURCE_POLL);
    }

    Mouse_state.forced_request_success++;
    return true;
}

static bool Mouse_init_controller(void)
{
    if (!Mouse_wait_input_clear(2000000U))
        return false;
    IO_outb(PS2_PORT_STATUS, PS2_CMD_ENABLE_KBD);

    if (!Mouse_wait_input_clear(2000000U))
        return false;
    IO_outb(PS2_PORT_STATUS, PS2_CMD_ENABLE_AUX);

    if (!Mouse_wait_input_clear(2000000U))
        return false;
    IO_outb(PS2_PORT_STATUS, PS2_CMD_READ_CMD_BYTE);
    if (!Mouse_wait_output_full(2000000U))
        return false;
    uint8_t cmd_byte = IO_inb(PS2_PORT_DATA);

    cmd_byte |= 0x03U;  // Enable IRQ1 + IRQ12.
    cmd_byte &= (uint8_t) ~0x30U; // Enable kbd + aux clocks.

    if (!Mouse_wait_input_clear(2000000U))
        return false;
    IO_outb(PS2_PORT_STATUS, PS2_CMD_WRITE_CMD_BYTE);
    if (!Mouse_wait_input_clear(2000000U))
        return false;
    IO_outb(PS2_PORT_DATA, cmd_byte);

    if (!Mouse_write_aux_command(PS2_MOUSE_CMD_SET_DEFAULTS))
        return false;
    if (!Mouse_write_aux_command(PS2_MOUSE_CMD_SET_SAMPLE_RATE))
        return false;
    if (!Mouse_write_aux_command(PS2_MOUSE_SAMPLE_RATE_FAST))
        return false;
    if (!Mouse_write_aux_command(PS2_MOUSE_CMD_SET_RESOLUTION))
        return false;
    if (!Mouse_write_aux_command(PS2_MOUSE_RESOLUTION_8CMM))
        return false;
    if (!Mouse_write_aux_command(PS2_MOUSE_CMD_ENABLE_DATA))
        return false;

    return true;
}

void Mouse_init(void)
{
    spinlock_init(&Mouse_state.lock);

    uint64_t flags = Mouse_lock_irqsave();
    Mouse_state.event_count = 0U;
    Mouse_state.write_pos = 0U;
    Mouse_state.read_pos = 0U;
    Mouse_state.packet_index = 0U;
    Mouse_state.irq12_callbacks = 0U;
    Mouse_state.irq12_bytes_total = 0U;
    Mouse_state.irq12_aux_bytes = 0U;
    Mouse_state.irq12_non_aux_bytes = 0U;
    Mouse_state.irq12_drain_budget_hits = 0U;
    Mouse_state.irq1_aux_bytes = 0U;
    Mouse_state.poll_cycles = 0U;
    Mouse_state.poll_bytes_total = 0U;
    Mouse_state.poll_aux_bytes = 0U;
    Mouse_state.poll_non_aux_bytes = 0U;
    Mouse_state.events_pushed = 0U;
    Mouse_state.events_dropped_full = 0U;
    Mouse_state.events_popped = 0U;
    Mouse_state.get_event_empty = 0U;
    Mouse_state.packet_sync_drops = 0U;
    Mouse_state.packet_overflow_drops = 0U;
    Mouse_state.forced_request_attempts = 0U;
    Mouse_state.forced_request_success = 0U;
    Mouse_state.forced_request_fail = 0U;
    Mouse_state.last_forced_request_tick = 0U;
    Mouse_state.last_buttons = 0U;
    Mouse_state.last_buttons_valid = false;
    Mouse_state.ready = false;
    Mouse_unlock_irqrestore(flags);

    if (!Mouse_init_controller())
        return;

    flags = Mouse_lock_irqsave();
    Mouse_state.ready = true;
    Mouse_unlock_irqrestore(flags);

    ISR_register_IRQ(IRQ12, Mouse_callback);
}

bool Mouse_get_event(syscall_mouse_event_t* out_event)
{
    if (!out_event)
        return false;

    uint64_t flags = Mouse_lock_irqsave();
    if (Mouse_state.event_count == 0U)
    {
        (void) Mouse_poll_aux_bytes_locked(MOUSE_POLL_DRAIN_BUDGET);
        if (Mouse_state.event_count == 0U && Mouse_state.ready)
        {
            uint64_t now_tick = ISR_get_timer_ticks();
            if (now_tick - Mouse_state.last_forced_request_tick >= MOUSE_FORCED_REQUEST_MIN_TICKS)
            {
                Mouse_state.last_forced_request_tick = now_tick;
                (void) Mouse_force_read_data_locked();
            }
        }
        if (Mouse_state.event_count == 0U)
        {
            Mouse_state.get_event_empty++;
            Mouse_unlock_irqrestore(flags);
            return false;
        }
    }

    *out_event = Mouse_state.events[Mouse_state.read_pos];
    Mouse_state.read_pos++;
    if (Mouse_state.read_pos >= MOUSE_EVENT_BUFFER_SIZE)
        Mouse_state.read_pos = 0U;
    Mouse_state.event_count--;
    Mouse_state.events_popped++;
    Mouse_unlock_irqrestore(flags);
    return true;
}

bool Mouse_is_ready(void)
{
    uint64_t flags = Mouse_lock_irqsave();
    bool ready = Mouse_state.ready;
    Mouse_unlock_irqrestore(flags);
    return ready;
}

static bool Mouse_handle_controller_byte_locked(uint8_t status, uint8_t byte, uint8_t source)
{
    if (source == MOUSE_BYTE_SOURCE_IRQ12)
    {
        Mouse_state.irq12_bytes_total++;
        if ((status & PS2_STATUS_AUX_DATA) != 0U)
            Mouse_state.irq12_aux_bytes++;
        else
            Mouse_state.irq12_non_aux_bytes++;
    }
    else if (source == MOUSE_BYTE_SOURCE_IRQ1 &&
             (status & PS2_STATUS_AUX_DATA) != 0U)
    {
        Mouse_state.irq1_aux_bytes++;
    }
    else if (source == MOUSE_BYTE_SOURCE_POLL)
    {
        Mouse_state.poll_bytes_total++;
        if ((status & PS2_STATUS_AUX_DATA) != 0U)
            Mouse_state.poll_aux_bytes++;
        else
            Mouse_state.poll_non_aux_bytes++;
    }

    if ((status & PS2_STATUS_AUX_DATA) == 0U)
        return false;

    if (!Mouse_state.ready)
        return true;

    if (Mouse_state.packet_index == 0U && (byte & 0x08U) == 0U)
    {
        Mouse_state.packet_sync_drops++;
        return true;
    }

    Mouse_state.packet[Mouse_state.packet_index++] = byte;
    if (Mouse_state.packet_index < 3U)
        return true;

    Mouse_state.packet_index = 0U;
    uint8_t b0 = Mouse_state.packet[0];
    uint8_t b1 = Mouse_state.packet[1];
    uint8_t b2 = Mouse_state.packet[2];
    if ((b0 & 0xC0U) != 0U)
    {
        Mouse_state.packet_overflow_drops++;
        return true;
    }

    syscall_mouse_event_t event;
    event.dx = (int16_t) ((int8_t) b1);
    event.dy = (int16_t) ((int8_t) b2);
    event.buttons = (uint8_t) (b0 & 0x07U);
    event.reserved0 = 0U;
    event.reserved1 = 0U;
    event.reserved2 = 0U;
    if (event.dx == 0 &&
        event.dy == 0 &&
        Mouse_state.last_buttons_valid &&
        event.buttons == Mouse_state.last_buttons)
    {
        return true;
    }

    Mouse_state.last_buttons = event.buttons;
    Mouse_state.last_buttons_valid = true;
    Mouse_push_event_locked(&event);
    return true;
}

bool Mouse_handle_controller_byte(uint8_t status, uint8_t byte, uint8_t source)
{
    Mouse_lock_noirq();
    bool handled = Mouse_handle_controller_byte_locked(status, byte, source);
    Mouse_unlock_noirq();
    return handled;
}

bool Mouse_get_debug_info(syscall_mouse_debug_info_t* out_info)
{
    if (!out_info)
        return false;

    uint64_t flags = Mouse_lock_irqsave();
    out_info->irq12_callbacks = Mouse_state.irq12_callbacks;
    out_info->irq12_bytes_total = Mouse_state.irq12_bytes_total;
    out_info->irq12_aux_bytes = Mouse_state.irq12_aux_bytes;
    out_info->irq12_non_aux_bytes = Mouse_state.irq12_non_aux_bytes;
    out_info->irq12_drain_budget_hits = Mouse_state.irq12_drain_budget_hits;
    out_info->irq1_aux_bytes = Mouse_state.irq1_aux_bytes;
    out_info->poll_cycles = Mouse_state.poll_cycles;
    out_info->poll_bytes_total = Mouse_state.poll_bytes_total;
    out_info->poll_aux_bytes = Mouse_state.poll_aux_bytes;
    out_info->poll_non_aux_bytes = Mouse_state.poll_non_aux_bytes;
    out_info->events_pushed = Mouse_state.events_pushed;
    out_info->events_dropped_full = Mouse_state.events_dropped_full;
    out_info->events_popped = Mouse_state.events_popped;
    out_info->get_event_empty = Mouse_state.get_event_empty;
    out_info->packet_sync_drops = Mouse_state.packet_sync_drops;
    out_info->packet_overflow_drops = Mouse_state.packet_overflow_drops;
    out_info->queue_count = Mouse_state.event_count;
    out_info->queue_write_pos = Mouse_state.write_pos;
    out_info->queue_read_pos = Mouse_state.read_pos;
    out_info->packet_index = Mouse_state.packet_index;
    out_info->ready = Mouse_state.ready ? 1U : 0U;
    for (uint32_t i = 0U; i < sizeof(out_info->reserved); i++)
        out_info->reserved[i] = 0U;
    out_info->forced_request_attempts = Mouse_state.forced_request_attempts;
    out_info->forced_request_success = Mouse_state.forced_request_success;
    out_info->forced_request_fail = Mouse_state.forced_request_fail;
    Mouse_unlock_irqrestore(flags);
    return true;
}

static void Mouse_callback(interrupt_frame_t* frame)
{
    (void) frame;

    Mouse_lock_noirq();
    Mouse_state.irq12_callbacks++;

    /* Drain the controller output buffer every time: if OUT_FULL is set we must
     * read port 0x60 or the i8042 will not raise further IRQ12. A keyboard byte
     * (aux bit clear) on a shared controller path used to leave the buffer stuck
     * and look like "one nudge then mouse dead". */
    uint32_t drained = 0U;
    for (; drained < MOUSE_IRQ_DRAIN_BUDGET; drained++)
    {
        uint8_t status = IO_inb(PS2_PORT_STATUS);
        if ((status & PS2_STATUS_OUT_FULL) == 0U)
            break;

        uint8_t byte = IO_inb(PS2_PORT_DATA);
        (void) Mouse_handle_controller_byte_locked(status, byte, MOUSE_BYTE_SOURCE_IRQ12);
    }
    if (drained >= MOUSE_IRQ_DRAIN_BUDGET)
        Mouse_state.irq12_drain_budget_hits++;
    Mouse_unlock_noirq();
}
