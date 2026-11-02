/*
 * Jupiter Ace tape emulation on the SD card (.tap and .tzx files via FatFs).
 *
 * The ROM block read (0x18A7) and block write (0x1820) routines are
 * replaced by the ED FC / ED FD traps, which call tape_load_p() and
 * tape_save_p(). The ROM's block read loads with carry set and verifies with it
 * clear (VERIFY / BVERIFY): then the block is compared with memory, not loaded. A .tap file is a sequence of blocks:
 *     length (2 bytes, little endian, includes checksum), data, XOR checksum
 * Each Ace file is a 25 byte header block followed by a data block.
 * A .tzx file holds the same blocks in TZX data blocks, each with a flag byte
 * first (00h header, FFh data) that .tap files leave out (see read_block()).
 *
 * The block read is done as the ROM does it, on the tape in the player (inserted
 * with F5) only: the next block, with the flag byte, length and checksum asked
 * for. The ROM's LOAD reads headers until one has the wanted name, showing
 * each; once round the tape without finding it (or with no tape in the player)
 * the trap gives the ROM's ERROR 10. Without a name nothing matches, so the
 * files on the tape are listed, from its start.
 * SAVE name appends to the current tape (as on a cassette): to a .tap as above,
 * to a .tzx as standard speed data blocks (10h) with the flag byte; with no
 * tape in the player it writes <name>.tap in the F5 folder (overwriting it).
 *
 * The files are in SD:/programs and its subfolders (see tape_sd.h).
 *
 * F5 also loads .ace snapshots (memory and CPU registers), see TapeLoadSnapshot().
 *
 * Based on the xAce tape.c by Edward Patel and Lawrence Woodman.
 */
#include "tape_sd.h"
#include "ram_ace.h"
#include "zip_sd.h"

#include <circle/logger.h>
#include <circle/util.h>
#include <fatfs/ff.h>

extern "C" {
#include "z80.h"
}

#define DRIVE           "SD:"
#define PROGRAMS        DRIVE "/" TAPE_FOLDER       // SD:/programs
#define PROGRAMS_LEN    (sizeof(PROGRAMS) - 1)

#define HEADER_SIZE     25          // header block payload
#define NAME_SIZE       10
#define REQUEST_HEADER  0x2701      // header the ROM is looking for
#define HEADER_BUFFER   0x231A      // where the ROM reads headers to

#define MAX_PATH        TAPE_FULL_PATH_SIZE
#define MAX_DEPTH       32          // folder levels walked by TapeWalkFolder()

static const char FromTape[] = "tape";

static FATFS s_FileSystem;
static boolean s_bMounted = FALSE;


// The folder last shown by F5, relative to SD:/programs
static char s_Folder[TAPE_PATH_SIZE] = "";

// Tape the last file was loaded from, for multi-file tapes (full path; in a .zip:
// SD:/programs/<zip>/<file in it>, with the file unpacked in s_pTapeData)
static char s_CurrentTape[MAX_PATH] = "";
static u8 *s_pTapeData = nullptr;
static unsigned s_nTapeLength = 0;
static FSIZE_t s_nCurrentPos = 0;
static boolean s_bAfterHeader = FALSE;      // .tap: the last block read was a header

// The ROM's LOAD looking for a header: once round the tape from s_nSearchStart
static boolean s_bSearching = FALSE;
static FSIZE_t s_nSearchStart = 0;
static boolean s_bSearchWrapped = FALSE;

// Current SAVE
static FIL s_SaveFile;
static boolean s_bSaveOpen = FALSE;
static boolean s_bSaveTzx = FALSE;          // writing TZX blocks

// .ace snapshot registers for mainloop() (z80.h)
extern "C" {
    struct z80_snapshot snapshot;
    int load_snapshot = 0;
    struct z80_snapshot cpu_state;
}

void TapePatchROM(unsigned char *pROM)
{
    pROM[0x18a7] = 0xed;    // tape_load_p
    pROM[0x18a8] = 0xfc;
    pROM[0x18a9] = 0xc9;

    pROM[0x1820] = 0xed;    // tape_save_p
    pROM[0x1821] = 0xfd;
    pROM[0x1822] = 0xc9;
}

static boolean is_ace_extension(const char *pName);

// The Ace's files in the root of the card (where they were kept before 0.7, and where
// a computer may put them) go into SD:/programs. A file whose name is taken there stays.
static void move_root_files(void)
{
    static char Names[32][TAPE_LIST_NAME_SIZE];
    for (;;) {
        // Gather some names, then move them: the root is not read while it changes
        DIR Dir;
        FILINFO Info;
        if (f_opendir(&Dir, DRIVE "/") != FR_OK) {
            return;
        }
        unsigned nNames = 0;
        while (nNames < 32 && f_readdir(&Dir, &Info) == FR_OK && Info.fname[0] != '\0') {
            if (!(Info.fattrib & (AM_DIR | AM_HID | AM_SYS)) && is_ace_extension(Info.fname)) {
                strcpy(Names[nNames++], Info.fname);
            }
        }
        f_closedir(&Dir);

        unsigned nMoved = 0;
        for (unsigned i = 0; i < nNames; i++) {
            char From[MAX_PATH], To[MAX_PATH];
            strcpy(From, DRIVE "/");
            strcat(From, Names[i]);
            strcpy(To, PROGRAMS "/");
            strcat(To, Names[i]);
            if (f_rename(From, To) == FR_OK) {
                nMoved++;
                CLogger::Get()->Write(FromTape, LogNotice, "Moved %s to %s", From, To);
            } else {
                CLogger::Get()->Write(FromTape, LogWarning, "Cannot move %s to %s", From, To);
            }
        }
        if (nNames < 32 || nMoved == 0) {
            return;                     // all done, or only files that cannot move are left
        }
    }
}

boolean TapeInitialize(void)
{
    if (f_mount(&s_FileSystem, DRIVE, 1) != FR_OK) {
        CLogger::Get()->Write(FromTape, LogWarning, "Cannot mount SD card, tape disabled");
        return FALSE;
    }

    s_bMounted = TRUE;
    CLogger::Get()->Write(FromTape, LogNotice, "SD card mounted");

    FILINFO Info;
    if (f_stat(PROGRAMS, &Info) != FR_OK) {
        FRESULT Result = f_mkdir(PROGRAMS);
        CLogger::Get()->Write(FromTape, Result == FR_OK ? LogNotice : LogError,
                              Result == FR_OK ? "Made %s" : "Cannot make %s", PROGRAMS);
    }
    move_root_files();

    return TRUE;
}

void TapeReset(void)
{
    s_bSearching = FALSE;
    s_nCurrentPos = 0;
    s_bAfterHeader = FALSE;                      // the current tape stays in, rewound

    if (s_bSaveOpen) {
        f_close(&s_SaveFile);
        s_bSaveOpen = FALSE;
    }
}

void TapeShutdown(void)
{
    TapeReset();

    if (s_bMounted) {
        f_mount(0, DRIVE, 0);
        s_bMounted = FALSE;
    }
}

// Write bytes into Ace memory through the Z80 store path (ROM protection, mirrors)
static void poke_block(unsigned short nDest, const unsigned char *pData, unsigned nSize)
{
    for (unsigned i = 0; i < nSize && nDest + i < 0x10000; i++) {
        unsigned short nAddr = (unsigned short) (nDest + i);
        store(nAddr, pData[i]);
    }
}

static void folder_full_path(char *pFullPath);

// Ace name (10 chars, space padded) -> <F5 folder>/<name>.tap with FAT-safe characters
static void make_path(char *pPath, const unsigned char *pName)
{
    char Name[NAME_SIZE + 1];
    int nLen = 0;

    for (int i = 0; i < NAME_SIZE; i++) {
        unsigned char c = pName[i] & 0x7F;
        if (   (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            Name[i] = c;
        } else {
            Name[i] = c == ' ' ? ' ' : '_';
        }
        if (Name[i] != ' ') {
            nLen = i + 1;
        }
    }
    Name[nLen] = '\0';

    // Embedded spaces are fine in long file names; trailing ones are dropped
    folder_full_path(pPath);
    strcat(pPath, "/");
    strcat(pPath, nLen > 0 ? Name : "noname");
    strcat(pPath, ".tap");
}

// LOAD without a name: the ROM stores 00h then spaces
static boolean is_blank_name(const unsigned char *pName)
{
    if (pName[0] == 0x00) {
        return TRUE;
    }
    for (int i = 0; i < NAME_SIZE; i++) {
        if (pName[i] != ' ') {
            return FALSE;
        }
    }
    return TRUE;
}

// pExt: e.g. ".tap" (any case in pName)
static boolean has_extension(const char *pName, const char *pExt)
{
    size_t nLen = strlen(pName), nExt = strlen(pExt);
    return nLen > nExt && strcasecmp(pName + nLen - nExt, pExt) == 0;
}

// A Forth source: .fth, .fs or .f
static boolean has_source_extension(const char *pName)
{
    return has_extension(pName, ".fth") || has_extension(pName, ".fs") || has_extension(pName, ".f");
}

// Start of a .tzx file: signature and format version 1.20
static const unsigned char s_TzxHeader[10] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20};
#define TZX_PAUSE_MS    1000        // silence after each saved block

// A tape: .tap or .tzx
static boolean has_tape_extension(const char *pName)
{
    return has_extension(pName, ".tap") || has_extension(pName, ".tzx");
}

// --- Reading a file on the card or unpacked from a .zip ---------------------------

static boolean split_zip(const char *pPath, char *pZipPath, char *pInside);

static TTapeFileResult zip_result(TZipResult Result)
{
    switch (Result) {
    case ZipOK:             return TapeFileOK;
    case ZipNotFound:       return TapeFileNotFound;
    case ZipTooBig:         return TapeFileTooBig;
    case ZipNotZip:
    case ZipUnsupported:
    case ZipBadData:        return TapeFileZipUnsupported;
    default:                return TapeFileError;
    }
}

