#ifndef _ZIP_SD_H
#define _ZIP_SD_H

#include <circle/types.h>

/*
 * Reading .zip files on the SD card (F5 opens them like folders, read only): the
 * central directory (the list of files at the end of the .zip), and one file at a
 * time unpacked into memory. Stored (0) and deflate (8, with puff.c) files only; no
 * encryption, ZIP64 or archives split over several files.
 *
 * A .zip in a .zip opens too (collections often hold one .zip per program): it is
 * unpacked into memory (at most ZIP_MAX_ENTRY bytes), and read from there.
 */

#define ZIP_MAX_ENTRY   (4 * 1024 * 1024)   // largest file unpacked from a .zip
#define ZIP_NAME_SIZE   512                 // longest name (path) of a file in a .zip, + 1

enum TZipResult {
    ZipOK,
    ZipNotFound,            // no such .zip, or no such file in it
    ZipNotZip,              // not a .zip file, or a damaged one
    ZipUnsupported,         // ZIP64, split, encrypted, or another compression method
    ZipTooBig,              // over ZIP_MAX_ENTRY unpacked
    ZipBadData,             // damaged compressed data, or wrong CRC
    ZipNoMemory,
    ZipReadError
};

struct TZipEntry {
    const char *pName;      // path in the .zip ('/' between folders), 0-terminated
    unsigned    nSize;      // unpacked
    boolean     bFolder;    // a folder entry ("name/")
};

// Called for each file in a .zip, in the order of its central directory; stop by
// returning FALSE
typedef boolean TZipEntryHandler(const TZipEntry *pEntry, void *pParam);

// Open the .zip pZipPath (full FatFs path: "SD:/programs/x.zip"), or a .zip in it that
// the path pInside ("" for none) goes through: for "a/b.zip/c.tap" that is a/b.zip, if
// it is a file in pZipPath (in a .zip in a .zip as well). *pnPrefix (if given) gets
// the length of the part of pInside that took ("a/b.zip/"; 0 for pZipPath itself);
// the rest is the path in the .zip opened. ZipForEach() and ZipExtract() read it.
TZipResult ZipOpen(const char *pZipPath, const char *pInside = "", unsigned *pnPrefix = nullptr);

TZipResult ZipForEach(TZipEntryHandler *pHandler, void *pParam);

// Unpack the file pEntry (its path) of the open .zip into memory: *ppData (delete []
// it) gets its *pLength bytes
TZipResult ZipExtract(const char *pEntry, u8 **ppData, unsigned *pLength);

// The .zip open is cached (with the central directory of the one on the card): forget
// it when files change
void ZipForget(void);

// CRC-32 as in .zip files (and gzip, PNG)
u32 ZipCrc32(const u8 *pData, unsigned nLength);

#endif
