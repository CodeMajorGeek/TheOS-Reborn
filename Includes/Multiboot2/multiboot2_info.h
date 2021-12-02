#ifndef _MULTIBOOT2_INFO_H
#define _MULTIBOOT2_INFO_H

#include <CPU/ACPI.h>
#include <Multiboot2/multiboot2.h>

#include <stdint.h>

#include <Debug/logger.h>

void Multiboot2_info_read(const void* info_ptr);

#endif