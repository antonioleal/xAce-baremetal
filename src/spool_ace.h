#ifndef _SPOOL_ACE_H
#define _SPOOL_ACE_H

#include <circle/types.h>

/*
 * Spooling (F5): a Forth source file (.fth, .fs, .f) on the SD card is typed into
 * the Ace as if from the keyboard, line by line, while the host keyboard is off
 * (Esc stops). Spooling stops at the end of the file and at the first line the
 * Ace rejects (left in the editor with "?"); after ERROR n it goes on.
 */

// Patch the ROM's error restart (RST 20h) with the ED FB trap
void SpoolPatchROM(unsigned char *pROM);

enum TSpoolResult {
    SpoolOK,
    SpoolNotFound,
    SpoolInvalidName,
    SpoolNoCard
};

// Start spooling SD:/<name>
TSpoolResult SpoolStart(const char *pInput);

// Ace keys that text from memory can type, besides characters and '\n' (Enter)
#define SPOOL_KEY_DELETE_LINE   0x01    // Shift+1
#define SPOOL_KEY_INVERSE_VIDEO 0x02    // Shift+4: inverse video on / off (it toggles)

// Type text from memory the same way (save_dictionary.cpp, line_history.cpp): nothing
// is logged and the keyboard is left as it is; the caller keeps pText until
// SpoolActive() is FALSE. Without bFinalEnter a last line without '\n' is left unentered.
boolean SpoolStartText(const char *pText, boolean bFinalEnter = TRUE);

boolean SpoolActive(void);

// Once per Ace frame: press / release the next key when the ROM wants input
void SpoolFrame(void);

#endif
