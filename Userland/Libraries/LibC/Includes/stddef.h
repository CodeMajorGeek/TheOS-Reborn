#ifndef _STDDEF_H
#define _STDDEF_H

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __PTRDIFF_TYPE__ ssize_t;

#ifndef NULL
#define NULL ((void*) 0)
#endif

#ifndef SIZE_MAX
#define SIZE_MAX __SIZE_MAX__
#endif

#ifndef PTRDIFF_MAX
#define PTRDIFF_MAX __PTRDIFF_MAX__
#endif
#ifndef PTRDIFF_MIN
#define PTRDIFF_MIN (-PTRDIFF_MAX - 1)
#endif

#ifndef SSIZE_MAX
#define SSIZE_MAX ((ssize_t) (SIZE_MAX >> 1))
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif
