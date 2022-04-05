#include <Device/Keyboard.h>

#include <CPU/IO.h>

#include <stdio.h>

static uint8_t scancode_buffer[SCANCODE_BUFFER_SIZE];
static uint8_t scancode_buffer_length = 0;
static uint8_t write_pos = 0;
static uint8_t read_pos = 0;

static bool is_shifting = FALSE;
static bool is_caplocked = FALSE;
static bool is_vernum = FALSE;

void keyboard_init(void)
{
    ISR_register_IRQ(IRQ1, keyboard_callback);
}

void keyboard_wait_ack(void)
{
    while (!(IO_inb(PORT_DATA) == PS2_ACK))
        __asm__("nop");
}

void keyboard_update_leds(uint8_t status)
{
    IO_outb(PORT_DATA, KEYBOARD_LEDS);
    keyboard_wait_ack();
    IO_outb(PORT_DATA, status);
}

uint8_t keyboard_get_scancode(void)
{
    if (scancode_buffer_length == 0)
        return NULL;

    uint8_t sc = scancode_buffer[read_pos++];
    scancode_buffer_length--;

    if (read_pos == SCANCODE_BUFFER_SIZE)
        read_pos = 0;

    return sc;
}

bool keyboard_is_uppercase(void)
{
    return is_shifting || is_caplocked;
}

static void keyboard_callback(interrupt_frame_t* frame)
{
    uint8_t status = IO_inb(PORT_STATUS);
    if (status & 0x01)
    {
        uint8_t scancode = IO_inb(PORT_DATA);
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
                break;
            case 0x45:
                is_vernum = !is_vernum;
                break;
            default:
                goto no_changing_state; // Ugly but avoid updating LEDs when not needed...
        }
        keyboard_update_leds((is_vernum << 1) | (is_caplocked << 2));
        return;
no_changing_state:
        if (scancode_buffer_length == SCANCODE_BUFFER_SIZE) // The scancode buffer is full.
            return;

        scancode_buffer[write_pos++] = scancode;
        scancode_buffer_length++;

        if (write_pos == SCANCODE_BUFFER_SIZE)
            write_pos = 0;
    }
}