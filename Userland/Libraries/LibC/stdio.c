#include <stdio.h>

#if defined(__THEOS_KERNEL)
#include <Device/TTY.h>
#if defined(__THEOS_KERNEL) && defined(THEOS_ENABLE_KDEBUG)
#include <Debug/KDebug.h>
#endif
#else
#include <syscall.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

/* We are building stdio as kernel (not for long, we will use syscall later on). */

int putc(int c)
{
#if defined(__THEOS_KERNEL)
    TTY_putc(c);
    return (char) c;
#else
    char ch = (char) c;
    if (sys_console_write(&ch, 1) < 0)
        return EOF;
    return (unsigned char) ch;
#endif
}

int puts(const char *s)
{
#if defined(__THEOS_KERNEL)
    TTY_puts(s);
    return 1;
#else
    if (!s)
        return EOF;

    size_t len = strlen(s);
    if (len == 0)
        return 1;

    return sys_console_write(s, len) < 0 ? EOF : 1;
#endif
}

#if !defined(__THEOS_KERNEL)
static bool stdio_input_shift = false;
static bool stdio_input_capslock = false;
static bool stdio_input_altgr = false;
#define STDIO_KEYMAP_SIZE         128U
#define STDIO_KBD_CONF_DEFAULT    "/system/keyboard.conf"
#define STDIO_KBD_PATH_MAX        256U
#define STDIO_KBD_FILE_MAX        8192U

static char stdio_keymap_base[STDIO_KEYMAP_SIZE];
static char stdio_keymap_shift[STDIO_KEYMAP_SIZE];
static char stdio_keymap_altgr[STDIO_KEYMAP_SIZE];
static char stdio_keymap_altgr_shift[STDIO_KEYMAP_SIZE];
static bool stdio_keyboard_ready = false;

static inline bool stdio_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char* stdio_ltrim(char* str)
{
    while (str && *str && stdio_is_space(*str))
        str++;

    return str;
}

static void stdio_rtrim(char* str)
{
    if (!str)
        return;

    size_t len = strlen(str);
    while (len > 0 && stdio_is_space(str[len - 1]))
    {
        str[len - 1] = '\0';
        len--;
    }
}

static char* stdio_next_token(char** cursor)
{
    if (!cursor || !*cursor)
        return NULL;

    char* token = stdio_ltrim(*cursor);
    if (!token || *token == '\0')
    {
        *cursor = token;
        return NULL;
    }

    char* end = token;
    while (*end && !stdio_is_space(*end))
        end++;

    if (*end)
    {
        *end = '\0';
        end++;
    }

    *cursor = end;
    return token;
}

