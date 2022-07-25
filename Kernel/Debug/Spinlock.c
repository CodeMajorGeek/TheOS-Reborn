#include <Debug/Spinlock.h>

#include <Memory/PMM.h>

void get_caller_pcs(void* v, uintptr_t pcs[])
{
    uintptr_t* ebp;
    __asm__ __volatile__("mov %%rbp, %0" : "=r" (ebp));
    get_stack_pcs(ebp, pcs);
}

void get_stack_pcs(uintptr_t* ebp, uintptr_t pcs[])
{
    for (int i = 0; i < 10; i++)
    {
        if (ebp == 0 || ebp < (uintptr_t*) PMM_get_kernel_start() || ebp == (uintptr_t*) 0xFFFFFFFF)
            break;

        pcs[i] = ebp[1];
        ebp = (uintptr_t*) ebp[0];
    }

    for (int i = 0; i < 10; i++)
        pcs[i] = 0;
}