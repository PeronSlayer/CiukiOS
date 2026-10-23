/* newlib's tzinfo layout (sys/_tz_structs.h), for the G1 host shim. */
#ifndef CIUKI_TZ_SHIM_STRUCTS_H
#define CIUKI_TZ_SHIM_STRUCTS_H
#include <time.h>
typedef struct { char ch; int m, n, d, s; time_t change; long offset; } __tzrule_type;
typedef struct { int __tznorth, __tzyear; __tzrule_type __tzrule[2]; } __tzinfo_type;
__tzinfo_type *__gettzinfo(void);
#endif
