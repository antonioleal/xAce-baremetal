/*
 * Saving the dictionary as Forth source (F7), the reverse of spooling.
 *
 * The dictionary is walked forwards from the first user word (3C51h) by the
 * length fields, so words hidden by a zeroed link are found too. Each word
 * header is: name (last character with bit 7 set), length field (2), link (2),
 * name length field (1), code field (2), parameter field.
 *
 * Colon (code field 0EC3h), DEFINER (1085h) and COMPILER (1108h) words are
 * listed by the ROM's LIST, typed by the spooler: before each one the FORTH
 * vocabulary is pointed at that word, so the Ace sees the dictionary as it was
 * when the word was defined (an older word of the same name is listed, not the
 * newest). LIST's output is caught through the EXWRCH print vector (3C29h),
 * pointed at an ED F9 trap; the trap and a patch that stops LIST pausing after
 * 16 lines go over ROM bytes (the character set, only read at boot, and the
 * pause test) and are undone when saving ends, as are the links repaired so
 * that the ROM can find LIST, BASE (decimal while saving) and the vocabulary.
 *
 * Variables (0FF0h) and constants (0FF5h) are written with their current value,
 * other words as CREATE with their bytes, then their code field if it is not
 * CREATE's. Links, name length flags and the newest word's length field that a
 * re-spool would not reproduce are set at the end, oldest word first.
 *
 * Words are written in dictionary order unless a word uses a newer one (after
 * REDEFINE): then each word goes after the words it uses, found by looking for
 * their code field addresses in its parameter field. Addresses in the fix-ups
 * are written relative to a word ("find WORD n +"), so they hold wherever the
 * words end up.
 */
#include "save_dictionary.h"
#include "spool_ace.h"
#include "keyboard_circle.h"

#include <circle/logger.h>
#include <circle/string.h>
#include <circle/util.h>
#include <fatfs/ff.h>

extern "C" {
#include "z80.h"
}

#define DRIVE           "SD:"
#define MAX_PATH        (sizeof(DRIVE) + 1 + FF_MAX_LFN + 1)

#define FIRST_WORD      0x3C51      // after the FORTH vocabulary word
#define FORTH_NLF       0x3C49      // FORTH's name length field: the link of the oldest word
#define VOC_HEAD        0x3C4C      // FORTH vocabulary: newest word's name length field
#define SYS_EXWRCH      0x3C29
#define SYS_CONTEXT     0x3C33
#define SYS_STKBOT      0x3C37
#define SYS_BASE        0x3C3F
#define SYS_STATIN      0x3C28      // bit 0: waiting for input; bits 1-3: caps, graphics, inverse
#define SYS_INSCRN      0x3C1E
#define SYS_ENDBUF      0x3C22

#define CF_COLON        0x0EC3
#define CF_DEFINER      0x1085
#define CF_COMPILER     0x1108
#define CF_VARIABLE     0x0FF0
#define CF_CONSTANT     0x0FF5
#define CF_CREATE       0x0FEC

#define TRAP_ADDR       0x1F14      // two zero bytes of the ROM character set
#define LIST_PAUSE      0x16CF      // JP P,16C7h: LIST's "screen full" test

#define MAX_WORDS       2048
#define MAX_DEPENDENCIES 8192       // in all; more are ignored
#define MAX_NAME        63
#define CAPTURE_SIZE    16384
#define BYTES_PER_LINE  8

struct TWord {
    char     Name[MAX_NAME + 1];
    u16      nStart;                // first byte of the name
    u16      nNlf;                  // name length field
    u16      nEnd;                  // first byte after the word
    u16      nFirstDependency;      // in s_Dependencies
    u16      nDependencies;
    boolean  bWritten;              // placed in s_Order
    u16      nLink;                 // as found
    u16      nLength;               // length field, as found
    u8       nNameLength;           // name length field, as found (with flags)
};

static const char FromSave[] = "save";

