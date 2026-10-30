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
 * first (00h header, FFh data) that .tap files leave out (see next_block()).
 *
 * LOAD name searches, in order:
 *     1. the rest of the current tape (inserted with F5, or the one the last
 *        file was loaded from), then its start
 *     2. SD:/<name>.tap, then SD:/<name>.tzx
 *     3. every other *.tap and *.tzx in the SD card root
 * LOAD without a name takes the next file on the current tape. The real ROM
 * has no such LOAD (it compares the whole name), so the name found is written
 * into the request for the ROM's check to pass.
 * SAVE name appends to the current tape when it is a .tap (as on a cassette),
 * otherwise it writes SD:/<name>.tap (overwriting it).
 *
 * F5 also loads .ace snapshots (memory and CPU registers), see TapeLoadSnapshot().
 *
 * Based on the xAce tape.c by Edward Patel and Lawrence Woodman.
 */
#include "tape_sd.h"
#include "ram_ace.h"

#include <circle/logger.h>
#include <circle/util.h>
#include <fatfs/ff.h>

extern "C" {
#include "z80.h"
}

#define DRIVE           "SD:"

#define HEADER_SIZE     25          // header block payload
#define NAME_SIZE       10
#define REQUEST_HEADER  0x2701      // header the ROM is looking for

#define MAX_PATH        (sizeof(DRIVE) + 1 + FF_MAX_LFN + 1)

static const char FromTape[] = "tape";

static FATFS s_FileSystem;
static boolean s_bMounted = FALSE;

// Current LOAD: header handed to the ROM, data block waiting for the next call
static boolean s_bDataPending = FALSE;
static unsigned short s_nHeaderDest = 0;
static unsigned char s_Header[HEADER_SIZE];
static unsigned char s_Data[65536];
static unsigned s_nDataSize = 0;

// Tape the last file was loaded from, for multi-file tapes
static char s_CurrentTape[MAX_PATH] = "";
static FSIZE_t s_nCurrentPos = 0;

// Current SAVE
static FIL s_SaveFile;
static boolean s_bSaveOpen = FALSE;

// .ace snapshot registers for mainloop() (z80.h)
extern "C" {
    struct z80_snapshot snapshot;
    int load_snapshot = 0;
}

/* Shown when the wanted file can't be found: a screen with a message ... */
static const unsigned char empty_bytes[799] = {
  0x1a,0x00,0x20,0x6f,
  0x74,0x68,0x65,0x72,
  0x20,0x20,0x20,0x20,
  0x20,0x00,0x03,0x00,
  0x24,0x20,0x20,0x20,
  0x20,0x20,0x20,0x20,
  0x20,0x20,0x20,0x20,
  0x01,0x03,0x43,0x6f,
  0x75,0x6c,0x64,0x6e,
  0x27,0x74,0x20,0x6c,
  0x6f,0x61,0x64,0x20,
  0x79,0x6f,0x75,0x72,
  0x20,0x66,0x69,0x6c,
  0x65,0x21,0x20,
};

/* ... or a small dictionary whose word prints the same message */
static const unsigned char empty_dict[] = {
  0x1a,0x00,0x00,0x6f,
  0x74,0x68,0x65,0x72,
  0x20,0x20,0x20,0x20,
  0x20,0x2a,0x00,0x51,
  0x3c,0x58,0x3c,0x4c,
  0x3c,0x4c,0x3c,0x4f,
  0x3c,0x7b,0x3c,0x20,
  0x2b,0x00,0x52,0x55,
  0xce,0x27,0x00,0x49,
  0x3c,0x03,0xc3,0x0e,
  0x1d,0x0a,0x96,0x13,
  0x18,0x00,0x43,0x6f,
  0x75,0x6c,0x64,0x6e,
  0x27,0x74,0x20,0x6c,
  0x6f,0x61,0x64,0x20,
  0x79,0x6f,0x75,0x72,
  0x20,0x66,0x69,0x6c,
  0x65,0x21,0xb6,0x04,
  0xff,0x00
};

