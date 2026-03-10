#include <Boot/Limine.h>

.section .text
.code64

.globl limine_entry
.type limine_entry, @function

.extern boot_limine_entry

limine_entry:
    jmp boot_limine_entry

.size limine_entry, . - limine_entry

.section .limine_requests_start_marker, "aw"
.align 8

.globl limine_requests_start_marker
limine_requests_start_marker:
    .quad LIMINE_REQUESTS_START_MARKER_0
    .quad LIMINE_REQUESTS_START_MARKER_1
    .quad LIMINE_REQUESTS_START_MARKER_2
    .quad LIMINE_REQUESTS_START_MARKER_3

.section .limine_requests, "aw"
.align 8

.globl limine_base_revision
limine_base_revision:
    .quad LIMINE_BASE_REVISION_0
    .quad LIMINE_BASE_REVISION_1
    .quad 3

.globl limine_entry_point_request
limine_entry_point_request:
    .quad LIMINE_COMMON_MAGIC_0
    .quad LIMINE_COMMON_MAGIC_1
    .quad LIMINE_ENTRY_POINT_REQUEST_ID_0
    .quad LIMINE_ENTRY_POINT_REQUEST_ID_1
    .quad 0
    .quad 0
    .quad limine_entry

.globl limine_executable_address_request
limine_executable_address_request:
    .quad LIMINE_COMMON_MAGIC_0
    .quad LIMINE_COMMON_MAGIC_1
    .quad LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID_0
    .quad LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID_1
    .quad 0
    .quad 0

.globl limine_cmdline_request
limine_cmdline_request:
    .quad LIMINE_COMMON_MAGIC_0
    .quad LIMINE_COMMON_MAGIC_1
    .quad LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID_0
    .quad LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID_1
    .quad 0
    .quad 0

.globl limine_executable_file_request
limine_executable_file_request:
    .quad LIMINE_COMMON_MAGIC_0
    .quad LIMINE_COMMON_MAGIC_1
    .quad LIMINE_EXECUTABLE_FILE_REQUEST_ID_0
    .quad LIMINE_EXECUTABLE_FILE_REQUEST_ID_1
    .quad 0
    .quad 0

.globl limine_memmap_request
limine_memmap_request:
    .quad LIMINE_COMMON_MAGIC_0
    .quad LIMINE_COMMON_MAGIC_1
    .quad LIMINE_MEMMAP_REQUEST_ID_0
    .quad LIMINE_MEMMAP_REQUEST_ID_1
    .quad 0
    .quad 0

.globl limine_hhdm_request
limine_hhdm_request:
    .quad LIMINE_COMMON_MAGIC_0
    .quad LIMINE_COMMON_MAGIC_1
    .quad LIMINE_HHDM_REQUEST_ID_0
    .quad LIMINE_HHDM_REQUEST_ID_1
    .quad 0
    .quad 0

.globl limine_framebuffer_request
limine_framebuffer_request:
    .quad LIMINE_COMMON_MAGIC_0
    .quad LIMINE_COMMON_MAGIC_1
    .quad LIMINE_FRAMEBUFFER_REQUEST_ID_0
    .quad LIMINE_FRAMEBUFFER_REQUEST_ID_1
    .quad 0
    .quad 0

.globl limine_rsdp_request
limine_rsdp_request:
    .quad LIMINE_COMMON_MAGIC_0
    .quad LIMINE_COMMON_MAGIC_1
    .quad LIMINE_RSDP_REQUEST_ID_0
    .quad LIMINE_RSDP_REQUEST_ID_1
    .quad 0
    .quad 0

.section .limine_requests_end_marker, "aw"
.align 8

.globl limine_requests_end_marker
limine_requests_end_marker:
    .quad LIMINE_REQUESTS_END_MARKER_0
    .quad LIMINE_REQUESTS_END_MARKER_1