static TWord s_Words[MAX_WORDS];
static unsigned s_nWords;
static u16 s_Order[MAX_WORDS];      // the order the words are written in
static boolean s_bReordered;
static u16 s_Dependencies[MAX_DEPENDENCIES];   // indices of the words a word uses
static unsigned s_nNext;            // next word to save (in s_Order)
static boolean s_bListing;          // waiting for the spooler to type "list NAME"
static char s_Command[MAX_NAME + 8];
static unsigned s_nListed, s_nAsBytes;

static boolean s_bActive = FALSE;
static boolean s_bAbort = FALSE;
static boolean s_bWriteError = FALSE;
static FIL s_File;
static char s_Path[MAX_PATH];

static char s_Capture[CAPTURE_SIZE];
static unsigned s_nCapture;
static boolean s_bCapture = FALSE;

// Saved state, put back when saving ends
static u16 s_nOldHead, s_nOldContext, s_nOldExwrch;
static u8 s_nOldBase, s_nOldStatin;
static u8 s_OldTrap[2], s_nOldPause;

static boolean s_bResult = FALSE;
static char s_ResultLine1[40], s_ResultLine2[40];

static void copy_string(char *pTo, const char *pFrom, unsigned nSize)
{
    strncpy(pTo, pFrom, nSize - 1);
    pTo[nSize - 1] = '\0';
}

static u16 word(unsigned nAddr)
{
    return mem[nAddr] | (mem[nAddr + 1] << 8);
}

static void set_word(unsigned nAddr, u16 nValue)
{
    mem[nAddr] = nValue & 0xFF;
    mem[nAddr + 1] = nValue >> 8;
}

// The EXWRCH vector, from the ED F9 trap: a character LIST (or the ROM) prints
extern "C" void save_dictionary_out_p(unsigned char c)
{
    if (s_bCapture && s_nCapture < CAPTURE_SIZE) {
        s_Capture[s_nCapture++] = (char) c;
    }
}

static void write_text(const char *pText)
{
    UINT nWritten;
    unsigned nLength = strlen(pText);
    if (f_write(&s_File, pText, nLength, &nWritten) != FR_OK || nWritten != nLength) {
        s_bWriteError = TRUE;
    }
}

// Find the words: from the first user word to STKBOT, by the length fields
static unsigned find_words(void)
{
    unsigned nStkbot = word(SYS_STKBOT);
    unsigned nStart = FIRST_WORD;
    unsigned nCount = 0;

    while (nStart < nStkbot && nCount < MAX_WORDS) {
        unsigned nLast = nStart;                // last name character
        while (nLast < nStkbot && !(mem[nLast] & 0x80) && nLast - nStart < MAX_NAME) {
            nLast++;
        }
        unsigned nNameLength = nLast - nStart + 1;
        unsigned nNlf = nLast + 5;
        if (   nLast >= nStkbot || !(mem[nLast] & 0x80) || nNlf + 3 > nStkbot
            || (mem[nNlf] & 0x3F) != nNameLength) {
            break;                              // not a word header: stop here
        }

        TWord *pWord = &s_Words[nCount++];
        pWord->nStart = nStart;
        for (unsigned i = 0; i < nNameLength; i++) {
            pWord->Name[i] = (char) (mem[nStart + i] & 0x7F);
        }
        pWord->Name[nNameLength] = '\0';
        pWord->nNlf = nNlf;
        pWord->nLength = word(nLast + 1);
        pWord->nLink = word(nLast + 3);
        pWord->nNameLength = mem[nNlf];

        // The newest word's length field is only filled in when the next word is defined
        unsigned nEnd = nStart + pWord->nLength + nNameLength;
        pWord->nEnd = pWord->nLength != 0 && nEnd > nNlf + 2 && nEnd <= nStkbot ? nEnd : nStkbot;
        nStart = pWord->nEnd;
    }

    return nCount;
}