static bool stdio_parse_uint(const char* text, uint32_t* out_value)
{
    if (!text || !out_value || *text == '\0')
        return false;

    uint32_t value = 0;
    uint32_t base = 10;
    size_t index = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        index = 2;
        if (text[index] == '\0')
            return false;
    }

    while (text[index] != '\0')
    {
        char c = text[index];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9')
            digit = (uint32_t) (c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = 10U + (uint32_t) (c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = 10U + (uint32_t) (c - 'A');
        else
            return false;

        if (digit >= base)
            return false;

        value = (value * base) + digit;
        index++;
    }

    *out_value = value;
    return true;
}

static bool stdio_copy_cstr(char* dst, size_t dst_size, const char* src)
{
    if (!dst || dst_size == 0 || !src)
        return false;

    size_t len = strlen(src);
    if (len + 1U > dst_size)
        return false;

    memcpy(dst, src, len + 1U);
    return true;
}

static bool stdio_decode_single_utf8_codepoint(const char* token, uint32_t* out_cp)
{
    if (!token || !out_cp || token[0] == '\0')
        return false;

    const uint8_t* bytes = (const uint8_t*) token;
    if (bytes[0] < 0x80U)
    {
        if (bytes[1] != '\0')
            return false;
        *out_cp = bytes[0];
        return true;
    }

    if ((bytes[0] & 0xE0U) == 0xC0U)
    {
        if ((bytes[1] & 0xC0U) != 0x80U || bytes[2] != '\0')
            return false;

        uint32_t cp = ((uint32_t) (bytes[0] & 0x1FU) << 6) |
                      (uint32_t) (bytes[1] & 0x3FU);
        if (cp < 0x80U)
            return false;

        *out_cp = cp;
        return true;
    }

    return false;
}

static bool stdio_decode_char_token(const char* token, char* out_char)
{
    if (!token || !out_char || token[0] == '\0')
        return false;

    if (strcmp(token, "NONE") == 0)
    {
        *out_char = 0;
        return true;
    }
    if (strcmp(token, "SPACE") == 0)
    {
        *out_char = ' ';
        return true;
    }
    if (strcmp(token, "TAB") == 0)
    {
        *out_char = '\t';
        return true;
    }
    if (strcmp(token, "ENTER") == 0)
    {
        *out_char = '\n';
        return true;
    }
    if (strcmp(token, "BACKSPACE") == 0)
    {
        *out_char = '\b';
        return true;
    }
    if (token[0] == '\\' && token[1] != '\0' && token[2] == '\0')
    {
        switch (token[1])
        {
            case 'n':
                *out_char = '\n';
                return true;
            case 't':
                *out_char = '\t';
                return true;
            case 'b':
                *out_char = '\b';
                return true;
            case 's':
                *out_char = ' ';
                return true;
            default:
                *out_char = token[1];
                return true;
        }
    }

    uint32_t cp = 0;
    if (stdio_decode_single_utf8_codepoint(token, &cp) && cp <= 255U)
    {
        *out_char = (char) cp;
        return true;
    }

    uint32_t code = 0;
    if (stdio_parse_uint(token, &code) && code <= 255U)
    {
        *out_char = (char) code;
        return true;
    }

    return false;
}

static void stdio_keyboard_set_default_qwerty(void)
{
    memset(stdio_keymap_base, 0, sizeof(stdio_keymap_base));
    memset(stdio_keymap_shift, 0, sizeof(stdio_keymap_shift));
    memset(stdio_keymap_altgr, 0, sizeof(stdio_keymap_altgr));
    memset(stdio_keymap_altgr_shift, 0, sizeof(stdio_keymap_altgr_shift));

    stdio_keymap_base[0x02] = '1'; stdio_keymap_shift[0x02] = '!';
    stdio_keymap_base[0x03] = '2'; stdio_keymap_shift[0x03] = '@';
    stdio_keymap_base[0x04] = '3'; stdio_keymap_shift[0x04] = '#';
    stdio_keymap_base[0x05] = '4'; stdio_keymap_shift[0x05] = '$';
    stdio_keymap_base[0x06] = '5'; stdio_keymap_shift[0x06] = '%';
    stdio_keymap_base[0x07] = '6'; stdio_keymap_shift[0x07] = '^';
    stdio_keymap_base[0x08] = '7'; stdio_keymap_shift[0x08] = '&';
    stdio_keymap_base[0x09] = '8'; stdio_keymap_shift[0x09] = '*';
    stdio_keymap_base[0x0A] = '9'; stdio_keymap_shift[0x0A] = '(';
    stdio_keymap_base[0x0B] = '0'; stdio_keymap_shift[0x0B] = ')';
    stdio_keymap_base[0x0C] = '-'; stdio_keymap_shift[0x0C] = '_';
    stdio_keymap_base[0x0D] = '='; stdio_keymap_shift[0x0D] = '+';
    stdio_keymap_base[0x0E] = '\b'; stdio_keymap_shift[0x0E] = '\b';
    stdio_keymap_base[0x0F] = '\t'; stdio_keymap_shift[0x0F] = '\t';

    stdio_keymap_base[0x10] = 'q'; stdio_keymap_shift[0x10] = 'Q';
    stdio_keymap_base[0x11] = 'w'; stdio_keymap_shift[0x11] = 'W';
    stdio_keymap_base[0x12] = 'e'; stdio_keymap_shift[0x12] = 'E';
    stdio_keymap_base[0x13] = 'r'; stdio_keymap_shift[0x13] = 'R';
    stdio_keymap_base[0x14] = 't'; stdio_keymap_shift[0x14] = 'T';
    stdio_keymap_base[0x15] = 'y'; stdio_keymap_shift[0x15] = 'Y';
    stdio_keymap_base[0x16] = 'u'; stdio_keymap_shift[0x16] = 'U';
    stdio_keymap_base[0x17] = 'i'; stdio_keymap_shift[0x17] = 'I';
    stdio_keymap_base[0x18] = 'o'; stdio_keymap_shift[0x18] = 'O';
    stdio_keymap_base[0x19] = 'p'; stdio_keymap_shift[0x19] = 'P';
    stdio_keymap_base[0x1A] = '['; stdio_keymap_shift[0x1A] = '{';
    stdio_keymap_base[0x1B] = ']'; stdio_keymap_shift[0x1B] = '}';
    stdio_keymap_base[0x1C] = '\n'; stdio_keymap_shift[0x1C] = '\n';

    stdio_keymap_base[0x1E] = 'a'; stdio_keymap_shift[0x1E] = 'A';
    stdio_keymap_base[0x1F] = 's'; stdio_keymap_shift[0x1F] = 'S';
    stdio_keymap_base[0x20] = 'd'; stdio_keymap_shift[0x20] = 'D';
    stdio_keymap_base[0x21] = 'f'; stdio_keymap_shift[0x21] = 'F';
    stdio_keymap_base[0x22] = 'g'; stdio_keymap_shift[0x22] = 'G';
    stdio_keymap_base[0x23] = 'h'; stdio_keymap_shift[0x23] = 'H';
    stdio_keymap_base[0x24] = 'j'; stdio_keymap_shift[0x24] = 'J';
    stdio_keymap_base[0x25] = 'k'; stdio_keymap_shift[0x25] = 'K';
    stdio_keymap_base[0x26] = 'l'; stdio_keymap_shift[0x26] = 'L';
    stdio_keymap_base[0x27] = ';'; stdio_keymap_shift[0x27] = ':';
    stdio_keymap_base[0x28] = '\''; stdio_keymap_shift[0x28] = '"';
    stdio_keymap_base[0x29] = '`'; stdio_keymap_shift[0x29] = '~';
    stdio_keymap_base[0x2B] = '\\'; stdio_keymap_shift[0x2B] = '|';

    stdio_keymap_base[0x2C] = 'z'; stdio_keymap_shift[0x2C] = 'Z';
    stdio_keymap_base[0x2D] = 'x'; stdio_keymap_shift[0x2D] = 'X';
    stdio_keymap_base[0x2E] = 'c'; stdio_keymap_shift[0x2E] = 'C';
    stdio_keymap_base[0x2F] = 'v'; stdio_keymap_shift[0x2F] = 'V';
    stdio_keymap_base[0x30] = 'b'; stdio_keymap_shift[0x30] = 'B';
    stdio_keymap_base[0x31] = 'n'; stdio_keymap_shift[0x31] = 'N';
    stdio_keymap_base[0x32] = 'm'; stdio_keymap_shift[0x32] = 'M';
    stdio_keymap_base[0x33] = ','; stdio_keymap_shift[0x33] = '<';
    stdio_keymap_base[0x34] = '.'; stdio_keymap_shift[0x34] = '>';
    stdio_keymap_base[0x35] = '/'; stdio_keymap_shift[0x35] = '?';
    stdio_keymap_base[0x39] = ' '; stdio_keymap_shift[0x39] = ' ';
}

static bool stdio_keyboard_parse_layout(char* content)
{
    if (!content)
        return false;

    bool applied = false;
    char* cursor = content;
    while (*cursor != '\0')
    {
        char* line = cursor;
        while (*cursor != '\0' && *cursor != '\n')
            cursor++;
        if (*cursor == '\n')
        {
            *cursor = '\0';
            cursor++;
        }

        char* comment = line;
        while (*comment != '\0')
        {
            if (*comment == '#')
            {
                *comment = '\0';
                break;
            }
            comment++;
        }

        stdio_rtrim(line);
        line = stdio_ltrim(line);
        if (!line || *line == '\0')
            continue;

        char* parse = line;
        char* mode = stdio_next_token(&parse);
        char* scancode_txt = stdio_next_token(&parse);
        char* value_txt = stdio_next_token(&parse);
        if (!mode || !scancode_txt || !value_txt)
            continue;

        uint32_t scancode = 0;
        char mapped = 0;
        if (!stdio_parse_uint(scancode_txt, &scancode) ||
            scancode >= STDIO_KEYMAP_SIZE ||
            !stdio_decode_char_token(value_txt, &mapped))
            continue;

        if (strcmp(mode, "normal") == 0)
            stdio_keymap_base[scancode] = mapped;
        else if (strcmp(mode, "shift") == 0)
            stdio_keymap_shift[scancode] = mapped;
        else if (strcmp(mode, "altgr") == 0)
            stdio_keymap_altgr[scancode] = mapped;
        else if (strcmp(mode, "altgr_shift") == 0 ||
                 strcmp(mode, "shift_altgr") == 0 ||
                 strcmp(mode, "altgrshift") == 0)
            stdio_keymap_altgr_shift[scancode] = mapped;
        else
            continue;

        applied = true;
    }

    return applied;
}

static bool stdio_keyboard_extract_layout_path(char* conf,
                                               char* out_path,
                                               size_t out_path_size)
{
    if (!conf || !out_path || out_path_size < 2U)
        return false;

    char* cursor = conf;
    while (*cursor != '\0')
    {
        char* line = cursor;
        while (*cursor != '\0' && *cursor != '\n')
            cursor++;
        if (*cursor == '\n')
        {
            *cursor = '\0';
            cursor++;
        }

        char* comment = line;
        while (*comment != '\0')
        {
            if (*comment == '#')
            {
                *comment = '\0';
                break;
            }
            comment++;
        }

        stdio_rtrim(line);
        line = stdio_ltrim(line);
        if (!line || *line == '\0')
            continue;

        if (strncmp(line, "layout=", 7U) == 0)
        {
            char* path = stdio_ltrim(line + 7U);
            stdio_rtrim(path);
            return stdio_copy_cstr(out_path, out_path_size, path);
        }

        if (strncmp(line, "layout", 6U) == 0 && stdio_is_space(line[6]))
        {
            char* parse = line + 6U;
            char* path = stdio_next_token(&parse);
            if (path)
                return stdio_copy_cstr(out_path, out_path_size, path);
        }

        if (line[0] == '/')
            return stdio_copy_cstr(out_path, out_path_size, line);
    }

    return false;
}

static bool stdio_keyboard_load_layout_file(const char* layout_path)
{
    if (!layout_path || layout_path[0] == '\0')
        return false;

    static uint8_t layout_buf[STDIO_KBD_FILE_MAX];
    size_t layout_size = 0;
    if (fs_read(layout_path, layout_buf, sizeof(layout_buf) - 1U, &layout_size) != 0)
        return false;

    layout_buf[layout_size] = '\0';
    return stdio_keyboard_parse_layout((char*) layout_buf);
}

static void stdio_keyboard_ensure_ready(void)
{
    if (!stdio_keyboard_ready)
        (void) keyboard_load_config(NULL);
}

static bool stdio_is_ascii_lower(char c)
{
    return c >= 'a' && c <= 'z';
}

static bool stdio_is_ascii_upper(char c)
{
    return c >= 'A' && c <= 'Z';
}

static char stdio_scancode_to_ascii(uint8_t scancode)
{
    if (scancode >= STDIO_KEYMAP_SIZE)
        return 0;

    char base = stdio_keymap_base[scancode];
    char shifted = stdio_keymap_shift[scancode];

    if (stdio_input_altgr)
    {
        char altgr = stdio_input_shift
                   ? stdio_keymap_altgr_shift[scancode]
                   : stdio_keymap_altgr[scancode];
        if (altgr == 0 && stdio_input_shift)
            altgr = stdio_keymap_altgr[scancode];
        if (altgr != 0)
            return altgr;
    }

    bool alpha_pair = stdio_is_ascii_lower(base) && stdio_is_ascii_upper(shifted);
    bool use_shift = stdio_input_shift;
    if (alpha_pair && stdio_input_capslock)
        use_shift = !use_shift;

    return use_shift ? (shifted ? shifted : base) : base;
}

static int stdio_read_key(void)
{
    stdio_keyboard_ensure_ready();
    bool extended_prefix = false;

    for (;;)
    {
        int code = sys_kbd_get_scancode();
        if (code <= 0)
        {
            (void) sys_sleep_ms(10);
            continue;
        }

        uint8_t scancode = (uint8_t) code;
        if (scancode == 0xE0U)
        {
            extended_prefix = true;
            continue;
        }

        if (extended_prefix)
        {
            if (scancode == 0x38U)
            {
                stdio_input_altgr = true;
                extended_prefix = false;
                continue;
            }
            if (scancode == 0xB8U)
            {
                stdio_input_altgr = false;
                extended_prefix = false;
                continue;
            }
        }

        switch (scancode)
        {
            case 0x2A:
            case 0x36:
                stdio_input_shift = true;
                extended_prefix = false;
                continue;
            case 0xAA:
            case 0xB6:
                stdio_input_shift = false;
                extended_prefix = false;
                continue;
            case 0x3A:
                stdio_input_capslock = !stdio_input_capslock;
                extended_prefix = false;
                continue;
            default:
                break;
        }

        extended_prefix = false;
        if ((scancode & 0x80U) != 0)
            continue;

        char c = stdio_scancode_to_ascii(scancode);
        if (c == 0)
            continue;

        return (unsigned char) c;
    }
}
#endif

int keyboard_load_config(const char* config_path)
{
#if defined(__THEOS_KERNEL)
    (void) config_path;
    return -1;
#else
    stdio_keyboard_set_default_qwerty();
    stdio_input_shift = false;
    stdio_input_capslock = false;
    stdio_input_altgr = false;

    const char* conf_path = config_path;
    if (!conf_path || conf_path[0] == '\0')
        conf_path = STDIO_KBD_CONF_DEFAULT;

    static uint8_t conf_buf[STDIO_KBD_FILE_MAX];
    size_t conf_size = 0;
    if (fs_read(conf_path, conf_buf, sizeof(conf_buf) - 1U, &conf_size) != 0)
    {
        stdio_keyboard_ready = true;
        return -1;
    }
    conf_buf[conf_size] = '\0';

    char layout_path[STDIO_KBD_PATH_MAX];
    if (!stdio_keyboard_extract_layout_path((char*) conf_buf,
                                            layout_path,
                                            sizeof(layout_path)) ||
        !stdio_keyboard_load_layout_file(layout_path))
    {
        stdio_keyboard_ready = true;
        return -1;
    }

    stdio_keyboard_ready = true;
    return 0;
#endif
}

int getchar(void)
{
#if defined(__THEOS_KERNEL)
    return EOF;
#else
    return stdio_read_key();
#endif
}

char* fgets(char* str, int size)
{
    if (!str || size <= 1)
        return NULL;

#if defined(__THEOS_KERNEL)
    str[0] = '\0';
    return NULL;
#else
    int length = 0;

    for (;;)
    {
        int ch = getchar();
        if (ch == EOF)
        {
            if (length == 0)
            {
                str[0] = '\0';
                return NULL;
            }
            break;
        }

        char c = (char) ch;
        if (c == '\r')
            continue;

        if (c == '\b')
        {
            if (length > 0)
            {
                length--;
                putc('\b');
                putc(' ');
                putc('\b');
            }
            continue;
        }

        if (c == '\n')
        {
            if (length < size - 1)
            {
                str[length++] = '\n';
                putc('\n');
            }
            break;
        }

        if (length < size - 1)
        {
            str[length++] = c;
            putc(c);
        }
    }

    str[length] = '\0';
    return str;
#endif
}

static bool printf_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int printf_append_char(char* buff, size_t write_limit, int* written, char c)
{
    if (!buff || !written)
        return EOF;
    if ((size_t) *written >= write_limit)
        return EOF;

    buff[*written] = c;
    (*written)++;
    return 0;
}

static int printf_append_repeat(char* buff, size_t write_limit, int* written, char c, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        if (printf_append_char(buff, write_limit, written, c) == EOF)
            return EOF;
    }

    return 0;
}

