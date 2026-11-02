#ifndef _TAPE_SD_H
#define _TAPE_SD_H

#include <circle/types.h>
#include <fatfs/ff.h>

// Patch the ROM tape routines with the ED FC / ED FD emulator traps
void TapePatchROM(unsigned char *pROM);

/*
 * The Ace's files (.tap, .tzx, .ace, .fth, .fs, .f) are kept in SD:/programs and its
 * subfolders, which can be any. Paths in this interface are relative to that folder,
 * with / between the folder names: "games/arcade/invaders.tap"; "" is programs itself.
 *
 * A .zip file is read like a folder (read only): "games/collection.zip/manic/manic.tap"
 * is manic/manic.tap in games/collection.zip (zip_sd.h). Its files are unpacked into
 * memory when they are used.
 */
#define TAPE_FOLDER         "programs"
#define TAPE_PATH_SIZE      512         // a path, with its terminating 0
#define TAPE_FULL_PATH_SIZE (sizeof("SD:/" TAPE_FOLDER "/") + TAPE_PATH_SIZE)

// Mount the SD card (EMMC device must already be initialized), make SD:/programs if
// it is missing, and move the Ace's files in the root of the card into it
boolean TapeInitialize(void);

// Forget any half-finished LOAD / SAVE and rewind the current tape (on Ace reset)
void TapeReset(void);

// Close any open file and unmount the SD card (before power off)
void TapeShutdown(void);

#define TAPE_LIST_NAME_SIZE 256     // FatFs long names are at most 255 characters

struct TTapeListEntry {
    char     Name[TAPE_LIST_NAME_SIZE];     // the name only, without the folder
    unsigned nSize;                         // bytes
    boolean  bFolder;                       // a subfolder, or a .zip (opened like one)
    boolean  bZip;                          // a .zip file
    boolean  bSnapshot;                     // .ace snapshot
    boolean  bSource;                       // .fth / .fs / .f Forth source (spooled)
                                            // (none of them: a .tap or .tzx tape)
};

// Fill pEntries with the subfolders of the folder pFolder, then its *.tap and *.tzx
// tapes (and *.ace snapshots and *.fth, *.fs and *.f sources if bWithSnapshots), each
// sorted by name. Returns the number of entries (at most nMax), or -1 if there is no
// SD card or no such folder.
int TapeListFiles(const char *pFolder, TTapeListEntry *pEntries, unsigned nMax,
                  boolean bWithSnapshots = FALSE);

enum TTapeFileResult {
    TapeFileOK,
    TapeFileNotFound,
    TapeFileInvalidName,
    TapeFileNoCard,
    TapeFileInvalidSnapshot,
    TapeFileExists,
    TapeFileTooBig,
    TapeFileZipUnsupported,                 // a .zip (or file in it) that cannot be read
    TapeFileReadOnly,                       // inside a .zip
    TapeFileError
};

// --- Paths ---

// A path of plain names (see is_valid_name() in tape_sd.cpp) joined by /; "" (programs
// itself) only if bAllowEmpty. bAscii: the names must be plain ASCII too (web page, new
// folders).
boolean TapeIsValidPath(const char *pPath, boolean bAllowEmpty, boolean bAscii = FALSE);

// pFolder/pName into pPath (TAPE_PATH_SIZE bytes); FALSE if too long
boolean TapeJoinPath(char *pPath, const char *pFolder, const char *pName);

// The part after the last /
const char *TapeBaseName(const char *pPath);

// The folder of a path into pFolder (TAPE_PATH_SIZE bytes): "" for a name in programs
void TapeParentFolder(const char *pPath, char *pFolder);

// SD:/programs/<path> into pFullPath (TAPE_FULL_PATH_SIZE bytes); FALSE if the path is
// not valid
boolean TapeFullPath(const char *pPath, char *pFullPath);

// The folder last shown by F5 (it may be in a .zip). If it no longer exists, the nearest
// folder above it that does.
const char *TapeGetFolder(void);
void TapeSetFolder(const char *pFolder);

// Where new files go (SAVE with no tape in the player, F5 N, F7): the F5 folder, or if
// that is in a .zip, the folder the .zip is in
const char *TapeGetSaveFolder(void);

// Is the path inside a .zip (not the .zip itself)?
boolean TapeIsInZip(const char *pPath);

// --- Reading a file, on the card or in a .zip (spooling, snapshots, the tape) ---