// A file unpacked from a .zip, into *ppData (delete [] it)
static TTapeFileResult unpack(const char *pPath, u8 **ppData, unsigned *pLength)
{
    char Zip[MAX_PATH], Inside[TAPE_PATH_SIZE];
    if (!split_zip(pPath, Zip, Inside) || Inside[0] == '\0') {
        return TapeFileNotFound;
    }
    unsigned nPrefix;
    TZipResult Result = ZipOpen(Zip, Inside, &nPrefix);
    if (Result != ZipOK) {
        return zip_result(Result);
    }
    if (Inside[nPrefix] == '\0') {
        return TapeFileNotFound;        // a .zip in the .zip, not a file to read
    }
    return zip_result(ZipExtract(Inside + nPrefix, ppData, pLength));
}

TTapeFileResult TapeReaderOpen(TTapeReader *pReader, const char *pPath)
{
    pReader->pData = nullptr;
    pReader->nLength = pReader->nPos = 0;
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    if (TapeIsInZip(pPath)) {
        return unpack(pPath, &pReader->pData, &pReader->nLength);
    }
    char Full[MAX_PATH];
    if (!TapeFullPath(pPath, Full)) {
        return TapeFileInvalidName;
    }
    return f_open(&pReader->File, Full, FA_READ | FA_OPEN_EXISTING) == FR_OK ? TapeFileOK
                                                                            : TapeFileNotFound;
}

boolean TapeReaderRead(TTapeReader *pReader, void *pBuffer, unsigned nCount, unsigned *pRead)
{
    if (pReader->pData == nullptr) {
        UINT nRead = 0;
        FRESULT Result = f_read(&pReader->File, pBuffer, nCount, &nRead);
        *pRead = nRead;
        return Result == FR_OK;
    }
    unsigned nLeft = pReader->nPos < pReader->nLength ? pReader->nLength - pReader->nPos : 0;
    *pRead = nCount < nLeft ? nCount : nLeft;
    memcpy(pBuffer, pReader->pData + pReader->nPos, *pRead);
    pReader->nPos += *pRead;
    return TRUE;
}

boolean TapeReaderSeek(TTapeReader *pReader, FSIZE_t nPos)
{
    if (pReader->pData == nullptr) {
        return f_lseek(&pReader->File, nPos) == FR_OK;
    }
    pReader->nPos = nPos < pReader->nLength ? (unsigned) nPos : pReader->nLength;
    return TRUE;
}

FSIZE_t TapeReaderTell(TTapeReader *pReader)
{
    return pReader->pData == nullptr ? f_tell(&pReader->File) : pReader->nPos;
}

void TapeReaderClose(TTapeReader *pReader)
{
    if (pReader->pData == nullptr) {
        f_close(&pReader->File);
    } else {
        delete [] pReader->pData;
        pReader->pData = nullptr;
    }
}

static boolean read_bytes(TTapeReader *pFile, unsigned char *pBuffer, unsigned nCount)
{
    unsigned nRead;
    return TapeReaderRead(pFile, pBuffer, nCount, &nRead) && nRead == nCount;
}

static boolean skip_bytes(TTapeReader *pFile, FSIZE_t nCount)
{
    return TapeReaderSeek(pFile, TapeReaderTell(pFile) + nCount);
}

static unsigned le(const unsigned char *p, unsigned nBytes)
{
    unsigned nValue = 0;
    while (nBytes-- > 0) {
        nValue = (nValue << 8) | p[nBytes];
    }
    return nValue;
}

/*
 * TZX: find the next data block (the file is left at its flag byte). Data blocks are the
 * standard (10h), turbo (11h) and pure data (14h) blocks, and generalized data
 * blocks (19h) with one bit per symbol. Other blocks are skipped, including
 * direct recordings (15h) and CSW (18h), which hold sampled sound, not bytes.
 */
static boolean next_tzx_block(TTapeReader *pFile, unsigned *pSize)
{
    if (TapeReaderTell(pFile) == 0) {
        unsigned char Header[10];
        if (!read_bytes(pFile, Header, sizeof(Header)) || memcmp(Header, "ZXTape!\x1A", 8) != 0) {
            return FALSE;
        }
    }

    for (;;) {
        unsigned char ID;
        if (!read_bytes(pFile, &ID, 1)) {
            return FALSE;
        }

        unsigned char Field[18];
        unsigned nData = 0;                 // data bytes in a data block
        FSIZE_t nSkip = 0;                  // bytes to skip in other blocks

        switch (ID) {
        case 0x10:                          // standard speed data
            if (!read_bytes(pFile, Field, 4)) return FALSE;
            nData = le(Field + 2, 2);
            break;
        case 0x11:                          // turbo speed data
            if (!read_bytes(pFile, Field, 18)) return FALSE;
            nData = le(Field + 15, 3);
            break;
        case 0x14:                          // pure data
            if (!read_bytes(pFile, Field, 10)) return FALSE;
            nData = le(Field + 7, 3);
            break;
        case 0x19: {                        // generalized data
            if (!read_bytes(pFile, Field, 18)) return FALSE;
            FSIZE_t nEnd = TapeReaderTell(pFile) - 14 + le(Field, 4);
            unsigned nPilotSymbols = le(Field + 6, 4), nPilotPulses = Field[10];
            unsigned nPilotAlphabet = Field[11] ? Field[11] : 256;
            unsigned nDataSymbols = le(Field + 12, 4), nDataPulses = Field[16];
            unsigned nDataAlphabet = Field[17] ? Field[17] : 256;
            FSIZE_t nTables = 0;
            if (nPilotSymbols > 0) {
                nTables += nPilotAlphabet * (1 + 2 * nPilotPulses) + nPilotSymbols * 3;
            }
            if (nDataSymbols > 0) {
                nTables += nDataAlphabet * (1 + 2 * nDataPulses);
            }
            if (nDataAlphabet == 2 && nDataSymbols >= 16 && nDataSymbols % 8 == 0) {
                if (!skip_bytes(pFile, nTables)) return FALSE;
                nData = nDataSymbols / 8;   // one bit per symbol: the stream is the bytes
            } else {
                nSkip = nEnd - TapeReaderTell(pFile);
            }
            break;
        }
        case 0x12: nSkip = 4; break;
        case 0x13: { unsigned char n; if (!read_bytes(pFile, &n, 1)) return FALSE; nSkip = n * 2; break; }
        case 0x15: if (!read_bytes(pFile, Field, 8)) return FALSE; nSkip = le(Field + 5, 3); break;
        case 0x20: case 0x23: case 0x24: nSkip = 2; break;
        case 0x21: case 0x30: { unsigned char n; if (!read_bytes(pFile, &n, 1)) return FALSE; nSkip = n; break; }
        case 0x22: case 0x25: case 0x27: nSkip = 0; break;
        case 0x26: if (!read_bytes(pFile, Field, 2)) return FALSE; nSkip = le(Field, 2) * 2; break;
        case 0x28: case 0x32: if (!read_bytes(pFile, Field, 2)) return FALSE; nSkip = le(Field, 2); break;
        case 0x31: if (!read_bytes(pFile, Field, 2)) return FALSE; nSkip = Field[1]; break;
        case 0x33: { unsigned char n; if (!read_bytes(pFile, &n, 1)) return FALSE; nSkip = n * 3; break; }
        case 0x35: if (!read_bytes(pFile, Field, 14)) return FALSE; nSkip = le(Field + 10, 4); break;
        case 0x5A: nSkip = 9; break;
        default:                            // 18h, 2Ah, 2Bh and later blocks start with their length
            if (!read_bytes(pFile, Field, 4)) return FALSE;
            nSkip = le(Field, 4);
            break;
        }

        if (nData >= 2) {
            *pSize = nData;                 // flag byte, bytes, checksum
            return TRUE;
        }
        if (!skip_bytes(pFile, nSkip + nData)) {
            return FALSE;
        }
    }
}

// The block being read: up to 64K bytes and the checksum
static unsigned char s_Block[0x10000 + 1];

/*
 * Read the next block on the current tape from s_nCurrentPos, which moves past
 * it: *pFlag gets its flag byte (00h header, FFh data), s_Block its bytes and
 * checksum (*pSize bytes, at most sizeof(s_Block)), *pTotal the bytes and
 * checksum it really has. .tap files have no flag byte: a block is a header
 * when it has the header's size and does not follow a header (it is then the
 * header's data). FALSE at the end of the tape.
 */
