#include <cccompat.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>

#include "kstring.h"
#include "kutil.h"
#include "kio.h"
#include "con.h"

#define OUTBUF_SIZE PAGE_SIZE

static PAGE_ALIGNED char outbuf[OUTBUF_SIZE];
static int outbuf_zeroed = 0;

extern int con_write_str(const char *);
extern int con_write_char(char);

static int (*const puts_ptr)(const char *) = con_write_str;
static int (*const putc_ptr)(char)         = con_write_char;

// Format specifiers (the type of the var arg)
// Note that differences in case ('X' vs 'x') are signified by fmtinfo.is_upper
enum fmttype
{
    FT_INVALID,
    FT_SIGNED,              // 'd', 'i'
    FT_UNSIGNED,            // 'u'
    FT_OCTAL,               // 'o'
    FT_HEX,                 // 'x', 'X'
    FT_FLOAT,               // 'f', 'F'
    FT_SCIENTIFIC,          // 'e', 'E'
    FT_SHORTEST,            // 'g', 'G': Shortest out of float and scientific
    FT_HEXFLOAT,            // 'a', 'A': Hexadecimal as float
    FT_CHAR,                // 'c'
    FT_STR,                 // 's'
    FT_PTR,                 // 'p'
    FT_STORE_CHAR_COUNT,    // 'n'
    FT_ESCAPE,              // '%'
};

// Format specifier flags
enum fmtflags
{
    FF_NONE     = 0x00,
    FF_LJUSTIFY = 0x10, // '-': Left-justify within width
    FF_ZEROPAD  = 0x01, // '0': Zero-pad within width
    FF_POSSIGN  = 0x02, // '+': Prefix positive values with a plus
    FF_PADSIGN  = 0x04, // ' ': Prefix positive values with a space
    FF_EXPLICIT = 0x08, // '#': Prefix hex with '0x' or always show decimal dot
};

// Format specifier size (long vs short int, etc.)
enum fmtlen
{
    FL_INT,         // (none)
    FL_CHAR,        // hh 
    FL_SHORT,       // h
    FL_LONG,        // l
    FL_LONG_LONG,   // ll
    FL_INTMAX,      // j
    FL_SIZE,        // z
    FL_PTRDIFF,     // t
    FL_LONG_DOUBLE, // L
};

// Pack all of this information up neatly
struct fmtinfo
{
    enum fmttype    ft;
    enum fmtflags   ff;
    enum fmtlen     fl;
    int             width;      // -1 if specified in vararg, 0 if unspecified
    int             precision;  // -1 if specified in vararg, 0 if unspecified
    int             is_upper;   // Make letter digit chars uppercase (eg 0xFF)
};

ALWAYS_INLINE size_t
__va_str_format_proc(
    const char      *fmt,
    struct fmtinfo  *fip)
{
    // Format specifiers follow this scheme (fixed order, thank God):
    //  %[flags][width]['.', precision][length]<specifier>
    //
    // Each part can be:
    //  Flags:      -, +, ' ', #, 0
    //  Width:      an unsigned number (cannot start with zero), or a '*'.
    //  Precision:  '.' followed by an unsigned number, or followed by a '*'.
    //  Length:     hh, h, l, ll, j, z, t, L
    //  Specifier:  d, i, u, o, x, X, f, F, e, E, g, G, a, A, c, s, p, n, %
    //
    // Note that we only implement part of this mess. Also the actual formatter
    // may also ignore some of the information we acquire.

    struct fmtinfo fi = { 0 };

    enum {
        could_be_flags      = 1,
        could_be_width      = 2,
        could_be_presision  = 3,
        could_be_length     = 4,
        could_be_spec       = 5,
    } could_be = could_be_spec;

