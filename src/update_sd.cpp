/*
 * System update from a release .zip (see update_sd.h).
 *
 * The kernel image carries a build marker, s_Marker below: "XACE-BAREMETAL-BUILD:" then
 * version|PI<model>|build date and time|. An uploaded kernel is checked by it (an
 * xAce-baremetal kernel for this model), and after a trial restart the running kernel
 * knows by it whether it is the one being tried.
 *
 * Files on the card while an update is going on:
 *   SD:/update.zip              the uploaded release (until installed or discarded)
 *   SD:/update/...              new files, put in place after the trial
 *   SD:/update-<name>           Pi 4 / 400: the new kernel and boot firmware, tried first
 *   SD:/update/manifest.txt     the build tried, then "staged path|live path" lines
 *   SD:/tryboot.txt             Pi 4 / 400: config.txt for the trial restart, which
 *                               also names SD:/update-cmdline.txt: cmdline.txt with
 *                               "xacetrial=1", so the tried start-up knows it is one
 *   SD:/update-backup/...       the files replaced by the last update
 */
#include "update_sd.h"
#include "version.h"
#include "tape_sd.h"
#include "zip_sd.h"

#include <circle/bcmpropertytags.h>
#include <circle/startup.h>
#include <circle/logger.h>
#include <circle/util.h>
#include <fatfs/ff.h>

#define STR2(x)             #x
#define STR(x)              STR2(x)

#define MARKER_PREFIX       "XACE-BAREMETAL-BUILD:"
static const char s_Marker[] = MARKER_PREFIX XACE_VERSION "|PI" STR(RASPPI) "|" __DATE__ " " __TIME__ "|";

#if RASPPI == 1
#define KERNEL_NAME         "kernel.img"
#elif RASPPI == 2
#define KERNEL_NAME         "kernel7.img"
#elif RASPPI == 3
#define KERNEL_NAME         "kernel8-32.img"
#else
#define KERNEL_NAME         "kernel7l.img"
#endif

#define DRIVE               "SD:"
#define UPDATE_ZIP          DRIVE "/update.zip"
#define UPDATE_DIR          DRIVE "/update"
#define MANIFEST            UPDATE_DIR "/manifest.txt"
#define BACKUP_DIR          DRIVE "/update-backup"
#define TRYBOOT_TXT         DRIVE "/tryboot.txt"
#define CONFIG_TXT          DRIVE "/config.txt"
#define CMDLINE_TXT         DRIVE "/cmdline.txt"
#define TRIAL_CMDLINE       "update-cmdline.txt"
#define TRIAL_PREFIX        "update-"           // SD:/update-kernel7l.img ...

#define MAX_FILES           64                  // in a release .zip
#define NAME_SIZE           64
#define COMMIT_FRAMES       150                 // the tried Ace has run ~3 s: keep it
#define PROPTAG_SET_REBOOT_FLAGS 0x00038064     // as Linux's "reboot 0 tryboot"

static const char FromUpdate[] = "update";

// This kernel's marker fields
static char s_Version[16], s_Build[32];

// The pending update (checked .zip)
struct TPendingFile {
    char Name[NAME_SIZE];           // in the .zip, e.g. "start4.elf", "firmware/x.bin"
    unsigned nSize;
};
static boolean      s_bPending = FALSE;
static char         s_PendingVersion[16], s_PendingBuild[32];
static TPendingFile s_Files[MAX_FILES];
static unsigned     s_nFiles = 0;

static CString      s_Last;                     // what happened last (for the web page)

// At start-up
static boolean      s_bCommit = FALSE;          // the tried update is running: keep it
static unsigned     s_nFrames = 0;
static boolean      s_bMessage = FALSE;
static CString      s_Line1, s_Line2;

// --- The marker ----------------------------------------------------------------------