static boolean read_block(unsigned char *pFlag, unsigned *pSize, unsigned *pTotal)
{
    // The tape: on the card, or unpacked from a .zip when it was inserted
    TTapeReader File;
    File.pData = s_pTapeData;
    File.nLength = s_nTapeLength;
    File.nPos = 0;
    if (s_pTapeData == nullptr && f_open(&File.File, s_CurrentTape, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return FALSE;
    }
    if (s_nCurrentPos > 0 && !TapeReaderSeek(&File, s_nCurrentPos)) {
        if (s_pTapeData == nullptr) {
            f_close(&File.File);
        }
        return FALSE;
    }

    boolean bOK;
    unsigned nTotal = 0;
    if (has_extension(s_CurrentTape, ".tzx")) {
        bOK = next_tzx_block(&File, &nTotal) && read_bytes(&File, pFlag, 1);
        nTotal--;
    } else {
        unsigned char Len[2];
        bOK = read_bytes(&File, Len, 2);
        nTotal = le(Len, 2);
        *pFlag = !s_bAfterHeader && nTotal == HEADER_SIZE + 1 ? 0x00 : 0xFF;
    }

    if (bOK) {
        unsigned nWant = nTotal < sizeof(s_Block) ? nTotal : sizeof(s_Block);
        unsigned nRead;
        bOK =    TapeReaderRead(&File, s_Block, nWant, &nRead)
              && skip_bytes(&File, nTotal - nRead);
        *pSize = nRead;                 // less than nTotal if the file ends early
        *pTotal = nTotal;
        s_nCurrentPos = TapeReaderTell(&File);
        s_bAfterHeader = *pFlag == 0x00;
    }

    if (s_pTapeData == nullptr) {
        f_close(&File.File);            // (the unpacked tape stays while it is in)
    }

    return bOK;
}

// A file the Ace uses: tape, snapshot or Forth source
static boolean is_ace_file(const char *pName)
{
    return has_tape_extension(pName) || has_extension(pName, ".ace") || has_source_extension(pName);
}

// One of those, or a .zip (the files kept in programs)
static boolean is_ace_extension(const char *pName)
{
    return is_ace_file(pName) || has_extension(pName, ".zip");
}

// One plain name: no paths, drives or wildcards
static boolean is_valid_name(const char *pInput)
{
    if (pInput[0] == '\0' || pInput[0] == ' ' || pInput[0] == '.') {
        return FALSE;
    }
    for (const char *p = pInput; *p != '\0'; p++) {
        if ((unsigned char) *p < 0x20 || strchr("\\/:*?\"<>|", *p) != nullptr) {
            return FALSE;
        }
    }
    return TRUE;
}

// --- Paths --------------------------------------------------------------------

// Cut a path at its last / (Circle has no strrchr())
static void strip_last_name(char *pPath)
{
    char *pSlash = nullptr;
    for (char *p = pPath; *p != '\0'; p++) {
        if (*p == '/') {
            pSlash = p;
        }
    }
    if (pSlash != nullptr) {
        *pSlash = '\0';
    }
}

boolean TapeIsValidPath(const char *pPath, boolean bAllowEmpty, boolean bAscii)
{
    if (pPath[0] == '\0') {
        return bAllowEmpty;
    }
    if (strlen(pPath) >= TAPE_PATH_SIZE) {
        return FALSE;
    }

    // Each name between the / on its own
    const char *pStart = pPath;
    for (;;) {
        const char *pEnd = strchr(pStart, '/');
        unsigned nLen = pEnd != nullptr ? (unsigned) (pEnd - pStart) : strlen(pStart);
        if (nLen == 0 || nLen > FF_MAX_LFN) {
            return FALSE;               // empty (// or / at an end) or too long
        }
        char Name[FF_MAX_LFN + 1];
        memcpy(Name, pStart, nLen);
        Name[nLen] = '\0';
        if (!is_valid_name(Name) || Name[nLen - 1] == ' ' || Name[nLen - 1] == '.') {
            return FALSE;               // FAT drops trailing dots and spaces
        }
        if (bAscii) {
            for (const char *p = Name; *p != '\0'; p++) {
                if ((unsigned char) *p >= 0x7F) {
                    return FALSE;
                }
            }
        }
        if (pEnd == nullptr) {
            return TRUE;
        }
        pStart = pEnd + 1;
    }
}

boolean TapeJoinPath(char *pPath, const char *pFolder, const char *pName)
{
    unsigned nFolder = strlen(pFolder), nName = strlen(pName);
    if (nFolder + 1 + nName >= TAPE_PATH_SIZE) {
        return FALSE;
    }
    strcpy(pPath, pFolder);
    if (nFolder > 0 && nName > 0) {
        strcat(pPath, "/");
    }
    strcat(pPath, pName);
    return TRUE;
}

const char *TapeBaseName(const char *pPath)
{
    const char *pName = pPath;
    for (const char *p = pPath; *p != '\0'; p++) {
        if (*p == '/') {
            pName = p + 1;
        }
    }
    return pName;
}

void TapeParentFolder(const char *pPath, char *pFolder)
{
    const char *pName = TapeBaseName(pPath);
    unsigned nLen = pName > pPath ? (unsigned) (pName - pPath - 1) : 0;
    memcpy(pFolder, pPath, nLen);
    pFolder[nLen] = '\0';
}

boolean TapeFullPath(const char *pPath, char *pFullPath)
{
    if (!TapeIsValidPath(pPath, TRUE)) {
        return FALSE;
    }
    strcpy(pFullPath, PROGRAMS);
    if (pPath[0] != '\0') {
        strcat(pFullPath, "/");
        strcat(pFullPath, pPath);
    }
    return TRUE;
}

// If a name in pPath is a .zip file: its full path into pZipPath (MAX_PATH bytes) and
// the rest of pPath (the path in it, "" for the .zip itself) into pInside
// (TAPE_PATH_SIZE bytes). FALSE if there is no .zip on the way (a folder may be called
// "x.zip").
static boolean split_zip(const char *pPath, char *pZipPath, char *pInside)
{
    if (!TapeIsValidPath(pPath, FALSE)) {
        return FALSE;
    }
    unsigned nLen = strlen(pPath);
    for (unsigned i = 0; i <= nLen; i++) {
        if (pPath[i] != '/' && pPath[i] != '\0') {
            continue;
        }
        if (i >= 4 && strncasecmp(pPath + i - 4, ".zip", 4) == 0) {
            char Prefix[TAPE_PATH_SIZE];
            memcpy(Prefix, pPath, i);
            Prefix[i] = '\0';
            FILINFO Info;
            if (   TapeFullPath(Prefix, pZipPath) && f_stat(pZipPath, &Info) == FR_OK
                && !(Info.fattrib & AM_DIR)) {
                strcpy(pInside, pPath[i] == '/' ? pPath + i + 1 : "");
                return TRUE;
            }
        }
    }
    return FALSE;
}

boolean TapeIsInZip(const char *pPath)
{
    char Zip[MAX_PATH], Inside[TAPE_PATH_SIZE];
    return split_zip(pPath, Zip, Inside) && Inside[0] != '\0';
}

// Is there a folder in a .zip: pInside ("" for the .zip itself) is the start of the
// path of a file in it, or a .zip in it? Opens that .zip (see ZipOpen()); *pnPrefix
// gets the length of the part of pInside that took.
struct TZipFolderParam {
    const char *pInside;
    unsigned    nInside;
    boolean     bFound;
};

static boolean zip_folder_handler(const TZipEntry *pEntry, void *pParam)
{
    TZipFolderParam *pFolder = (TZipFolderParam *) pParam;
    if (   strncmp(pEntry->pName, pFolder->pInside, pFolder->nInside) == 0
        && (pEntry->pName[pFolder->nInside] == '/'
            || (pEntry->bFolder && pEntry->pName[pFolder->nInside] == '\0'))) {
        pFolder->bFound = TRUE;
        return FALSE;
    }
    return TRUE;
}

static boolean zip_folder_exists(const char *pZipPath, const char *pInside,
                                  unsigned *pnPrefix = nullptr)
{
    unsigned nPrefix;
    if (ZipOpen(pZipPath, pInside, &nPrefix) != ZipOK) {
        return FALSE;
    }
    if (pnPrefix != nullptr) {
        *pnPrefix = nPrefix;
    }
    pInside += nPrefix;
    if (pInside[0] == '\0') {
        return TRUE;
    }
    TZipFolderParam Param = {pInside, (unsigned) strlen(pInside), FALSE};
    return ZipForEach(zip_folder_handler, &Param) == ZipOK && Param.bFound;
}

// SD:/programs/<path> -> <path>
static const char *relative_path(const char *pFullPath)
{
    return pFullPath[PROGRAMS_LEN] == '/' ? pFullPath + PROGRAMS_LEN + 1 : "";
}

// Is pPath pFolder or inside it (any case, as FAT names)?
static boolean is_in_folder(const char *pPath, const char *pFolder)
{
    unsigned n = strlen(pFolder);
    return strncasecmp(pPath, pFolder, n) == 0 && (pPath[n] == '\0' || pPath[n] == '/');
}

static boolean is_folder(const char *pFullPath)
{
    if (strcmp(pFullPath, PROGRAMS) == 0) {
        return TRUE;                    // f_stat() does not report on it reliably
    }
    FILINFO Info;
    return f_stat(pFullPath, &Info) == FR_OK && (Info.fattrib & AM_DIR);
}

// A folder on the card, or in a .zip
static boolean folder_exists(const char *pPath)
{
    char Full[MAX_PATH], Inside[TAPE_PATH_SIZE];
    if (split_zip(pPath, Full, Inside)) {
        return zip_folder_exists(Full, Inside);
    }
    return TapeFullPath(pPath, Full) && is_folder(Full);
}

const char *TapeGetFolder(void)
{
    // A folder deleted or renamed meanwhile (on the web page): the nearest one above
    while (s_Folder[0] != '\0' && !folder_exists(s_Folder)) {
        TapeParentFolder(s_Folder, s_Folder);
    }
    return s_Folder;
}

const char *TapeGetSaveFolder(void)
{
    static char Folder[TAPE_PATH_SIZE];
    strcpy(Folder, TapeGetFolder());
    char Zip[MAX_PATH], Inside[TAPE_PATH_SIZE];
    if (split_zip(Folder, Zip, Inside)) {
        TapeParentFolder(relative_path(Zip), Folder);   // the folder the .zip is in
    }
    return Folder;
}

void TapeSetFolder(const char *pFolder)
{
    if (TapeIsValidPath(pFolder, TRUE)) {
        strcpy(s_Folder, pFolder);
    }
}

// Where new files go, as a full path
static void folder_full_path(char *pFullPath)
{
    TapeFullPath(TapeGetSaveFolder(), pFullPath);
}

// pName where new files go, as a full path; FALSE if too long
static boolean new_file_path(const char *pName, char *pFullPath)
{
    char Path[TAPE_PATH_SIZE];
    return TapeJoinPath(Path, TapeGetSaveFolder(), pName) && TapeFullPath(Path, pFullPath);
}

// The tape in the player is no longer there
static void forget_tape(void)
{
    delete [] s_pTapeData;
    s_pTapeData = nullptr;
    s_nTapeLength = 0;
    s_CurrentTape[0] = '\0';
    s_nCurrentPos = 0;
    s_bAfterHeader = FALSE;
    s_bSearching = FALSE;
}

// --- Listing --------------------------------------------------------------------

// Add an entry to a list sorted with folders (and .zip files) first, then by name
// ignoring case
static void add_entry(TTapeListEntry *pEntries, unsigned *pCount, const char *pName,
                      unsigned nSize, boolean bFolder, boolean bZip, boolean bSnapshot,
                      boolean bSource)
{
    unsigned i = (*pCount)++;
    while (i > 0 && (   (bFolder && !pEntries[i - 1].bFolder)
                     || (   bFolder == pEntries[i - 1].bFolder
                         && strcasecmp(pEntries[i - 1].Name, pName) > 0))) {
        pEntries[i] = pEntries[i - 1];
        i--;
    }
    strncpy(pEntries[i].Name, pName, TAPE_LIST_NAME_SIZE - 1);
    pEntries[i].Name[TAPE_LIST_NAME_SIZE - 1] = '\0';
    pEntries[i].nSize = nSize;
    pEntries[i].bFolder = bFolder;
    pEntries[i].bZip = bZip;
    pEntries[i].bSnapshot = bSnapshot;
    pEntries[i].bSource = bSource;
}

// Listing a folder in a .zip: its files' paths give the subfolders
struct TZipListParam {
    const char     *pInside;            // "" for the top of the .zip
    unsigned        nInside;
    TTapeListEntry *pEntries;
    unsigned        nMax;
    unsigned        nCount;
    boolean         bWithSnapshots;
};

static boolean zip_list_handler(const TZipEntry *pEntry, void *pParam)
{
    TZipListParam *pList = (TZipListParam *) pParam;
    const char *pRest = pEntry->pName;
    if (pList->nInside > 0) {
        if (strncmp(pRest, pList->pInside, pList->nInside) != 0 || pRest[pList->nInside] != '/') {
            return TRUE;
        }
        pRest += pList->nInside + 1;
    }
    if (pRest[0] == '\0' || strncmp(pEntry->pName, "__MACOSX/", 9) == 0) {
        return TRUE;
    }

    // A file here, or something in a subfolder (or the subfolder itself)
    char Name[TAPE_LIST_NAME_SIZE];
    const char *pSlash = strchr(pRest, '/');
    unsigned nLen = pSlash != nullptr ? (unsigned) (pSlash - pRest) : strlen(pRest);
    if (nLen >= sizeof Name) {
        return TRUE;
    }
    memcpy(Name, pRest, nLen);
    Name[nLen] = '\0';
    if (!is_valid_name(Name)) {
        return TRUE;                    // also leaves out hidden (.name) files and folders
    }

    boolean bFolder = pSlash != nullptr || pEntry->bFolder;
    if (bFolder) {
        for (unsigned i = 0; i < pList->nCount; i++) {
            if (pList->pEntries[i].bFolder && strcmp(pList->pEntries[i].Name, Name) == 0) {
                return TRUE;            // listed already
            }
        }
    }
    boolean bZip = !bFolder && has_extension(Name, ".zip");     // opens like a folder
    boolean bSnapshot = !bFolder && pList->bWithSnapshots && has_extension(Name, ".ace");
    boolean bSource = !bFolder && pList->bWithSnapshots && has_source_extension(Name);
    if (!bFolder && !bZip && !(has_tape_extension(Name) || bSnapshot || bSource)) {
        return TRUE;                    // not an Ace file
    }
    if (pList->nCount >= pList->nMax) {
        return FALSE;
    }
    add_entry(pList->pEntries, &pList->nCount, Name, bFolder ? 0 : pEntry->nSize,
              bFolder || bZip, bZip, bSnapshot, bSource);
    return TRUE;
}

int TapeListFiles(const char *pFolder, TTapeListEntry *pEntries, unsigned nMax,
                  boolean bWithSnapshots)
{
    if (!s_bMounted) {
        return -1;
    }

    char Full[MAX_PATH], Inside[TAPE_PATH_SIZE];
    if (split_zip(pFolder, Full, Inside)) {
        unsigned nPrefix;
        if (!zip_folder_exists(Full, Inside, &nPrefix)) {
            return -1;
        }
        const char *pIn = Inside + nPrefix;     // the path in the .zip open
        TZipListParam List = {pIn, (unsigned) strlen(pIn), pEntries, nMax, 0, bWithSnapshots};
        return ZipForEach(zip_list_handler, &List) == ZipOK ? (int) List.nCount : -1;
    }

    if (!TapeFullPath(pFolder, Full)) {
        return -1;
    }

    DIR Dir;
    FILINFO Info;
    if (f_opendir(&Dir, Full) != FR_OK) {
        return -1;
    }

    unsigned nCount = 0;
    while (nCount < nMax && f_readdir(&Dir, &Info) == FR_OK && Info.fname[0] != '\0') {
        if (Info.fattrib & (AM_HID | AM_SYS)) {
            continue;
        }
        boolean bFolder = (Info.fattrib & AM_DIR) != 0;
        boolean bZip = !bFolder && has_extension(Info.fname, ".zip");
        boolean bSnapshot = !bFolder && bWithSnapshots && has_extension(Info.fname, ".ace");
        boolean bSource = !bFolder && bWithSnapshots && has_source_extension(Info.fname);
        if (bFolder || bZip ? !is_valid_name(Info.fname)
                            : !(has_tape_extension(Info.fname) || bSnapshot || bSource)) {
            continue;
        }
        add_entry(pEntries, &nCount, Info.fname, bFolder ? 0 : (unsigned) Info.fsize,
                  bFolder || bZip, bZip, bSnapshot, bSource);
    }

    f_closedir(&Dir);

    return (int) nCount;
}

// --- Folders --------------------------------------------------------------------

TTapeFileResult TapeMakeFolder(const char *pPath)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    char Full[MAX_PATH];
    if (!TapeIsValidPath(pPath, FALSE, TRUE) || !TapeFullPath(pPath, Full)) {
        return TapeFileInvalidName;
    }
    if (TapeIsInZip(pPath)) {
        return TapeFileReadOnly;
    }
    FRESULT Result = f_mkdir(Full);
    if (Result == FR_EXIST) {
        return TapeFileExists;
    }
    if (Result == FR_NO_PATH) {
        return TapeFileNotFound;
    }
    if (Result != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot make %s", Full);
        return TapeFileError;
    }
    CLogger::Get()->Write(FromTape, LogNotice, "Made %s", Full);
    return TapeFileOK;
}

