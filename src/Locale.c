#include "Locale.h"
#include "String_.h"
#include "Options.h"
#include "Utils.h"
#include "Constants.h"

/* Loaded translation entries, each stored as a "key=value" string */
static CC_BIG_VAR struct StringsBuffer locale_entries;
static cc_bool locale_loaded;

#define LOCALE_SEPARATOR '='

/* "Español" - the ñ is written as the CP437 byte 0xA4 so it renders correctly */
/*   (the game uses CP437 internally, not UTF-8, for string literals) */
const char* const Locale_Names[LOCALE_COUNT] = { "English", "Espa\xA4ol" };
const char* const Locale_Codes[LOCALE_COUNT] = { "en",      "es"          };

void Locale_Load(void) {
	cc_string lang; char langBuffer[STRING_SIZE];
	cc_string path; char pathBuffer[FILENAME_SIZE + 1];

	Locale_Free();
	String_InitArray(lang, langBuffer);
	Options_Get(OPT_LANGUAGE, &lang, "");
	String_UNSAFE_TrimStart(&lang);
	String_UNSAFE_TrimEnd(&lang);

	/* English is the built-in language, so no translations are needed */
	if (!lang.length)                                 return;
	if (String_CaselessEqualsConst(&lang, "en"))      return;
	if (String_CaselessEqualsConst(&lang, "english")) return;

	String_InitArray(path, pathBuffer);
	String_Format1(&path, "locale/%s.txt", &lang);
	/* EntryList_Load takes a null terminated path */
	pathBuffer[path.length] = '\0';

	EntryList_Load(&locale_entries, pathBuffer, LOCALE_SEPARATOR, NULL);
	/* A missing or empty locale file simply leaves everything in English */
	locale_loaded = locale_entries.count > 0;
}

void Locale_Free(void) {
	StringsBuffer_Clear(&locale_entries);
	locale_loaded = false;
}

cc_string Locale_TranslateString(const cc_string* eng) {
	cc_string entry, key, value;
	int i;

	if (locale_loaded) {
		for (i = 0; i < locale_entries.count; i++) {
			entry = StringsBuffer_UNSAFE_Get(&locale_entries, i);
			String_UNSAFE_Separate(&entry, LOCALE_SEPARATOR, &key, &value);

			if (String_CaselessEquals(&key, eng)) return value;
		}
	}
	/* Fall back to the original English text when no translation is found */
	return *eng;
}

cc_string Locale_Translate(const char* eng) {
	cc_string str = String_FromReadonly(eng);
	return Locale_TranslateString(&str);
}

int Locale_GetActive(void) {
	cc_string lang; char langBuffer[STRING_SIZE];
	int i;

	String_InitArray(lang, langBuffer);
	Options_Get(OPT_LANGUAGE, &lang, Locale_Codes[0]);

	for (i = 0; i < LOCALE_COUNT; i++) {
		if (String_CaselessEqualsConst(&lang, Locale_Codes[i])) return i;
	}
	return 0; /* Default to English when unset or unrecognised */
}

void Locale_SetActive(int index) {
	cc_string code;
	if (index < 0 || index >= LOCALE_COUNT) index = 0;

	code = String_FromReadonly(Locale_Codes[index]);
	Options_Set(OPT_LANGUAGE, &code);
	Locale_Load();
}
