/*
 * hud_i18n.c - ASCII HUD word translations (see hud_i18n.h).
 *
 * Machine-quality, upper-case ASCII only (accents dropped so the 8x8 font can draw
 * them), short to fit a HUD line. Non-Latin-script languages (Japanese,
 * Russian, Korean, Chinese) fall back to English; corrections from native
 * speakers are welcome. Row index = Vita system-language id.
 * Column order: INTERNAL, ON, OFF, STANDARD, ERROR, UNAVAILABLE, ERR, VERIFY, MEMCARD, SLOW.
 */
#include "hud_i18n.h"

#define VM_HUD_LANG_COUNT 20

#define EN { "INTERNAL", "ON", "OFF", "STANDARD", "ERROR", "UNAVAILABLE", "ERR", "VERIFY", "MEMORY CARD", "SLOW" }

static const char *const T[VM_HUD_LANG_COUNT][HTR_COUNT] = {
/* 0  JA (non-Latin -> EN) */ EN,
/* 1  EN-US              */ EN,
/* 2  FR                 */ { "INTERNE", "ACTIVE", "DESACTIVE", "STANDARD", "ERREUR", "INDISPONIBLE", "ERR", "VERIF", "CARTE MEMOIRE", "LENT" },
/* 3  ES                 */ { "INTERNO", "ACTIVADO", "DESACTIVADO", "ESTANDAR", "ERROR", "NO DISPONIBLE", "ERR", "VERIF", "TARJ. MEMORIA", "LENTO" },
/* 4  DE                 */ { "INTERN", "AN", "AUS", "NORMAL", "FEHLER", "NICHT VERFUEGBAR", "FEHL", "PRUEF", "SPEICHERKARTE", "LANGSAM" },
/* 5  IT                 */ { "INTERNA", "ATTIVO", "DISATTIVATO", "STANDARD", "ERRORE", "NON DISPONIBILE", "ERR", "VERIFICA", "SCHEDA MEMORIA", "LENTO" },
/* 6  NL                 */ { "INTERN", "AAN", "UIT", "STANDAARD", "FOUT", "NIET BESCHIKBAAR", "FOUT", "CONTROLE", "GEHEUGENKAART", "TRAAG" },
/* 7  PT-PT              */ { "INTERNO", "LIGADO", "DESLIGADO", "PADRAO", "ERRO", "INDISPONIVEL", "ERR", "VERIF", "CARTAO MEMORIA", "LENTO" },
/* 8  RU (non-Latin->EN) */ EN,
/* 9  KO (non-Latin->EN) */ EN,
/* 10 ZH-T (non-Latin)   */ EN,
/* 11 ZH-S (non-Latin)   */ EN,
/* 12 FI                 */ { "SISAINEN", "PAALLA", "POIS", "VAKIO", "VIRHE", "EI KAYTETTAVISSA", "VIRH", "TARKISTUS", "MUISTIKORTTI", "HIDAS" },
/* 13 SV                 */ { "INTERN", "PA", "AV", "STANDARD", "FEL", "EJ TILLGANGLIG", "FEL", "KONTROLL", "MINNESKORT", "LANGSAM" },
/* 14 DA                 */ { "INTERN", "TIL", "FRA", "STANDARD", "FEJL", "UTILGAENGELIG", "FEJL", "KONTROL", "HUK.-KORT", "LANGSOM" },
/* 15 NO                 */ { "INTERN", "PA", "AV", "STANDARD", "FEIL", "UTILGJENGELIG", "FEIL", "KONTROLL", "MINNEKORT", "TREG" },
/* 16 PL                 */ { "WEWN.", "WL.", "WYL.", "STANDARD", "BLAD", "NIEDOSTEPNE", "BL.", "WERYF.", "KARTA PAMIECI", "WOLNO" },
/* 17 PT-BR              */ { "INTERNO", "LIGADO", "DESLIGADO", "PADRAO", "ERRO", "INDISPONIVEL", "ERR", "VERIF", "CARTAO MEMORIA", "LENTO" },
/* 18 EN-GB              */ EN,
/* 19 TR                 */ { "DAHILI", "ACIK", "KAPALI", "STANDART", "HATA", "KULLANILAMAZ", "HATA", "DOGRULAMA", "HAFIZA KARTI", "YAVAS" },
};

const char *hud_tr(int lang, int tok)
{
	if (tok < 0 || tok >= HTR_COUNT)
		return "";
	if (lang < 0 || lang >= VM_HUD_LANG_COUNT)
		lang = 1;                      /* English (US) fallback */
	return T[lang][tok];
}
