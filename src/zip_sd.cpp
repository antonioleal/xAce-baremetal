/*
 * .zip files on the SD card, read only (see zip_sd.h).
 *
 * A .zip ends with the "end of central directory" record (EOCD, after which only an
 * archive comment of up to 64K may follow). It gives the place and size of the
 * central directory, which has one record per file: its name, method, CRC, sizes and
 * the place of its "local header", which the compressed data follows. The sizes are
 * taken from the central directory: the local header may leave them out (when the
 * .zip was written as a stream, with a "data descriptor" after the data).
 */
#include "zip_sd.h"
#include "tape_sd.h"

extern "C" {
#include "puff.h"
}

#include <circle/logger.h>
#include <circle/util.h>
#include <fatfs/ff.h>

#define EOCD_SIZE           22
#define EOCD_SEARCH         (EOCD_SIZE + 0xFFFF)    // the record and the longest comment
#define CENTRAL_HEADER_SIZE 46
#define LOCAL_HEADER_SIZE   30
#define MAX_CENTRAL_SIZE    (2 * 1024 * 1024)       // central directory: ~20000 files

#define SIG_EOCD            0x06054B50
#define SIG_CENTRAL         0x02014B50
#define SIG_LOCAL           0x04034B50

#define METHOD_STORED       0
#define METHOD_DEFLATE      8
#define FLAG_ENCRYPTED      0x0001

static const char FromZip[] = "zip";

static unsigned get16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 get32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24); }

// A .zip: on the card (pData nullptr; its central directory read into memory), or
// unpacked into memory from another .zip (pData; the central directory is in it)
struct TArchive {
    const u8   *pData;
    unsigned    nDataSize;
    const u8   *pCentral;
    unsigned    nCentralSize;
    unsigned    nEntries;
};

// The cached .zip on the card: which (path, size and time, so that a .zip replaced
// since is read again), and its central directory
static char      s_CachePath[TAPE_FULL_PATH_SIZE] = "";
static FSIZE_t   s_nCacheSize = 0;
static u16       s_nCacheDate = 0, s_nCacheTime = 0;
static u8       *s_pCardCentral = nullptr;
static TArchive  s_Card;

// The cached .zip in it (or in a .zip in it...): its path in s_CachePath, and its data
static char      s_NestedPath[TAPE_PATH_SIZE] = "";
static u8       *s_pNestedData = nullptr;
static TArchive  s_Nested;

// The .zip ZipOpen() opened: &s_Card, &s_Nested, or nullptr
static const TArchive *s_pOpen = nullptr;

static void forget_nested(void)
{
    if (s_pOpen == &s_Nested) {
        s_pOpen = nullptr;
    }
    s_NestedPath[0] = '\0';
    delete [] s_pNestedData;
    s_pNestedData = nullptr;
    memset(&s_Nested, 0, sizeof s_Nested);
}

void ZipForget(void)
{
    forget_nested();
    s_pOpen = nullptr;
    s_CachePath[0] = '\0';
    delete [] s_pCardCentral;
    s_pCardCentral = nullptr;
    memset(&s_Card, 0, sizeof s_Card);
}

static boolean read_at(FIL *pFile, FSIZE_t nPos, void *pBuffer, unsigned nCount)
{
    UINT nRead;
    return    f_lseek(pFile, nPos) == FR_OK
           && f_read(pFile, pBuffer, nCount, &nRead) == FR_OK && nRead == nCount;
}

// The EOCD record in pTail, the last nTail bytes of a .zip of nZipSize bytes: the
// number of files, size and place of the central directory
static TZipResult find_central(const u8 *pTail, unsigned nTail, FSIZE_t nZipSize,
                               unsigned *pnEntries, unsigned *pnSize, u32 *pnOffset)
{
    const u8 *pEOCD = nullptr;
    for (int i = (int) nTail - EOCD_SIZE; i >= 0; i--) {
        if (get32(pTail + i) == SIG_EOCD && i + EOCD_SIZE + get16(pTail + i + 20) <= nTail) {
            pEOCD = pTail + i;
            break;
        }
    }
    if (pEOCD == nullptr) {
        return ZipNotZip;
    }

    unsigned nDisk = get16(pEOCD + 4), nCentralDisk = get16(pEOCD + 6);
    unsigned nDiskEntries = get16(pEOCD + 8);
    *pnEntries = get16(pEOCD + 10);
    *pnSize = get32(pEOCD + 12);
    *pnOffset = get32(pEOCD + 16);
    if (   *pnEntries == 0xFFFF || *pnSize == 0xFFFFFFFF || *pnOffset == 0xFFFFFFFF
        || nDisk != 0 || nCentralDisk != 0 || nDiskEntries != *pnEntries) {
        return ZipUnsupported;                  // ZIP64, or split over several files
    }
    if (*pnSize > MAX_CENTRAL_SIZE) {
        return ZipTooBig;
    }
    if ((FSIZE_t) *pnOffset + *pnSize > nZipSize) {
        return ZipNotZip;
    }
    return ZipOK;
}

