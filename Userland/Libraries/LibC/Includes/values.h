#ifndef _VALUES_H
#define _VALUES_H

#include <limits.h>

#ifndef BITSPERBYTE
#define BITSPERBYTE CHAR_BIT
#endif

#ifndef BITS
#define BITS(type)   ((int) (BITSPERBYTE * (int) sizeof(type)))
#endif

#ifndef MAXCHAR
#define MAXCHAR      CHAR_MAX
#endif
#ifndef MINCHAR
#define MINCHAR      CHAR_MIN
#endif

#ifndef MAXSHORT
#define MAXSHORT     SHRT_MAX
#endif
#ifndef MINSHORT
#define MINSHORT     SHRT_MIN
#endif

#ifndef MAXINT
#define MAXINT       INT_MAX
#endif
#ifndef MININT
#define MININT       INT_MIN
#endif

#ifndef MAXLONG
#define MAXLONG      LONG_MAX
#endif
#ifndef MINLONG
#define MINLONG      LONG_MIN
#endif

#ifndef MAXDOUBLE
#define MAXDOUBLE    __DBL_MAX__
#endif
#ifndef MINDOUBLE
#define MINDOUBLE    __DBL_MIN__
#endif
#ifndef MAXFLOAT
#define MAXFLOAT     __FLT_MAX__
#endif
#ifndef MINFLOAT
#define MINFLOAT     __FLT_MIN__
#endif

#ifndef HIBITS
#define HIBITS       ((int) (1U << (BITS(int) - 1)))
#endif
#ifndef HIBITL
#define HIBITL       ((long) (1UL << (BITS(long) - 1)))
#endif

#endif
