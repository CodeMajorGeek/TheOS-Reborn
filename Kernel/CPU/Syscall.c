#include <CPU/Syscall.h>

#include <CPU/MSR.h>
#include <FileSystem/ext4.h>
#include <Memory/KMem.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>

void Syscall_init(void)
{
    MSR_set(IA32_LSTAR, (uint64_t) &syscall_handler_stub);
    MSR_set(IA32_FMASK, SYSCALL_FMASK_TF_BIT | SYSCALL_FMASK_DF_BIT);

    enable_syscall_ext();
}

uint64_t Syscall_interupt_handler(uint64_t syscall_num, syscall_frame_t* frame)
{
    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
        return (uint64_t) -1;

    switch (syscall_num)
    {
        case SYS_FS_LS:
            return ext4_list_root(fs) ? 0 : (uint64_t) -1;

        case SYS_FS_READ:
        {
            const char* name = (const char*) frame->rdi;
            uint8_t* user_buf = (uint8_t*) frame->rsi;
            size_t buf_size = (size_t) frame->rdx;
            size_t* out_size = (size_t*) frame->r10;

            uint8_t* data = NULL;
            size_t size = 0;
            if (!ext4_read_file(fs, name, &data, &size))
                return (uint64_t) -1;
            if (size > buf_size)
            {
                kfree(data);
                return (uint64_t) -1;
            }
            memcpy(user_buf, data, size);
            if (out_size)
                *out_size = size;
            kfree(data);
            return 0;
        }

        case SYS_FS_CREATE:
        {
            const char* name = (const char*) frame->rdi;
            const uint8_t* data = (const uint8_t*) frame->rsi;
            size_t size = (size_t) frame->rdx;
            return ext4_create_file(fs, name, data, size) ? 0 : (uint64_t) -1;
        }

        default:
            return (uint64_t) -1;
    }
}
