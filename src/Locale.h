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

CC_END_HEADER
#endif