void TapePatchROM(unsigned char *pROM)
{
    pROM[0x18a7] = 0xed;    // tape_load_p
    pROM[0x18a8] = 0xfc;
    pROM[0x18a9] = 0xc9;

    pROM[0x1820] = 0xed;    // tape_save_p
    pROM[0x1821] = 0xfd;
    pROM[0x1822] = 0xc9;
}

boolean TapeInitialize(void)
{
    if (f_mount(&s_FileSystem, DRIVE, 1) != FR_OK) {
        CLogger::Get()->Write(FromTape, LogWarning, "Cannot mount SD card, tape disabled");
        return FALSE;
    }

    s_bMounted = TRUE;
    CLogger::Get()->Write(FromTape, LogNotice, "SD card mounted");

    return TRUE;
}

void TapeReset(void)
{
    s_bDataPending = FALSE;
    s_nCurrentPos = 0;                      // the current tape stays in, rewound

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

// Ace name (10 chars, space padded) -> SD:/<name>.tap with FAT-safe characters
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
    strcpy(pPath, DRIVE "/");
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

// A tape: .tap or .tzx
static boolean has_tape_extension(const char *pName)
{
    return has_extension(pName, ".tap") || has_extension(pName, ".tzx");
}

static boolean read_bytes(FIL *pFile, unsigned char *pBuffer, unsigned nCount)
{
    UINT nRead;
    return f_read(pFile, pBuffer, nCount, &nRead) == FR_OK && nRead == nCount;
}

static boolean skip_bytes(FIL *pFile, FSIZE_t nCount)
{
    return f_lseek(pFile, f_tell(pFile) + nCount) == FR_OK;
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
 * TZX: find the next data block and skip its flag byte. Data blocks are the
 * standard (10h), turbo (11h) and pure data (14h) blocks, and generalized data
 * blocks (19h) with one bit per symbol. Other blocks are skipped, including
 * direct recordings (15h) and CSW (18h), which hold sampled sound, not bytes.
 */
static boolean next_tzx_block(FIL *pFile, unsigned *pSize)
{
    if (f_tell(pFile) == 0) {
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
            FSIZE_t nEnd = f_tell(pFile) - 14 + le(Field, 4);
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
                nSkip = nEnd - f_tell(pFile);
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
            *pSize = nData - 1;             // without the flag byte
            return skip_bytes(pFile, 1);
        }
        if (!skip_bytes(pFile, nSkip + nData)) {
            return FALSE;
        }
    }
}

// The next tape block: its size (including the checksum), with the file at its first byte
static boolean next_block(FIL *pFile, boolean bTzx, unsigned *pSize)
{
    if (bTzx) {
        return next_tzx_block(pFile, pSize);
    }

    unsigned char Len[2];
    if (!read_bytes(pFile, Len, 2)) {
        return FALSE;
    }
    *pSize = le(Len, 2);

    return TRUE;
}

/*
 * Look for a header matching the requested type and name in pPath, starting at
 * nStart. On success the header and the following data block are buffered and
 * the tape position after the data block is remembered.
 */
static boolean search_tape(const char *pPath, FSIZE_t nStart, const unsigned char *pRequest)
{
    FIL File;
    if (f_open(&File, pPath, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        return FALSE;
    }

    if (nStart > 0 && f_lseek(&File, nStart) != FR_OK) {
        f_close(&File);
        return FALSE;
    }

    const boolean bTzx = has_extension(pPath, ".tzx");
    const boolean bAnyName = is_blank_name(pRequest + 1);
    boolean bFound = FALSE;
    unsigned nSize;

    while (!bFound && next_block(&File, bTzx, &nSize)) {
        if (nSize != HEADER_SIZE + 1) {
            f_lseek(&File, f_tell(&File) + nSize);      // skip non-header block
            continue;
        }

        unsigned char Block[HEADER_SIZE + 1];
        UINT nRead;
        if (f_read(&File, Block, sizeof(Block), &nRead) != FR_OK || nRead != sizeof(Block)) {
            break;
        }

        if (   Block[0] != pRequest[0]
            || (!bAnyName && memcmp(Block + 1, pRequest + 1, NAME_SIZE) != 0)) {
            continue;
        }

        // Matching header: buffer the data block that follows it
        unsigned nDataSize;
        if (!next_block(&File, bTzx, &nDataSize) || nDataSize == 0) {
            break;
        }
        nDataSize--;                                    // without checksum
        unsigned nWant = nDataSize < sizeof(s_Data) ? nDataSize : sizeof(s_Data);
        if (f_read(&File, s_Data, nWant, &nRead) != FR_OK || nRead != nWant) {
            break;
        }
        f_lseek(&File, f_tell(&File) + (nDataSize - nWant) + 1);   // skip rest + checksum

        memcpy(s_Header, Block, HEADER_SIZE);
        s_nDataSize = nWant;
        strncpy(s_CurrentTape, pPath, sizeof(s_CurrentTape) - 1);
        s_CurrentTape[sizeof(s_CurrentTape) - 1] = '\0';
        s_nCurrentPos = f_tell(&File);
        bFound = TRUE;
    }

    f_close(&File);

    return bFound;
}

int TapeListFiles(TTapeListEntry *pEntries, unsigned nMax, boolean bWithSnapshots)
{
    if (!s_bMounted) {
        return -1;
    }

    DIR Dir;
    FILINFO Info;
    if (f_opendir(&Dir, DRIVE "/") != FR_OK) {
        return -1;
    }

    unsigned nCount = 0;
    while (nCount < nMax && f_readdir(&Dir, &Info) == FR_OK && Info.fname[0] != '\0') {
        boolean bSnapshot = bWithSnapshots && has_extension(Info.fname, ".ace");
        boolean bSource = bWithSnapshots && has_source_extension(Info.fname);
        if ((Info.fattrib & (AM_DIR | AM_HID | AM_SYS))
            || !(has_tape_extension(Info.fname) || bSnapshot || bSource)) {
            continue;
        }

        // Insertion sort by name, ignoring case
        unsigned i = nCount++;
        while (i > 0 && strcasecmp(pEntries[i - 1].Name, Info.fname) > 0) {
            pEntries[i] = pEntries[i - 1];
            i--;
        }
        strncpy(pEntries[i].Name, Info.fname, TAPE_LIST_NAME_SIZE - 1);
        pEntries[i].Name[TAPE_LIST_NAME_SIZE - 1] = '\0';
        pEntries[i].nSize = (unsigned) Info.fsize;
        pEntries[i].bSnapshot = bSnapshot;
        pEntries[i].bSource = bSource;
    }

    f_closedir(&Dir);

    return (int) nCount;
}

// Plain names in the root only: no paths, drives or wildcards
static boolean is_valid_name(const char *pInput)
{
    if (pInput[0] == '\0' || pInput[0] == ' ' || pInput[0] == '.') {
        return FALSE;
    }
    for (const char *p = pInput; *p != '\0'; p++) {
        if (*p < 0x20 || strchr("\\/:*?\"<>|", *p) != nullptr) {
            return FALSE;
        }
    }
    return TRUE;
}

TTapeFileResult TapeInsert(const char *pName)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    if (!has_tape_extension(pName)) {
        return TapeFileInvalidName;
    }

    char Path[MAX_PATH];
    strcpy(Path, DRIVE "/");
    strncat(Path, pName, sizeof(Path) - strlen(Path) - 1);

    FILINFO Info;
    if (f_stat(Path, &Info) != FR_OK || (Info.fattrib & AM_DIR)) {
        return TapeFileNotFound;
    }

    strcpy(s_CurrentTape, Path);
    s_nCurrentPos = 0;
    s_bDataPending = FALSE;
    CLogger::Get()->Write(FromTape, LogNotice, "Inserted %s", Path);

    return TapeFileOK;
}

void TapeEject(void)
{
    if (s_CurrentTape[0] != '\0') {
        CLogger::Get()->Write(FromTape, LogNotice, "Ejected %s", s_CurrentTape);
    }
    s_CurrentTape[0] = '\0';
    s_nCurrentPos = 0;
    s_bDataPending = FALSE;
}

TTapeFileResult TapeCreate(const char *pInput, char *pName, unsigned nNameSize)
{
    pName[0] = '\0';
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    if (!is_valid_name(pInput) || has_extension(pInput, ".tzx") || has_extension(pInput, ".ace")) {
        return TapeFileInvalidName;
    }

    char Path[MAX_PATH];
    strcpy(Path, DRIVE "/");
    strncat(Path, pInput, sizeof(Path) - strlen(Path) - 5);
    if (!has_extension(Path, ".tap")) {
        strcat(Path, ".tap");
    }
    strncpy(pName, Path + sizeof(DRIVE), nNameSize - 1);
    pName[nNameSize - 1] = '\0';

    FIL File;
    FRESULT Result = f_open(&File, Path, FA_WRITE | FA_CREATE_NEW);
    if (Result == FR_EXIST) {
        return TapeFileExists;
    }
    if (Result != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot create %s", Path);
        return TapeFileError;
    }
    f_close(&File);

    CLogger::Get()->Write(FromTape, LogNotice, "New tape %s", Path);

    return TapeInsert(Path + sizeof(DRIVE));
}

const char *TapeGetInserted(void)
{
    return s_CurrentTape[0] != '\0' ? s_CurrentTape + sizeof(DRIVE) : "";
}

TTapeFileResult TapeDeleteFile(const char *pName)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }
    if (!has_tape_extension(pName) && !has_extension(pName, ".ace") && !has_source_extension(pName)) {
        return TapeFileInvalidName;
    }

    char Path[MAX_PATH];
    strcpy(Path, DRIVE "/");
    strncat(Path, pName, sizeof(Path) - strlen(Path) - 1);

    // Don't keep reading from a tape that no longer exists
    if (strcasecmp(Path, s_CurrentTape) == 0) {
        s_CurrentTape[0] = '\0';
        s_nCurrentPos = 0;
        s_bDataPending = FALSE;
    }

    if (f_unlink(Path) != FR_OK) {
        CLogger::Get()->Write(FromTape, LogError, "Cannot delete %s", Path);
        return TapeFileError;
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Deleted %s", Path);

    return TapeFileOK;
}

