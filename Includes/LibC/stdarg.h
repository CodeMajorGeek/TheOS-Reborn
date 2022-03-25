#ifndef _STDARG_H
#define _STDARG_H

#define va_start(v, l)  __builtin_va_start(v, l)
#define va_end(v)       __builtin_va_end(v);
#define va_arg(v, l)    __builtin_va_arg(v, l)
#define va_copy(v, l)   __builtin_va_copy(v, l)
#define va_list         __builtin_va_list

#endif