// The word whose code field is at nCfa, or the word that holds nAddr: -1 if none
static int word_by_cfa(unsigned nCfa)
{
    int nLow = 0, nHigh = (int) s_nWords - 1;
    while (nLow <= nHigh) {
        int nMid = (nLow + nHigh) / 2;
        unsigned nMidCfa = s_Words[nMid].nNlf + 1u;
        if (nMidCfa == nCfa) {
            return nMid;
        }
        if (nMidCfa < nCfa) {
            nLow = nMid + 1;
        } else {
            nHigh = nMid - 1;
        }
    }
    return -1;
}

static int word_holding(unsigned nAddr)
{
    for (unsigned i = 0; i < s_nWords; i++) {
        if (nAddr >= s_Words[i].nStart && nAddr < s_Words[i].nEnd) {
            return (int) i;
        }
    }
    return -1;
}

// The words each word uses (code field addresses in its parameter field, and the
// word holding its code field), then an order where every word comes after them
static void order_words(void)
{
    unsigned nUsed = 0;
    for (unsigned i = 0; i < s_nWords; i++) {
        TWord *pWord = &s_Words[i];
        pWord->nFirstDependency = nUsed;
        pWord->nDependencies = 0;
        pWord->bWritten = FALSE;

        unsigned nCf = word(pWord->nNlf + 1);
        int nOwner = nCf >= FIRST_WORD ? word_holding(nCf) : -1;
        if (nOwner >= 0 && nOwner != (int) i && nUsed < MAX_DEPENDENCIES) {
            s_Dependencies[nUsed++] = nOwner;
            pWord->nDependencies++;
        }
        for (unsigned a = pWord->nNlf + 3; a + 1 < pWord->nEnd && nUsed < MAX_DEPENDENCIES; a++) {
            int nUses = word_by_cfa(word(a));
            if (nUses >= 0 && nUses != (int) i) {
                s_Dependencies[nUsed++] = nUses;
                pWord->nDependencies++;
            }
        }
    }

    // Repeatedly the oldest word whose words are all placed; a cycle (or a false
    // match in data) is broken by placing the oldest word left
    s_bReordered = FALSE;
    for (unsigned n = 0; n < s_nWords; n++) {
        int nChosen = -1, nOldestLeft = -1;
        for (unsigned i = 0; i < s_nWords && nChosen < 0; i++) {
            const TWord *pWord = &s_Words[i];
            if (pWord->bWritten) {
                continue;
            }
            if (nOldestLeft < 0) {
                nOldestLeft = i;
            }
            boolean bReady = TRUE;
            for (unsigned d = 0; d < pWord->nDependencies && bReady; d++) {
                bReady = s_Words[s_Dependencies[pWord->nFirstDependency + d]].bWritten;
            }
            if (bReady) {
                nChosen = i;
            }
        }
        if (nChosen < 0) {
            nChosen = nOldestLeft;
        }
        s_Words[nChosen].bWritten = TRUE;
        s_Order[n] = nChosen;
        if ((unsigned) nChosen != n) {
            s_bReordered = TRUE;
        }
    }
}

// An address for the file: relative to the word holding it ("find WORD n +"), or as is
static void format_address(CString *pText, unsigned nAddr)
{
    int nOwner = nAddr >= FIRST_WORD ? word_holding(nAddr) : -1;
    if (nOwner < 0) {
        pText->Format("%d", (s16) nAddr);
        return;
    }
    int nOffset = (int) nAddr - (s_Words[nOwner].nNlf + 1);
    if (nOffset == 0) {
        pText->Format("find %s", s_Words[nOwner].Name);
    } else {
        pText->Format("find %s %d +", s_Words[nOwner].Name, nOffset);
    }
}

static boolean is_listable(const TWord *pWord)
{
    u16 nCf = word(pWord->nNlf + 1);
    return nCf == CF_COLON || nCf == CF_DEFINER || nCf == CF_COMPILER;
}

