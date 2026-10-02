#include "selftest_line.h"

/* Writes v right-aligned in at least `width` characters, padded with `pad`. */
static char *put_uint(char *p, uint64_t v, unsigned width, char pad) {
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    for (unsigned i = n; i < width; i++)
        *p++ = pad;
    while (n > 0)
        *p++ = digits[--n];
    return p;
}

static char *put_str(char *p, const char *s) {
    while (*s)
        *p++ = *s++;
    return p;
}

size_t selftest_format_line(char *buf, uint64_t ms) {
    char *p = buf;
    p = put_str(p, "[");
    p = put_uint(p, ms / 1000, 7, ' ');
    p = put_str(p, ".");
    p = put_uint(p, ms % 1000, 3, '0');
    p = put_str(p, "] t=");
    p = put_uint(p, ms, 1, '0');
    p = put_str(p, " ms\r\n");
    return (size_t)(p - buf);
}