// The fields of a marker found in pData: version, model, build. FALSE if there is none.
static boolean find_marker(const u8 *pData, unsigned nLength, char *pVersion, unsigned *pModel,
                           char *pBuild)
{
    const unsigned nPrefix = sizeof(MARKER_PREFIX) - 1;
    for (unsigned i = 0; i + nPrefix + 8 < nLength; i++) {
        if (pData[i] != 'X' || memcmp(pData + i, MARKER_PREFIX, nPrefix) != 0) {
            continue;
        }
        // version|PIn|build|
        const char *p = (const char *) pData + i + nPrefix;
        const char *pEnd = (const char *) pData + nLength;
        char Fields[3][32];
        unsigned nField = 0, n = 0;
        while (p < pEnd && nField < 3 && *p >= ' ' && *p <= '~') {
            if (*p == '|') {
                Fields[nField++][n] = '\0';
                n = 0;
            } else if (n < 31) {
                Fields[nField][n++] = *p;
            }
            p++;
        }
        if (nField == 3 && Fields[1][0] == 'P' && Fields[1][1] == 'I' && Fields[1][2] >= '1'
            && Fields[1][2] <= '9' && Fields[1][3] == '\0' && Fields[0][0] != '\0') {
            strcpy(pVersion, Fields[0]);
            *pModel = Fields[1][2] - '0';
            strcpy(pBuild, Fields[2]);
            return TRUE;
        }
    }
    return FALSE;
}

static void parse_own_marker(void)
{
    if (s_Version[0] == '\0') {
        unsigned nModel;
        find_marker((const u8 *) s_Marker, sizeof s_Marker, s_Version, &nModel, s_Build);
    }
}

const char *UpdateVersion(void) { parse_own_marker(); return s_Version; }
unsigned UpdateModel(void) { return RASPPI; }
const char *UpdateBuild(void) { parse_own_marker(); return s_Build; }

boolean UpdateCanTryBoot(void)
{
    return RASPPI >= 4;
}

// --- Files -----------------------------------------------------------------------------

static boolean write_file(const char *pPath, const void *pData, unsigned nLength)
{
    FIL File;
    if (f_open(&File, pPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return FALSE;
    }
    UINT nWritten = 0;
    FRESULT Result = nLength > 0 ? f_write(&File, pData, nLength, &nWritten) : FR_OK;
    boolean bOK = f_close(&File) == FR_OK && Result == FR_OK && nWritten == nLength;
    if (!bOK) {
        f_unlink(pPath);
    }
    return bOK;
}

// A whole file into a new buffer (delete [] it); nullptr if missing or over nMax
static u8 *read_file(const char *pPath, unsigned nMax, unsigned *pLength)
{
    FILINFO Info;
    if (f_stat(pPath, &Info) != FR_OK || (Info.fattrib & AM_DIR) || Info.fsize > nMax) {
        return nullptr;
    }
    FIL File;
    if (f_open(&File, pPath, FA_READ) != FR_OK) {
        return nullptr;
    }
    u8 *pData = new u8[Info.fsize + 1];
    UINT nRead = 0;
    if (pData == nullptr || f_read(&File, pData, (UINT) Info.fsize, &nRead) != FR_OK
        || nRead != Info.fsize) {
        delete [] pData;
        pData = nullptr;
    }
    f_close(&File);
    *pLength = nRead;
    return pData;
}

static boolean exists(const char *pPath)
{
    FILINFO Info;
    return f_stat(pPath, &Info) == FR_OK;
}

// Delete a folder and what is in it (files and one level of subfolders, as made here)
static void delete_tree(const char *pPath)
{
    for (int nPass = 0; nPass < 1000; nPass++) {
        DIR Dir;
        FILINFO Info;
        if (f_opendir(&Dir, pPath) != FR_OK) {
            return;
        }
        FRESULT Result = f_readdir(&Dir, &Info);
        f_closedir(&Dir);
        if (Result != FR_OK) {
            return;
        }
        if (Info.fname[0] == '\0') {
            f_unlink(pPath);
            return;
        }
        CString Child;
        Child.Format("%s/%s", pPath, Info.fname);
        if (Info.fattrib & AM_DIR) {
            delete_tree(Child);
        } else {
            f_chmod(Child, 0, AM_RDO);
            f_unlink(Child);
        }
    }
}

// Make the folders of a path ("SD:/update/firmware/x" -> SD:/update, SD:/update/firmware)
static void make_folders(const char *pPath)
{
    char Path[TAPE_FULL_PATH_SIZE];
    strncpy(Path, pPath, sizeof Path - 1);
    Path[sizeof Path - 1] = '\0';
    for (char *p = Path + sizeof(DRIVE); *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            f_mkdir(Path);
            *p = '/';
        }
    }
}

// --- What a release .zip may change ----------------------------------------------------

static boolean has_ending(const char *pName, const char *pEnd)
{
    unsigned n = strlen(pName), e = strlen(pEnd);
    return n >= e && strcasecmp(pName + n - e, pEnd) == 0;
}

