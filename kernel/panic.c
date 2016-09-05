#include <stdarg.h>
#include <sys/asm.h>

#include "kio.h"
#include "con.h"
#include "panic.h"

static int s_use_bsod = 1;

void panic_set_use_bsod(int use_bsod) 
{
    s_use_bsod = use_bsod ? 1 : 0;
}

static void print_register_set(struct register_set regset)
{
    kprintf(
        " eax=%08x ebx=%08x ecx=%08x edx=%08x\n"
        " esi=%08x edi=%08x ebp=%08x esp=%08x\n",
        regset.a, regset.b, regset.c, regset.d,
        regset.si, regset.di, regset.bp, regset.sp);
}

ALWAYS_INLINE NO_RETURN void
__panic(
    const struct register_set   regset,
    const char                  *fmt,
    va_list                     args)
{   
    if (s_use_bsod) {
        // Yes, really.
        con_set_bgcol(COL_BLUE);
        con_set_fgcol(COL_BRWHITE);
        con_clear();    
        kprintf(
            "\n"
            "An error has been detected and your computer has been\n"
            "frozen to prevent damage to your devices and data.\n"
            "\n"
            "It is OK to turn off or restart your computer.\n"
            "\n"
            "Details:\n"
            "\n"
        );
    }

    kprintf("panic: ");
    kvprintf(fmt, args);

    print_register_set(regset);

    cli();
    hlt();
    while (1);
}

void NO_RETURN panicrs(const struct register_set regset, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    __panic(regset, fmt, args);
    va_end(args);
}

void NO_RETURN panic(const char *fmt, ...)
{
    struct register_set regset = get_registers();
    va_list args;
    va_start(args, fmt);
    __panic(regset, fmt, args);
    va_end(args);
}
