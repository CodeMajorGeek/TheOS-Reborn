#include <CPU/MADT.h>

void MADT_detect_cores(uint8_t* rsdt)
{
    uint8_t* ptr = rsdt + 36; 
    uint32_t len = *((uint32_t*) (rsdt + 4));

    for (;ptr < rsdt + len; ptr += rsdt[0] == 'X' ? 8 : 4) // Iterate on ACPI table pointers.
    {
        uint8_t* ptr2 = (uint8_t*) (uintptr_t) (rsdt[0] == 'X' ? *((uint64_t*) ptr) : *((uint32_t*) ptr));
        if (!memcmp(ptr, "APIC", 4)) // We have found the MADT.
        {
            MADT_LAPIC_ptr = (uintptr_t) *((uint32_t*) (ptr2 + 0x24));
            ptr = ptr2 + *((uint32_t*) (ptr2 + 4));

            for (ptr2 += 44; ptr2 < ptr; ptr2 += ptr2[1])   // Iterate on variable length records.
            {
                switch (ptr2[0])
                {
                    case 0:                                 // Found Processor Local APIC.
                        if (ptr2[4] & 1)
                            MADT_LAPIC_ids[MADT_num_core++] = ptr2[3];
                        break;
                    case 1:                                 // Found IOAPIC.
                        MADT_IOAPIC_ptr = (uintptr_t) *((uint32_t*) (ptr2 + 4));
                        break;
                    case 5:                                 // Found 64 bit LAPIC.
                        MADT_LAPIC_ptr = *((uint64_t*) (ptr2 + 4));
                        break;
                }
            }

            break;
        }
    }
}