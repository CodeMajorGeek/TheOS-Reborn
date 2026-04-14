#include <Device/Keyboard.h>
#include <Device/Keyboard_private.h>
#include <Device/Mouse.h>
#include <CPU/IO.h>
#include <Debug/KDebug.h>
#include <Debug/Spinlock.h>
#include <stddef.h>

#define KEYBOARD_IRQ_DRAIN_BUDGET 64U

static Keyboard_runtime_state_t Keyboard_state;
static uint64_t Keyboard_dbg_enqueued = 0ULL;
static uint64_t Keyboard_dbg_dequeued = 0ULL;
static uint64_t Keyboard_dbg_empty_reads = 0ULL;
static uint64_t Keyboard_dbg_dropped = 0ULL;

static inline uint64_t Keyboard_lock_irqsave(void)
{
    return spin_lock_irqsave(&Keyboard_state.lock);
}

static inline void Keyboard_unlock_irqrestore(uint64_t flags)
{
    spin_unlock_irqrestore(&Keyboard_state.lock, flags);
}

static inline void Keyboard_lock_noirq(void)
{
    spin_lock(&Keyboard_state.lock);
}

static inline void Keyboard_unlock_noirq(void)
{
    spin_unlock(&Keyboard_state.lock);
}

static uint8_t Keyboard_led_mask_locked(void)
{
    return (uint8_t) ((Keyboard_state.is_vernum << 1) | (Keyboard_state.is_caplocked << 2));
}

static void Keyboard_process_scancode_locked(uint8_t scancode,
                                             bool* out_refresh_leds,
                                             uint8_t* out_leds)
{
    bool refresh_leds = false;
    uint8_t leds = 0U;

    switch (scancode)
    {
        case 0x2A:
        case 0x36:
            Keyboard_state.is_shifting = true;
            break;
        case 0xAA:
        case 0xB6:
            Keyboard_state.is_shifting = false;
            break;
        case 0x3A:
            Keyboard_state.is_caplocked = !Keyboard_state.is_caplocked;
            refresh_leds = true;
            break;
        case 0x45:
            Keyboard_state.is_vernum = !Keyboard_state.is_vernum;
            refresh_leds = true;
            break;
        default:
            break;
    }

    if (refresh_leds)
        leds = Keyboard_led_mask_locked();

    // Keep raw scancode stream available to userland (Shift/Caps included),
    // so libc can apply layout + modifiers consistently.
    if (Keyboard_state.scancode_buffer_length < SCANCODE_BUFFER_SIZE)
    {
        Keyboard_state.scancode_buffer[Keyboard_state.write_pos++] = scancode;
        Keyboard_state.scancode_buffer_length++;
        Keyboard_dbg_enqueued++;

        if (Keyboard_state.write_pos == SCANCODE_BUFFER_SIZE)
            Keyboard_state.write_pos = 0;
    }
    else
    {
        Keyboard_dbg_dropped++;
        if ((Keyboard_dbg_dropped & 0x3FULL) == 1ULL)
        {
            // #region agent log
            kdebug_printf("[AGENTDBG H41 KBD_OVERFLOW] dropped=%llu enq=%llu deq=%llu empty=%llu cap=%u\n",
                          (unsigned long long) Keyboard_dbg_dropped,
                          (unsigned long long) Keyboard_dbg_enqueued,
                          (unsigned long long) Keyboard_dbg_dequeued,
                          (unsigned long long) Keyboard_dbg_empty_reads,
                          (unsigned int) SCANCODE_BUFFER_SIZE);
            // #endregion
        }
    }

    if (out_refresh_leds)
        *out_refresh_leds = refresh_leds;
    if (out_leds)
        *out_leds = leds;
}

void Keyboard_init(void)
{
    spinlock_init(&Keyboard_state.lock);
    ISR_register_IRQ(IRQ1, Keyboard_callback);
}

void Keyboard_wait_ack(void)
{
    for (;;)
    {
        uint8_t status = IO_inb(PORT_STATUS);
        if ((status & 0x01U) == 0U)
        {
            __asm__ __volatile__("pause");
            continue;
        }

        uint8_t data = IO_inb(PORT_DATA);
        if (Mouse_handle_controller_byte(status, data, MOUSE_BYTE_SOURCE_IRQ1))
            continue;
        if (data == PS2_ACK)
            return;
    }
}

void Keyboard_update_leds(uint8_t status)
{
    IO_outb(PORT_DATA, KEYBOARD_LEDS);
    Keyboard_wait_ack();
    IO_outb(PORT_DATA, status);
}

uint8_t Keyboard_get_scancode(void)
{
    uint64_t flags = Keyboard_lock_irqsave();
    if (Keyboard_state.scancode_buffer_length == 0)
    {
        Keyboard_dbg_empty_reads++;
        Keyboard_unlock_irqrestore(flags);
        return 0;
    }

    uint8_t sc = Keyboard_state.scancode_buffer[Keyboard_state.read_pos++];
    Keyboard_state.scancode_buffer_length--;
    Keyboard_dbg_dequeued++;

    if (Keyboard_state.read_pos == SCANCODE_BUFFER_SIZE)
        Keyboard_state.read_pos = 0;

    Keyboard_unlock_irqrestore(flags);
    return sc;
}

bool Keyboard_is_uppercase(void)
{
    uint64_t flags = Keyboard_lock_irqsave();
    bool uppercase = Keyboard_state.is_shifting || Keyboard_state.is_caplocked;
    Keyboard_unlock_irqrestore(flags);
    return uppercase;
}

void Keyboard_enqueue_scancode_from_poll(uint8_t scancode)
{
    Keyboard_lock_noirq();
    Keyboard_process_scancode_locked(scancode, NULL, NULL);
    Keyboard_unlock_noirq();
}

static void Keyboard_callback(interrupt_frame_t* frame)
{
    (void) frame;
    bool refresh_leds = false;
    uint8_t leds = 0U;

    for (uint32_t drain_iter = 0U; drain_iter < KEYBOARD_IRQ_DRAIN_BUDGET; drain_iter++)
    {
        uint8_t status = IO_inb(PORT_STATUS);
        if ((status & 0x01U) == 0U)
            break;

        uint8_t scancode = IO_inb(PORT_DATA);
        if (Mouse_handle_controller_byte(status, scancode, MOUSE_BYTE_SOURCE_IRQ1))
            continue;

        Keyboard_lock_noirq();
        bool sc_refresh_leds = false;
        uint8_t sc_leds = 0U;
        Keyboard_process_scancode_locked(scancode, &sc_refresh_leds, &sc_leds);
        Keyboard_unlock_noirq();

        if (sc_refresh_leds)
        {
            refresh_leds = true;
            leds = sc_leds;
        }
    }

    if (refresh_leds)
        Keyboard_update_leds(leds);
}
