/*
 * tslog.c - Settings-page logger. See tslog.h.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>   /* sceKernelGetProcessTimeWide            */
#include <stdarg.h>
#include <stdint.h>

#include "tslog.h"

/* ---- self-contained printf subset (kernel/klog.c has the twin) ---------- */

static int fmt_uint(char *buf, int cap, int pos, unsigned v, unsigned base,
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

static int fmt_v(char *buf, int cap, const char *fmt, va_list ap)
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
				pos = fmt_uint(buf, cap, pos, (unsigned)(-(v + 1)) + 1u, 10, 0,
				               width > 0 ? width - 1 : 0, zero);
			} else {
				pos = fmt_uint(buf, cap, pos, (unsigned)v, 10, 0, width, zero);
			}
			break;
		}
		case 'u':
			pos = fmt_uint(buf, cap, pos, va_arg(ap, unsigned), 10, 0, width, zero);
			break;
		case 'x':
			pos = fmt_uint(buf, cap, pos, va_arg(ap, unsigned), 16, 0, width, zero);
			break;
		case 'X':
			pos = fmt_uint(buf, cap, pos, va_arg(ap, unsigned), 16, 1, width, zero);
			break;
		case 'p':
			if (pos < cap - 1) buf[pos++] = '0';
			if (pos < cap - 1) buf[pos++] = 'x';
			pos = fmt_uint(buf, cap, pos,
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

/* ---- public API --------------------------------------------------------- */

void ts_log(const char *fmt, ...)
{
	char line[192];
	int pos = 0;

	uint64_t t = (uint64_t)sceKernelGetProcessTimeWide();   /* microseconds  */
	const char *tag = "[user] [";
	while (*tag && pos < (int)sizeof(line) - 1) line[pos++] = *tag++;
	pos = fmt_uint(line, sizeof(line), pos, (unsigned)(t / 1000000u), 10, 0, 0, 0);
	if (pos < (int)sizeof(line) - 1) line[pos++] = '.';
	pos = fmt_uint(line, sizeof(line), pos, (unsigned)(t % 1000000u), 10, 0, 6, 1);
	if (pos < (int)sizeof(line) - 1) line[pos++] = ']';
	if (pos < (int)sizeof(line) - 1) line[pos++] = ' ';

	va_list ap;
	va_start(ap, fmt);
	pos += fmt_v(line + pos, (int)sizeof(line) - pos, fmt, ap);
	va_end(ap);
	if (pos < (int)sizeof(line) - 1) line[pos++] = '\n';

	sceIoMkdir("ur0:data", 0777);
	sceIoMkdir(TS_DATA_DIR, 0777);
	SceUID fd = sceIoOpen(TS_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;
	SceOff end = sceIoLseek(fd, 0, SCE_SEEK_END);
	if (end > TS_LOG_MAX_BYTES) {
		sceIoClose(fd);
		sceIoRemove(TS_LOG_PATH_OLD);
		sceIoRename(TS_LOG_PATH, TS_LOG_PATH_OLD);
		fd = sceIoOpen(TS_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd < 0)
			return;
	}
	sceIoWrite(fd, line, pos);
	sceIoClose(fd);
}