struct TTapeReader {
    FIL       File;
    u8       *pData;                        // a file unpacked from a .zip, or nullptr
    unsigned  nLength;
    unsigned  nPos;
};

TTapeFileResult TapeReaderOpen(TTapeReader *pReader, const char *pPath);
boolean TapeReaderRead(TTapeReader *pReader, void *pBuffer, unsigned nCount, unsigned *pRead);
boolean TapeReaderSeek(TTapeReader *pReader, FSIZE_t nPos);
FSIZE_t TapeReaderTell(TTapeReader *pReader);
void TapeReaderClose(TTapeReader *pReader);

// --- Folders ---

// Make a folder (its parent must exist); TapeFileExists if there is one
TTapeFileResult TapeMakeFolder(const char *pPath);

// Make a folder and those above it that are missing (uploads of folders); fine if it exists
TTapeFileResult TapeMakeFolders(const char *pPath);

// Is there a folder (bFolder) or file at the path? FALSE if neither
boolean TapeExists(const char *pPath, boolean *pbFolder);

// Count the files and folders in a folder and its subfolders
void TapeCountFolder(const char *pPath, unsigned *pFiles, unsigned *pFolders);

// Delete a folder with all its files and subfolders (F6, web page). The tape in the
// player is ejected if it was in it.
TTapeFileResult TapeDeleteFolder(const char *pPath);

// Rename or move a folder; the tape in the player and the F5 folder follow it.
// TapeFileExists rather than overwrite; TapeFileInvalidName to move it into itself.
TTapeFileResult TapeRenameFolder(const char *pFrom, const char *pTo);

// Call pHandler for each file in a folder and its subfolders, with the path of the file
// relative to pFolder; stops when it returns FALSE. FALSE if pFolder cannot be read.
typedef boolean TTapeTreeHandler(const char *pPath, unsigned nSize, void *pParam);
boolean TapeWalkFolder(const char *pFolder, TTapeTreeHandler *pHandler, void *pParam);


// --- Files (paths relative to programs) ---

// Insert a tape (F5): LOAD / BLOAD read from it first, from its start
TTapeFileResult TapeInsert(const char *pPath);

// The path of the current tape: inserted with F5, or the one the last file was loaded
// from ("" if none). SAVE appends to it (.tap or .tzx).
const char *TapeGetInserted(void);

// Take the current tape out (F5 E)
void TapeEject(void);

// A new, empty .tap or .tzx in the F5 folder (".tap" is added when the name ends in
// neither), inserted (F5 N). pPath gets its path (TAPE_PATH_SIZE bytes); fails with
// TapeFileExists rather than overwrite a file.
TTapeFileResult TapeCreate(const char *pInput, char *pPath);

// Delete a .tap / .tzx tape, .ace snapshot or .fth / .fs / .f source (F6)
TTapeFileResult TapeDeleteFile(const char *pPath);

// For the web page (F11): a path of plain ASCII names with an Ace extension
// (.tap, .tzx, .ace, .fth, .fs, .f) or .zip
boolean TapeIsAceFileName(const char *pPath);

// Read such a file whole into pBuffer (TapeFileTooBig if over nMax bytes); pDate / pTime
// (may be null) get its FAT date and time
TTapeFileResult TapeReadFile(const char *pName, u8 *pBuffer, unsigned nMax, unsigned *pLength,
                             u16 *pDate = nullptr, u16 *pTime = nullptr);

// Write such a file (TapeFileExists unless bReplace); a tape replaced while in the
// player is rewound
TTapeFileResult TapeWriteFile(const char *pName, const u8 *pData, unsigned nLength, boolean bReplace);

// Rename such a file (TapeFileExists rather than overwrite); the tape in the player
// follows its new name
TTapeFileResult TapeRenameFile(const char *pFrom, const char *pTo);

// The ROM gave an error (ED FB trap): a LOAD looking for its file has stopped
void TapeRomError(void);

// Load an .ace snapshot into the Ace (only while the Z80 is paused): memory now, CPU
// registers when mainloop() continues
TTapeFileResult TapeLoadSnapshot(const char *pPath);

// Save the Ace (memory and CPU registers, only while the Z80 is paused) as an .ace
// snapshot in the F5 folder (F7): ".ace" is added when there is no extension,
// other extensions are refused. pPath gets its path (TAPE_PATH_SIZE bytes); fails with
// TapeFileExists rather than overwrite a file.
TTapeFileResult TapeSaveSnapshot(const char *pInput, char *pPath);

#endif
