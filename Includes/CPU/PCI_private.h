#ifndef _PCI_PRIVATE_H
#define _PCI_PRIVATE_H

#include <CPU/PCI.h>

static void PCI_try_attach(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
static void PCI_attach_storage_dev(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
static void PCI_attach_network_dev(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
static void PCI_attach_audio_dev(uint8_t bus, uint8_t slot, uint8_t function, uint16_t vendor, uint16_t device);
static bool PCI_read_bar_address(uint8_t bus, uint8_t slot, uint8_t function, uint8_t bar_index, uintptr_t* phys_out, bool* is_io_out, bool* is_64_out);
static uint16_t PCI_set_intx_disable(uint8_t bus, uint8_t slot, uint8_t function, bool disable);
static void PCI_log_append_line(const char* line);

#endif
