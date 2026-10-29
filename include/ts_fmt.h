/*
 * ts_fmt.h - the tiny printf subset shared by the kernel logger (kernel/klog.c)
 * and the user-module logger (settings_app/tslog.c).
 *
 * Neither module has a libc printf, so both format with this: %s %d %i %u %x %X
 * %c %p %%, with an optional zero-pad width. It writes at most cap-1 characters
 * and always terminates the buffer. Header-only (static inline) so each module
 * gets its own copy and there is nothing to link.
 */
#ifndef STORAGETURBO_FMT_H
#define STORAGETURBO_FMT_H

#include <stdarg.h>
#include <stdint.h>

static inline int ts_fmt_uint(char *buf, int cap, int pos, unsigned v, unsigned base,
                    int upper, int width, int zero)
{
	const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[16];
	int n = 0;
	if (!v) tmp[n++] = '0';
	while (v && n < (int)sizeof(tmp)) { tmp[n++] = digs[v % base]; v /= base; }
	int pad = width - n;
	char padc = zero ? '0' : ' ';
	while (pad-- > 0 && pos < cap - 1) buf[pos++] = padc;
	while (n && pos < cap - 1) buf[pos++] = tmp[--n];
	return pos;
}

static inline int ts_fmt_v(char *buf, int cap, const char *fmt, va_list ap)
{
	int pos = 0;
	if (cap <= 0)
		return 0;
	for (const char *f = fmt; *f && pos < cap - 1; f++) {
		if (*f != '%') { buf[pos++] = *f; continue; }
		f++;
		if (*f == 0) break;
		int zero = 0, width = 0;
		if (*f == '0') { zero = 1; f++; }
		while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
		switch (*f) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s) s = "(null)";
			while (*s && pos < cap - 1) buf[pos++] = *s++;
			break;
		}
		case 'd': case 'i': {
			int v = va_arg(ap, int);
			if (v < 0) {
				if (pos < cap - 1) buf[pos++] = '-';
				pos = ts_fmt_uint(buf, cap, pos, (unsigned)(-(v + 1)) + 1u, 10, 0,
				               width > 0 ? width - 1 : 0, zero);
			} else {
				pos = ts_fmt_uint(buf, cap, pos, (unsigned)v, 10, 0, width, zero);
			}
			break;
		}
		case 'u':
			pos = ts_fmt_uint(buf, cap, pos, va_arg(ap, unsigned), 10, 0, width, zero);
			break;
		case 'x':
			pos = ts_fmt_uint(buf, cap, pos, va_arg(ap, unsigned), 16, 0, width, zero);
			break;
		case 'X':
			pos = ts_fmt_uint(buf, cap, pos, va_arg(ap, unsigned), 16, 1, width, zero);
			break;
		case 'p':
			if (pos < cap - 1) buf[pos++] = '0';
			if (pos < cap - 1) buf[pos++] = 'x';
			pos = ts_fmt_uint(buf, cap, pos,
			               (unsigned)(uintptr_t)va_arg(ap, void *), 16, 0, 8, 1);
			break;
		case 'c': {
			char c = (char)va_arg(ap, int);
			if (pos < cap - 1) buf[pos++] = c;
			break;
		}
		case '%':
			if (pos < cap - 1) buf[pos++] = '%';
			break;
		default:
			if (pos < cap - 1) buf[pos++] = '%';
			if (pos < cap - 1) buf[pos++] = *f;
			break;
		}
	}
	buf[pos] = 0;
	return pos;
}

#endif /* STORAGETURBO_FMT_H */
