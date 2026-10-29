/*
 * main.c - vitamemio.suprx, the user module.
 *
 * The ONE vitamemio.suprx is listed under both *main (SceShell) and
 * *NPXS10015 (the Settings app) in config.txt. Branch on the
 * process:
 *   - Settings app: install only the Settings page hooks
 *     (settings_app/vitamemio_settings.c);
 *   - SceShell: start the developer command server (user/dispatch.c), which is
 *     deferred and only listens while Developer mode is on.
 * Everything that matters - the storage hooks, the HUD, saving settings - lives
 * in the kernel module, so neither instance has anything to undo but its own
 * hooks or thread.
 *
 * The kernel library SceVitaMemio is a strong import: if the kernel module
 * is not loaded, taiHEN refuses this module and nothing appears.
 */
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include "../include/vitamemio.h"
#include "../include/vitamemio_kernel_api.h"
#include "../settings_app/settings_ui.h"
#include "../settings_app/vmlog.h"
#include "dispatch.h"

static int in_shell(void)
{
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	return taiGetModuleInfo("SceShell", &ti) >= 0;
}

int module_start(SceSize argc, const void *args);
/* Entry symbol for the linker; taiHEN itself enters through module_start. */
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;

	/* Logged only when the kernel module does not answer. */
	int ping = vmSelfTestPing();
	if (ping != (int)VM_SELFTEST_MAGIC)
		VM_LOGI("user v%s (%s): kernel module not answering (ping 0x%08x)",
		        VM_VERSION_STR, in_shell() ? "shell" : "settings", (unsigned)ping);

	if (in_shell())
		vm_dispatch_start();          /* deferred; returns immediately */
	else
		vm_settings_hooks_install();
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	if (in_shell())
		vm_dispatch_stop();
	else
		vm_settings_hooks_release();
	return SCE_KERNEL_STOP_SUCCESS;
}
