#include <Debug/KDebug.h>

#ifdef THEOS_ENABLE_KDEBUG
#include <Device/COM.h>
#include <Debug/Spinlock.h>
#if defined(THEOS_KDEBUG_LOG_FILE) && (THEOS_KDEBUG_LOG_FILE)
#include <FileSystem/ext4.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>

#ifndef THEOS_KDEBUG_LOG_SERIAL
#define THEOS_KDEBUG_LOG_SERIAL 1
#endif

#ifndef THEOS_KDEBUG_LOG_FILE
#define THEOS_KDEBUG_LOG_FILE 0
#endif

#define KDEBUG_FILE_RAM_BUFFER_SIZE (1024U * 1024U)
#define KDEBUG_FILE_NAME_MAX 64U
#define KDEBUG_FILE_CHUNK_STACK_MAX 4096U

static bool kdebug_initialized = false;
static bool kdebug_serial_ready = false;
static spinlock_t kdebug_lock = { 0 };

#if THEOS_KDEBUG_LOG_FILE
static char kdebug_file_ram[KDEBUG_FILE_RAM_BUFFER_SIZE];
static size_t kdebug_file_len = 0;
static size_t kdebug_file_dropped = 0;
static bool kdebug_file_fs_ready = false;
#endif

static void kdebug_itoa(uint64_t value, char* buf, size_t length, unsigned int base, bool uppercase);

static inline void kdebug_sink_putc(char c)
{
#if THEOS_KDEBUG_LOG_SERIAL
    if (kdebug_serial_ready)
        COM_putc(KDEBUG_COM_PORT, c);
#endif

#if THEOS_KDEBUG_LOG_FILE
    if (kdebug_file_len < KDEBUG_FILE_RAM_BUFFER_SIZE)
        kdebug_file_ram[kdebug_file_len++] = c;
    else
        kdebug_file_dropped++;
#endif
}

static inline void kdebug_putc_raw(char c)
{
    kdebug_sink_putc(c);
}

static inline void kdebug_puts_raw(const char* str)
{
    if (!str)
        return;

    while (*str)
    {
        kdebug_sink_putc(*str);
        str++;
    }
}

static void kdebug_move_bytes(char* dest, const char* src, size_t len)
{
    if (!dest || !src || len == 0 || dest == src)
        return;

    if (dest < src)
    {
        for (size_t i = 0; i < len; i++)
            dest[i] = src[i];
        return;
    }

    while (len != 0)
    {
        len--;
        dest[len] = src[len];
    }
}

void kdebug_init(void)
{
    spinlock_init(&kdebug_lock);
    kdebug_initialized = true;

#if THEOS_KDEBUG_LOG_SERIAL
    kdebug_serial_ready = COM_init(KDEBUG_COM_PORT);
#endif

#if THEOS_KDEBUG_LOG_SERIAL
    if (kdebug_serial_ready)
        kdebug_puts("[KDEBUG] Serial debug initialized\n");
#endif
#if THEOS_KDEBUG_LOG_FILE
    kdebug_puts("[KDEBUG] RAM file buffer initialized\n");
#endif
}

void kdebug_putc(char c)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_putc_raw(c);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}

void kdebug_puts(const char* str)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_puts_raw(str);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}

static void kdebug_itoa(uint64_t value, char* buf, size_t length, unsigned int base, bool uppercase)
{
    if (base < 2 || base > 36)
        return;

    uint64_t v = value;
    char digits[length];
    size_t index = 0;
    
    if (value == 0)
        digits[index++] = '0';

    while (v && index < (length - 1))
    {
        uint64_t digit = v % base;
        if (digit <= 9)
            digits[index++] = '0' + digit;
        else
            digits[index++] = (uppercase ? 'A' : 'a') + (digit - 10);
        v /= base;
    }

    int i = index;
    index = 0;
    for (i--; i >= 0; i--)
        buf[index++] = digits[i];

    buf[index] = '\0';
}

#if THEOS_KDEBUG_LOG_FILE
static bool kdebug_make_file_chunk_name(uint32_t chunk_index, char* out, size_t out_size)
{
    if (!out || out_size < sizeof("kdebug.log"))
        return false;

    if (chunk_index == 0)
    {
        const char* base = "kdebug.log";
        size_t base_len = strlen(base);
        if (base_len + 1 > out_size)
            return false;
        memcpy(out, base, base_len + 1);
        return true;
    }

    const char* prefix = "kdebug.log.";
    size_t prefix_len = strlen(prefix);
    if (prefix_len + 2 > out_size)
        return false;

    memcpy(out, prefix, prefix_len);

    char digits[12];
    kdebug_itoa(chunk_index, digits, sizeof(digits), 10, false);
    size_t digits_len = strlen(digits);
    if (prefix_len + digits_len + 1 > out_size)
        return false;

    memcpy(out + prefix_len, digits, digits_len);
    out[prefix_len + digits_len] = '\0';
    return true;
}