    size_t  fmti = 0;
    char    fmtc;
    while (++fmti, (fmtc = *(fmt++))) {

        // Could this char be a flag specifier?
        if (could_be <= could_be_flags) {

            // Let's assume it is unless we find out otherwise
            int is_flags = 1;

            switch (fmtc) {
            case '-':
                fi.ff |= FF_LJUSTIFY;
                break;
            case '+':
                fi.ff |= FF_POSSIGN;
                break;
            case ' ':
                fi.ff |= FF_PADSIGN;
                break;
            case '#':
                fi.ff |= FF_EXPLICIT;
                break;
            case '0':
                fi.ff |= FF_ZEROPAD;
                break;
            default:
                // If this char was none of the above, then it wasn't a flag
                is_flags = 0;
                break;
            }

            // If this _was_ a flag, then move onto the next character.
            if(is_flags) {
                continue;
            }

            // Otherwise, fall through to the next check
        } 

        // Not a flag. Could it be the first char of the width specifier?
        // If so, it's going to be a string of digits.
        if (could_be <= could_be_width) {
            could_be = could_be_width;

            // There's a chance it's a width, let's assume so.
            int is_width = 1;

            // Is it even a digit? If so, try to parse it. Otherwise move on
            if (isdigit(fmtc)) {
                int width = 0;

                // Parse the number string. Just continue to loop over fmt
                // here, to reduce complications (in theory).
                while (isdigit(fmtc)) {
                    width *= 10;
                    width += (fmtc - '0');

                    // Advance fmt pointer, and check for null-term
                    if (++fmti, !(fmtc = *(fmt++))) {
                        // Null-term, bail out
                        break;
                    }
                }

                fi.width = width;

                // We'll have overshot by one, so sneakily backtrack
                --fmti;
                --fmt;

            } else if (fmtc == '*') {
                // Asterisk denotes that the width is specified by the next
                // var arg
                fi.width = -1;
            } else {
                is_width = 0;
            }

            // Same drill as before: move onto next char if this was actually
            // width, otherwise fall-through to the next check.
            if (is_width) {
                continue;
            }
        }

        // Same ordeal as above
        if (could_be <= could_be_presision) {
            could_be = could_be_presision;

            int is_precision = 1;

            // Precision _must_ start with a '.'
            if (fmtc == '.') {

                // Advance fmt pointer over the '.', and check for null-term
                if (++fmti, !(fmtc = *(fmt++))) {
                    // Null-term, cannot be precision
                    is_precision = 0;
                } else {
                    if (isdigit(fmtc)) {
                        int precision = 0;                    
                        // Parse this number
                        while (isdigit(fmtc)) {
                            precision *= 10;
                            precision += (fmtc - '0');

                            // Advance fmt pointer, and check for null-term
                            if (++fmti, !(fmtc = *(fmt++))) {
                                // Null-term, bail out
                                break;
                            }
                        }

                        fi.precision = precision;
                    } else if (fmtc == '*') {
                        // Asterisk denotes that the precision is specified by 
                        // the next var arg.
                        fi.width = -1;
                    } else {
                        is_precision = 0;
                    }
                }

                // Backtrack by 1 (accounts for null-term check overshot)
                --fmti;
                --fmt;

            } else {
                is_precision = 0;
            }

            if (is_precision) {
                continue;
            }
        }

        if (could_be <= could_be_length) {
            could_be = could_be_length;

            int is_length = 1;

            switch (fmtc) {
            case 'h':
                fi.fl = FL_SHORT;               
                // Check to see if the next char is another h (for 'hh') 
                if (++fmti, (fmtc = *(fmt++)) && fmtc == 'h') {
                    fi.fl = FL_CHAR;
                } else {
                    // If not, backtrack
                    --fmti;
                    --fmt;
                }
                break;
            case 'l':
                fi.fl = FL_LONG_LONG;
                // Check to see if the next char is another l (for 'll')
                if (++fmti, (fmtc = *(fmt++)) && fmtc == 'l') {
                    fi.fl = FL_LONG;
                } else {
                    // If not, backtrack
                    --fmti;
                    --fmt;
                }
                break;
            case 'j':
                fi.fl = FL_INTMAX;
                break;
            case 'z':
                fi.fl = FL_SIZE;
                break;
            case 't':
                fi.fl = FL_PTRDIFF;
                break;
            case 'L':
                fi.fl = FL_LONG_DOUBLE;
                break;
            default:
                // If we had no matches, then this was not a length specifier
                is_length = 0;
                break;
            }

            if (is_length) {
                continue;
            }
        }

        if (could_be <= could_be_spec) {
            could_be = could_be_spec;

            int is_spec = 1;

            switch (fmtc) {
            case 'd':
            case 'i':
                fi.ft = FT_SIGNED;
                break;
            case 'u':
                fi.ft = FT_UNSIGNED;
                break;
            case 'o':
                fi.ft = FT_OCTAL;
                break;
            case 'X':
                fi.is_upper = 1;
            case 'x':
                fi.ft = FT_HEX;
                break;
            case 'F':
                fi.is_upper = 1;
            case 'f':
                fi.ft = FT_FLOAT;
                break;
            case 'E':
                fi.is_upper = 1;
            case 'e':
                fi.ft = FT_SCIENTIFIC;
                break;
            case 'G':
                fi.is_upper = 1;
            case 'g':
                fi.ft = FT_SHORTEST;
                break;
            case 'A':
                fi.is_upper = 1;
            case 'a':
                fi.ft = FT_HEXFLOAT;
                break;
            case 'c':
                fi.ft = FT_CHAR;
                break;
            case 's':
                fi.ft = FT_STR;
                break;
            case 'p':
                fi.ft = FT_PTR;
                break;
            case 'n':
                fi.ft = FT_STORE_CHAR_COUNT;
                break;
            case '%':
                fi.ft = FT_ESCAPE;
                break;
            default:
                is_spec = 0;
                break;
            }

            if (is_spec) {
                break;
            }
        }

        // We've fallen through all checks. This was an invalid format 
        // specifier, or it at least contains unaccounted-for characters.
        fi.ft = FT_INVALID;
        break;
    }

    *fip = fi;
    return fmti;
}

