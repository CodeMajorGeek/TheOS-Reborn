#ifndef _VMM_LAYOUT_H
#define _VMM_LAYOUT_H

/*
 * Shared virtual memory layout used by both C and assembly code.
 * Keep these constants single-sourced to avoid BSP/AP divergence.
 */
#ifdef ASM_FILE
#define VMM_HHDM_BASE              0xFFFF800000000000
#define VMM_KERNEL_VIRT_BASE       0xFFFFFFFF80000000
#define VMM_MMIO_BASE              0xFFFFC00000000000
#define VMM_USER_SPACE_MAX         0x00007FFFFFFFFFFF
#define VMM_KERNEL_SPACE_MIN       0xFFFF800000000000
#else
#define VMM_HHDM_BASE              0xFFFF800000000000ULL
#define VMM_KERNEL_VIRT_BASE       0xFFFFFFFF80000000ULL
#define VMM_MMIO_BASE              0xFFFFC00000000000ULL
#define VMM_USER_SPACE_MAX         0x00007FFFFFFFFFFFULL
#define VMM_KERNEL_SPACE_MIN       0xFFFF800000000000ULL
#endif

#define VMM_HHDM_PML4_INDEX        256U
#define VMM_KERNEL_PML4_INDEX      511U
#define VMM_KERNEL_PDPT_INDEX      510U

#endif