static boolean has_start(const char *pName, const char *pStart)
{
    return strncasecmp(pName, pStart, strlen(pStart)) == 0;
}

static boolean is_plain(const char *pName)
{
    if (pName[0] == '\0' || pName[0] == '.') {
        return FALSE;
    }
    for (const char *p = pName; *p != '\0'; p++) {
        if (*p <= ' ' || *p > '~' || strchr("\\:*?\"<>|/", *p) != nullptr) {
            return FALSE;
        }
    }
    return TRUE;
}

// Is a boot firmware file tried with the kernel (Pi 4 / 400)?
static boolean is_trial_firmware(const char *pName)
{
    return (has_start(pName, "start") && has_ending(pName, ".elf"))
        || (has_start(pName, "fixup") && has_ending(pName, ".dat"));
}

// A file of the .zip that the update may write (its name in the root, or firmware/x)
static boolean is_system_file(const char *pName)
{
    if (strncmp(pName, "firmware/", 9) == 0) {
        const char *p = pName + 9;
        return is_plain(p) && (has_start(p, "brcmfmac") || has_start(p, "LICENCE"));
    }
    if (!is_plain(pName)) {
        return FALSE;
    }
    return    strcmp(pName, KERNEL_NAME) == 0 || is_trial_firmware(pName)
           || strcasecmp(pName, "bootcode.bin") == 0 || has_ending(pName, ".dtb")
           || has_start(pName, "LICENCE") || has_start(pName, "COPYING");
}

// Where a file of the .zip goes, and where it waits until then
static void live_path(const char *pName, CString *pPath)
{
    pPath->Format(DRIVE "/%s", pName);
}

static void staged_path(const char *pName, CString *pPath)
{
    if (UpdateCanTryBoot() && (strcmp(pName, KERNEL_NAME) == 0 || is_trial_firmware(pName))) {
        pPath->Format(DRIVE "/" TRIAL_PREFIX "%s", pName);    // tried before being kept
    } else {
        pPath->Format(UPDATE_DIR "/%s", pName);
    }
}

// --- Receiving ---------------------------------------------------------------------------

struct TScan {
    CString *pReport;               // the files left as they are
    unsigned nKernels;              // this model's kernel
    unsigned nOthers;               // files left out
    boolean  bTooMany;
};

static boolean scan_handler(const TZipEntry *pEntry, void *pParam)
{
    TScan *pScan = (TScan *) pParam;
    if (pEntry->bFolder) {
        return TRUE;
    }
    if (!is_system_file(pEntry->pName) || strlen(pEntry->pName) >= NAME_SIZE) {
        pScan->nOthers++;
        CString Line;
        Line.Format("  %s: left as it is on the card\n", pEntry->pName);
        if (   strcasecmp(pEntry->pName, "config.txt") != 0 && strcasecmp(pEntry->pName, "cmdline.txt") != 0
            && !has_start(pEntry->pName, "kernel")) {
            Line.Format("  %s: not a file of the update, left out\n", pEntry->pName);
        }
        pScan->pReport->Append(Line);
        return TRUE;
    }
    if (s_nFiles >= MAX_FILES) {
        pScan->bTooMany = TRUE;
        return FALSE;
    }
    strcpy(s_Files[s_nFiles].Name, pEntry->pName);
    s_Files[s_nFiles].nSize = pEntry->nSize;
    s_nFiles++;
    if (strcmp(pEntry->pName, KERNEL_NAME) == 0) {
        pScan->nKernels++;
    }
    return TRUE;
}

// Does a file of the .zip differ from the one on the card? (pData: its contents)
static boolean differs(const char *pName, const u8 *pData, unsigned nLength)
{
    CString Live;
    live_path(pName, &Live);
    unsigned nOld;
    u8 *pOld = read_file(Live, nLength, &nOld);
    boolean bDiffers = pOld == nullptr || nOld != nLength || memcmp(pOld, pData, nLength) != 0;
    delete [] pOld;
    return bDiffers;
}

