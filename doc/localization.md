Localization (translating the interface into other languages)
=============================================================

ClassiCube can translate user interface text into other languages by loading a
*locale file*. The English text built into the source code stays as the
fallback, so any text that hasn't been translated simply shows in English.

Choosing a language
-------------------
The language can be changed from within ClassiCube, or by editing options:

* In the launcher: open the **Options** screen and click the **Language**
  button to cycle through the available languages.
* In game: open **Options... -> Misc options...** and use the **Language**
  setting.
* Manually: set `language=<code>` in `options.txt` (for example `language=es`).

After changing the language in game, reopen the menus (or restart) for all
text to update. The list of selectable languages is defined by `Locale_Names`
and `Locale_Codes` in `src/Locale.c`.

How it works
------------
* The active language is chosen by the `language` option in `options.txt`
  (for example `language=es` for Spanish).
* At startup (`Locale_Load`, called from `SetupProgram`), the game loads the
  matching file `locale/<language>.txt` from the working directory.
* When the launcher or in-game UI sets the text of a button or label through the
  constant text helpers (`ButtonWidget_SetConst`, `TextWidget_SetConst`,
  `LButton_SetConst`, `LLabel_SetConst`), the text is passed through
  `Locale_Translate`, which looks up a translation and uses it if one exists.
* Setting `language` to nothing, `en`, or `english` disables translation.

This means any UI text that already goes through those helpers becomes
translatable without changing the call site - only the locale file needs a
matching entry.

What is currently translated
----------------------------
* In-game button and text widgets (the pause menu, options menus, generate
  level screen, etc.)
* Block names shown in the inventory selection title. Only the *displayed* name
  is translated - the English name is still used internally for block lookup
  (e.g. `/place stone`) and when serializing levels, so saved maps and commands
  are unaffected.
* Launcher buttons, labels, option checkboxes, server list column headers, and
  text input hints.

`Locale_TranslateString` is the same as `Locale_Translate` but takes a
`cc_string` (used for text that isn't a compile time constant, such as block
names).

Supported characters
---------------------
Locale files are UTF-8 encoded, but ClassiCube converts the text to its internal
CP437 character set when reading. CP437 covers the common accented Spanish
letters (`á é í ó ú ñ ü ¿ ¡`), but it does **not** include the accented capital
letters `Á Í Ó Ú` (only `É Ñ Ü Ö` and friends exist). Any character without a
CP437 equivalent is shown as `?`, so avoid those (for example, write `Indigo`
rather than `Índigo`).

Locale file format
------------------
A locale file is a plain UTF-8 text file with one `key=value` pair per line:

    # comments and blank lines are ignored
    Done=Hecho
    Cancel=Cancelar
    Options...=Opciones...

* The key (before the first `=`) is the **original English text**, exactly as it
  appears in the interface (including any `&`-color codes, e.g. `&cCheck failed`).
* The value (after the first `=`) is the translated text shown in game.
* Lines starting with `#` and blank lines are ignored.
* Entries with no matching English text in the interface are harmless.

Adding a new language
---------------------
1. Copy `locale/es.txt` to `locale/<code>.txt` (e.g. `locale/fr.txt`).
2. Translate the right-hand side of each line.
3. Set `language=<code>` in `options.txt`.

The bundled `locale/es.txt` (Spanish) can be used as a reference for which
strings are commonly shown in the launcher and in-game menus.

Verifying a locale file
-----------------------
Run `python3 misc/verify_locale.py` to check every `locale/*.txt` file (or pass
specific files as arguments). It validates the format, flags duplicate keys,
ensures every character is representable in CP437, and confirms each key matches
a real English string in `src/` (catching typos and dead keys).

Relevant source files
---------------------
|File|Functionality|
|--------|-------|
|Locale|Loads a locale file and translates English UI text into the active language
