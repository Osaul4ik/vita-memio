/*
 * main.c - vitamemio.suprx, the user module.
 *
 * Loaded only in the Settings app (*NPXS10015 in config.txt), where it adds
 * the vita-memio page under Settings > Devices
 * (settings_app/vitamemio_settings.c). Everything that matters - the storage
 * hooks, the HUD, saving settings - lives in the kernel module, so this module
 * has nothing to undo but its own hooks.
 *
 * Loaded anywhere else (an old config.txt that still lists it under *main),
 * it does nothing.
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

/* The Settings app is the process that has SceSettings loaded. */
static int in_settings(void)
{
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	return taiGetModuleInfo("SceSettings", &ti) >= 0;
}

static int g_active = 0;

int module_start(SceSize argc, const void *args);
/* Entry symbol for the linker; taiHEN itself enters through module_start. */
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;

	if (!in_settings())
		return SCE_KERNEL_START_SUCCESS;     /* not the Settings app: nothing to do */
	g_active = 1;

	/* Logged only when the kernel module does not answer. */
	int ping = vmSelfTestPing();
	if (ping != (int)VM_SELFTEST_MAGIC)
		VM_LOGI("user v%s: kernel module not answering (ping 0x%08x)",
		        VM_VERSION_STR, (unsigned)ping);

	vm_settings_hooks_install();
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	if (g_active)
		vm_settings_hooks_release();
	return SCE_KERNEL_STOP_SUCCESS;
}