boolean UpdateReceive(const u8 *pZip, unsigned nLength, CString *pReport)
{
    UpdateDiscard();
    if (!write_file(UPDATE_ZIP, pZip, nLength)) {
        pReport->Append("Could not write the SD card\n");
        return FALSE;
    }

    // Its files
    CString Kept;
    TScan Scan = {&Kept, 0, 0, FALSE};
    s_nFiles = 0;
    TZipResult Result = ZipOpen(UPDATE_ZIP);
    if (Result == ZipOK) {
        Result = ZipForEach(scan_handler, &Scan);
    }
    if (Result != ZipOK || Scan.bTooMany) {
        pReport->Append(Result != ZipOK ? "Not a .zip file that can be read\n" : "Too many files in the .zip\n");
        UpdateDiscard();
        return FALSE;
    }
    if (Scan.nKernels == 0) {
        CString Line;
        Line.Format("No %s in it: not an xAce-baremetal release for the Raspberry Pi %u\n",
                    KERNEL_NAME, RASPPI);
        pReport->Append(Line);
        UpdateDiscard();
        return FALSE;
    }

    // The kernel: an xAce-baremetal one for this model?
    u8 *pKernel;
    unsigned nKernel;
    if (ZipExtract(KERNEL_NAME, &pKernel, &nKernel) != ZipOK) {
        pReport->Append("The kernel cannot be unpacked from the .zip\n");
        UpdateDiscard();
        return FALSE;
    }
    unsigned nModel = 0;
    boolean bMarker = find_marker(pKernel, nKernel, s_PendingVersion, &nModel, s_PendingBuild);
    delete [] pKernel;
    if (!bMarker || nModel != RASPPI) {
        CString Line;
        if (!bMarker) {
            Line.Format("%s is not an xAce-baremetal kernel of version 0.7 or later\n", KERNEL_NAME);
        } else {
            Line.Format("This release is for the Raspberry Pi %u, not this one (%u)\n", nModel, RASPPI);
        }
        pReport->Append(Line);
        UpdateDiscard();
        return FALSE;
    }

    // Keep only the files that differ from those on the card
    unsigned nKept = 0;
    CString Changes;
    for (unsigned i = 0; i < s_nFiles; i++) {
        u8 *pData;
        unsigned nData;
        if (ZipExtract(s_Files[i].Name, &pData, &nData) != ZipOK) {
            CString Line;
            Line.Format("%s cannot be unpacked from the .zip\n", s_Files[i].Name);
            pReport->Append(Line);
            UpdateDiscard();
            return FALSE;
        }
        boolean bDiffers = differs(s_Files[i].Name, pData, nData);
        delete [] pData;
        if (bDiffers) {
            s_Files[nKept++] = s_Files[i];
            CString Line;
            Line.Format("  %s (%u bytes)\n", s_Files[i].Name, s_Files[i].nSize);
            Changes.Append(Line);
        }
    }
    s_nFiles = nKept;

    CString Head;
    Head.Format("xAce-baremetal %s for the Raspberry Pi %u, built %s (this one: %s, built %s)\n",
                s_PendingVersion, RASPPI, s_PendingBuild, UpdateVersion(), UpdateBuild());
    pReport->Append(Head);
    if (s_nFiles == 0) {
        pReport->Append("Its files are the same as those on the card: nothing to update\n");
        UpdateDiscard();
        return FALSE;
    }

    // Room for the new files and the old ones kept
    DWORD nFree = 0;
    FATFS *pFS;
    unsigned nNeeded = 0;
    for (unsigned i = 0; i < s_nFiles; i++) {
        nNeeded += s_Files[i].nSize * 2 + 32768;
    }
    if (f_getfree(DRIVE, &nFree, &pFS) == FR_OK
        && (unsigned long long) nFree * pFS->csize * 512 < nNeeded) {
        pReport->Append("Not enough room on the SD card\n");
        UpdateDiscard();
        return FALSE;
    }

    CString Line;
    Line.Format("%u file%s to change:\n", s_nFiles, s_nFiles == 1 ? "" : "s");
    pReport->Append(Line);
    pReport->Append(Changes);
    if (Kept.GetLength() > 0) {
        pReport->Append("Not changed by the update:\n");
        pReport->Append(Kept);
    }
    pReport->Append("Press Y on the Ace (F11 screen) to install it.\n");

    s_bPending = TRUE;
    s_Last.Format("Version %s received, waiting for Y on the Ace", s_PendingVersion);
    CLogger::Get()->Write(FromUpdate, LogNotice, "Version %s (%s) received: %u files",
                          s_PendingVersion, s_PendingBuild, s_nFiles);
    return TRUE;
}

