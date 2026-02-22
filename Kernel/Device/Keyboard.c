#include <Device/Keyboard.h>
#include <CPU/IO.h>

#include <stdio.h>

static uint8_t scancode_buffer[SCANCODE_BUFFER_SIZE];
static uint8_t scancode_buffer_length = 0;
static uint8_t write_pos = 0;
static uint8_t read_pos = 0;
static volatile uint32_t keyboard_lock = 0;

static bool is_shifting = FALSE;
static bool is_caplocked = FALSE;
static bool is_vernum = FALSE;

static inline void Keyboard_lock(void)
{
    while (__atomic_test_and_set(&keyboard_lock, __ATOMIC_ACQUIRE))
    {
        while (__atomic_load_n(&keyboard_lock, __ATOMIC_RELAXED))
            __asm__ __volatile__("pause");
    }
}

static inline void Keyboard_unlock(void)
{
    __atomic_clear(&keyboard_lock, __ATOMIC_RELEASE);
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
    if (scancode_buffer_length == 0)
    {
        Keyboard_unlock();
        return 0;
    }

    uint8_t sc = scancode_buffer[read_pos++];
    scancode_buffer_length--;

    if (read_pos == SCANCODE_BUFFER_SIZE)
        read_pos = 0;

    Keyboard_unlock();
    return sc;
}

bool Keyboard_is_uppercase(void)
{
    return is_shifting || is_caplocked;
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
                is_shifting = TRUE;
                break;
            case 0xAA:
            case 0xB6:
                is_shifting = FALSE;
                break;
            case 0x3A:
                is_caplocked = !is_caplocked;
                refresh_leds = true;
                break;
            case 0x45:
                is_vernum = !is_vernum;
                refresh_leds = true;
                break;
            default:
                break;
        }

        if (refresh_leds)
            leds = (uint8_t) ((is_vernum << 1) | (is_caplocked << 2));

        // Keep raw scancode stream available to userland (Shift/Caps included),
        // so libc can apply layout + modifiers consistently.
        if (scancode_buffer_length == SCANCODE_BUFFER_SIZE) // The scancode buffer is full.
        {
            Keyboard_unlock();
            if (refresh_leds)
                Keyboard_update_leds(leds);
            return;
        }

        scancode_buffer[write_pos++] = scancode;
        scancode_buffer_length++;

        if (write_pos == SCANCODE_BUFFER_SIZE)
            write_pos = 0;
        Keyboard_unlock();
        if (refresh_leds)
            Keyboard_update_leds(leds);
    }
}
