#include <Multiboot2/multiboot2_info.h>

struct multiboot_tag_new_acpi* new_acpi_tag = { 0 };;

void Multiboot2_info_read(const void* info_ptr)
{
    for (struct multiboot_tag* tag = (struct multiboot_tag*) ((uint8_t*) info_ptr + 8);
       tag->type != MULTIBOOT_TAG_TYPE_END;
       tag = (struct multiboot_tag*) ((multiboot_uint8_t*) tag + ((tag->size + 7) & ~7)))
    {
        switch (tag->type)
        {
            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
                break;
            case MULTIBOOT_TAG_TYPE_ACPI_NEW:
                new_acpi_tag = (struct multiboot_tag_new_acpi*) tag;
                break;
            case MULTIBOOT_TAG_TYPE_ACPI_OLD:
                break;
            default:
                break;
        };
    }
}