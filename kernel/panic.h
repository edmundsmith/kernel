#ifndef _INC_PANIC
#define _INC_PANIC 1

#include <sys/asm.h>

void NO_RETURN panicrs(const struct register_set regset, const char *fmt, ...);
void NO_RETURN panic(const char *fmt, ...);

#endif /* _INC_PANIC */