TTapeFileResult TapeMakeFolders(const char *pPath)
{
    if (!TapeIsValidPath(pPath, TRUE, TRUE)) {
        return TapeFileInvalidName;
    }
    // Each folder from the top: a/, a/b/, a/b/c
    char Part[TAPE_PATH_SIZE];
    for (unsigned i = 0; ; i++) {
        if (pPath[i] == '/' || pPath[i] == '\0') {
            if (i > 0) {
                memcpy(Part, pPath, i);
                Part[i] = '\0';
                TTapeFileResult Result = TapeMakeFolder(Part);
                if (Result == TapeFileExists) {
                    char Full[MAX_PATH];
                    TapeFullPath(Part, Full);
                    if (!is_folder(Full)) {
                        return TapeFileExists;      // a file with the folder's name
                    }
                } else if (Result != TapeFileOK) {
                    return Result;
                }
            }
            if (pPath[i] == '\0') {
                return TapeFileOK;
            }
        }
    }
}

boolean TapeExists(const char *pPath, boolean *pbFolder)
{
    char Full[MAX_PATH];
    if (!s_bMounted || !TapeFullPath(pPath, Full)) {
        return FALSE;
    }
    if (pPath[0] == '\0') {
        *pbFolder = TRUE;
        return TRUE;
    }
    FILINFO Info;
    if (f_stat(Full, &Info) != FR_OK) {
        return FALSE;
    }
    *pbFolder = (Info.fattrib & AM_DIR) != 0;
    return TRUE;
}

/*
 * Walk a folder and its subfolders, depth first, with one DIR per level (no recursion:
 * the stack is small). pHandler gets each file, bFolders also each subfolder after
 * its contents.
 */
typedef boolean TWalkHandler(const char *pRelative, const FILINFO *pInfo, void *pParam);

static boolean walk_folder(const char *pFolder, TWalkHandler *pHandler, void *pParam)
{
    static DIR Dirs[MAX_DEPTH];
    static char Full[MAX_PATH];
    if (!s_bMounted || !TapeFullPath(pFolder, Full) || f_opendir(&Dirs[0], Full) != FR_OK) {
        return FALSE;
    }
    unsigned nBase = strlen(Full) + 1;      // where the paths relative to pFolder start

    int nDepth = 0;
    boolean bGoOn = TRUE;
    while (nDepth >= 0) {
        FILINFO Info;
        if (!bGoOn || f_readdir(&Dirs[nDepth], &Info) != FR_OK || Info.fname[0] == '\0') {
            // End of this folder: back up a level, then report the folder itself
            f_closedir(&Dirs[nDepth]);
            if (nDepth-- == 0) {
                break;
            }
            if (bGoOn) {
                FILINFO Folder;
                memset(&Folder, 0, sizeof Folder);
                Folder.fattrib = AM_DIR;
                bGoOn = (*pHandler)(Full + nBase, &Folder, pParam);
            }
            strip_last_name(Full);
            continue;
        }
        if ((Info.fattrib & AM_SYS) || strlen(Full) + 1 + strlen(Info.fname) >= MAX_PATH) {
            continue;
        }
        strcat(Full, "/");
        strcat(Full, Info.fname);
        if (Info.fattrib & AM_DIR) {
            if (nDepth + 1 < MAX_DEPTH && f_opendir(&Dirs[nDepth + 1], Full) == FR_OK) {
                nDepth++;
                continue;               // its contents, then the folder (above)
            }
        } else {
            bGoOn = (*pHandler)(Full + nBase, &Info, pParam);
        }
        strip_last_name(Full);
    }
    return TRUE;
}

struct TCountParam {
    unsigned nFiles;
    unsigned nFolders;
};

static boolean count_handler(const char *pRelative, const FILINFO *pInfo, void *pParam)
{
    TCountParam *pCount = (TCountParam *) pParam;
    if (pInfo->fattrib & AM_DIR) {
        pCount->nFolders++;
    } else {
        pCount->nFiles++;
    }
    return TRUE;
}

void TapeCountFolder(const char *pPath, unsigned *pFiles, unsigned *pFolders)
{
    TCountParam Count = {0, 0};
    walk_folder(pPath, count_handler, &Count);
    *pFiles = Count.nFiles;
    *pFolders = Count.nFolders;
}

struct TTreeParam {
    TTapeTreeHandler *pHandler;
    void *pParam;
};

