/* util.c -- low-level helpers and the output buffer */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* small utilities                                                    */
/* ------------------------------------------------------------------ */

void write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += k;
        n -= (size_t)k;
    }
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/*
 * out = a + b (b may be NULL).  Returns 0, leaving nothing behind, when the
 * result would not fit.  Doing the copy by hand keeps the compiler from
 * having to guess at snprintf() truncation, and the length check makes
 * over-long paths a reported error instead of a silent truncation.
 */
int str_cat2(char *out, size_t cap, const char *a, const char *b)
{
    size_t la = a ? strlen(a) : 0;
    size_t lb = b ? strlen(b) : 0;

    if (la + lb + 1 > cap)
        return 0;
    if (la)
        memmove(out, a, la);
    if (lb)
        memmove(out + la, b, lb);
    out[la + lb] = '\0';
    return 1;
}

void emit_raw(const char *s, size_t n)
{
    if (g_out_fd >= 0 && n)
        write_all(g_out_fd, s, n);
}

void emit(const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (n <= 0)
        return;

    if ((size_t)n > sizeof buf - 1)
        n = (int)sizeof buf - 1;

    emit_raw(buf, (size_t)n);
}

