#include <CPU/Syscall.h>

#include <CPU/MSR.h>

#include <stdio.h>

void Syscall_init(void)
{
    enable_syscall_ext();

    
}

void Syscall_interupt_handler(interrupt_frame_t* frame)
{
    printf("Syscall interupt handled !\n");
}