static boolean tree_handler(const char *pRelative, const FILINFO *pInfo, void *pParam)
{
    TTreeParam *pTree = (TTreeParam *) pParam;
    if ((pInfo->fattrib & (AM_DIR | AM_HID)) || !is_ace_extension(pInfo->fname)) {
        return TRUE;
    }
    // Names the web page could not handle are left out
    for (const char *p = pRelative; *p != '\0'; p++) {
        if ((unsigned char) *p >= 0x7F) {
            return TRUE;
        }
    }
    return (*pTree->pHandler)(pRelative, (unsigned) pInfo->fsize, pTree->pParam);
}

boolean TapeWalkFolder(const char *pFolder, TTapeTreeHandler *pHandler, void *pParam)
{
    TTreeParam Tree = {pHandler, pParam};
    return walk_folder(pFolder, tree_handler, &Tree);
}

TTapeFileResult TapeDeleteFolder(const char *pPath)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    char Full[MAX_PATH];
    if (pPath[0] == '\0' || !TapeFullPath(pPath, Full)) {
        return TapeFileInvalidName;     // never programs itself
    }
    if (!is_folder(Full)) {
        return TapeFileNotFound;
    }

    // Don't keep reading from a tape that no longer exists
    if (s_CurrentTape[0] != '\0' && is_in_folder(relative_path(s_CurrentTape), pPath)) {
        forget_tape();
    }

    ZipForget();

    // Delete what is in it, deepest first: each folder is read from its start again
    // after every deletion, so the folder is never read while it changes
    char Path[MAX_PATH];
    strcpy(Path, Full);
    unsigned nBase = strlen(Full);
    unsigned nFiles = 0;
    for (;;) {
        DIR Dir;
        FILINFO Info;
        if (f_opendir(&Dir, Path) != FR_OK) {
            CLogger::Get()->Write(FromTape, LogError, "Cannot read %s", Path);
            return TapeFileError;
        }
        FRESULT Result = f_readdir(&Dir, &Info);
        f_closedir(&Dir);
        if (Result != FR_OK) {
            return TapeFileError;
        }

        if (Info.fname[0] == '\0') {
            // Empty: delete it, and carry on in the folder above (until the one asked for)
            if (f_unlink(Path) != FR_OK) {
                CLogger::Get()->Write(FromTape, LogError, "Cannot delete %s", Path);
                return TapeFileError;
            }
            if (strlen(Path) == nBase) {
                break;
            }
            strip_last_name(Path);
            continue;
        }

        if (strlen(Path) + 1 + strlen(Info.fname) >= sizeof(Path)) {
            return TapeFileError;
        }
        strcat(Path, "/");
        strcat(Path, Info.fname);
        if (Info.fattrib & AM_DIR) {
            continue;                   // empty it first
        }
        if (Info.fattrib & AM_RDO) {
            f_chmod(Path, 0, AM_RDO);
        }
        if (f_unlink(Path) != FR_OK) {
            CLogger::Get()->Write(FromTape, LogError, "Cannot delete %s", Path);
            return TapeFileError;
        }
        nFiles++;
        strip_last_name(Path);
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Deleted %s (%u files)", Full, nFiles);
    return TapeFileOK;
}

TTapeFileResult TapeRenameFolder(const char *pFrom, const char *pTo)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    char From[MAX_PATH], To[MAX_PATH];
    if (   pFrom[0] == '\0' || !TapeFullPath(pFrom, From)
        || !TapeIsValidPath(pTo, FALSE, TRUE) || !TapeFullPath(pTo, To)) {
        return TapeFileInvalidName;
    }
    if (!is_folder(From)) {
        return TapeFileNotFound;
    }
    if (is_in_folder(pTo, pFrom) && strcasecmp(pTo, pFrom) != 0) {
        return TapeFileInvalidName;     // into itself (FatFs would lose it)
    }

    ZipForget();
    FRESULT Result = f_rename(From, To);
    if (Result == FR_EXIST) {
        return TapeFileExists;
    }
    if (Result == FR_NO_FILE || Result == FR_NO_PATH) {
        return TapeFileNotFound;
    }
    if (Result != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot rename %s", From);
        return TapeFileError;
    }

    // The tape in the player and the F5 folder go with it
    unsigned nFrom = strlen(pFrom);
    const char *pTape = relative_path(s_CurrentTape);
    if (s_CurrentTape[0] != '\0' && is_in_folder(pTape, pFrom)) {
        char Path[TAPE_PATH_SIZE];
        if (TapeJoinPath(Path, pTo, pTape + nFrom + (pTape[nFrom] == '/' ? 1 : 0))) {
            TapeFullPath(Path, s_CurrentTape);
        } else {
            forget_tape();
        }
    }
    if (is_in_folder(s_Folder, pFrom)) {
        char Path[TAPE_PATH_SIZE];
        if (TapeJoinPath(Path, pTo, s_Folder + nFrom + (s_Folder[nFrom] == '/' ? 1 : 0))) {
            strcpy(s_Folder, Path);
        }
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Renamed %s to %s", From, To);
    return TapeFileOK;
}

// --- Files ----------------------------------------------------------------------

TTapeFileResult TapeInsert(const char *pPath)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    char Path[MAX_PATH];
    if (!has_tape_extension(pPath) || !TapeFullPath(pPath, Path)) {
        return TapeFileInvalidName;
    }

    // A tape in a .zip is unpacked now, and read from memory while it is in
    u8 *pData = nullptr;
    unsigned nLength = 0;
    if (TapeIsInZip(pPath)) {
        TTapeFileResult Result = unpack(pPath, &pData, &nLength);
        if (Result != TapeFileOK) {
            return Result;
        }
    } else {
        FILINFO Info;
        if (f_stat(Path, &Info) != FR_OK || (Info.fattrib & AM_DIR)) {
            return TapeFileNotFound;
        }
    }

    forget_tape();
    s_pTapeData = pData;
    s_nTapeLength = nLength;
    strcpy(s_CurrentTape, Path);
    s_nCurrentPos = 0;
    s_bAfterHeader = FALSE;
    s_bSearching = FALSE;
    CLogger::Get()->Write(FromTape, LogNotice, "Inserted %s", Path);

    return TapeFileOK;
}

void TapeEject(void)
{
    if (s_CurrentTape[0] != '\0') {
        CLogger::Get()->Write(FromTape, LogNotice, "Ejected %s", s_CurrentTape);
    }
    forget_tape();
}

TTapeFileResult TapeCreate(const char *pInput, char *pPath)
{
    pPath[0] = '\0';
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    if (!is_valid_name(pInput) || has_extension(pInput, ".ace")) {
        return TapeFileInvalidName;
    }

    char Name[FF_MAX_LFN + 5];
    strncpy(Name, pInput, FF_MAX_LFN);
    Name[FF_MAX_LFN] = '\0';
    boolean bTzx = has_extension(Name, ".tzx");
    if (!bTzx && !has_extension(Name, ".tap")) {
        strcat(Name, ".tap");
    }
    char Path[MAX_PATH];
    if (!new_file_path(Name, Path)) {
        return TapeFileInvalidName;
    }

    FIL File;
    FRESULT Result = f_open(&File, Path, FA_WRITE | FA_CREATE_NEW);
    if (Result == FR_EXIST) {
        return TapeFileExists;
    }
    if (Result != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot create %s", Path);
        return TapeFileError;
    }
    UINT nWritten = sizeof(s_TzxHeader);
    if (bTzx) {
        Result = f_write(&File, s_TzxHeader, sizeof(s_TzxHeader), &nWritten);
    }
    if (f_close(&File) != FR_OK || Result != FR_OK || nWritten != sizeof(s_TzxHeader)) {
        f_unlink(Path);
        CLogger::Get()->Write(FromTape, LogError, "Cannot write %s", Path);
        return TapeFileError;
    }

    CLogger::Get()->Write(FromTape, LogNotice, "New tape %s", Path);

    strcpy(pPath, relative_path(Path));
    return TapeInsert(pPath);
}

const char *TapeGetInserted(void)
{
    return s_CurrentTape[0] != '\0' ? relative_path(s_CurrentTape) : "";
}

TTapeFileResult TapeDeleteFile(const char *pPath)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    char Path[MAX_PATH];
    if (!is_ace_extension(pPath) || !TapeFullPath(pPath, Path)) {
        return TapeFileInvalidName;
    }

    if (TapeIsInZip(pPath)) {
        return TapeFileReadOnly;
    }

    // Don't keep reading from a tape that no longer exists (or a .zip it was in)
    if (s_CurrentTape[0] != '\0' && is_in_folder(relative_path(s_CurrentTape), pPath)) {
        forget_tape();
    }

    ZipForget();
    FRESULT Result = f_unlink(Path);
    if (Result != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot delete %s", Path);
        return Result == FR_NO_FILE || Result == FR_NO_PATH ? TapeFileNotFound : TapeFileError;
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Deleted %s", Path);

    return TapeFileOK;
}

// A file the web page (F11) may handle: plain ASCII names with one of the Ace extensions
// (and only in SD:/programs, so the boot files on the card cannot be touched)
boolean TapeIsAceFileName(const char *pPath)
{
    return TapeIsValidPath(pPath, FALSE, TRUE) && is_ace_extension(pPath);
}

TTapeFileResult TapeReadFile(const char *pPath, u8 *pBuffer, unsigned nMax, unsigned *pLength,
                             u16 *pDate, u16 *pTime)
{
    *pLength = 0;
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    char Path[MAX_PATH];
    if (!TapeIsAceFileName(pPath) || !TapeFullPath(pPath, Path)) {
        return TapeFileInvalidName;
    }

    FILINFO Info;
    if (f_stat(Path, &Info) != FR_OK || (Info.fattrib & AM_DIR)) {
        return TapeFileNotFound;
    }
    if (Info.fsize > nMax) {
        return TapeFileTooBig;
    }
    if (pDate != nullptr) *pDate = Info.fdate;
    if (pTime != nullptr) *pTime = Info.ftime;

    FIL File;
    if (f_open(&File, Path, FA_READ) != FR_OK) {
        return TapeFileError;
    }
    UINT nRead = 0;
    FRESULT Result = f_read(&File, pBuffer, (UINT) Info.fsize, &nRead);
    f_close(&File);
    if (Result != FR_OK || nRead != Info.fsize) {
        return TapeFileError;
    }

    *pLength = nRead;

    return TapeFileOK;
}

