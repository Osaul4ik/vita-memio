/*
 * vitamemio_i18n.h - text for the "vita-memio" Settings page.
 *
 * All 20 Vita system languages. English (US/UK) is authoritative; the others
 * are machine translations (see tools/gen_i18n.py). The language follows the
 * system setting and falls back to English (US).
 * C99, no char16_t: UTF-16 output uses plain unsigned short.
 */
#ifndef VITAMEMIO_I18N_H
#define VITAMEMIO_I18N_H

enum vm_i18n_id {
	VM_I18N_TITLE = 0,          /* "vita-memio"                                */
	VM_I18N_ROW_SD2VITA,        /* row titles                                     */
	VM_I18N_ROW_INTERNAL,
	VM_I18N_ROW_HUD,
	VM_I18N_ROW_MEMCARD,
	VM_I18N_ROW_VERIFY,
	VM_I18N_ROW_LOG,
	VM_I18N_ROW_HUD_STYLE,
	VM_I18N_ROW_SAFE,
	VM_I18N_HINT_SD2VITA,       /* help text                                      */
	VM_I18N_HINT_INTERNAL,
	VM_I18N_HINT_HUD,
	VM_I18N_HINT_MEMCARD,
	VM_I18N_HINT_VERIFY,
	VM_I18N_HINT_LOG,
	VM_I18N_HINT_HUD_STYLE,
	VM_I18N_HINT_SAFE_ON,       /* the safe-mode row's help, by the state         */
	VM_I18N_HINT_SAFE_OFF,
	VM_I18N_TITLE_ADVANCED,     /* the "Advanced" sub-page                        */
	VM_I18N_MSG_SAFE_OK,        /* after the reset button                         */
	VM_I18N_MSG_SAFE_ERR,
	VM_I18N_ITEM_HUD_OFF,       /* HUD list items, in VM_HUD_* order              */
	VM_I18N_ITEM_HUD_COMPACT,
	VM_I18N_ITEM_HUD_EXTENDED,
	VM_I18N_ITEM_STYLE_CLASSIC, /* HUD style items, in VM_HUD_STYLE_* order       */
	VM_I18N_ITEM_STYLE_PSVSHELL,
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
	VM_I18N_COUNT
};

/* Vita system-language indices (the raw /CONFIG/SYSTEM/language value). */
enum vm_lang_id {
	VM_LANG_JAPANESE = 0, VM_LANG_ENGLISH_US, VM_LANG_FRENCH, VM_LANG_SPANISH,
	VM_LANG_GERMAN, VM_LANG_ITALIAN, VM_LANG_DUTCH, VM_LANG_PORTUGUESE_PT,
	VM_LANG_RUSSIAN, VM_LANG_KOREAN, VM_LANG_CHINESE_T, VM_LANG_CHINESE_S,
	VM_LANG_FINNISH, VM_LANG_SWEDISH, VM_LANG_DANISH, VM_LANG_NORWEGIAN,
	VM_LANG_POLISH, VM_LANG_PORTUGUESE_BR, VM_LANG_ENGLISH_GB, VM_LANG_TURKISH,
	VM_LANG_COUNT
};

int vm_i18n_lang(void);
const char *vm_i18n_for(int id, int lang);
const char *vm_i18n(int id);   /* in the current system language */

/* UTF-8 -> UTF-16 (NUL-terminated, truncated to cap units). Returns the length. */
int vm_i18n_to_u16(const char *utf8, unsigned short *out, int cap);

#endif /* VITAMEMIO_I18N_H */
