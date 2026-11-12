/*
 * vitamemio_i18n.h - text for the "vita-memio" Settings page.
 *
 * English only, whatever the system language (one string per id, from
 * tools/gen_i18n.py), so the page carries no other languages' text.
 * C99, no char16_t: UTF-16 output uses plain unsigned short.
 */
#ifndef VITAMEMIO_I18N_H
#define VITAMEMIO_I18N_H

enum vm_i18n_id {
	VM_I18N_TITLE = 0,          /* "vita-memio"                                */
	VM_I18N_ROW_SD2VITA,        /* row titles                                     */
	VM_I18N_ROW_HUD,
	VM_I18N_ROW_MEMCARD,
	VM_I18N_ROW_VERIFY,
	VM_I18N_ROW_LOG,
	VM_I18N_ROW_SAFE,
	VM_I18N_HINT_SD2VITA,       /* help text                                      */
	VM_I18N_HINT_HUD,
	VM_I18N_HINT_MEMCARD,
	VM_I18N_HINT_VERIFY,
	VM_I18N_HINT_LOG,
	VM_I18N_HINT_SAFE_ON,       /* the safe-mode row's help, by the state         */
	VM_I18N_HINT_SAFE_OFF,
	VM_I18N_TITLE_ADVANCED,     /* the "Advanced" sub-page                        */
	VM_I18N_MSG_SAFE_OK,        /* after the reset button                         */
	VM_I18N_MSG_SAFE_ERR,
	VM_I18N_ITEM_HUD_OFF,       /* HUD list items, in VM_HUD_* order              */
	VM_I18N_ITEM_HUD_COMPACT,
	VM_I18N_ITEM_HUD_EXTENDED,
	VM_I18N_ST_FALLBACK,        /* added to a storage's help when it isn't working */
	VM_I18N_ST_NO_BUFFER,
	VM_I18N_ST_FIRMWARE,
	VM_I18N_ST_HOOKS,
	VM_I18N_ST_SAFE,
	VM_I18N_ST_NO_KERNEL,
	VM_I18N_ST_SLOW,
	VM_I18N_STATUS_ACTIVE,      /* the status row's title, by VmState.disabled   */
	VM_I18N_STATUS_NO_KERNEL,
	VM_I18N_STATUS_SAFE,
	VM_I18N_STATUS_FIRMWARE,
	VM_I18N_STATUS_HOOKS,
	VM_I18N_HINT_STATUS_ACTIVE, /* ... and its help                              */
	VM_I18N_HINT_STATUS_NO_KERNEL,
	VM_I18N_ROW_SMALL,          /* Advanced > small transfers too                 */
	VM_I18N_HINT_SMALL,
	VM_I18N_ROW_STATS,          /* Advanced > statistics                          */
	VM_I18N_HINT_STATS,
	VM_I18N_ROW_GPUSAFE,        /* Advanced > GPU-safe reads                      */
	VM_I18N_HINT_GPUSAFE,
	VM_I18N_ROW_PIPELINE,       /* Advanced > Overlapped reads                    */
	VM_I18N_HINT_PIPELINE,
	VM_I18N_ITEM_PIPE_OFF,      /* its choices, in VM_PIPE_* order                */
	VM_I18N_ITEM_PIPE_AUTO,
	VM_I18N_ITEM_PIPE_FIXED,
	VM_I18N_ROW_OFFLOAD,        /* Advanced > Copy on the system core             */
	VM_I18N_HINT_OFFLOAD,
	VM_I18N_ITEM_OFF_OFF,       /* its choices, in VM_OFFLOAD_* order             */
	VM_I18N_ITEM_OFF_ALWAYS,
	VM_I18N_ITEM_OFF_ALL,       /* C + NC R/A                                     */
	VM_I18N_ITEM_OFF_FULL,      /* C + NC All                                     */
	VM_I18N_ROW_READAHEAD,      /* Advanced > Read ahead (experimental)           */
	VM_I18N_HINT_READAHEAD,
	VM_I18N_ITEM_RA_ON,         /* its "On" (Off and Auto: the pipeline's)        */
	VM_I18N_ROW_OVERLAY,        /* Advanced > Overlay (SELECT + arrows)           */
	VM_I18N_HINT_OVERLAY,
	VM_I18N_TITLE_DEVELOPER,
	VM_I18N_COUNT
};

const char *vm_i18n(int id);   /* "" for an unknown id */

/* UTF-8 -> UTF-16 (NUL-terminated, truncated to cap units). Returns the length. */
int vm_i18n_to_u16(const char *utf8, unsigned short *out, int cap);

#endif /* VITAMEMIO_I18N_H */
