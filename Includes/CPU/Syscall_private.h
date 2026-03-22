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
static bool Syscall_is_audio_dsp_path(const char* path);
static bool Syscall_is_net_raw_path(const char* path);
static uint16_t Syscall_read_be16(const uint8_t* bytes);
static uint32_t Syscall_read_be32(const uint8_t* bytes);
static void Syscall_write_be16(uint8_t* bytes, uint16_t value);
static void Syscall_write_be32(uint8_t* bytes, uint32_t value);
static bool Syscall_copy_in_sockaddr_in(const void* user_addr, size_t user_len, uint32_t* out_addr_be, uint16_t* out_port);
static bool Syscall_copy_out_sockaddr_in(void* user_addr, void* user_addrlen_ptr, uint32_t addr_be, uint16_t port);
static uint64_t Syscall_handle_socket(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_bind(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_connect(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_listen(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_accept(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_sendto(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_recvfrom(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_getsockname(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_getpeername(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_audio_ioctl(unsigned long request, void* user_arg);
static uint64_t Syscall_handle_net_raw_ioctl(uint32_t owner_pid, int64_t fd, unsigned long request, void* user_arg);
static uint64_t Syscall_handle_udp_socket_ioctl(uint32_t owner_pid, int64_t fd, unsigned long request, void* user_arg);
static uint64_t Syscall_handle_tcp_socket_ioctl(uint32_t owner_pid, int64_t fd, unsigned long request, void* user_arg);
static uint64_t Syscall_handle_open(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_close(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_read(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_write(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_lseek(uint32_t cpu_index, const syscall_frame_t* frame);
static uint64_t Syscall_handle_ioctl(uint32_t cpu_index, const syscall_frame_t* frame);

#endif
