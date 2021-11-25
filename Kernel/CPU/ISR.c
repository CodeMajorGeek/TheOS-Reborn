#include <CPU/ISR.h>

void ISR_exception_handler(interrupt_frame_t* frame)
{
    TTY_puts("Exception handle no: ");
    TTY_putc(frame->err_code + '0');
    TTY_putc('\n');

    abort();
}