#ifndef _COM_H
#define _COM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define COM1                    0x3F8
#define COM2                    0x2F8
#define COM3                    0x3E8
#define COM4                    0x2E8

#define COM_DATA_OFFSET         0
#define COM_INTERRUPT_OFFSET    1
#define COM_FIFO_OFFSET         2
#define COM_CONTROL_OFFSET      3
#define COM_MODEM_CTRL_OFFSET   4
#define COM_MODEM_STATUS_OFFSET 5

#define COM_STATUS_EMPTY_BIT    0x20

#define TEST_LOOPBACK_BYTE      0xAE

bool COM_init(uint16_t port);

bool COM_transmit_empty(uint16_t port);
void COM_putc(uint16_t port, char c);
void COM_write(uint16_t port, const char* str, size_t len);
void COM_puts(uint16_t port, const char* str);

uint8_t COM_read(uint16_t port);

#endif