// Read the central directory of a .zip on the card into the cache (if not there
// already)
static TZipResult load_card(const char *pZipPath)
{
    FILINFO Info;
    if (f_stat(pZipPath, &Info) != FR_OK || (Info.fattrib & AM_DIR)) {
        return ZipNotFound;
    }
    if (   s_pCardCentral != nullptr && strcmp(s_CachePath, pZipPath) == 0
        && s_nCacheSize == Info.fsize && s_nCacheDate == Info.fdate && s_nCacheTime == Info.ftime) {
        return ZipOK;
    }
    ZipForget();
    if (Info.fsize < EOCD_SIZE) {
        return ZipNotZip;
    }

    FIL File;
    if (f_open(&File, pZipPath, FA_READ) != FR_OK) {
        return ZipReadError;
    }

    // The end of the file, with the EOCD record
    unsigned nTail = Info.fsize < EOCD_SEARCH ? (unsigned) Info.fsize : EOCD_SEARCH;
    u8 *pTail = new u8[nTail];
    if (pTail == nullptr) {
        f_close(&File);
        return ZipNoMemory;
    }
    unsigned nEntries = 0, nSize = 0;
    u32 nOffset = 0;
    TZipResult Result = ZipOK;
    if (!read_at(&File, Info.fsize - nTail, pTail, nTail)) {
        Result = ZipReadError;
    } else {
        Result = find_central(pTail, nTail, Info.fsize, &nEntries, &nSize, &nOffset);
    }
    delete [] pTail;

    if (Result == ZipOK) {
        s_pCardCentral = new u8[nSize + 1];
        if (s_pCardCentral == nullptr) {
            Result = ZipNoMemory;
        } else if (!read_at(&File, nOffset, s_pCardCentral, nSize)) {
            Result = ZipReadError;
        }
    }
    f_close(&File);

    if (Result != ZipOK) {
        ZipForget();
        CLogger::Get()->Write(FromZip, LogWarning, "Cannot read %s (%d)", pZipPath, Result);
        return Result;
    }

    strcpy(s_CachePath, pZipPath);
    s_nCacheSize = Info.fsize;
    s_nCacheDate = Info.fdate;
    s_nCacheTime = Info.ftime;
    s_Card.pData = nullptr;
    s_Card.nDataSize = 0;
    s_Card.pCentral = s_pCardCentral;
    s_Card.nCentralSize = nSize;
    s_Card.nEntries = nEntries;
    return ZipOK;
}

// The central directory record of the next file from *pPos; FALSE at the end or
// when the directory is damaged
struct TCentralRecord {
    const u8 *pRecord;
    char      Name[ZIP_NAME_SIZE];
    boolean   bNameTooLong;
};

static boolean next_record(const TArchive *pZip, unsigned *pPos, TCentralRecord *pRec)
{
    unsigned nPos = *pPos;
    if (   nPos + CENTRAL_HEADER_SIZE > pZip->nCentralSize
        || get32(pZip->pCentral + nPos) != SIG_CENTRAL) {
        return FALSE;
    }
    const u8 *p = pZip->pCentral + nPos;
    unsigned nName = get16(p + 28), nExtra = get16(p + 30), nComment = get16(p + 32);
    unsigned nNext = nPos + CENTRAL_HEADER_SIZE + nName + nExtra + nComment;
    if (nNext > pZip->nCentralSize) {
        return FALSE;
    }

    pRec->pRecord = p;
    pRec->bNameTooLong = nName >= ZIP_NAME_SIZE;
    unsigned n = pRec->bNameTooLong ? 0 : nName;
    memcpy(pRec->Name, p + CENTRAL_HEADER_SIZE, n);
    pRec->Name[n] = '\0';
    for (unsigned i = 0; i < n; i++) {
        if (pRec->Name[i] == '\\') {
            pRec->Name[i] = '/';                // some old tools wrote DOS paths
        }
    }

    *pPos = nNext;
    return TRUE;
}

