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

void Keyboard_init(void);

void Keyboard_wait_ack(void);
void Keyboard_update_leds(uint8_t);

uint8_t Keyboard_get_scancode(void);
bool Keyboard_is_uppercase(void);

static void Keyboard_callback(interrupt_frame_t*);

#endif