#include <stdio.h>

#if defined(__THEOS_KERNEL)
#include <Device/TTY.h>
#if defined(__THEOS_KERNEL) && defined(THEOS_ENABLE_KDEBUG)
#include <Debug/KDebug.h>
#endif
#else
#include <syscall.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* We are building stdio as kernel (not for long, we will use syscall later on). */

static FILE stdio_stdin_stream = { .fd = STDIN_FILENO };
static FILE stdio_stdout_stream = { .fd = STDOUT_FILENO };
static FILE stdio_stderr_stream = { .fd = STDERR_FILENO };

FILE* stdin = &stdio_stdin_stream;
FILE* stdout = &stdio_stdout_stream;
FILE* stderr = &stdio_stderr_stream;

#if !defined(__THEOS_KERNEL)
static bool stdio_console_silent = false;
#endif

#if !defined(__THEOS_KERNEL)
typedef struct stdio_open_mode
{
    int flags;
    bool seek_end;
} stdio_open_mode_t;

static bool stdio_parse_open_mode(const char* mode, stdio_open_mode_t* out_mode)
{
    if (!mode || !out_mode || mode[0] == '\0')
    {
        errno = EINVAL;
        return false;
    }

    char access = mode[0];
    bool plus = false;
    for (size_t i = 1; mode[i] != '\0'; i++)
    {
        if (mode[i] == '+')
        {
            plus = true;
            continue;
        }
        if (mode[i] == 'b' || mode[i] == 't')
            continue;

        errno = EINVAL;
        return false;
    }

    out_mode->seek_end = false;
    switch (access)
    {
        case 'r':
            out_mode->flags = plus ? O_RDWR : O_RDONLY;
            return true;
        case 'w':
            out_mode->flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
            return true;
        case 'a':
            out_mode->flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT;
            out_mode->seek_end = true;
            return true;
        default:
            errno = EINVAL;
            return false;
    }
}

FILE* fopen(const char* path, const char* mode)
{
    stdio_open_mode_t open_mode;
    if (!path || !stdio_parse_open_mode(mode, &open_mode))
        return NULL;

    int fd = open(path, open_mode.flags, 0);
    if (fd < 0)
        return NULL;

    if (open_mode.seek_end && lseek(fd, 0, SEEK_END) < 0)
    {
        (void) close(fd);
        return NULL;
    }

    FILE* stream = (FILE*) malloc(sizeof(FILE));
    if (!stream)
    {
        (void) close(fd);
        errno = ENOMEM;
        return NULL;
    }

    stream->fd = fd;
    return stream;
}

int fclose(FILE* stream)
{
    if (!stream)
    {
        errno = EINVAL;
        return EOF;
    }

    int fd = stream->fd;
    if (fd < 0)
    {
        errno = EBADF;
        return EOF;
    }

    int rc = close(fd);
    stream->fd = -1;

    if (stream != stdin && stream != stdout && stream != stderr)
        free(stream);

    if (rc < 0)
        return EOF;

    return 0;
}

int fflush(FILE* stream)
{
    if (!stream)
        return 0;

    if (stream->fd < 0)
    {
        errno = EBADF;
        return EOF;
    }

    return 0;
}

int fileno(FILE* stream)
{
    if (!stream)
    {
        errno = EINVAL;
        return -1;
    }

    return stream->fd;
}

void setbuf(FILE* stream, char* buf)
{
    (void) stream;
    (void) buf;
}
#endif

int putc(int c)
{
#if defined(__THEOS_KERNEL)
    TTY_putc(c);
    return (char) c;
#else
    char ch = (char) c;
    if (stdio_console_silent)
        return (unsigned char) ch;
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
    if (stdio_console_silent)
        return 1;

    return sys_console_write(s, len) < 0 ? EOF : 1;
#endif
}