// CREATE NAME with the word's parameter bytes, then its code field if not CREATE's
static void write_as_bytes(const TWord *pWord)
{
    CString Line;
    Line.Format("create %s\n", pWord->Name);
    write_text(Line);

    unsigned nParam = pWord->nNlf + 3;
    unsigned nCount = 0;
    Line = "";
    for (unsigned a = nParam; a < pWord->nEnd; ) {
        CString Item;
        if (a + 1 < pWord->nEnd) {
            Item.Format("%d , ", (s16) word(a));
            a += 2;
        } else {
            Item.Format("%u c, ", mem[a]);
            a++;
        }
        Line.Append(Item);
        if (++nCount == BYTES_PER_LINE / 2) {
            Line.Append("\n");
            write_text(Line);
            Line = "";
            nCount = 0;
        }
    }
    if (nCount > 0) {
        Line.Append("\n");
        write_text(Line);
    }

    u16 nCf = word(pWord->nNlf + 1);
    if (nCf != CF_CREATE) {
        CString Address;
        format_address(&Address, nCf);
        Line.Format("%s find %s !\n", (const char *) Address, pWord->Name);
        write_text(Line);
    }
    s_nAsBytes++;
}

// LIST's output for one word: "list NAME \r" echoed, the listing, "  OK  \r"
static boolean write_listing(void)
{
    static const char Ok[] = "  OK  \r";
    unsigned nOk = sizeof(Ok) - 1;
    if (s_nCapture >= CAPTURE_SIZE || s_nCapture < nOk
        || memcmp(s_Capture + s_nCapture - nOk, Ok, nOk) != 0) {
        return FALSE;
    }

    unsigned i = 0;
    while (i < s_nCapture && s_Capture[i] != '\r') {
        i++;                                    // the echoed command
    }

    unsigned nEnd = s_nCapture - nOk;
    char Line[128];
    unsigned n = 0;
    for (i++; i <= nEnd; i++) {
        char c = i < nEnd ? s_Capture[i] : '\r';
        if (c != '\r') {
            if (n < sizeof(Line) - 2) {
                Line[n++] = c >= 0x20 && c < 0x7F ? c : ' ';
            }
            continue;
        }
        while (n > 0 && Line[n - 1] == ' ') {
            n--;
        }
        if (n > 0) {
            Line[n++] = '\n';
            Line[n] = '\0';
            write_text(Line);
        }
        n = 0;
    }

    s_nListed++;
    return TRUE;
}

// Links, flags and the newest length field that spooling the file would not reproduce
static void write_fixups(void)
{
    CString Line;
    const TWord *pNewest = &s_Words[s_nWords - 1];
    if (pNewest->nLength != 0 && !s_bReordered) {
        Line.Format("%d find %s 5 - !\n", (s16) pNewest->nLength, pNewest->Name);
        write_text(Line);
    }

    for (unsigned n = 0; n < s_nWords; n++) {
        unsigned i = s_Order[n];
        const TWord *pWord = &s_Words[i];
        u16 nExpected = i == 0 ? FORTH_NLF : s_Words[i - 1].nNlf;
        if (pWord->nLink != nExpected) {
            CString Link;
            int nTarget = word_by_cfa(pWord->nLink + 1u);
            if (pWord->nLink == 0 || pWord->nLink == FORTH_NLF || nTarget < 0) {
                Link.Format("%d", (s16) pWord->nLink);
            } else {
                Link.Format("find %s 1-", s_Words[nTarget].Name);
            }
            Line.Format("%s find %s 3 - !\n", (const char *) Link, pWord->Name);
            write_text(Line);
        }
        if (pWord->nNameLength & 0xC0) {
            Line.Format("%u find %s 1- c!\n", pWord->nNameLength, pWord->Name);
            write_text(Line);
        }
    }
}

