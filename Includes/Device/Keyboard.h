#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <CPU/ISR.h>

#include <stdbool.h>
#include <stdint.h>

#define PORT_STATUS             0x64
#define PORT_DATA               0x60

#define KEYBOARD_LEDS           0xED

#define PS2_ACK                 0xFA

#define SCANCODE_BUFFER_SIZE    8

typedef struct Keyboard_runtime_state
{
    uint8_t scancode_buffer[SCANCODE_BUFFER_SIZE];
    uint8_t scancode_buffer_length;
    uint8_t write_pos;
    uint8_t read_pos;
    volatile uint32_t lock;
    bool is_shifting;
    bool is_caplocked;
    bool is_vernum;
} Keyboard_runtime_state_t;

void Keyboard_init(void);

void Keyboard_wait_ack(void);
void Keyboard_update_leds(uint8_t);

uint8_t Keyboard_get_scancode(void);
bool Keyboard_is_uppercase(void);

#endif