static boolean find_file(const unsigned char *pRequest)
{
    if (!s_bMounted) {
        return FALSE;
    }

    // 1. Continue on the current tape, then from its start
    if (s_CurrentTape[0] != '\0') {
        char Path[MAX_PATH];
        strcpy(Path, s_CurrentTape);
        if (   search_tape(Path, s_nCurrentPos, pRequest)
            || (s_nCurrentPos > 0 && search_tape(Path, 0, pRequest))) {
            return TRUE;
        }
    }

    // 2. A tape named after the file: .tap, then .tzx
    char NamedPath[MAX_PATH];
    make_path(NamedPath, pRequest + 1);
    char NamedTzxPath[MAX_PATH];
    strcpy(NamedTzxPath, NamedPath);
    strcpy(NamedTzxPath + strlen(NamedTzxPath) - 4, ".tzx");
    if (   !is_blank_name(pRequest + 1)
        && (search_tape(NamedPath, 0, pRequest) || search_tape(NamedTzxPath, 0, pRequest))) {
        return TRUE;
    }

    // 3. Any tape in the root directory
    DIR Dir;
    FILINFO Info;
    if (f_opendir(&Dir, DRIVE "/") != FR_OK) {
        return FALSE;
    }

    boolean bFound = FALSE;
    while (!bFound && f_readdir(&Dir, &Info) == FR_OK && Info.fname[0] != '\0') {
        if ((Info.fattrib & (AM_DIR | AM_HID | AM_SYS)) || !has_tape_extension(Info.fname)) {
            continue;
        }

        char Path[MAX_PATH];
        strcpy(Path, DRIVE "/");
        strncat(Path, Info.fname, sizeof(Path) - strlen(Path) - 1);
        if (   strcasecmp(Path, NamedPath) == 0 || strcasecmp(Path, NamedTzxPath) == 0
            || strcasecmp(Path, s_CurrentTape) == 0) {
            continue;                                   // already searched
        }

        bFound = search_tape(Path, 0, pRequest);
    }

    f_closedir(&Dir);

    return bFound;
}