static int printf_append_string(char* buff,
                                size_t write_limit,
                                int* written,
                                const char* str,
                                size_t len,
                                bool uppercase)
{
    if (!str)
        str = "(null)";

    for (size_t i = 0; i < len; i++)
    {
        char c = str[i];
        if (uppercase && c >= 'a' && c <= 'z')
            c = (char) (c - ('a' - 'A'));
        if (printf_append_char(buff, write_limit, written, c) == EOF)
            return EOF;
    }

    return 0;
}

int __printf(char* buff, size_t buff_len, const char* __restrict format, va_list parameters)
{
    if (!buff || !format)
        return EOF;

    int written = 0;
    size_t write_limit = buff_len ? (buff_len - 1) : ((size_t) INT_MAX - 1);

    while (*format)
    {
        if (*format != '%')
        {
            if (printf_append_char(buff, write_limit, &written, *format++) == EOF)
                return EOF;
            continue;
        }

        if (format[1] == '%')
        {
            if (printf_append_char(buff, write_limit, &written, '%') == EOF)
                return EOF;
            format += 2;
            continue;
        }

        const char* format_begun_at = format;
        format++; // Skip '%'

        bool zero_pad = false;
        size_t width = 0;
        int precision = -1;
        bool is_long_long = false;

        if (*format == '0')
        {
            zero_pad = true;
            format++;
        }

        while (printf_is_digit(*format))
        {
            width = (width * 10U) + (size_t) (*format - '0');
            format++;
        }

        if (*format == '.')
        {
            precision = 0;
            format++;
            while (printf_is_digit(*format))
            {
                precision = (precision * 10) + (int) (*format - '0');
                format++;
            }
        }

        if (*format == 'l' && format[1] == 'l')
        {
            is_long_long = true;
            format += 2;
        }

        char spec = *format;
        if (!spec)
        {
            format = format_begun_at;
            size_t len = strlen(format);
            if (printf_append_string(buff, write_limit, &written, format, len, false) == EOF)
                return EOF;
            break;
        }
        format++;

        if (spec == 'c' || spec == 'C')
        {
            bool uppercase = spec == 'C';
            char c = (char) va_arg(parameters, int);
            size_t pad = (width > 1U) ? (width - 1U) : 0U;

            if (printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
            if (uppercase && c >= 'a' && c <= 'z')
                c = (char) (c - ('a' - 'A'));
            if (printf_append_char(buff, write_limit, &written, c) == EOF)
                return EOF;
        }
        else if (spec == 's' || spec == 'S')
        {
            bool uppercase = spec == 'S';
            const char* str = va_arg(parameters, const char*);
            if (!str)
                str = "(null)";

            size_t len = strlen(str);
            if (precision >= 0 && len > (size_t) precision)
                len = (size_t) precision;
            size_t pad = (width > len) ? (width - len) : 0U;

            if (printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
            if (printf_append_string(buff, write_limit, &written, str, len, uppercase) == EOF)
                return EOF;
        }
        else if (spec == 'd')
        {
            long long value = is_long_long ? va_arg(parameters, long long) : (long long) va_arg(parameters, int);
            bool negative = value < 0;
            unsigned long long magnitude;
            if (negative)
                magnitude = (unsigned long long) (-(value + 1LL)) + 1ULL;
            else
                magnitude = (unsigned long long) value;

            char digits[32];
            lltoa(magnitude, digits, sizeof(digits), DECIMAL);
            size_t len = strlen(digits);
            size_t prefix = negative ? 1U : 0U;
            size_t total = prefix + len;
            size_t pad = (width > total) ? (width - total) : 0U;

            if (!zero_pad && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;

            if (negative && printf_append_char(buff, write_limit, &written, '-') == EOF)
                return EOF;

            if (zero_pad && printf_append_repeat(buff, write_limit, &written, '0', pad) == EOF)
                return EOF;

            if (printf_append_string(buff, write_limit, &written, digits, len, false) == EOF)
                return EOF;
        }
        else if (spec == 'u')
        {
            unsigned long long value = is_long_long ? va_arg(parameters, unsigned long long) : (unsigned long long) va_arg(parameters, unsigned int);
            char digits[32];
            lltoa(value, digits, sizeof(digits), DECIMAL);
            size_t len = strlen(digits);
            size_t pad = (width > len) ? (width - len) : 0U;

            char pad_char = zero_pad ? '0' : ' ';
            if (printf_append_repeat(buff, write_limit, &written, pad_char, pad) == EOF)
                return EOF;
            if (printf_append_string(buff, write_limit, &written, digits, len, false) == EOF)
                return EOF;
        }
        else if (spec == 'x' || spec == 'X')
        {
            bool uppercase = spec == 'X';
            unsigned long long value = is_long_long ? va_arg(parameters, unsigned long long) : (unsigned long long) va_arg(parameters, unsigned int);
            char digits[32];
            lltoa(value, digits, sizeof(digits), HEXADECIMAL);
            size_t len = strlen(digits);
            size_t pad = (width > len) ? (width - len) : 0U;

            char pad_char = zero_pad ? '0' : ' ';
            if (printf_append_repeat(buff, write_limit, &written, pad_char, pad) == EOF)
                return EOF;
            if (printf_append_string(buff, write_limit, &written, digits, len, uppercase) == EOF)
                return EOF;
        }
        else if (spec == 'b' || spec == 'B')
        {
            bool uppercase = spec == 'B';
            int v = va_arg(parameters, bool);
            const char* str = v ? "true" : "false";
            size_t len = strlen(str);
            size_t pad = (width > len) ? (width - len) : 0U;

            if (printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
            if (printf_append_string(buff, write_limit, &written, str, len, uppercase) == EOF)
                return EOF;
        }
        else
        {
            format = format_begun_at;
            size_t len = strlen(format);
            if (printf_append_string(buff, write_limit, &written, format, len, false) == EOF)
                return EOF;
            format += len;
        }
    }

    if (buff_len == 0 || written < (int) buff_len)
        buff[written] = '\0';
    else if (buff_len > 0)
        buff[buff_len - 1] = '\0';

    return written;
}

int printf(const char* __restrict format, ...)
{
    int result = EOF;
    /* TODO: find an algorithm to determine the ideal buffer length. */
    size_t len = 255;

    char buf[len];
    memset(buf, '\0', len);

    va_list parameters;
    va_start(parameters, format);

    result = __printf(buf, len, format, parameters);

    size_t output_len = strlen(buf);
    for (size_t i = 0; i < output_len; i++)
        putc(buf[i]);

#if defined(__THEOS_KERNEL) && defined(THEOS_ENABLE_KDEBUG)
    for (size_t i = 0; i < output_len; i++)
        kdebug_putc(buf[i]);
#endif

    va_end(parameters);

    return result;
}

int sprintf(char* str, const char* __restrict format, ...)
{
    int result = EOF;

    va_list parameters;
    va_start(parameters, format);

    result = __printf(str, NULL, format, parameters);

    va_end(parameters);

    return result;
}

int snprintf(char* str, size_t size, const char* __restrict format, ...)
{
    int result = EOF;

    va_list parameters;
    va_start(parameters, format);

    result = __printf(str, size, format, parameters);

    va_end(parameters);

    return result;
}

char* itoa(int value, char* buf, size_t length, unsigned int base)
{
    if (base < 2 || base > 36)
        return buf;

    int v = value;
    char digits[length];

    size_t index = 0;
    if (value == 0)
        digits[index++] = '0';

    while (v && index < (length - 1))
    {
        if (base == 2)
        {
            digits[index++] = '0' + (v & 1);
            v >>= 1;
        }
        else
        {
            int digit = v % base;
            if (digit <= 9)
                digits[index++] = '0' + digit;
            else
                digits[index++] = 'a' + (digit - 10);
            v /= base;
        }
    }
    

    int i = index;
    index = 0;
    for (i--; i >= 0; i--)
        buf[index++] = digits[i]; // Ugly but work...

    buf[index] = '\0';
    return buf;
}

char* lltoa(unsigned long long value, char* buf, size_t length, unsigned int base)
{
    if (base < 2 || base > 36)
        return buf;

    unsigned long long v = value;
    char digits[length];

    size_t index = 0;
    if (value == 0)
        digits[index++] = '0';

    while (v && index < (length - 1))
    {
        if (base == 2)
        {
            digits[index++] = '0' + (v & 1);
            v >>= 1;
        }
        else
        {
            unsigned long long digit = v % base;
            if (digit <= 9)
                digits[index++] = '0' + digit;
            else
                digits[index++] = 'a' + (digit - 10);
            v /= base;
        }
    }
    

    int i = index;
    index = 0;
    for (i--; i >= 0; i--)
        buf[index++] = digits[i]; // Ugly but work...

    buf[index] = '\0';
    return buf;
}