// The record of the file pEntry (a file, not a folder)
static boolean find_record(const TArchive *pZip, const char *pEntry, TCentralRecord *pRec)
{
    unsigned nPos = 0;
    for (unsigned i = 0; i < pZip->nEntries && next_record(pZip, &nPos, pRec); i++) {
        if (!pRec->bNameTooLong && strcmp(pRec->Name, pEntry) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

// Bytes of a .zip: from the card (s_CachePath, open in pFile) or from memory
static boolean read_zip(const TArchive *pZip, FIL *pFile, u32 nPos, void *pBuffer, unsigned nCount)
{
    if (pZip->pData == nullptr) {
        return read_at(pFile, nPos, pBuffer, nCount);
    }
    if (nPos > pZip->nDataSize || nCount > pZip->nDataSize - nPos) {
        return FALSE;
    }
    memcpy(pBuffer, pZip->pData + nPos, nCount);
    return TRUE;
}

// Unpack the file of the record into memory; pEntry for messages
static TZipResult extract(const TArchive *pZip, const TCentralRecord *pRec, const char *pEntry,
                          u8 **ppData, unsigned *pLength)
{
    *ppData = nullptr;
    *pLength = 0;

    const u8 *p = pRec->pRecord;
    unsigned nFlags = get16(p + 8), nMethod = get16(p + 10);
    u32 nCrc = get32(p + 16), nCompressed = get32(p + 20), nSize = get32(p + 24);
    u32 nLocal = get32(p + 42);
    if (   (nFlags & FLAG_ENCRYPTED) || (nMethod != METHOD_STORED && nMethod != METHOD_DEFLATE)
        || nCompressed == 0xFFFFFFFF || nSize == 0xFFFFFFFF || nLocal == 0xFFFFFFFF) {
        return ZipUnsupported;
    }
    if (nSize > ZIP_MAX_ENTRY || (nMethod == METHOD_STORED && nCompressed != nSize)
        || nCompressed > ZIP_MAX_ENTRY + ZIP_MAX_ENTRY / 8 + 1024) {
        return nSize > ZIP_MAX_ENTRY ? ZipTooBig : ZipNotZip;
    }

    // The local header gives where the data starts (its name and extra field may
    // differ in length from those in the central directory)
    FIL File;
    boolean bCard = pZip->pData == nullptr;
    if (bCard && f_open(&File, s_CachePath, FA_READ) != FR_OK) {
        return ZipReadError;
    }
    u8 Local[LOCAL_HEADER_SIZE];
    TZipResult Result = ZipOK;
    if (!read_zip(pZip, &File, nLocal, Local, sizeof Local)) {
        Result = bCard ? ZipReadError : ZipNotZip;
    } else if (get32(Local) != SIG_LOCAL) {
        Result = ZipNotZip;
    }
    u32 nData = nLocal + LOCAL_HEADER_SIZE + get16(Local + 26) + get16(Local + 28);

    u8 *pPacked = nullptr, *pData = nullptr;
    if (Result == ZipOK) {
        pPacked = new u8[nCompressed + 1];
        pData = nMethod == METHOD_STORED ? pPacked : new u8[nSize + 1];
        if (pPacked == nullptr || pData == nullptr) {
            Result = ZipNoMemory;
        } else if (!read_zip(pZip, &File, nData, pPacked, nCompressed)) {
            Result = bCard ? ZipReadError : ZipNotZip;
        }
    }
    if (bCard) {
        f_close(&File);
    }

    if (Result == ZipOK && nMethod == METHOD_DEFLATE) {
        unsigned long nOut = nSize, nIn = nCompressed;
        if (puff(pData, &nOut, pPacked, &nIn) != 0 || nOut != nSize) {
            Result = ZipBadData;
        }
    }
    if (pPacked != pData) {
        delete [] pPacked;
    }
    if (Result == ZipOK && ZipCrc32(pData, nSize) != nCrc) {
        Result = ZipBadData;
    }
    if (Result != ZipOK) {
        delete [] pData;
        CLogger::Get()->Write(FromZip, LogWarning, "Cannot unpack %s from %s (%d)",
                              pEntry, s_CachePath, Result);
        return Result;
    }

    *ppData = pData;
    *pLength = nSize;
    return ZipOK;
}

// Make the .zip pPath (in the .zip open; pPath is its path from s_CachePath) the open
// one, unpacking it into s_Nested
static TZipResult open_nested(const char *pEntry, const char *pPath)
{
    TCentralRecord Rec;
    if (!find_record(s_pOpen, pEntry, &Rec)) {
        return ZipNotFound;
    }
    u8 *pData;
    unsigned nLength;
    TZipResult Result = extract(s_pOpen, &Rec, pEntry, &pData, &nLength);
    if (Result != ZipOK) {
        return Result;
    }

    unsigned nEntries = 0, nSize = 0;
    u32 nOffset = 0;
    unsigned nTail = nLength < EOCD_SEARCH ? nLength : EOCD_SEARCH;
    Result = nLength < EOCD_SIZE ? ZipNotZip
                                 : find_central(pData + nLength - nTail, nTail, nLength,
                                                &nEntries, &nSize, &nOffset);
    if (Result != ZipOK) {
        delete [] pData;
        CLogger::Get()->Write(FromZip, LogWarning, "Cannot read %s in %s (%d)",
                              pPath, s_CachePath, Result);
        return Result;
    }

    forget_nested();
    strcpy(s_NestedPath, pPath);
    s_pNestedData = pData;
    s_Nested.pData = pData;
    s_Nested.nDataSize = nLength;
    s_Nested.pCentral = pData + nOffset;
    s_Nested.nCentralSize = nSize;
    s_Nested.nEntries = nEntries;
    s_pOpen = &s_Nested;
    return ZipOK;
}

// Is pName (nLen characters) a file in the .zip open, not a folder?
static boolean is_file(const char *pName, unsigned nLen)
{
    char Name[ZIP_NAME_SIZE];
    if (nLen >= sizeof Name) {
        return FALSE;
    }
    memcpy(Name, pName, nLen);
    Name[nLen] = '\0';
    TCentralRecord Rec;
    return find_record(s_pOpen, Name, &Rec);
}

TZipResult ZipOpen(const char *pZipPath, const char *pInside, unsigned *pnPrefix)
{
    s_pOpen = nullptr;
    if (pnPrefix != nullptr) {
        *pnPrefix = 0;
    }
    TZipResult Result = load_card(pZipPath);
    if (Result != ZipOK) {
        return Result;
    }
    s_pOpen = &s_Card;

    // Each name in pInside, from the start: a .zip file in the .zip open opens. Its
    // path in the .zip open starts at nStart, its path from the card .zip at 0.
    unsigned nStart = 0;
    unsigned nLen = strlen(pInside);
    unsigned nNested = strlen(s_NestedPath);
    if (   s_pNestedData != nullptr && strncmp(pInside, s_NestedPath, nNested) == 0
        && (pInside[nNested] == '/' || pInside[nNested] == '\0')) {
        s_pOpen = &s_Nested;                    // unpacked already
        nStart = pInside[nNested] == '/' ? nNested + 1 : nNested;
    }
    for (unsigned i = nStart; i <= nLen; i++) {
        if (pInside[i] != '/' && pInside[i] != '\0') {
            continue;
        }
        if (   i < nStart + 4 || strncasecmp(pInside + i - 4, ".zip", 4) != 0
            || !is_file(pInside + nStart, i - nStart)) {
            continue;
        }
        char Path[TAPE_PATH_SIZE];
        if (i >= sizeof Path) {
            return ZipNotFound;
        }
        memcpy(Path, pInside, i);
        Path[i] = '\0';
        char Entry[ZIP_NAME_SIZE];
        memcpy(Entry, pInside + nStart, i - nStart);    // is_file(): shorter than that
        Entry[i - nStart] = '\0';
        Result = open_nested(Entry, Path);
        if (Result != ZipOK) {
            s_pOpen = nullptr;
            return Result;
        }
        nStart = pInside[i] == '/' ? i + 1 : i;
    }

    if (pnPrefix != nullptr) {
        *pnPrefix = nStart;
    }
    return ZipOK;
}

TZipResult ZipForEach(TZipEntryHandler *pHandler, void *pParam)
{
    if (s_pOpen == nullptr) {
        return ZipNotFound;
    }
    unsigned nPos = 0;
    TCentralRecord Rec;
    for (unsigned i = 0; i < s_pOpen->nEntries && next_record(s_pOpen, &nPos, &Rec); i++) {
        if (Rec.bNameTooLong || Rec.Name[0] == '\0') {
            continue;
        }
        unsigned nLen = strlen(Rec.Name);
        TZipEntry Entry;
        Entry.bFolder = Rec.Name[nLen - 1] == '/';
        if (Entry.bFolder) {
            Rec.Name[nLen - 1] = '\0';
        }
        Entry.pName = Rec.Name;
        Entry.nSize = get32(Rec.pRecord + 24);
        if (!(*pHandler)(&Entry, pParam)) {
            break;
        }
    }
    return ZipOK;
}

TZipResult ZipExtract(const char *pEntry, u8 **ppData, unsigned *pLength)
{
    *ppData = nullptr;
    *pLength = 0;
    if (s_pOpen == nullptr) {
        return ZipNotFound;
    }
    TCentralRecord Rec;
    if (!find_record(s_pOpen, pEntry, &Rec)) {
        return ZipNotFound;
    }
    return extract(s_pOpen, &Rec, pEntry, ppData, pLength);
}

u32 ZipCrc32(const u8 *pData, unsigned nLength)
{
    static u32 Table[256];
    static boolean bTable = FALSE;
    if (!bTable) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            }
            Table[i] = c;
        }
        bTable = TRUE;
    }
    u32 c = 0xFFFFFFFF;
    for (unsigned i = 0; i < nLength; i++) {
        c = Table[(c ^ pData[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFF;
}