TTapeFileResult TapeWriteFile(const char *pPath, const u8 *pData, unsigned nLength, boolean bReplace)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    char Path[MAX_PATH];
    if (!TapeIsAceFileName(pPath) || !TapeFullPath(pPath, Path)) {
        return TapeFileInvalidName;
    }
    if (TapeIsInZip(pPath)) {
        return TapeFileReadOnly;
    }
    ZipForget();

    FILINFO Info;
    if (f_stat(Path, &Info) == FR_OK && (Info.fattrib & (AM_DIR | AM_RDO | AM_SYS))) {
        return (Info.fattrib & AM_DIR) ? TapeFileExists : TapeFileError;  // not a file we may overwrite
    }

    FIL File;
    FRESULT Result = f_open(&File, Path, FA_WRITE | (bReplace ? FA_CREATE_ALWAYS : FA_CREATE_NEW));
    if (Result == FR_EXIST) {
        return TapeFileExists;
    }
    if (Result == FR_NO_PATH) {
        return TapeFileNotFound;        // no such folder
    }
    if (Result != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot create %s", Path);
        return TapeFileError;
    }
    UINT nWritten = 0;
    Result = nLength > 0 ? f_write(&File, pData, nLength, &nWritten) : FR_OK;
    if (f_close(&File) != FR_OK || Result != FR_OK || nWritten != nLength) {
        f_unlink(Path);
        CLogger::Get()->Write(FromTape, LogError, "Cannot write %s", Path);
        return TapeFileError;
    }

    // A tape replaced while in the player starts again from its beginning
    if (strcasecmp(Path, s_CurrentTape) == 0) {
        s_nCurrentPos = 0;
        s_bAfterHeader = FALSE;
        s_bSearching = FALSE;
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Received %s (%u bytes)", Path, nLength);

    return TapeFileOK;
}

TTapeFileResult TapeRenameFile(const char *pFrom, const char *pTo)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    char From[MAX_PATH], To[MAX_PATH];
    if (   !TapeIsAceFileName(pFrom) || !TapeIsAceFileName(pTo)
        || !TapeFullPath(pFrom, From) || !TapeFullPath(pTo, To)) {
        return TapeFileInvalidName;
    }
    if (TapeIsInZip(pFrom) || TapeIsInZip(pTo)) {
        return TapeFileReadOnly;
    }
    if (has_extension(pFrom, ".zip") != has_extension(pTo, ".zip")) {
        return TapeFileInvalidName;     // a .zip stays a .zip
    }

    ZipForget();
    FRESULT Result = f_rename(From, To);
    if (Result == FR_NO_FILE || Result == FR_NO_PATH) {
        return TapeFileNotFound;
    }
    if (Result == FR_EXIST) {
        return TapeFileExists;
    }
    if (Result != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot rename %s", From);
        return TapeFileError;
    }

    // The tape in the player keeps its place under its new name (out if no longer a
    // tape); in a renamed .zip, it goes with it
    if (strcasecmp(From, s_CurrentTape) == 0) {
        if (has_tape_extension(To)) {
            strcpy(s_CurrentTape, To);
        } else {
            TapeEject();
        }
    } else if (s_CurrentTape[0] != '\0' && is_in_folder(relative_path(s_CurrentTape), pFrom)) {
        char Path[TAPE_PATH_SIZE];
        if (TapeJoinPath(Path, pTo, relative_path(s_CurrentTape) + strlen(pFrom) + 1)) {
            TapeFullPath(Path, s_CurrentTape);
        }
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Renamed %s to %s", From, To);

    return TapeFileOK;
}

/*
 * .ace snapshots (Ace32 / EightyOne format, as loaded by MAME): the RAM from
 * 0x2000 onwards, run-length coded:
 *     ED 00          end of the snapshot
 *     ED <n> <byte>  <byte> repeated n times
 *     <byte>         itself
 * The CPU registers are kept in 4 byte slots from 0x2100 (AF, BC, DE, HL, IX,
 * IY, SP, PC, AF', BC', DE', HL', IM, IFF1, IFF2, I, R). Some snapshots have
 * rubbish there: like MAME, the registers are only used when the word at 0x2080
 * (memory size of the machine that made it) is a multiple of 16K.
 *
 * Snapshots often stop at 0x7FFF although the machine had more RAM, so the
 * Z80 stack (Forth's return stack, just below RAMTOP) may not be in the file.
 * Memory after the file is left as it is (as in MAME), and SP is handled like this:
 *   - stack in the file: SP is used;
 *   - stack not in the file but holding only the outer interpreter's return
 *     address (SP = RAMTOP - 2, a snapshot taken at the Forth prompt): SP is
 *     used and that address, the same on every Ace, is put back;
 *   - otherwise SP is not changed (as in MAME).
 *
 * With the 96K RAM pack (two 48K pages at 0x4000, see ram_ace.h) the snapshot
 * holds the page in use as usual, and its other page follows the end marker,
 * where other emulators stop reading (EightyOne, MAME):
 *     "XACE96K" 01 <page>  signature, version, the page in use (0 or 1)
 *     the other page, 0x4000 - 0xFFFF, coded as above, and ED 00
 * Loading it chooses the 96K pack.
 */
#define SNAPSHOT_START  0x2000
#define SYSVAR_RAMTOP   0x3C18      // first byte above the RAM, 0 for 64K
#define ROM_QUIT_RETURN 0x04F7      // return stack entry of an Ace at the Forth prompt

static unsigned char s_Snapshot[0x10000];   // decoded, indexed by Ace address

static const char PageSignature[] = "XACE96K";  // 7 characters, then version 1
#define PAGE_VERSION    1

// Buffered byte reader for the decoder
struct TSnapshotReader {
    TTapeReader   *pFile;
    unsigned char  Buffer[1024];
    UINT           nCount;
    UINT           nPos;
};

static int read_snapshot_byte(TSnapshotReader *pReader)
{
    if (pReader->nPos >= pReader->nCount) {
        unsigned nCount;
        if (!TapeReaderRead(pReader->pFile, pReader->Buffer, sizeof(pReader->Buffer), &nCount)
            || nCount == 0) {
            pReader->nCount = 0;
            return -1;
        }
        pReader->nCount = nCount;
        pReader->nPos = 0;
    }
    return pReader->Buffer[pReader->nPos++];
}

static unsigned short snapshot_word(unsigned nAddr)
{
    return s_Snapshot[nAddr] | (s_Snapshot[nAddr + 1] << 8);
}

// After a snapshot's end marker: nothing (0), or the 96K pack's page block: the other
// page into *ppPage (RAM_PAGE_SIZE bytes from the heap; delete [] it) and the page in
// use into *pnPage (1). -1 if the block is damaged, -2 if there is no memory for it.
static int read_page_block(TSnapshotReader *pReader, u8 **ppPage, int *pnPage)
{
    *ppPage = nullptr;
    *pnPage = -1;
    int nByte = read_snapshot_byte(pReader);
    if (nByte < 0) {
        return 0;                               // the usual end of the file
    }
    for (unsigned i = 0; i < sizeof PageSignature - 1; i++, nByte = read_snapshot_byte(pReader)) {
        if (nByte != (unsigned char) PageSignature[i]) {
            return 0;                           // something else: ignored, as elsewhere
        }
    }
    int nPage = read_snapshot_byte(pReader);
    if (nByte != PAGE_VERSION || (nPage != 0 && nPage != 1)) {
        return -1;
    }

    u8 *pPage = new u8[RAM_PAGE_SIZE];
    if (pPage == nullptr) {
        return -2;
    }
    unsigned nPos = 0;
    for (;;) {
        nByte = read_snapshot_byte(pReader);
        if (nByte < 0) {
            break;
        }
        if (nByte != 0xED) {
            if (nPos >= RAM_PAGE_SIZE) {
                break;
            }
            pPage[nPos++] = (u8) nByte;
            continue;
        }
        int nCount = read_snapshot_byte(pReader);
        if (nCount == 0 && nPos == RAM_PAGE_SIZE) {
            *ppPage = pPage;
            *pnPage = nPage;
            return 1;
        }
        int nValue = read_snapshot_byte(pReader);
        if (nCount <= 0 || nValue < 0 || nPos + nCount > RAM_PAGE_SIZE) {
            break;
        }
        memset(pPage + nPos, nValue, nCount);
        nPos += nCount;
    }
    delete [] pPage;
    return -1;
}

