#include <Device/Keyboard.h>
#include <Device/Keyboard_private.h>
#include <CPU/IO.h>

#include <stdio.h>

static Keyboard_runtime_state_t Keyboard_state;

static inline void Keyboard_lock(void)
{
    while (__atomic_test_and_set(&Keyboard_state.lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&Keyboard_state.lock, __ATOMIC_RELAXED))
            __asm__ __volatile__("pause");
    }
}

static inline void Keyboard_unlock(void)
{
    __atomic_clear(&Keyboard_state.lock, __ATOMIC_RELEASE);
}

void Keyboard_init(void)
{
    ISR_register_IRQ(IRQ1, Keyboard_callback);
}

void Keyboard_wait_ack(void)
{
    while (!(IO_inb(PORT_DATA) == PS2_ACK))
        __asm__("nop");
}

void Keyboard_update_leds(uint8_t status)
{
    IO_outb(PORT_DATA, KEYBOARD_LEDS);
    Keyboard_wait_ack();
    IO_outb(PORT_DATA, status);
}

uint8_t Keyboard_get_scancode(void)
{
    Keyboard_lock();
    if (Keyboard_state.scancode_buffer_length == 0)
    {
        Keyboard_unlock();
        return 0;
    }

    uint8_t sc = Keyboard_state.scancode_buffer[Keyboard_state.read_pos++];
    Keyboard_state.scancode_buffer_length--;

    if (Keyboard_state.read_pos == SCANCODE_BUFFER_SIZE)
        Keyboard_state.read_pos = 0;

    Keyboard_unlock();
    return sc;
}

bool Keyboard_is_uppercase(void)
{
    return Keyboard_state.is_shifting || Keyboard_state.is_caplocked;
}

static void Keyboard_callback(interrupt_frame_t* frame)
{
    (void) frame;
    uint8_t status = IO_inb(PORT_STATUS);
    if (status & 0x01)
    {
        uint8_t scancode = IO_inb(PORT_DATA);
        bool refresh_leds = false;
        uint8_t leds = 0;

        Keyboard_lock();
        switch (scancode)
        {
            case 0x2A:
            case 0x36:
                Keyboard_state.is_shifting = TRUE;
                break;
            case 0xAA:
            case 0xB6:
                Keyboard_state.is_shifting = FALSE;
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
            leds = (uint8_t) ((Keyboard_state.is_vernum << 1) | (Keyboard_state.is_caplocked << 2));

        // Keep raw scancode stream available to userland (Shift/Caps included),
        // so libc can apply layout + modifiers consistently.
        if (Keyboard_state.scancode_buffer_length == SCANCODE_BUFFER_SIZE) // The scancode buffer is full.
        {
            Keyboard_unlock();
            if (refresh_leds)
                Keyboard_update_leds(leds);
            return;
        }

        Keyboard_state.scancode_buffer[Keyboard_state.write_pos++] = scancode;
        Keyboard_state.scancode_buffer_length++;

        if (Keyboard_state.write_pos == SCANCODE_BUFFER_SIZE)
            Keyboard_state.write_pos = 0;
        Keyboard_unlock();
        if (refresh_leds)
            Keyboard_update_leds(leds);
    }
}
