#include <CPU/Syscall.h>

#include <CPU/MSR.h>

#include <stdint.h>
#include <stdio.h>

void Syscall_init(void)
{
    MSR_set(IA32_LSTAR, (uint64_t) &syscall_handler_stub);

    uint64_t fmask = ~0x202; // All the bit that should be cleared into the RFLAG reg before returning into usermode.
    MSR_set(IA32_FMASK, fmask);

    enable_syscall_ext();
}

void Syscall_interupt_handler(interrupt_frame_t* frame)
{
    printf("Syscall interupt handled !\n");
}

void syscall_handler(void)
{
    printf("Syscall handled !\n");
}