void kdebug_file_sink_ready(void)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_file_fs_ready = true;
    spin_unlock_irqrestore(&kdebug_lock, flags);

    kdebug_file_flush();
}

void kdebug_file_flush(void)
{
    if (!kdebug_initialized)
        return;

    ext4_fs_t* fs = ext4_get_active();
    if (!fs)
        return;

    size_t snapshot_len = 0;
    size_t snapshot_dropped = 0;
    bool can_flush = false;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    can_flush = kdebug_file_fs_ready;
    snapshot_len = kdebug_file_len;
    snapshot_dropped = kdebug_file_dropped;
    spin_unlock_irqrestore(&kdebug_lock, flags);

    if (!can_flush)
        return;
    if (snapshot_len == 0 && snapshot_dropped == 0)
        return;

    size_t chunk_size = fs->block_size;
    if (chunk_size == 0)
        return;
    if (chunk_size > KDEBUG_FILE_CHUNK_STACK_MAX)
        chunk_size = KDEBUG_FILE_CHUNK_STACK_MAX;

    bool success = true;
    bool drop_note_written = false;
    uint32_t chunk_count_written = 0;

    if (snapshot_dropped != 0)
    {
        char drop_msg[128];
        char dropped_digits[32];
        kdebug_itoa(snapshot_dropped, dropped_digits, sizeof(dropped_digits), 10, false);

        const char* p0 = "[KDEBUG] dropped ";
        const char* p1 = " bytes (RAM log buffer full)\n";
        size_t p0_len = strlen(p0);
        size_t p1_len = strlen(p1);
        size_t d_len = strlen(dropped_digits);
        size_t total = p0_len + d_len + p1_len;
        if (total >= sizeof(drop_msg))
            total = sizeof(drop_msg) - 1;

        size_t cursor = 0;
        for (size_t i = 0; i < p0_len && cursor < total; i++)
            drop_msg[cursor++] = p0[i];
        for (size_t i = 0; i < d_len && cursor < total; i++)
            drop_msg[cursor++] = dropped_digits[i];
        for (size_t i = 0; i < p1_len && cursor < total; i++)
            drop_msg[cursor++] = p1[i];
        drop_msg[cursor] = '\0';

        success = ext4_create_file(fs, "kdebug.log.overflow", (const uint8_t*) drop_msg, cursor);
        drop_note_written = success;
    }

    uint8_t chunk[KDEBUG_FILE_CHUNK_STACK_MAX];
    size_t offset = 0;
    uint32_t chunk_index = 0;

    while (success && offset < snapshot_len)
    {
        size_t part = snapshot_len - offset;
        if (part > chunk_size)
            part = chunk_size;

        flags = spin_lock_irqsave(&kdebug_lock);
        memcpy(chunk, &kdebug_file_ram[offset], part);
        spin_unlock_irqrestore(&kdebug_lock, flags);

        char file_name[KDEBUG_FILE_NAME_MAX];
        if (!kdebug_make_file_chunk_name(chunk_index, file_name, sizeof(file_name)))
        {
            success = false;
            break;
        }

        if (!ext4_create_file(fs, file_name, chunk, part))
        {
            success = false;
            break;
        }

        offset += part;
        chunk_index++;
        chunk_count_written++;
    }

    if (!success)
    {
#if THEOS_KDEBUG_LOG_SERIAL
        if (kdebug_serial_ready)
            COM_puts(KDEBUG_COM_PORT, "[KDEBUG] file flush failed\n");
#endif
        return;
    }

    flags = spin_lock_irqsave(&kdebug_lock);
    if (kdebug_file_len >= snapshot_len)
    {
        size_t remaining = kdebug_file_len - snapshot_len;
        if (remaining != 0)
            kdebug_move_bytes(kdebug_file_ram, kdebug_file_ram + snapshot_len, remaining);
        kdebug_file_len = remaining;
    }
    else
    {
        kdebug_file_len = 0;
    }

    if (kdebug_file_dropped >= snapshot_dropped)
        kdebug_file_dropped -= snapshot_dropped;
    else
        kdebug_file_dropped = 0;

    spin_unlock_irqrestore(&kdebug_lock, flags);

#if THEOS_KDEBUG_LOG_SERIAL
    if (kdebug_serial_ready)
    {
        char bytes_buf[32];
        char chunks_buf[16];
        kdebug_itoa(snapshot_len, bytes_buf, sizeof(bytes_buf), 10, false);
        kdebug_itoa(chunk_count_written, chunks_buf, sizeof(chunks_buf), 10, false);

        COM_puts(KDEBUG_COM_PORT, "[KDEBUG] file flush ok bytes=");
        COM_puts(KDEBUG_COM_PORT, bytes_buf);
        COM_puts(KDEBUG_COM_PORT, " chunks=");
        COM_puts(KDEBUG_COM_PORT, chunks_buf);
        if (drop_note_written)
            COM_puts(KDEBUG_COM_PORT, " overflow_note=1");
        COM_putc(KDEBUG_COM_PORT, '\n');
    }
#endif
}
#else
void kdebug_file_sink_ready(void)
{
}