ALWAYS_INLINE size_t
__str_puts(
    char        **dst,
    const char  *src,
    size_t      sz)
{
    size_t i = 0;
    while (*src) {
        **dst = *src;
        ++i;
        if (!--sz) {
            **dst = 0;
            break;
        }
        src++;
        (*dst)++;
    }
    return i;
}

// Processes string 'fmt' into buffer 'str' of size 'sz' using data from 'args'
// Returns the number of characters written into buffer 'str'
ALWAYS_INLINE size_t
__va_str_format_impl(
    char        *str, 
    size_t      sz,
    const char  *fmt,
    va_list     args) 
{
    size_t          old_sz = sz;
    char            fmtbuf[9];
    struct fmtinfo  fi;
    union {
        int32_t         i32;
        int32_t         u32;
        int8_t          i16;
        const char      *str;
    } arg;

    char fmtc;
    while ((fmtc = *(fmt++))) {
        if (fmtc == '%') {
            size_t count = __va_str_format_proc(fmt, &fi);
            fmt += count;

            if (fi.ft == FT_ESCAPE) {
                sz -= __str_puts(&str, "%", sz);
                continue;
            }

            if (fi.ft == FT_STR) {
                arg.str = va_arg(args, const char *);
                sz -= __str_puts(&str, arg.str, sz);
                continue;                
            }

            if (fi.ft == FT_SIGNED || fi.ft == FT_UNSIGNED) {
                arg.i32 = va_arg(args, int32_t);                    
                itoa10(arg.i32, fmtbuf);
                sz -= __str_puts(&str, fmtbuf, sz);
                continue;                
            }

            if (fi.ft == FT_PTR || fi.ft == FT_HEX) {
                arg.u32 = va_arg(args, uint32_t);                    
                itoa16(arg.u32, fmtbuf);
                sz -= __str_puts(&str, fmtbuf, sz);
                continue;                
            }

            sz -= __str_puts(&str, "<invalid>", sz);
            (void) va_arg(args, uint32_t);
            continue;

            if (fi.is_upper) {
                char *bufptr = fmtbuf;
                while (*bufptr) {
                    *bufptr = toupper(*bufptr);
                    ++bufptr;
                }
            }

            sz -= __str_puts(&str, fmtbuf, sz);

        } else {
            *(str++) = fmtc;
            --sz;
        }
    }

    *(str++) = 0;
    --sz;

    return old_sz - sz;

#if 0    
    size_t count = 0;
    char buf[64];
    void *ptr;
    const char *str;
    int i32;  

    char c;
    while ((c = *(fmt++))) {
        if (c == '%') {
            switch (c = *(fmt++)) {
            case '%':
            default:
                putc_ptr(c);
                break;
            case 's':
                str = va_arg(args, const char *);
                puts_ptr(str);
                break;
            case 'u':
            case 'i':
            case 'd':
                i32 = va_arg(args, int32_t);
                itoa10(i32, buf);
                puts_ptr(buf);
                break;
            case 'x':
                i32 = va_arg(args, uint32_t);
                itoa16(i32, buf, 8);
                puts_ptr(buf);
                break;
            case 'X':
                i32 = va_arg(args, uint32_t);
                itoa16(i32, buf, 0);
                for (size_t i = 0; i < sizeof(buf); ++i) {
                    buf[i] = toupper(buf[i]);
                }
                puts_ptr(buf);
                break;
            case 'p':
                ptr = va_arg(args, void *);
                itoa16((int32_t) ptr, buf, sizeof(void *) * 2);
                puts_ptr(buf);
                break;
            }
        } else {
            putc_ptr(c);
        }
    }    

    return count;
#endif
}

int kvsnprintf(char *str, size_t sz, const char *fmt, va_list args)
{
    return (int) __va_str_format_impl(str, sz, fmt, args);
}

int ksnprintf(char *str, size_t sz, const char *fmt, ...)
{
    int result;
    va_list args;

    va_start(args, fmt);
    result = (int) kvsnprintf(str, sz, fmt, args);
    va_end(args);

    return result;
}

int kvsprintf(char *str, const char *fmt, va_list args)
{
    return (int) __va_str_format_impl(str, 0, fmt, args);
}

int ksprintf(char *str, const char *fmt, ...)
{
    int result;
    va_list args;

    va_start(args, fmt);
    result = (int) kvsprintf(str, fmt, args);
    va_end(args);

    return result;
}

int kvprintf(const char *fmt, va_list args)
{
    if (!outbuf_zeroed) {
        kmemset((void *) outbuf, OUTBUF_SIZE, 0);
        outbuf_zeroed = 1;
    }

    int result = (int) __va_str_format_impl(outbuf, OUTBUF_SIZE, fmt, args);
    con_write_str(outbuf);
    return result;
}

int kprintf(const char *fmt, ...)
{
    int result;    
    va_list args;

    va_start(args, fmt);
    result = (int) kvprintf(fmt, args);
    va_end(args);

    return result;
}