// Fallback when the file can't be found anywhere: a stand-in with the wanted name
static void use_empty_file(const unsigned char *pRequest)
{
    const unsigned char *pTape = pRequest[0] == 0 ? empty_dict : empty_bytes;

    memcpy(s_Header, pTape + 2, HEADER_SIZE);
    memcpy(s_Header + 1, pRequest + 1, NAME_SIZE);

    const unsigned char *pData = pTape + 2 + HEADER_SIZE + 1;
    s_nDataSize = (pData[0] | (pData[1] << 8)) - 1;
    memcpy(s_Data, pData + 2, s_nDataSize);
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
 */
#define SNAPSHOT_START  0x2000
#define SYSVAR_RAMTOP   0x3C18      // first byte above the RAM, 0 for 64K
#define ROM_QUIT_RETURN 0x04F7      // return stack entry of an Ace at the Forth prompt

static unsigned char s_Snapshot[0x10000];   // decoded, indexed by Ace address

// Buffered byte reader for the decoder
struct TSnapshotReader {
    FIL           *pFile;
    unsigned char  Buffer[1024];
    UINT           nCount;
    UINT           nPos;
};

static int read_snapshot_byte(TSnapshotReader *pReader)
{
    if (pReader->nPos >= pReader->nCount) {
        if (f_read(pReader->pFile, pReader->Buffer, sizeof(pReader->Buffer), &pReader->nCount) != FR_OK
            || pReader->nCount == 0) {
            return -1;
        }
        pReader->nPos = 0;
    }
    return pReader->Buffer[pReader->nPos++];
}

static unsigned short snapshot_word(unsigned nAddr)
{
    return s_Snapshot[nAddr] | (s_Snapshot[nAddr + 1] << 8);
}

TTapeFileResult TapeLoadSnapshot(const char *pName)
{
    if (!s_bMounted) {
        return TapeFileNoCard;
    }

    char Path[MAX_PATH];
    strcpy(Path, DRIVE "/");
    strncat(Path, pName, sizeof(Path) - strlen(Path) - 1);

    FIL File;
    if (f_open(&File, Path, FA_READ) != FR_OK) {
        return TapeFileNotFound;
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

    f_close(&File);

    if (!bEnd || nAddr < SYSVAR_RAMTOP + 2) {   // must reach the registers and RAMTOP
        CLogger::Get()->Write(FromTape, LogWarning, "%s is not a valid .ace snapshot", pName);
        return TapeFileInvalidSnapshot;
    }

    // The RAM the snapshot's machine had (up to its RAMTOP), at least what the file holds
    unsigned nRamTop = snapshot_word(SYSVAR_RAMTOP);
    if (nRamTop == 0) {
        nRamTop = 0x10000;
    }
    unsigned nNeeded = nRamTop > nAddr ? nRamTop : nAddr;

    // A bigger RAM pack if the snapshot does not fit in the current one
    unsigned nPackEnd = RAM_PACK_START + RamGetPackKB() * 1024;
    if (nNeeded > nPackEnd) {
        unsigned nKB = (nNeeded - RAM_PACK_START + 0x3FFF) / 0x4000 * 16;
        RamSetPackKB(nKB);
        CLogger::Get()->Write(FromTape, LogNotice, "RAM pack enlarged to %uK for %s", nKB, pName);
    }

    // Through the Z80 store path, in address order: the last copy of each mirrored
    // area (video RAM, character set, 1K RAM and system variables) is the one kept
    TapeReset();
    for (unsigned a = SNAPSHOT_START; a < nAddr; a++) {
        unsigned short nDest = (unsigned short) a;
        store(nDest, s_Snapshot[a]);
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
            CLogger::Get()->Write(FromTape, LogWarning, "%s: stack not in the file, SP kept", pName);
        }
        load_snapshot = 1;
    } else {
        CLogger::Get()->Write(FromTape, LogWarning, "%s: no CPU registers, memory only", pName);
    }

    CLogger::Get()->Write(FromTape, LogNotice, "Loaded snapshot %s (0x%04X - 0x%04X)",
                          pName, SNAPSHOT_START, nAddr - 1);

    return TapeFileOK;
}

extern "C" int tape_load_p(unsigned char *mem, unsigned short dest, unsigned short length, int bLoad)
{
    (void) mem;

    // A data block request; a request to the header buffer means the ROM gave up
    // on the previous file and is looking for a new header
    if (s_bDataPending && dest != s_nHeaderDest) {
        s_bDataPending = FALSE;
        if (bLoad) {
            poke_block(dest, s_Data, s_nDataSize);
            CLogger::Get()->Write(FromTape, LogNotice, "Load complete (%u bytes)", s_nDataSize);
            return 1;
        }

        // VERIFY / BVERIFY: compare, memory is not changed
        boolean bSame = length == s_nDataSize;
        for (unsigned i = 0; i < s_nDataSize && bSame; i++) {
            bSame = ::mem[(dest + i) & 0xFFFF] == s_Data[i];
        }
        CLogger::Get()->Write(FromTape, bSame ? LogNotice : LogWarning, "Verify %s (%u bytes)",
                              bSame ? "OK" : "failed", s_nDataSize);
        return bSame;
    }

    unsigned char Request[1 + NAME_SIZE];
    memcpy(Request, ::mem + REQUEST_HEADER, sizeof(Request));

    char Name[NAME_SIZE + 1];
    memcpy(Name, Request + 1, NAME_SIZE);
    Name[NAME_SIZE] = '\0';

    if (find_file(Request)) {
        if (is_blank_name(Request + 1)) {
            poke_block(REQUEST_HEADER + 1, s_Header + 1, NAME_SIZE);   // what the ROM compares
            memcpy(Name, s_Header + 1, NAME_SIZE);
        }
        CLogger::Get()->Write(FromTape, LogNotice, "Found %s \"%s\" in %s",
                              Request[0] == 0 ? "dict" : "bytes", Name, s_CurrentTape);
    } else {
        CLogger::Get()->Write(FromTape, LogWarning, "File \"%s\" not found on SD card", Name);
        use_empty_file(Request);
    }

    poke_block(dest, s_Header, HEADER_SIZE);
    s_nHeaderDest = dest;
    s_bDataPending = TRUE;
    return 1;
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
        // Onto the current .tap like a cassette, else into a tape of its own
        char Path[MAX_PATH];
        boolean bAppend = has_extension(s_CurrentTape, ".tap");
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
        CLogger::Get()->Write(FromTape, LogNotice, "%s %s", bAppend ? "Appending to" : "Saving to", Path);
    } else if (!s_bSaveOpen) {
        return;
    }

    unsigned char Len[2] = { (unsigned char) ((size + 1) & 0xFF), (unsigned char) ((size + 1) >> 8) };
    unsigned char nChecksum = 0;
    for (unsigned i = 0; i < size; i++) {
        nChecksum ^= block[i];
    }

    UINT nWritten;
    boolean bOK =    f_write(&s_SaveFile, Len, 2, &nWritten) == FR_OK && nWritten == 2
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