TTapeFileResult TapeLoadSnapshot(const char *pPath)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }

    char Path[MAX_PATH];
    if (!TapeFullPath(pPath, Path)) {
        return TapeFileInvalidName;
    }

    static TTapeReader File;            // on the card, or unpacked from a .zip
    TTapeFileResult Result = TapeReaderOpen(&File, pPath);
    if (Result != TapeFileOK) {
        return Result;
    }

    // Decode the whole file before touching the Ace, so a bad file changes nothing
    static TSnapshotReader Reader;
    Reader.pFile = &File;
    Reader.nCount = Reader.nPos = 0;

    memset(s_Snapshot, 0xFF, sizeof(s_Snapshot));
    unsigned nAddr = SNAPSHOT_START;
    boolean bEnd = FALSE;
    while (nAddr < sizeof(s_Snapshot)) {
        int nByte = read_snapshot_byte(&Reader);
        if (nByte < 0) {
            break;
        }
        if (nByte != 0xED) {
            s_Snapshot[nAddr++] = (unsigned char) nByte;
            continue;
        }

        int nCount = read_snapshot_byte(&Reader);
        if (nCount <= 0) {
            bEnd = nCount == 0;
            break;
        }
        int nValue = read_snapshot_byte(&Reader);
        if (nValue < 0) {
            break;
        }
        while (nCount-- > 0 && nAddr < sizeof(s_Snapshot)) {
            s_Snapshot[nAddr++] = (unsigned char) nValue;
        }
    }

    // A snapshot of a 64K Ace fills memory: its end marker comes after FFFFh (a file
    // that ends there without one holds all the memory too)
    boolean bFileEnd = FALSE;
    if (!bEnd && nAddr == sizeof(s_Snapshot)) {
        int nByte = read_snapshot_byte(&Reader);
        bFileEnd = nByte < 0;
        bEnd = bFileEnd || (nByte == 0xED && read_snapshot_byte(&Reader) == 0x00);
    }

    // The 96K pack's other page, after the end marker
    u8 *pOtherPage = nullptr;
    int nPage = -1;
    boolean bValid = bEnd && nAddr >= SYSVAR_RAMTOP + 2;  // must reach the registers and RAMTOP
    if (bValid && !bFileEnd) {
        int nRead = read_page_block(&Reader, &pOtherPage, &nPage);
        if (nRead == -2) {
            TapeReaderClose(&File);
            return TapeFileError;               // no memory
        }
        bValid = nRead >= 0;
    }

    TapeReaderClose(&File);

    if (!bValid) {
        delete [] pOtherPage;
        CLogger::Get()->Write(FromTape, LogWarning, "%s is not a valid .ace snapshot", pPath);
        return TapeFileInvalidSnapshot;
    }
    if (pOtherPage != nullptr && RamGetPackKB() != RAM_PACK_96K) {
        if (!RamSetPackKB(RAM_PACK_96K)) {
            delete [] pOtherPage;
            return TapeFileError;
        }
        CLogger::Get()->Write(FromTape, LogNotice, "96K RAM pack chosen for %s", pPath);
    }

    // The RAM the snapshot's machine had (up to its RAMTOP), at least what the file holds
    unsigned nRamTop = snapshot_word(SYSVAR_RAMTOP);
    if (nRamTop == 0) {
        nRamTop = 0x10000;
    }
    unsigned nNeeded = nRamTop > nAddr ? nRamTop : nAddr;

    // A bigger RAM pack if the snapshot does not fit in the current one
    unsigned nPackEnd = RamGetEnd();
    if (nNeeded > nPackEnd) {
        unsigned nKB = (nNeeded - RAM_PACK_START + 0x3FFF) / 0x4000 * 16;
        RamSetPackKB(nKB);
        CLogger::Get()->Write(FromTape, LogNotice, "RAM pack enlarged to %uK for %s", nKB, pPath);
    }

    // Through the Z80 store path, in address order: the last copy of each mirrored
    // area (video RAM, character set, 1K RAM and system variables) is the one kept
    TapeReset();
    for (unsigned a = SNAPSHOT_START; a < nAddr; a++) {
        unsigned short nDest = (unsigned short) a;
        store(nDest, s_Snapshot[a]);
    }
    if (pOtherPage != nullptr) {
        memcpy(RamGetOtherPage(), pOtherPage, RAM_PAGE_SIZE);
        delete [] pOtherPage;
    }

    unsigned short nMachineRAM = snapshot_word(0x2080);
    if ((nMachineRAM & 0x3FFF) == 0) {
        snapshot.reg_af  = snapshot_word(0x2100);
        snapshot.reg_bc  = snapshot_word(0x2104);
        snapshot.reg_de  = snapshot_word(0x2108);
        snapshot.reg_hl  = snapshot_word(0x210C);
        snapshot.reg_ix  = snapshot_word(0x2110);
        snapshot.reg_iy  = snapshot_word(0x2114);
        snapshot.reg_sp  = snapshot_word(0x2118);
        snapshot.reg_pc  = snapshot_word(0x211C);
        snapshot.reg_af1 = snapshot_word(0x2120);
        snapshot.reg_bc1 = snapshot_word(0x2124);
        snapshot.reg_de1 = snapshot_word(0x2128);
        snapshot.reg_hl1 = snapshot_word(0x212C);
        snapshot.reg_im   = s_Snapshot[0x2130];
        snapshot.reg_iff1 = s_Snapshot[0x2134];
        snapshot.reg_iff2 = s_Snapshot[0x2138];
        snapshot.reg_i    = s_Snapshot[0x213C];
        snapshot.reg_r    = s_Snapshot[0x2140];

        unsigned nSP = snapshot.reg_sp;
        if (nSP >= SNAPSHOT_START && nRamTop <= nAddr) {
            snapshot.set_sp = 1;                        // the whole stack is in the file
        } else if (nSP == nRamTop - 2) {
            unsigned short nDest = (unsigned short) nSP;
            store2(nDest, ROM_QUIT_RETURN);             // taken at the Forth prompt
            snapshot.set_sp = 1;
        } else {
            snapshot.set_sp = 0;
            CLogger::Get()->Write(FromTape, LogWarning, "%s: stack not in the file, SP kept", pPath);
        }
        load_snapshot = 1;
    } else {
        CLogger::Get()->Write(FromTape, LogWarning, "%s: no CPU registers, memory only", pPath);
    }

    // With the 96K pack, the memory loaded is the page in use, which the latch must
    // agree with: bit 7 of R selects it at the next refresh (unless the snapshot says
    // an LD R,A had not reached one yet)
    if (RamGetPackKB() == RAM_PACK_96K) {
        unsigned nR7 = load_snapshot ? snapshot.reg_r >> 7 : RamGetPage();
        if (nPage >= 0) {
            RamSetPage((unsigned) nPage, nR7 != (unsigned) nPage);
        } else {
            RamSetPage(nR7, FALSE);
        }
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Loaded snapshot %s (0x%04X - 0x%04X%s)",
                          pPath, SNAPSHOT_START, nAddr - 1,
                          nPage >= 0 ? ", both pages of the 96K RAM pack" : "");

    return TapeFileOK;
}

// Buffered writer for the encoder
struct TSnapshotWriter {
    FIL           *pFile;
    unsigned char  Buffer[1024];
    UINT           nCount;
    boolean        bError;
};

static void flush_snapshot(TSnapshotWriter *pWriter)
{
    UINT nWritten;
    if (   pWriter->nCount > 0
        && (f_write(pWriter->pFile, pWriter->Buffer, pWriter->nCount, &nWritten) != FR_OK
            || nWritten != pWriter->nCount)) {
        pWriter->bError = TRUE;
    }
    pWriter->nCount = 0;
}

static void write_snapshot_byte(TSnapshotWriter *pWriter, unsigned char ucByte)
{
    if (pWriter->nCount == sizeof(pWriter->Buffer)) {
        flush_snapshot(pWriter);
    }
    pWriter->Buffer[pWriter->nCount++] = ucByte;
}

// nLength bytes, run-length coded, and the end marker
static void write_snapshot_data(TSnapshotWriter *pWriter, const u8 *pData, unsigned nLength)
{
    for (unsigned nPos = 0; nPos < nLength; ) {
        u8 ucByte = pData[nPos];
        unsigned nRun = 1;
        while (nPos + nRun < nLength && nRun < 255 && pData[nPos + nRun] == ucByte) {
            nRun++;
        }
        if (nRun > 3 || ucByte == 0xED) {
            write_snapshot_byte(pWriter, 0xED);
            write_snapshot_byte(pWriter, (unsigned char) nRun);
            write_snapshot_byte(pWriter, ucByte);
        } else {
            for (unsigned i = 0; i < nRun; i++) {
                write_snapshot_byte(pWriter, ucByte);
            }
        }
        nPos += nRun;
    }
    write_snapshot_byte(pWriter, 0xED);         // end
    write_snapshot_byte(pWriter, 0x00);
}

static void set_snapshot_word(unsigned nAddr, unsigned nValue)
{
    s_Snapshot[nAddr] = (unsigned char) nValue;
    s_Snapshot[nAddr + 1] = (unsigned char) (nValue >> 8);
}

/*
 * Saving (F7) writes the same format, as Ace32 and EightyOne do: all the RAM
 * there is (up to the end of the RAM pack), with the registers at 0x2100 and
 * the end of the RAM at 0x2080 in the first 1K, which is a copy of the video
 * RAM at 0x2400 and so free. Runs of more than 3 bytes, and every ED byte,
 * are coded as ED <n> <byte>.
 */
TTapeFileResult TapeSaveSnapshot(const char *pInput, char *pPath)
{
    pPath[0] = '\0';
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    if (!is_valid_name(pInput)) {
        return TapeFileInvalidName;
    }
    for (const char *p = pInput + 1; *p != '\0'; p++) {
        if (*p == '.' && strchr(p + 1, '.') == nullptr && strcasecmp(p, ".ace") != 0) {
            return TapeFileInvalidName;         // an extension, but not .ace
        }
    }

    char Name[FF_MAX_LFN + 5];
    strncpy(Name, pInput, FF_MAX_LFN);
    Name[FF_MAX_LFN] = '\0';
    if (!has_extension(Name, ".ace")) {
        strcat(Name, ".ace");
    }
    char Path[MAX_PATH];
    if (!new_file_path(Name, Path)) {
        return TapeFileInvalidName;
    }
    strcpy(pPath, relative_path(Path));

    // The image: header and registers, then the Ace's memory as it is
    unsigned nEnd = RamGetEnd();
    memset(s_Snapshot, 0, sizeof(s_Snapshot));
    memcpy(s_Snapshot + 0x2400, mem + 0x2400, nEnd - 0x2400);
    s_Snapshot[0x2000] = 0x01;                  // as in Ace32 / EightyOne snapshots
    s_Snapshot[0x2001] = 0x80;
    set_snapshot_word(0x2080, nEnd);            // 0 for 64K
    set_snapshot_word(0x2100, cpu_state.reg_af);
    set_snapshot_word(0x2104, cpu_state.reg_bc);
    set_snapshot_word(0x2108, cpu_state.reg_de);
    set_snapshot_word(0x210C, cpu_state.reg_hl);
    set_snapshot_word(0x2110, cpu_state.reg_ix);
    set_snapshot_word(0x2114, cpu_state.reg_iy);
    set_snapshot_word(0x2118, cpu_state.reg_sp);
    set_snapshot_word(0x211C, cpu_state.reg_pc);
    set_snapshot_word(0x2120, cpu_state.reg_af1);
    set_snapshot_word(0x2124, cpu_state.reg_bc1);
    set_snapshot_word(0x2128, cpu_state.reg_de1);
    set_snapshot_word(0x212C, cpu_state.reg_hl1);
    s_Snapshot[0x2130] = cpu_state.reg_im;
    s_Snapshot[0x2134] = cpu_state.reg_iff1;
    s_Snapshot[0x2138] = cpu_state.reg_iff2;
    s_Snapshot[0x213C] = cpu_state.reg_i;
    s_Snapshot[0x2140] = cpu_state.reg_r;

    FIL File;
    FRESULT Result = f_open(&File, Path, FA_WRITE | FA_CREATE_NEW);
    if (Result == FR_EXIST) {
        return TapeFileExists;
    }
    if (Result != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot create %s", Path);
        return TapeFileError;
    }

    static TSnapshotWriter Writer;
    Writer.pFile = &File;
    Writer.nCount = 0;
    Writer.bError = FALSE;

    write_snapshot_data(&Writer, s_Snapshot + SNAPSHOT_START, nEnd - SNAPSHOT_START);

    // The 96K pack: the page in use (an LD R,A may not have reached the latch yet:
    // then R differs from it), and the other page
    const u8 *pOtherPage = RamGetOtherPage();
    if (pOtherPage != nullptr) {
        for (const char *p = PageSignature; *p != '\0'; p++) {
            write_snapshot_byte(&Writer, (unsigned char) *p);
        }
        write_snapshot_byte(&Writer, PAGE_VERSION);
        write_snapshot_byte(&Writer, (unsigned char) RamGetPage());
        write_snapshot_data(&Writer, pOtherPage, RAM_PAGE_SIZE);
    }
    flush_snapshot(&Writer);

    if (f_close(&File) != FR_OK) {
        Writer.bError = TRUE;
    }
    if (Writer.bError) {
        f_unlink(Path);
        CLogger::Get()->Write(FromTape, LogError, "Write error, %s not saved", Path);
        return TapeFileError;
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Saved snapshot %s (0x%04X - 0x%04X%s, PC %04X SP %04X)",
                          Path, SNAPSHOT_START, nEnd - 1,
                          pOtherPage != nullptr ? " and the 96K pack's other page" : "",
                          cpu_state.reg_pc, cpu_state.reg_sp);

    return TapeFileOK;
}