static void finish(void)
{
    // Everything back as it was
    for (unsigned i = 0; i < s_nWords; i++) {
        set_word(s_Words[i].nNlf - 2, s_Words[i].nLink);
    }
    set_word(VOC_HEAD, s_nOldHead);
    set_word(SYS_CONTEXT, s_nOldContext);
    set_word(SYS_EXWRCH, s_nOldExwrch);
    mem[SYS_BASE] = s_nOldBase;
    mem[SYS_STATIN] = (mem[SYS_STATIN] & ~0x0E) | (s_nOldStatin & 0x0E);
    mem[TRAP_ADDR] = s_OldTrap[0];
    mem[TRAP_ADDR + 1] = s_OldTrap[1];
    mem[LIST_PAUSE] = s_nOldPause;
    s_bCapture = FALSE;
    keyboard_set_spooling(0);

    if (!s_bAbort && !s_bWriteError) {
        write_fixups();
    }
    if (f_close(&s_File) != FR_OK) {
        s_bWriteError = TRUE;
    }
    s_bActive = FALSE;

    const char *pName = s_Path + sizeof(DRIVE);
    CString Line;
    if (s_bAbort || s_bWriteError) {
        f_unlink(s_Path);
        copy_string(s_ResultLine1, s_bAbort ? "Stopped with Esc:" : "SD card write error:",
                    sizeof(s_ResultLine1));
        Line.Format("%s not saved", pName);
        copy_string(s_ResultLine2, Line, sizeof(s_ResultLine2));
        CLogger::Get()->Write(FromSave, LogWarning, "%s %s", s_ResultLine1, s_ResultLine2);
    } else {
        Line.Format("Saved %u words to", s_nWords);
        copy_string(s_ResultLine1, Line, sizeof(s_ResultLine1));
        copy_string(s_ResultLine2, pName, sizeof(s_ResultLine2));
        CLogger::Get()->Write(FromSave, LogNotice, "%s: %u words (%u listed, %u as bytes%s)",
                              s_Path, s_nWords, s_nListed, s_nAsBytes,
                              s_bReordered ? ", reordered" : "");
    }
    s_bResult = TRUE;
}

TSaveResult SaveDictionaryStart(const char *pInput, char *pName, unsigned nNameSize,
                                unsigned *pWords)
{
    pName[0] = '\0';
    *pWords = 0;
    if (s_bActive || SpoolActive()) {
        return SaveNotAtPrompt;
    }

    // At the prompt, with nothing typed on the input line
    if (!(mem[SYS_STATIN] & 0x01) || word(SYS_ENDBUF) != word(SYS_INSCRN) + 2) {
        return SaveNotAtPrompt;
    }

    if (pInput[0] == '\0' || pInput[0] == ' ' || pInput[0] == '.') {
        return SaveInvalidName;
    }
    for (const char *p = pInput; *p != '\0'; p++) {
        if (*p < 0x20 || strchr("\\/:*?\"<>|", *p) != nullptr) {
            return SaveInvalidName;
        }
    }

    // A Forth source extension (.fth, .fs, .f), or none: then .fth is added
    const char *pExtension = nullptr;
    for (const char *p = pInput + 1; *p != '\0'; p++) {
        if (*p == '.') {
            pExtension = p;
        }
    }
    if (   pExtension != nullptr && strcasecmp(pExtension, ".fth") != 0
        && strcasecmp(pExtension, ".fs") != 0 && strcasecmp(pExtension, ".f") != 0) {
        return SaveInvalidName;
    }

    s_nWords = find_words();
    if (s_nWords == 0) {
        return SaveNoWords;
    }
    order_words();
    *pWords = s_nWords;

    strcpy(s_Path, DRIVE "/");
    strncat(s_Path, pInput, sizeof(s_Path) - strlen(s_Path) - 5);
    if (pExtension == nullptr) {
        strcat(s_Path, ".fth");
    }
    copy_string(pName, s_Path + sizeof(DRIVE), nNameSize);

    FRESULT Result = f_open(&s_File, s_Path, FA_WRITE | FA_CREATE_NEW);
    if (Result == FR_EXIST) {
        return SaveExists;
    }
    if (Result == FR_NOT_READY || Result == FR_INVALID_DRIVE || Result == FR_NOT_ENABLED) {
        return SaveNoCard;
    }
    if (Result != FR_OK) {
        return SaveError;
    }

    // Save the state this changes
    s_nOldHead = word(VOC_HEAD);
    s_nOldContext = word(SYS_CONTEXT);
    s_nOldExwrch = word(SYS_EXWRCH);
    s_nOldBase = mem[SYS_BASE];
    s_nOldStatin = mem[SYS_STATIN];
    s_OldTrap[0] = mem[TRAP_ADDR];
    s_OldTrap[1] = mem[TRAP_ADDR + 1];
    s_nOldPause = mem[LIST_PAUSE];

    // Links as the Ace makes them, so that every word and LIST can be found
    for (unsigned i = 0; i < s_nWords; i++) {
        set_word(s_Words[i].nNlf - 2, i == 0 ? FORTH_NLF : s_Words[i - 1].nNlf);
    }
    set_word(SYS_CONTEXT, VOC_HEAD);
    mem[SYS_BASE] = 10;
    mem[SYS_STATIN] &= ~0x0E;                   // no caps lock, graphics or inverse
    mem[TRAP_ADDR] = 0xED;                      // EXWRCH -> ED F9, see edops.c
    mem[TRAP_ADDR + 1] = 0xF9;
    set_word(SYS_EXWRCH, TRAP_ADDR);
    mem[LIST_PAUSE] = 0xC3;                     // JP 16C7h: never pause

    s_nNext = 0;
    s_bListing = FALSE;
    s_nListed = s_nAsBytes = 0;
    s_bAbort = FALSE;
    s_bWriteError = FALSE;
    s_bResult = FALSE;
    keyboard_set_spooling(1);
    s_bActive = TRUE;

    write_text("decimal\n");
    CLogger::Get()->Write(FromSave, LogNotice, "Saving %u words to %s", s_nWords, s_Path);

    return SaveOK;
}

