#include <CPU/ISR.h>

void ISR_exception_handler(interrupt_frame_t frame)
{
    if (frame.int_no < MAX_KNOWN_EXCEPTIONS)
    {
        TTY_puts(exception_messages[frame.int_no]);
        TTY_puts(" Exception Handled !\n");
    }
    else
    {
        TTY_puts("Reserved Exception Handled !\n");
    }

    abort();
}