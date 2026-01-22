#include <CPU/Syscall.h>

#include <CPU/MSR.h>

#include <stdint.h>
#include <stdio.h>

void Syscall_init(void)
{
    MSR_set(IA32_LSTAR, (uint64_t) &syscall_handler_stub);
    MSR_set(IA32_FMASK, SYSCALL_FMASK_TF_BIT | SYSCALL_FMASK_DF_BIT);

    enable_syscall_ext();
}

void Syscall_interupt_handler(uint64_t syscall_num, interrupt_frame_t* frame)
{
    (void)syscall_num;
    (void)frame;
}
