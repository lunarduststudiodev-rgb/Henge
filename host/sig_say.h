#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>

/* printf to stderr for a signal handler: formats on the stack and write(2)s it, so it takes no lock and
   allocates nothing. stdio would block for good if the crashing thread, or any other, held stderr's lock
   (CPU threads log through it), and the crash would become a hang instead of reaching the prior handler.
   The conversions on_ill uses: %d %u %x %c %s %p and %%, with l or ll, the + and 0 flags and a width. */
static void sig_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

typedef struct {
    char buf[512];
    unsigned n;
} SigOut;

static void sig_flush(SigOut *o) {
    for (unsigned at = 0; at < o->n;) {
        ssize_t w = write(2, o->buf + at, o->n - at);
        if (w <= 0) break;
        at += (unsigned)w;
    }
    o->n = 0;
}

static void sig_put(SigOut *o, char c) {
    if (o->n == sizeof o->buf) sig_flush(o);
    o->buf[o->n++] = c;
}

static void sig_num(SigOut *o, uint64_t v, unsigned base, int neg, int plus, int zero, int width) {
    char d[24];
    int k = 0;
    do d[k++] = "0123456789abcdef"[v % base], v /= base; while (v);
    int sign = neg || plus;
    for (int pad = width - k - sign; !zero && pad > 0; pad--) sig_put(o, ' ');
    if (sign) sig_put(o, neg ? '-' : '+');
    for (int pad = width - k - sign; zero && pad > 0; pad--) sig_put(o, '0');
    while (k) sig_put(o, d[--k]);
}

static void sig_say(const char *fmt, ...) {
    SigOut o;
    o.n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { sig_put(&o, *f); continue; }
        int plus = 0, zero = 0, width = 0, longs = 0;
        for (f++; *f == '+' || *f == '0'; f++) plus |= *f == '+', zero |= *f == '0';
        for (; *f >= '0' && *f <= '9'; f++) width = width * 10 + (*f - '0');
        for (; *f == 'l'; f++) longs++;
        switch (*f) {
        case 'd': {
            int64_t v = longs ? va_arg(ap, long long) : va_arg(ap, int);
            sig_num(&o, v < 0 ? 0 - (uint64_t)v : (uint64_t)v, 10, v < 0, plus, zero, width);
            break;
        }
        case 'u': case 'x': {
            uint64_t v = longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            sig_num(&o, v, *f == 'u' ? 10 : 16, 0, 0, zero, width);
            break;
        }
        case 'p':
            sig_put(&o, '0'), sig_put(&o, 'x');
            sig_num(&o, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, 0, 0, 0);
            break;
        case 'c': sig_put(&o, (char)va_arg(ap, int)); break;
        case 's': for (const char *s = va_arg(ap, const char *); s && *s; s++) sig_put(&o, *s); break;
        case '%': sig_put(&o, '%'); break;
        default: va_end(ap); sig_flush(&o); return; /* not one of these: stop here rather than misread the arguments */
        }
    }
    va_end(ap);
    sig_flush(&o);
}