#if !defined(__THEOS_KERNEL)
static bool stdio_input_shift = false;
static bool stdio_input_capslock = false;
static bool stdio_input_altgr = false;
static bool stdio_input_ctrl = false;
static bool stdio_input_numlock = true;
static bool stdio_input_extended_prefix = false;
#define STDIO_KEYMAP_SIZE         128U
#define STDIO_KBD_CONF_DEFAULT    "/system/keyboard.conf"
#define STDIO_KBD_BASE_DEFAULT    "/system/qwerty.conf"
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

static bool stdio_read_file(const char* path, uint8_t* buf, size_t buf_size, size_t* out_size)
{
    if (!path || !buf || buf_size == 0U || !out_size)
        return false;

    int fd = sys_open(path, SYS_OPEN_READ);
    if (fd < 0)
        return false;

    size_t total = 0U;
    while (total < buf_size)
    {
        int rc = sys_read(fd, buf + total, buf_size - total);
        if (rc < 0)
        {
            (void) sys_close(fd);
            return false;
        }
        if (rc == 0)
            break;
        total += (size_t) rc;
    }

    (void) sys_close(fd);
    *out_size = total;
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

static void stdio_keyboard_reset_maps(void)
{
    memset(stdio_keymap_base, 0, sizeof(stdio_keymap_base));
    memset(stdio_keymap_shift, 0, sizeof(stdio_keymap_shift));
    memset(stdio_keymap_altgr, 0, sizeof(stdio_keymap_altgr));
    memset(stdio_keymap_altgr_shift, 0, sizeof(stdio_keymap_altgr_shift));
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
    if (!stdio_read_file(layout_path, layout_buf, sizeof(layout_buf) - 1U, &layout_size))
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

static char stdio_apply_ctrl_modifier(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char) ((c - 'a') + 1);
    if (c >= 'A' && c <= 'Z')
        return (char) ((c - 'A') + 1);
    return c;
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

    if (!stdio_input_numlock &&
        (scancode == 0x47U || scancode == 0x48U || scancode == 0x49U ||
         scancode == 0x4BU || scancode == 0x4CU || scancode == 0x4DU ||
         scancode == 0x4FU || scancode == 0x50U || scancode == 0x51U ||
         scancode == 0x52U || scancode == 0x53U))
    {
        return 0;
    }

    return use_shift ? (shifted ? shifted : base) : base;
}

int keyboard_decode_scancode(uint8_t raw_scancode, int* out_key)
{
    if (!out_key)
    {
        errno = EINVAL;
        return -1;
    }

    stdio_keyboard_ensure_ready();

    if (raw_scancode == 0xE0U)
    {
        stdio_input_extended_prefix = true;
        return 0;
    }

    if (stdio_input_extended_prefix)
    {
        stdio_input_extended_prefix = false;
        if (raw_scancode == 0x1DU)
        {
            stdio_input_ctrl = true;
            return 0;
        }
        if (raw_scancode == 0x9DU)
        {
            stdio_input_ctrl = false;
            return 0;
        }
        if (raw_scancode == 0x38U)
        {
            stdio_input_altgr = true;
            return 0;
        }
        if (raw_scancode == 0xB8U)
        {
            stdio_input_altgr = false;
            return 0;
        }
        if (raw_scancode == 0x48U)
        {
            *out_key = STDIO_KEY_UP;
            return 1;
        }
        if (raw_scancode == 0x50U)
        {
            *out_key = STDIO_KEY_DOWN;
            return 1;
        }
        if (raw_scancode == 0x4BU)
        {
            *out_key = STDIO_KEY_LEFT;
            return 1;
        }
        if (raw_scancode == 0x4DU)
        {
            *out_key = STDIO_KEY_RIGHT;
            return 1;
        }
        if (raw_scancode == 0x53U)
        {
            *out_key = STDIO_KEY_DELETE;
            return 1;
        }
    }

    switch (raw_scancode)
    {
        case 0x2A:
        case 0x36:
            stdio_input_shift = true;
            return 0;
        case 0xAA:
        case 0xB6:
            stdio_input_shift = false;
            return 0;
        case 0x1D:
            stdio_input_ctrl = true;
            return 0;
        case 0x9D:
            stdio_input_ctrl = false;
            return 0;
        case 0x3A:
            stdio_input_capslock = !stdio_input_capslock;
            return 0;
        case 0x45:
            stdio_input_numlock = !stdio_input_numlock;
            return 0;
        default:
            break;
    }

    if ((raw_scancode & 0x80U) != 0U)
        return 0;

    if (raw_scancode == 0x01U)
    {
        *out_key = 27;
        return 1;
    }

    char c = stdio_scancode_to_ascii(raw_scancode);
    if (c == 0)
        return 0;

    if (stdio_input_ctrl)
        c = stdio_apply_ctrl_modifier(c);

    *out_key = (unsigned char) c;
    return 1;
}

static int stdio_read_key(void)
{
    for (;;)
    {
        uint8_t pty_byte = 0U;
        int pty_rc = sys_console_route_input_read(&pty_byte, 1U);
        if (pty_rc == 1)
            return (int) pty_byte;

        int code = sys_kbd_get_scancode();
        if (code <= 0)
        {
            (void) sys_sleep_ms(1);
            continue;
        }

        int key = 0;
        int rc = keyboard_decode_scancode((uint8_t) code, &key);
        if (rc > 0)
            return key;
    }
}
#endif

int keyboard_load_config(const char* config_path)
{
#if defined(__THEOS_KERNEL)
    (void) config_path;
    return -1;
#else
    stdio_keyboard_reset_maps();
    stdio_input_shift = false;
    stdio_input_capslock = false;
    stdio_input_altgr = false;
    stdio_input_ctrl = false;
    stdio_input_numlock = true;
    stdio_input_extended_prefix = false;

    bool loaded_base = stdio_keyboard_load_layout_file(STDIO_KBD_BASE_DEFAULT);

    const char* conf_path = config_path;
    if (!conf_path || conf_path[0] == '\0')
        conf_path = STDIO_KBD_CONF_DEFAULT;

    static uint8_t conf_buf[STDIO_KBD_FILE_MAX];
    size_t conf_size = 0;
    if (!stdio_read_file(conf_path, conf_buf, sizeof(conf_buf) - 1U, &conf_size))
    {
        if (!loaded_base)
            (void) stdio_keyboard_load_layout_file("/system/azerty.conf");
        stdio_keyboard_ready = true;
        return loaded_base ? 0 : -1;
    }
    conf_buf[conf_size] = '\0';

    char layout_path[STDIO_KBD_PATH_MAX];
    bool loaded_layout = false;
    if (stdio_keyboard_extract_layout_path((char*) conf_buf, layout_path, sizeof(layout_path)))
        loaded_layout = stdio_keyboard_load_layout_file(layout_path);

    if (!loaded_layout && !loaded_base)
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
        bool left_align = false;
        size_t width = 0;
        int precision = -1;
        bool is_long = false;
        bool is_long_long = false;

        for (;;)
        {
            if (*format == '-')
            {
                left_align = true;
                format++;
                continue;
            }
            if (*format == '0')
            {
                zero_pad = true;
                format++;
                continue;
            }
            break;
        }
        if (left_align)
            zero_pad = false;

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
        else if (*format == 'l')
        {
            is_long = true;
            format++;
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

            if (!left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
            if (uppercase && c >= 'a' && c <= 'z')
                c = (char) (c - ('a' - 'A'));
            if (printf_append_char(buff, write_limit, &written, c) == EOF)
                return EOF;
            if (left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
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

            if (!left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
            if (printf_append_string(buff, write_limit, &written, str, len, uppercase) == EOF)
                return EOF;
            if (left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
        }
        else if (spec == 'd' || spec == 'i')
        {
            long long value = 0;
            if (is_long_long)
                value = va_arg(parameters, long long);
            else if (is_long)
                value = (long long) va_arg(parameters, long);
            else
                value = (long long) va_arg(parameters, int);
            bool negative = value < 0;
            unsigned long long magnitude;
            if (negative)
                magnitude = (unsigned long long) (-(value + 1LL)) + 1ULL;
            else
                magnitude = (unsigned long long) value;

            char digits[32];
            lltoa(magnitude, digits, sizeof(digits), DECIMAL);
            size_t len = strlen(digits);
            if (precision == 0 && magnitude == 0ULL)
                len = 0U;
            size_t digits_zero_pad = 0U;
            if (precision > 0 && (size_t) precision > len)
                digits_zero_pad = (size_t) precision - len;
            if (precision >= 0)
                zero_pad = false;
            if (left_align)
                zero_pad = false;
            size_t prefix = negative ? 1U : 0U;
            size_t total = prefix + digits_zero_pad + len;
            size_t pad = (width > total) ? (width - total) : 0U;

            if (!left_align && !zero_pad && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;

            if (negative && printf_append_char(buff, write_limit, &written, '-') == EOF)
                return EOF;

            if (!left_align && zero_pad && printf_append_repeat(buff, write_limit, &written, '0', pad) == EOF)
                return EOF;

            if (printf_append_repeat(buff, write_limit, &written, '0', digits_zero_pad) == EOF)
                return EOF;

            if (len > 0U && printf_append_string(buff, write_limit, &written, digits, len, false) == EOF)
                return EOF;

            if (left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
        }
        else if (spec == 'u')
        {
            unsigned long long value = 0;
            if (is_long_long)
                value = va_arg(parameters, unsigned long long);
            else if (is_long)
                value = (unsigned long long) va_arg(parameters, unsigned long);
            else
                value = (unsigned long long) va_arg(parameters, unsigned int);
            char digits[32];
            lltoa(value, digits, sizeof(digits), DECIMAL);
            size_t len = strlen(digits);
            if (precision == 0 && value == 0ULL)
                len = 0U;
            size_t digits_zero_pad = 0U;
            if (precision > 0 && (size_t) precision > len)
                digits_zero_pad = (size_t) precision - len;
            if (precision >= 0)
                zero_pad = false;
            if (left_align)
                zero_pad = false;
            size_t total = digits_zero_pad + len;
            size_t pad = (width > total) ? (width - total) : 0U;

            if (!left_align && printf_append_repeat(buff, write_limit, &written, zero_pad ? '0' : ' ', pad) == EOF)
                return EOF;
            if (printf_append_repeat(buff, write_limit, &written, '0', digits_zero_pad) == EOF)
                return EOF;
            if (len > 0U && printf_append_string(buff, write_limit, &written, digits, len, false) == EOF)
                return EOF;
            if (left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
        }
        else if (spec == 'x' || spec == 'X')
        {
            bool uppercase = spec == 'X';
            unsigned long long value = 0;
            if (is_long_long)
                value = va_arg(parameters, unsigned long long);
            else if (is_long)
                value = (unsigned long long) va_arg(parameters, unsigned long);
            else
                value = (unsigned long long) va_arg(parameters, unsigned int);
            char digits[32];
            lltoa(value, digits, sizeof(digits), HEXADECIMAL);
            size_t len = strlen(digits);
            if (precision == 0 && value == 0ULL)
                len = 0U;
            size_t digits_zero_pad = 0U;
            if (precision > 0 && (size_t) precision > len)
                digits_zero_pad = (size_t) precision - len;
            if (precision >= 0)
                zero_pad = false;
            if (left_align)
                zero_pad = false;
            size_t total = digits_zero_pad + len;
            size_t pad = (width > total) ? (width - total) : 0U;

            if (!left_align && printf_append_repeat(buff, write_limit, &written, zero_pad ? '0' : ' ', pad) == EOF)
                return EOF;
            if (printf_append_repeat(buff, write_limit, &written, '0', digits_zero_pad) == EOF)
                return EOF;
            if (len > 0U && printf_append_string(buff, write_limit, &written, digits, len, uppercase) == EOF)
                return EOF;
            if (left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
        }
        else if (spec == 'p' || spec == 'P')
        {
            bool uppercase = spec == 'P';
            uintptr_t value = (uintptr_t) va_arg(parameters, void*);
            char digits[32];
            lltoa((unsigned long long) value, digits, sizeof(digits), HEXADECIMAL);
            size_t len = strlen(digits);
            size_t total = 2U + len;
            size_t pad = (width > total) ? (width - total) : 0U;

            if (left_align)
                zero_pad = false;

            if (!left_align && !zero_pad && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
            if (printf_append_char(buff, write_limit, &written, '0') == EOF)
                return EOF;
            if (printf_append_char(buff, write_limit, &written, uppercase ? 'X' : 'x') == EOF)
                return EOF;
            if (!left_align && zero_pad && printf_append_repeat(buff, write_limit, &written, '0', pad) == EOF)
                return EOF;
            if (printf_append_string(buff, write_limit, &written, digits, len, uppercase) == EOF)
                return EOF;
            if (left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
        }
        else if (spec == 'b' || spec == 'B')
        {
            bool uppercase = spec == 'B';
            int v = va_arg(parameters, bool);
            const char* str = v ? "true" : "false";
            size_t len = strlen(str);
            size_t pad = (width > len) ? (width - len) : 0U;

            if (!left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
                return EOF;
            if (printf_append_string(buff, write_limit, &written, str, len, uppercase) == EOF)
                return EOF;
            if (left_align && printf_append_repeat(buff, write_limit, &written, ' ', pad) == EOF)
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
#if !defined(__THEOS_KERNEL)
    if (stdio_console_silent)
        return 0;
#endif

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

int vfprintf(FILE* stream, const char* __restrict format, va_list ap)
{
    if (!stream || !format)
        return EOF;

#if !defined(__THEOS_KERNEL)
    if (stdio_console_silent && (stream == stdout || stream == stderr))
        return 0;
#endif

    size_t len = 512U;
    char buf[512];
    memset(buf, '\0', sizeof(buf));

    va_list local_ap;
    va_copy(local_ap, ap);
    int result = __printf(buf, len, format, local_ap);
    va_end(local_ap);
    if (result < 0)
        return EOF;

#if defined(__THEOS_KERNEL)
    size_t out_len = strlen(buf);
    for (size_t i = 0; i < out_len; i++)
        putc(buf[i]);
    return result;
#else
    size_t out_len = strlen(buf);
    ssize_t rc = write(stream->fd, buf, out_len);
    if (rc < 0)
        return EOF;

    return result;
#endif
}

int fprintf(FILE* stream, const char* __restrict format, ...)
{
    va_list ap;
    va_start(ap, format);
    int result = vfprintf(stream, format, ap);
    va_end(ap);
    return result;
}

int sprintf(char* str, const char* __restrict format, ...)
{
    int result = EOF;

    va_list parameters;
    va_start(parameters, format);

    result = __printf(str, 0U, format, parameters);

    va_end(parameters);

    return result;
}

int vsnprintf(char* str, size_t size, const char* __restrict format, va_list parameters)
{
    return __printf(str, size, format, parameters);
}

int snprintf(char* str, size_t size, const char* __restrict format, ...)
{
    int result = EOF;

    va_list parameters;
    va_start(parameters, format);

    result = vsnprintf(str, size, format, parameters);

    va_end(parameters);

    return result;
}

void stdio_set_console_silent(int enabled)
{
#if defined(__THEOS_KERNEL)
    (void) enabled;
#else
    stdio_console_silent = (enabled != 0);
#endif
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
