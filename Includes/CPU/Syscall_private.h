#ifndef _SYSCALL_PRIVATE_H
#define _SYSCALL_PRIVATE_H

#include <CPU/Syscall.h>

static inline uintptr_t Syscall_read_cr3_phys(void);
static inline void Syscall_write_cr3_phys(uintptr_t cr3_phys);
static bool Syscall_cow_ref_add(uintptr_t phys, uint32_t delta);
static bool Syscall_cow_ref_sub(uintptr_t phys, bool* out_zero);
static uint32_t Syscall_cow_ref_get(uintptr_t phys);
static uint64_t* Syscall_get_user_pte_ptr(uintptr_t cr3_phys, uintptr_t virt);
static bool Syscall_resolve_cow_fault(uint32_t cpu_index, uintptr_t fault_addr, uint64_t err_code);
static bool Syscall_proc_owner_has_other_live_locked(uint32_t owner_pid, int32_t exclude_slot);
static uint32_t Syscall_proc_current_pid(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_open(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_close(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_read(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_write(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_lseek(uint32_t cpu_index, const syscall_frame_t* frame);

#endif