void kdebug_file_flush(void)
{
}
#endif

static void kdebug_hex_raw(uint64_t value, int width)
{
    char buf[17];
    kdebug_itoa(value, buf, sizeof(buf), 16, true);
    
    int len = strlen(buf);
    if (width > len)
    {
        for (int i = 0; i < width - len; i++)
            kdebug_putc_raw('0');
    }
    kdebug_puts_raw(buf);
}

static void kdebug_dec_raw(uint64_t value)
{
    char buf[21];
    kdebug_itoa(value, buf, sizeof(buf), 10, false);
    kdebug_puts_raw(buf);
}

void kdebug_hex(uint64_t value, int width)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_hex_raw(value, width);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}

void kdebug_dec(uint64_t value)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    kdebug_dec_raw(value);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}

void kdebug_printf(const char* format, ...)
{
    if (!kdebug_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&kdebug_lock);
    va_list parameters;
    va_start(parameters, format);

    while (*format != '\0')
    {
        if (*format == '%')
        {
            format++;
            
            if (*format == 's')
            {
                format++;
                const char* str = va_arg(parameters, const char*);
                kdebug_puts_raw(str);
            }
            else if (*format == 'c')
            {
                format++;
                char c = (char) va_arg(parameters, int);
                kdebug_putc_raw(c);
            }
            else if (*format == 'd' || *format == 'i')
            {
                format++;
                int v = va_arg(parameters, int);
                kdebug_dec_raw(v);
            }
            else if (*format == 'u')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_dec_raw(v);
            }
            else if (*format == 'x')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_hex_raw(v, 0);
            }
            else if (*format == 'X')
            {
                format++;
                unsigned int v = va_arg(parameters, unsigned int);
                kdebug_hex_raw(v, 0);
            }
            else if (*format == 'l')
            {
                format++;
                if (*format == 'l')
                {
                    format++;
                    if (*format == 'X')
                    {
                        format++;
                        unsigned long long v = va_arg(parameters, unsigned long long);
                        kdebug_hex_raw(v, 0);
                    }
                    else if (*format == 'x')
                    {
                        format++;
                        unsigned long long v = va_arg(parameters, unsigned long long);
                        kdebug_hex_raw(v, 0);
                    }
                    else if (*format == 'u')
                    {
                        format++;
                        unsigned long long v = va_arg(parameters, unsigned long long);
                        kdebug_dec_raw(v);
                    }
                    else if (*format == 'd' || *format == 'i')
                    {
                        format++;
                        long long v = va_arg(parameters, long long);
                        if (v < 0)
                        {
                            kdebug_putc_raw('-');
                            v = -v;
                        }
                        kdebug_dec_raw(v);
                    }
                }
            }
            else if (*format == 'p')
            {
                format++;
                void* ptr = va_arg(parameters, void*);
                kdebug_puts_raw("0x");
                kdebug_hex_raw((uintptr_t) ptr, 16);
            }
            else if (*format == '%')
            {
                kdebug_putc_raw('%');
                format++;
            }
            else
            {
                kdebug_putc_raw('%');
                kdebug_putc_raw(*format);
                format++;
            }
        }
        else
        {
            kdebug_putc_raw(*format);
            format++;
        }
    }

    va_end(parameters);
    spin_unlock_irqrestore(&kdebug_lock, flags);
}
#else
void kdebug_init(void)
{
}

void kdebug_putc(char c)
{
    (void)c;
}

void kdebug_puts(const char* str)
{
    (void)str;
}

void kdebug_hex(uint64_t value, int width)
{
    (void)value;
    (void)width;
}

void kdebug_dec(uint64_t value)
{
    (void)value;
}

void kdebug_printf(const char* format, ...)
{
    (void)format;
}

void kdebug_file_sink_ready(void)
{
}

void kdebug_file_flush(void)
{
}
#endif
