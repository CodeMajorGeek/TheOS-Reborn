#include <Debug/logger.h>

void logger_init(void)
{
#ifdef __USE_QEMU
    if (!COM_init(COM_PORT))
        abort();
#endif
}

void kputs(int level, const char* str)
{
#ifdef __USE_QEMU
    COM_puts(COM_PORT, level_messages[level]);
    COM_puts(COM_PORT, str);
    COM_putc(COM_PORT, '\n');
#else
    TTY_puts(level_messages[level]);
    TTY_puts(str);
    TTY_putc('\n');
#endif
}