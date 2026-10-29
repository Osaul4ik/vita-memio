/*
 * vmlog.c - Settings-page logger. See vmlog.h.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>   /* sceKernelGetProcessTimeWide            */
#include <stdarg.h>
#include <stdint.h>

#include "vmlog.h"
#include "../include/vm_fmt.h"

/* ---- public API --------------------------------------------------------- */

void vm_log(const char *fmt, ...)
{
	char line[192];
	int pos = 0;

	uint64_t t = (uint64_t)sceKernelGetProcessTimeWide();   /* microseconds  */
	const char *tag = "[user] [";
	while (*tag && pos < (int)sizeof(line) - 1) line[pos++] = *tag++;
	pos = vm_fmt_uint(line, sizeof(line), pos, (unsigned)(t / 1000000u), 10, 0, 0, 0);
	if (pos < (int)sizeof(line) - 1) line[pos++] = '.';
	pos = vm_fmt_uint(line, sizeof(line), pos, (unsigned)(t % 1000000u), 10, 0, 6, 1);
	if (pos < (int)sizeof(line) - 1) line[pos++] = ']';
	if (pos < (int)sizeof(line) - 1) line[pos++] = ' ';

	va_list ap;
	va_start(ap, fmt);
	pos += vm_fmt_v(line + pos, (int)sizeof(line) - pos, fmt, ap);
	va_end(ap);
	if (pos < (int)sizeof(line) - 1) line[pos++] = '\n';

	sceIoMkdir("ur0:data", 0777);
	sceIoMkdir(VM_DATA_DIR, 0777);
	SceUID fd = sceIoOpen(VM_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;
	SceOff end = sceIoLseek(fd, 0, SCE_SEEK_END);
	if (end > VM_LOG_MAX_BYTES) {
		sceIoClose(fd);
		sceIoRemove(VM_LOG_PATH_OLD);
		sceIoRename(VM_LOG_PATH, VM_LOG_PATH_OLD);
		fd = sceIoOpen(VM_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd < 0)
			return;
	}
	sceIoWrite(fd, line, pos);
	sceIoClose(fd);
}