boolean SaveDictionaryActive(void)
{
    return s_bActive;
}

void SaveDictionaryFrame(void)
{
    if (!s_bActive) {
        return;
    }

    if (keyboard_check_spool_abort()) {
        s_bAbort = TRUE;
    }
    if (SpoolActive()) {
        return;                                 // still typing "list NAME"
    }

    if (s_bListing) {
        s_bListing = FALSE;
        s_bCapture = FALSE;
        if (!write_listing()) {
            write_as_bytes(&s_Words[s_Order[s_nNext]]);     // LIST failed: keep the word anyway
        }
        s_nNext++;
    }

    // Write words directly until one needs LIST
    while (!s_bAbort && !s_bWriteError && s_nNext < s_nWords) {
        TWord *pWord = &s_Words[s_Order[s_nNext]];
        u16 nCf = word(pWord->nNlf + 1);
        unsigned nParam = pWord->nNlf + 3;
        boolean bOneCell = pWord->nEnd == nParam + 2;
        CString Line;

        if (is_listable(pWord)) {
            set_word(VOC_HEAD, pWord->nNlf);    // the dictionary as when it was defined
            strcpy(s_Command, "list ");
            strcat(s_Command, pWord->Name);
            strcat(s_Command, "\n");
            s_nCapture = 0;
            s_bCapture = TRUE;
            SpoolStartText(s_Command);
            s_bListing = TRUE;
            return;
        }

        if (nCf == CF_VARIABLE && bOneCell) {
            Line.Format("%d variable %s\n", (s16) word(nParam), pWord->Name);
            write_text(Line);
        } else if (nCf == CF_CONSTANT && bOneCell) {
            Line.Format("%d constant %s\n", (s16) word(nParam), pWord->Name);
            write_text(Line);
        } else {
            write_as_bytes(pWord);
        }
        s_nNext++;
    }

    finish();
}

boolean SaveDictionaryResult(char *pLine1, char *pLine2, unsigned nSize)
{
    if (!s_bResult) {
        return FALSE;
    }
    s_bResult = FALSE;

    copy_string(pLine1, s_ResultLine1, nSize);
    copy_string(pLine2, s_ResultLine2, nSize);

    return TRUE;
}