boolean UpdateIsPending(void) { return s_bPending; }
const char *UpdatePendingVersion(void) { return s_PendingVersion; }
const char *UpdatePendingBuild(void) { return s_PendingBuild; }
unsigned UpdatePendingFiles(void) { return s_nFiles; }

void UpdateDiscard(void)
{
    if (s_bPending) {
        s_Last = "The update was discarded";
    }
    s_bPending = FALSE;
    s_nFiles = 0;
    ZipForget();
    f_unlink(UPDATE_ZIP);
}

static void json_string(CString *pOut, const char *p)
{
    pOut->Append("\"");
    for (; *p != '\0'; p++) {
        char s[8];
        if (*p == '"' || *p == '\\') {
            s[0] = '\\'; s[1] = *p; s[2] = '\0';
        } else if (*p < ' ' || *p > '~') {
            strcpy(s, "?");
        } else {
            s[0] = *p; s[1] = '\0';
        }
        pOut->Append(s);
    }
    pOut->Append("\"");
}

void UpdateStatusJson(CString *pJson)
{
    *pJson = "{\"version\":";
    json_string(pJson, UpdateVersion());
    CString Part;
    Part.Format(",\"model\":%u,\"kernel\":\"%s\",\"build\":", RASPPI, KERNEL_NAME);
    pJson->Append(Part);
    json_string(pJson, UpdateBuild());
    pJson->Append(UpdateCanTryBoot() ? ",\"tryboot\":true" : ",\"tryboot\":false");
    pJson->Append(",\"pending\":");
    if (s_bPending) {
        pJson->Append("{\"version\":");
        json_string(pJson, s_PendingVersion);
        pJson->Append(",\"build\":");
        json_string(pJson, s_PendingBuild);
        Part.Format(",\"files\":%u}", s_nFiles);
        pJson->Append(Part);
    } else {
        pJson->Append("null");
    }
    pJson->Append(",\"last\":");
    json_string(pJson, s_Last);
    pJson->Append("}");
}

// --- Installing --------------------------------------------------------------------------

// Put the staged files in place, the replaced ones going to update-backup. The manifest
// lines after the first are "staged|live".
static boolean commit(const char *pManifest)
{
    delete_tree(BACKUP_DIR);
    f_mkdir(BACKUP_DIR);

    boolean bOK = TRUE;
    const char *p = strchr(pManifest, '\n');
    while (p != nullptr && *++p != '\0') {
        const char *pEnd = strchr(p, '\n');
        unsigned nLen = pEnd != nullptr ? (unsigned) (pEnd - p) : strlen(p);
        char Line[2 * TAPE_FULL_PATH_SIZE];
        if (nLen >= sizeof Line) {
            break;
        }
        memcpy(Line, p, nLen);
        Line[nLen] = '\0';
        char *pBar = strchr(Line, '|');
        if (pBar != nullptr) {
            *pBar = '\0';
            const char *pStaged = Line, *pLive = pBar + 1;
            if (exists(pLive)) {
                CString Backup;
                Backup.Format(BACKUP_DIR "%s", pLive + sizeof(DRIVE) - 1);   // SD:/x -> SD:/update-backup/x
                make_folders(Backup);
                f_chmod(pLive, 0, AM_RDO);
                if (f_rename(pLive, Backup) != FR_OK) {
                    f_unlink(pLive);
                }
            }
            make_folders(pLive);
            if (f_rename(pStaged, pLive) != FR_OK) {
                CLogger::Get()->Write(FromUpdate, LogError, "Cannot put %s in place", pLive);
                bOK = FALSE;
            }
        }
        p = pEnd;
    }

    f_unlink(TRYBOOT_TXT);
    f_unlink(DRIVE "/" TRIAL_CMDLINE);
    delete_tree(UPDATE_DIR);
    f_unlink(UPDATE_ZIP);
    return bOK;
}

// Remove what an update left that was not put in place
static void undo(const char *pManifest)
{
    const char *p = pManifest != nullptr ? strchr(pManifest, '\n') : nullptr;
    while (p != nullptr && *++p != '\0') {
        const char *pBar = strchr(p, '|');
        if (pBar == nullptr) {
            break;
        }
        char Staged[TAPE_FULL_PATH_SIZE];
        unsigned n = pBar - p < (int) sizeof Staged - 1 ? pBar - p : sizeof Staged - 1;
        memcpy(Staged, p, n);
        Staged[n] = '\0';
        if (strncmp(Staged, UPDATE_DIR "/", sizeof(UPDATE_DIR)) != 0) {
            f_unlink(Staged);               // SD:/update-<name>
        }
        p = strchr(pBar, '\n');
    }
    f_unlink(TRYBOOT_TXT);
    f_unlink(DRIVE "/" TRIAL_CMDLINE);
    delete_tree(UPDATE_DIR);
}

