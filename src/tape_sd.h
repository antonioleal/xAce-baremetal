#ifndef _TAPE_SD_H
#define _TAPE_SD_H

#include <circle/types.h>

// Patch the ROM tape routines with the ED FC / ED FD emulator traps
void TapePatchROM(unsigned char *pROM);

// Mount the SD card (EMMC device must already be initialized)
boolean TapeInitialize(void);

// Forget any half-finished LOAD / SAVE and rewind the current tape (on Ace reset)
void TapeReset(void);

// Close any open file and unmount the SD card (before power off)
void TapeShutdown(void);

#define TAPE_LIST_NAME_SIZE 256     // FatFs long names are at most 255 characters

struct TTapeListEntry {
    char     Name[TAPE_LIST_NAME_SIZE];
    unsigned nSize;                         // bytes
    boolean  bSnapshot;                     // .ace snapshot
    boolean  bSource;                       // .fth / .fs / .f Forth source (spooled)
                                            // (neither: a .tap or .tzx tape)
};

// Fill pEntries with the *.tap and *.tzx tapes (and *.ace snapshots and *.fth, *.fs
// and *.f sources if bWithSnapshots) in the
// SD card root, sorted by name.
// Returns the number of entries (at most nMax), or -1 if there is no SD card.
int TapeListFiles(TTapeListEntry *pEntries, unsigned nMax, boolean bWithSnapshots = FALSE);

enum TTapeFileResult {
    TapeFileOK,
    TapeFileNotFound,
    TapeFileInvalidName,
    TapeFileNoCard,
    TapeFileInvalidSnapshot,
    TapeFileExists,
    TapeFileError
};

// Insert a tape (F5): LOAD / BLOAD read from it first, from its start
TTapeFileResult TapeInsert(const char *pName);

// The current tape: inserted with F5, or the one the last file was loaded
// from ("" if none). SAVE appends to it when it is a .tap.
const char *TapeGetInserted(void);

// Take the current tape out (F5 E)
void TapeEject(void);

// A new, empty .tap (".tap" is added when missing), inserted (F5 N). pName gets
// its file name; fails with TapeFileExists rather than overwrite a file.
TTapeFileResult TapeCreate(const char *pInput, char *pName, unsigned nNameSize);

// Delete a .tap / .tzx tape, .ace snapshot or .fth / .fs / .f source in the SD card root (F6)
TTapeFileResult TapeDeleteFile(const char *pName);

// Load an .ace snapshot from the SD card root into the Ace (only while the Z80
// is paused): memory now, CPU registers when mainloop() continues
TTapeFileResult TapeLoadSnapshot(const char *pName);

#endif