// A ROM error (spool_error_p()): a LOAD searching for its file has ended
void TapeRomError(void)
{
    s_bSearching = FALSE;
}

/*
 * The ROM's block read (18A7h): the next block on the tape, as on a cassette.
 * It must have the flag asked for (00h header, FFh data), and exactly length
 * bytes and the right checksum; the bytes are loaded to dest (or compared with
 * memory to verify) as they come, as the ROM does, so a block that fails can
 * still have changed memory.
 *
 * The ROM's LOAD (and BLOAD, VERIFY, BVERIFY) reads headers until one has the
 * wanted name, showing each (return_address is then ROM_LOAD_HEADER_RETURN):
 * a real Ace waits for the tape for ever; here, with no tape in the player or
 * once round the tape (from its start for LOAD without a name, which lists the
 * files), the ROM's ERROR 10 is given. Other callers (programs reading blocks
 * themselves) just fail at the end of the tape, and get BREAK (ERROR 3) when
 * Space is held then, as in the ROM.
 */
extern "C" int tape_load_p(unsigned char *mem, unsigned short dest, unsigned short length,
                           unsigned char flag, int bLoad, unsigned short return_address)
{
    (void) mem;

    const boolean bSearch = return_address == ROM_LOAD_HEADER_RETURN;
    if (!bSearch) {
        s_bSearching = FALSE;           // the ROM took the header: its data, or another reader
    }

    if (!s_bMounted || s_CurrentTape[0] == '\0') {
        if (bSearch) {
            CLogger::Get()->Write(FromTape, LogWarning, "LOAD: no tape in the player");
            return TAPE_LOAD_ERROR;
        }
        return (in(0x7f, 0xfe) & 1) ? 0 : TAPE_LOAD_BREAK;
    }

    if (bSearch && !s_bSearching) {
        s_bSearching = TRUE;
        if (is_blank_name(::mem + REQUEST_HEADER + 1)) {
            s_nCurrentPos = 0;          // LOAD without a name: list the tape from its start
            s_bAfterHeader = FALSE;
        }
        s_nSearchStart = s_nCurrentPos;
        s_bSearchWrapped = s_nSearchStart == 0;
    }

    unsigned char ucFlag;
    unsigned nSize, nTotal;
    for (;;) {
        FSIZE_t nBlockPos = s_nCurrentPos;
        boolean bRead = read_block(&ucFlag, &nSize, &nTotal);
        if (bRead && bSearch && s_bSearchWrapped && s_nSearchStart > 0 && nBlockPos >= s_nSearchStart) {
            s_nCurrentPos = nBlockPos;  // back where the search started
            bRead = FALSE;
        } else if (!bRead && bSearch && !s_bSearchWrapped) {
            s_bSearchWrapped = TRUE;    // on from the start of the tape
            s_nCurrentPos = 0;
            s_bAfterHeader = FALSE;
            continue;
        }
        if (bRead) {
            break;
        }

        if (bSearch) {
            s_bSearching = FALSE;
            CLogger::Get()->Write(FromTape, LogWarning, "LOAD: file not found on %s", s_CurrentTape);
            return TAPE_LOAD_ERROR;
        }
        CLogger::Get()->Write(FromTape, LogWarning, "End of %s", s_CurrentTape);
        return (in(0x7f, 0xfe) & 1) ? 0 : TAPE_LOAD_BREAK;
    }

    if (ucFlag != flag) {
        return 0;                       // a header when data is wanted, or the other way round
    }

    // The bytes, as far as the block and the request go, then its checksum
    unsigned nBytes = nTotal > 0 ? nTotal - 1 : 0;
    unsigned nCommon = nBytes < length ? nBytes : length;
    if (nCommon > nSize) {
        nCommon = nSize;
    }
    if (bLoad) {
        poke_block(dest, s_Block, nCommon);
    } else {
        for (unsigned i = 0; i < nCommon; i++) {
            if (::mem[(dest + i) & 0xFFFF] != s_Block[i]) {
                CLogger::Get()->Write(FromTape, LogWarning, "Verify failed at byte %u", i);
                return 0;
            }
        }
    }

    unsigned char ucChecksum = 0;
    for (unsigned i = 0; i < nCommon; i++) {
        ucChecksum ^= s_Block[i];
    }
    boolean bOK = nBytes == length && nSize == nTotal && s_Block[nBytes] == ucChecksum;

    if (flag != 0x00 || !bSearch || !bOK) {
        CLogger::Get()->Write(FromTape, bOK ? LogNotice : LogWarning, "%s %s (%u bytes%s)",
                              bLoad ? "Load" : "Verify", bOK ? "OK" : "failed", nBytes,
                              nBytes != length ? ", wrong length" : bOK ? "" : ", bad checksum");
    }

    return bOK;
}

extern "C" void tape_save_p(unsigned char *block, unsigned short size)
{
    if (!s_bMounted) {
        return;
    }

    if ((unsigned) (block - ::mem) + size > 0x10000) {
        size = 0x10000 - (block - ::mem);
    }

    // A header block starts a new file; anything else completes the open one
    boolean bHeader = !s_bSaveOpen && size == HEADER_SIZE;
    if (bHeader) {
        // Onto the current tape like a cassette, else into a .tap of its own
        char Path[MAX_PATH];
        boolean bAppend = has_tape_extension(s_CurrentTape) && s_pTapeData == nullptr;
        if (bAppend) {
            strcpy(Path, s_CurrentTape);
        } else {
            make_path(Path, block + 1);
        }

        if (f_open(&s_SaveFile, Path, FA_WRITE | (bAppend ? FA_OPEN_APPEND : FA_CREATE_ALWAYS)) != FR_OK) {
            CLogger::Get()->Write(FromTape, LogError, "Cannot write %s", Path);
            return;
        }
        s_bSaveOpen = TRUE;
        s_bSaveTzx = has_extension(Path, ".tzx");
        CLogger::Get()->Write(FromTape, LogNotice, "%s %s", bAppend ? "Appending to" : "Saving to", Path);
    } else if (!s_bSaveOpen) {
        return;
    }

    // The checksum is of the bytes only: the ROM sends the flag byte (00h header,
    // FFh data) before them but leaves it out
    unsigned char nChecksum = 0;
    for (unsigned i = 0; i < size; i++) {
        nChecksum ^= block[i];
    }

    UINT nWritten;
    boolean bOK = TRUE;
    if (s_bSaveTzx) {
        if (f_tell(&s_SaveFile) == 0) {                 // an empty file: make it a .tzx
            bOK = f_write(&s_SaveFile, s_TzxHeader, sizeof(s_TzxHeader), &nWritten) == FR_OK
                  && nWritten == sizeof(s_TzxHeader);
        }
        unsigned nLength = size + 2;                    // flag, bytes, checksum
        unsigned char Block[6] = { 0x10, TZX_PAUSE_MS & 0xFF, TZX_PAUSE_MS >> 8,
                                   (unsigned char) (nLength & 0xFF), (unsigned char) (nLength >> 8),
                                   (unsigned char) (bHeader ? 0x00 : 0xFF) };
        bOK =    bOK
              && f_write(&s_SaveFile, Block, sizeof(Block), &nWritten) == FR_OK && nWritten == sizeof(Block);
    } else {
        unsigned char Len[2] = { (unsigned char) ((size + 1) & 0xFF), (unsigned char) ((size + 1) >> 8) };
        bOK = f_write(&s_SaveFile, Len, 2, &nWritten) == FR_OK && nWritten == 2;
    }
    bOK =    bOK
          && f_write(&s_SaveFile, block, size, &nWritten) == FR_OK && nWritten == size
          && f_write(&s_SaveFile, &nChecksum, 1, &nWritten) == FR_OK && nWritten == 1;

    if (!bOK) {
        CLogger::Get()->Write(FromTape, LogError, "Write error, save aborted");
        f_close(&s_SaveFile);
        s_bSaveOpen = FALSE;
        return;
    }

    if (bHeader) {
        f_sync(&s_SaveFile);                            // header written, data follows
    } else {
        f_close(&s_SaveFile);
        s_bSaveOpen = FALSE;
        CLogger::Get()->Write(FromTape, LogNotice, "Save complete");
    }
}