boolean UpdateInstall(CString *pMessage)
{
    if (!s_bPending) {
        *pMessage = "No update received";
        return FALSE;
    }

    // Start from a clean card: what an earlier attempt may have left
    undo(nullptr);
    f_mkdir(UPDATE_DIR);

    // Each file unpacked to where it waits, and read back
    if (ZipOpen(UPDATE_ZIP) != ZipOK) {
        *pMessage = "Cannot read the update";
        return FALSE;
    }
    CString Manifest;
    Manifest.Format("%s\n", s_PendingBuild);
    CString TrybootLines;
    for (unsigned i = 0; i < s_nFiles; i++) {
        const char *pName = s_Files[i].Name;
        CString Staged, Live;
        staged_path(pName, &Staged);
        live_path(pName, &Live);

        u8 *pData;
        unsigned nData;
        if (ZipExtract(pName, &pData, &nData) != ZipOK) {
            pMessage->Format("Cannot unpack %s", pName);
            undo(Manifest);
            return FALSE;
        }
        make_folders(Staged);
        unsigned nBack = 0;
        u8 *pBack = nullptr;
        boolean bOK = write_file(Staged, pData, nData)
                      && (pBack = read_file(Staged, nData, &nBack)) != nullptr
                      && nBack == nData && memcmp(pBack, pData, nData) == 0;
        delete [] pBack;
        delete [] pData;
        if (!bOK) {
            pMessage->Format("Cannot write %s", (const char *) Staged);
            undo(Manifest);
            return FALSE;
        }

        CString Line;
        Line.Format("%s|%s\n", (const char *) Staged, (const char *) Live);
        Manifest.Append(Line);

        if (UpdateCanTryBoot()) {
            const char *pTried = (const char *) Staged + sizeof(DRIVE);   // name in the root
            if (strcmp(pName, KERNEL_NAME) == 0) {
                Line.Format("kernel=%s\n", pTried);
            } else if (has_start(pName, "start")) {
                Line.Format("start_file=%s\n", pTried);
            } else if (has_start(pName, "fixup")) {
                Line.Format("fixup_file=%s\n", pTried);
            } else {
                continue;
            }
            TrybootLines.Append(Line);
        }
    }

    if (!UpdateCanTryBoot()) {
        // No trial: in place now
        if (!write_file(MANIFEST, (const char *) Manifest, Manifest.GetLength())) {
            *pMessage = "Cannot write the SD card";
            undo(Manifest);
            return FALSE;
        }
        boolean bOK = commit(Manifest);
        s_bPending = FALSE;
        s_Last.Format("Version %s installed: switch the Pi off and on", s_PendingVersion);
        CLogger::Get()->Write(FromUpdate, LogNotice, "Version %s installed", s_PendingVersion);
        if (!bOK) {
            *pMessage = "Some files could not be put in place";
            return FALSE;
        }
        return TRUE;
    }

    // tryboot.txt: config.txt as it is, then the new kernel and firmware (the last
    // setting wins; without a new kernel the running one is tried with the new
    // firmware), and cmdline.txt with xacetrial=1
    unsigned nCmdline = 0;
    u8 *pCmdline = read_file(CMDLINE_TXT, 4096, &nCmdline);
    CString Cmdline;
    if (pCmdline != nullptr) {
        while (nCmdline > 0 && (pCmdline[nCmdline - 1] == '\n' || pCmdline[nCmdline - 1] == '\r'
                                || pCmdline[nCmdline - 1] == ' ')) {
            nCmdline--;
        }
        pCmdline[nCmdline] = '\0';
        Cmdline = (const char *) pCmdline;
        Cmdline.Append(" ");
        delete [] pCmdline;
    }
    Cmdline.Append("xacetrial=1\n");
    TrybootLines.Append("cmdline=" TRIAL_CMDLINE "\n");

    unsigned nConfig = 0;
    u8 *pConfig = read_file(CONFIG_TXT, 64 * 1024, &nConfig);
    CString Tryboot;
    if (pConfig != nullptr) {
        pConfig[nConfig] = '\0';
        Tryboot = (const char *) pConfig;
        delete [] pConfig;
    }
    Tryboot.Append("\n# xAce-baremetal: trying a new version once (see update_sd.cpp)\n[all]\n");
    Tryboot.Append(TrybootLines);

    if (   !write_file(MANIFEST, (const char *) Manifest, Manifest.GetLength())
        || !write_file(DRIVE "/" TRIAL_CMDLINE, (const char *) Cmdline, Cmdline.GetLength())
        || !write_file(TRYBOOT_TXT, (const char *) Tryboot, Tryboot.GetLength())) {
        *pMessage = "Cannot write the SD card";
        undo(Manifest);
        return FALSE;
    }

    s_bPending = FALSE;
    s_Last.Format("Version %s is being tried", s_PendingVersion);
    CLogger::Get()->Write(FromUpdate, LogNotice, "Trying version %s", s_PendingVersion);
    return TRUE;
}

