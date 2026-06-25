#ifndef CC_LOCALE_H
#define CC_LOCALE_H
#include "Core.h"
CC_BEGIN_HEADER

/*
Implements localisation - translating user interface text into other languages

The active language is selected via the "language" option (e.g. language=es)
Translations are loaded from "locale/<language>.txt", which is a list of
  key=value lines, where 'key' is the original English text and 'value' is
  the translated text for that language

Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

/* Loads the translation table for the language given by the "language" option */
/* Does nothing when the language is unset or set to English ("en"/"english") */
void Locale_Load(void);
/* Frees all loaded translations */
void Locale_Free(void);

/* Returns the translation of the given English text for the active language */
/* If no translation exists (or no language is loaded), returns the original text */
/* NOTE: The returned string references memory that is only valid until Locale_Free */
CC_API cc_string Locale_Translate(const char* eng);
/* Same as Locale_Translate, but takes the English text as a cc_string */
/* Useful for translating text that isn't a compile time constant (e.g. block names) */
CC_API cc_string Locale_TranslateString(const cc_string* eng);

/* Number of languages that can be selected in the user interface */
#define LOCALE_COUNT 2
/* Display name of each language, written in that language (e.g. "English", "Español") */
extern const char* const Locale_Names[LOCALE_COUNT];
/* Option/file code for each language (e.g. "en", "es" -> locale/es.txt) */
extern const char* const Locale_Codes[LOCALE_COUNT];

/* Returns the index (into Locale_Names/Locale_Codes) of the active language */
/* Returns 0 (English) when the language option is unset or unrecognised */
int Locale_GetActive(void);
/* Sets the active language by index, saves the option, and reloads translations */
void Locale_SetActive(int index);

CC_END_HEADER
#endif
