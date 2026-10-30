#ifndef _SAVE_DICTIONARY_H
#define _SAVE_DICTIONARY_H

#include <circle/types.h>

/*
 * Saving the dictionary as Forth source (F7): every word the user defined is
 * written to a .fth file on the SD card, so that spooling the file into a fresh
 * Ace (F5) rebuilds the same dictionary. Colon, DEFINER and COMPILER words
 * are written as the ROM's own LIST prints them; variables and constants with
 * their current values; other words (CREATE, words made by a definer, machine
 * code) as their bytes. The Ace runs the LISTs itself, at spooling speed.
 */

enum TSaveResult {
    SaveOK,
    SaveNoCard,
    SaveInvalidName,
    SaveExists,
    SaveNotAtPrompt,                // the Ace is busy or has a line being typed
    SaveNoWords,
    SaveError
};

// Start saving to SD:/<name>: a .fth, .fs or .f file (".fth" is added when there is
// no extension; other extensions are refused as SaveInvalidName).
// pName gets the file name, *pWords the number of words to save.
TSaveResult SaveDictionaryStart(const char *pInput, char *pName, unsigned nNameSize,
                                unsigned *pWords);

boolean SaveDictionaryActive(void);

// Once per Ace frame, after SpoolFrame()
void SaveDictionaryFrame(void);

// The result of the last save, once it has finished (then cleared): FALSE if none
boolean SaveDictionaryResult(char *pLine1, char *pLine2, unsigned nSize);

#endif
