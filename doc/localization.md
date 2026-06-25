Localization (translating the interface into other languages)
=============================================================

ClassiCube can translate user interface text into other languages by loading a
*locale file*. The English text built into the source code stays as the
fallback, so any text that hasn't been translated simply shows in English.

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

Relevant source files
---------------------
|File|Functionality|
|--------|-------|
|Locale|Loads a locale file and translates English UI text into the active language
