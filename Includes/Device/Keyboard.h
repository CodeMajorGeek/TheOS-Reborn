#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <CPU/ISR.h>
#include <CPU/IO.h>

#include <stdbool.h>
#include <stdint.h>

#define PORT_STATUS             0x64
#define PORT_DATA               0x60

#define KEYBOARD_LEDS           0xED

#define PS2_ACK                 0xFA

#define SCANCODE_BUFFER_SIZE    8

void keyboard_init(void);

void keyboard_wait_ack(void);
void keyboard_update_leds(uint8_t);

uint8_t keyboard_get_scancode(void);
bool keyboard_is_uppercase(void);

static void keyboard_callback(interrupt_frame_t*);

#endif