void UpdateRestart(void)
{
    struct {
        TPropertyTag Tag;
        u32 nFlags;
    } RebootFlags;
    RebootFlags.nFlags = 1;                     // tryboot
    CBcmPropertyTags Tags;
    if (!Tags.GetTag(PROPTAG_SET_REBOOT_FLAGS, &RebootFlags, sizeof RebootFlags, 4)) {
        CLogger::Get()->Write(FromUpdate, LogError, "Cannot set the tryboot flag");
    }
    TapeShutdown();
    reboot();
}

// --- At start-up ---------------------------------------------------------------------------

static char *read_manifest(void)
{
    unsigned nLength;
    u8 *pData = read_file(MANIFEST, 64 * 1024, &nLength);
    if (pData != nullptr) {
        pData[nLength] = '\0';
    }
    return (char *) pData;
}

void UpdateStartup(boolean bTrial)
{
    char *pManifest = read_manifest();
    if (pManifest == nullptr) {
        f_unlink(TRYBOOT_TXT);                  // left by an attempt that stopped early
        f_unlink(DRIVE "/" TRIAL_CMDLINE);
        return;
    }

    char Build[32];
    const char *pEnd = strchr(pManifest, '\n');
    unsigned n = pEnd != nullptr ? (unsigned) (pEnd - pManifest) : 0;
    n = n < sizeof Build - 1 ? n : sizeof Build - 1;
    memcpy(Build, pManifest, n);
    Build[n] = '\0';

    // Kept only when this is the trial start-up (tryboot.txt's command line) of the
    // kernel being tried; any other start-up means the trial did not get this far
    if (bTrial && strcmp(Build, UpdateBuild()) == 0) {
        s_bCommit = TRUE;                       // the tried kernel is running: keep it soon
        CLogger::Get()->Write(FromUpdate, LogNotice, "Version %s started: keeping it once it runs",
                              UpdateVersion());
    } else {
        undo(pManifest);
        f_unlink(UPDATE_ZIP);
        s_bMessage = TRUE;
        s_Line1 = "The new version did not start:";
        s_Line2.Format("still %s (as before)", UpdateVersion());
        s_Last = "The update did not start: the old version was kept";
        CLogger::Get()->Write(FromUpdate, LogWarning, "The tried update did not start, undone");
    }
    delete [] pManifest;
}

boolean UpdateFrame(CString *pLine1, CString *pLine2)
{
    // The tried update: kept once the Ace has run for a few seconds
    if (s_bCommit) {
        if (++s_nFrames < COMMIT_FRAMES) {
            return FALSE;
        }
        s_bCommit = FALSE;
        char *pManifest = read_manifest();
        boolean bOK = pManifest != nullptr && commit(pManifest);
        delete [] pManifest;
        s_bMessage = TRUE;
        s_Line1.Format("Updated to version %s", UpdateVersion());
        s_Line2 = bOK ? "(old files in update-backup)" : "Some files not in place: see log";
        s_Last.Format("Updated to version %s", UpdateVersion());
        CLogger::Get()->Write(FromUpdate, LogNotice, "Updated to version %s", UpdateVersion());
    }

    // A message: once the Ace's screen is up
    if (!s_bMessage) {
        return FALSE;
    }
    if (s_nFrames < COMMIT_FRAMES) {
        s_nFrames++;
        return FALSE;
    }
    s_bMessage = FALSE;
    *pLine1 = s_Line1;
    *pLine2 = s_Line2;
    return TRUE;
}
