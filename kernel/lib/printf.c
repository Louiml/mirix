#include "printf.h"
#include "string.h"

/* Small buffered emitter: each conversion is formatted into a local buffer
 * so we can apply width / zero-padding before pushing to the sink. */
typedef struct {
    char buf[40];
    int len;
} out_t;

static void outc(out_t *o, char c)
{
    if (o->len < (int)sizeof(o->buf) - 1)
        o->buf[o->len++] = c;
}

static void outs(out_t *o, const char *s)
{
    if (s == 0)
        s = "(null)";
    while (*s)
        outc(o, *s++);
}

static void outu(out_t *o, unsigned long long v, unsigned base, int upper)
{
    char tmp[32];
    int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) {
        outc(o, '0');
        return;
    }
    while (v) {
        tmp[i++] = digits[v % base];
        v /= base;
    }
    while (i--)
        outc(o, tmp[i]);
}

int kvprintf(mirix_putc_fn putc, void *ctx, const char *fmt, va_list ap)
{
    int count = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            putc(ctx, *p);
            count++;
            continue;
        }
        p++;
        if (*p == 0)
            break;

        /* flags and width: %[0][width][.precision][l|ll|h]conv */
        int zero = 0, width = 0, numeric = 0;
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        if (*p == '.') {           /* precision: parsed, only used to cap %s */
            p++;
            int prec = 0;
            while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; }
        }
        int lmod = 0;
        while (*p == 'l') { lmod++; p++; }
        if (*p == 'h')
            p++;

        out_t o = { .len = 0 };
        o.buf[0] = 0;
        int negative = 0;
        switch (*p) {
        case '%':
            outc(&o, '%');
            break;
        case 'c':
            outc(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            outs(&o, s);
            break;
        }
        case 'd':
        case 'i': {
            long long v = (lmod >= 2) ? va_arg(ap, long long)
                        : (lmod == 1) ? va_arg(ap, long)
                        : va_arg(ap, int);
            numeric = 1;
            if (v < 0) {
                negative = 1;
                outu(&o, (unsigned long long)(-v), 10, 0);
            } else {
                outu(&o, (unsigned long long)v, 10, 0);
            }
            break;
        }
        case 'u':
            numeric = 1;
            outu(&o, (lmod >= 2) ? va_arg(ap, unsigned long long)
                 : (lmod == 1) ? va_arg(ap, unsigned long)
                 : va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
        case 'X':
            numeric = 1;
            outu(&o, (lmod >= 2) ? va_arg(ap, unsigned long long)
                 : (lmod == 1) ? va_arg(ap, unsigned long)
                 : va_arg(ap, unsigned int), 16, *p == 'X');
            break;
        case 'p':
            numeric = 1;
            outs(&o, "0x");
            outu(&o, (unsigned long long)(unsigned long)va_arg(ap, void *), 16, 0);
            break;
        default:
            outc(&o, '%');
            outc(&o, *p);
            break;
        }

        int len = o.len + (negative ? 1 : 0);
        if (negative) {
            putc(ctx, '-');
            count++;
        }
        for (int i = len; i < width; i++)
            putc(ctx, (zero && numeric) ? '0' : ' ');
        for (int i = 0; i < o.len; i++)
            putc(ctx, o.buf[i]);
        count += o.len;
    }
    return count;
}
