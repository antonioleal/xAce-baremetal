/*
 * Spooling Forth source files (.fth, .fs, .f) into the Ace, as the original xAce did (F5).
 *
 * Characters are typed on the emulated keyboard, so they go through the ROM's
 * own editor exactly as typed keys do. The ROM reads keys in its interrupt
 * routine and only takes them while it is waiting for input (STATIN bit 0); it
 * needs a key to be held for 3 frames and released for 1 before the next one.
 * While spooling the kernel runs the Ace without frame pacing, so this is fast.
 *
 * After Enter, once the ROM waits for input again, an empty input line
 * (ENDBUF = INSCRN + 2) means the line was accepted; otherwise the ROM put it
 * back with "?" and spooling stops there, as typing on would add to that line.
 * ERROR n (caught by the ED FB trap on the error restart) only loses the rest of
 * its line, so spooling goes on, as in the original xAce.
 *
 * A file is read a line at a time before it is typed, so that ( comments ) can be
 * left out where the Ace does not take them: outside definitions (where "(" gives
 * ERROR 4). Whether a line starts inside a definition is the ROM's compile mode
 * (FLAGS bit 6) when the line is about to be typed; within the line, ":",
 * DEFINER and COMPILER start a definition and ";" ends it. Strings (." ... ")
 * and the character after ASCII are passed as they are. Comments inside
 * definitions are kept: the Ace keeps them in the word for LIST and EDIT.
 * Lines left empty are not typed.
 */
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

#define STATIN          0x3C28      // bit 0: waiting for input; bits 1-3: caps, graphics, inverse
#define STATIN_INPUT    0x01
#define STATIN_MODES    0x0E
#define INSCRN          0x3C1E      // start of the input line
#define ENDBUF          0x3C22      // end of the input line
#define FLAGS           0x3C3E      // bit 6: compiling a definition
#define FLAGS_COMPILING 0x40

#define LINE_SIZE       1024        // a longer line is typed in parts, without comment removal

#define KEY_FRAMES      3           // frames a key is held (the ROM's debounce)

static const char FromSpool[] = "spool";

static FIL s_File;
static boolean s_bActive = FALSE;
static char s_Name[MAX_PATH];
static const char *s_pText = nullptr;   // typing text from memory (SpoolStartText()), not a file
static boolean s_bFinalEnter = TRUE;    // Enter after a last line that has none

static unsigned char s_Buffer[512];
static UINT s_nCount = 0;
static UINT s_nPos = 0;
static int s_nUnread = -1;          // a byte read ahead (after CR)

static unsigned s_nKeyFrames = 0;   // frames left with the key down
static boolean s_bLineText = FALSE; // the current line has text
static boolean s_bAfterEnter = FALSE;
static unsigned s_nLine = 1;
static unsigned s_nSkipped = 0;     // characters the Ace does not have
static unsigned s_nComments = 0;    // comments left out

static char s_Line[LINE_SIZE + 1];  // the line being typed (files only), with its '\n'
static unsigned s_nLineLength = 0;
static unsigned s_nLinePos = 0;
static boolean s_bLineContinues = FALSE;    // the line did not fit: its rest comes next
static unsigned s_nErrors = 0;      // ERROR n while spooling

static volatile boolean s_bRomError = FALSE;
static volatile unsigned char s_nRomError = 0;

void SpoolPatchROM(unsigned char *pROM)
{
    pROM[0x0020] = 0xED;            // error restart: ED FB, see edops.c
    pROM[0x0021] = 0xFB;
}

// The ROM's error restart, from the ED FB trap (FFh is no error: FORGET and others
// restart through it without a message)
extern "C" void spool_error_p(unsigned char nError)
{
    if (s_bActive && nError != 0xFF) {
        s_nRomError = nError;
        s_bRomError = TRUE;
    }
}

TSpoolResult SpoolStart(const char *pInput)
{
    if (s_bActive) {
        f_close(&s_File);
        s_bActive = FALSE;
    }

    if (pInput[0] == '\0' || pInput[0] == ' ' || pInput[0] == '.') {
        return SpoolInvalidName;
    }
    for (const char *p = pInput; *p != '\0'; p++) {
        if (*p < 0x20 || strchr("\\/:*?\"<>|", *p) != nullptr) {
            return SpoolInvalidName;
        }
    }

    strcpy(s_Name, DRIVE "/");
    strncat(s_Name, pInput, sizeof(s_Name) - strlen(s_Name) - 5);

    FRESULT Result = f_open(&s_File, s_Name, FA_READ | FA_OPEN_EXISTING);
    if (Result == FR_NOT_READY || Result == FR_INVALID_DRIVE || Result == FR_NOT_ENABLED) {
        return SpoolNoCard;
    }
    if (Result != FR_OK) {
        return SpoolNotFound;
    }

    s_pText = nullptr;
    s_bFinalEnter = TRUE;
    s_nCount = s_nPos = 0;
    s_nUnread = -1;
    s_nKeyFrames = 0;
    s_bLineText = FALSE;
    s_bAfterEnter = FALSE;
    s_nLine = 1;
    s_nSkipped = 0;
    s_nComments = 0;
    s_nLineLength = s_nLinePos = 0;
    s_bLineContinues = FALSE;
    s_nErrors = 0;
    s_bRomError = FALSE;

    // Type plain text: no caps lock, graphics or inverse mode
    unsigned short nStatin = STATIN;
    store(nStatin, mem[STATIN] & ~STATIN_MODES);

    keyboard_set_spooling(1);
    s_bActive = TRUE;
    CLogger::Get()->Write(FromSpool, LogNotice, "Spooling %s", s_Name);

    return SpoolOK;
}

boolean SpoolStartText(const char *pText, boolean bFinalEnter)
{
    if (s_bActive) {
        return FALSE;
    }

    s_pText = pText;
    s_bFinalEnter = bFinalEnter;
    s_nCount = s_nPos = 0;
    s_nUnread = -1;
    s_nKeyFrames = 0;
    s_bLineText = FALSE;
    s_bAfterEnter = FALSE;
    s_nLine = 1;
    s_nSkipped = 0;
    s_nComments = 0;
    s_nLineLength = s_nLinePos = 0;
    s_bLineContinues = FALSE;
    s_nErrors = 0;
    s_bRomError = FALSE;
    s_bActive = TRUE;

    return TRUE;
}

boolean SpoolActive(void)
{
    return s_bActive;
}

// pWhy: at line s_nLine; nullptr: the whole file was typed
static void stop(const char *pWhy)
{
    s_bActive = FALSE;
    if (s_pText != nullptr) {
        s_pText = nullptr;                      // the caller handles the rest
        return;
    }

    f_close(&s_File);
    keyboard_set_spooling(0);

    CString Notes;
    if (s_nErrors > 0) {
        Notes.Format(", %u ERRORs", s_nErrors);
    }
    if (s_nComments > 0) {
        CString Comments;
        Comments.Format("%s, %u comments left out", (const char *) Notes, s_nComments);
        Notes = Comments;
    }
    if (s_nSkipped > 0) {
        CString Skipped;
        Skipped.Format("%s, %u characters skipped", (const char *) Notes, s_nSkipped);
        Notes = Skipped;
    }
    if (pWhy == nullptr) {
        CLogger::Get()->Write(FromSpool, LogNotice, "%s: done, %u lines%s",
                              s_Name, s_nLine - 1, (const char *) Notes);
    } else {
        CLogger::Get()->Write(FromSpool, LogWarning, "%s: line %u: %s%s",
                              s_Name, s_nLine, pWhy, (const char *) Notes);
    }
}

static int read_byte(void)
{
    if (s_nUnread >= 0) {
        int c = s_nUnread;
        s_nUnread = -1;
        return c;
    }
    if (s_pText != nullptr) {
        return *s_pText != '\0' ? (unsigned char) *s_pText++ : -1;
    }
    if (s_nPos >= s_nCount) {
        if (f_read(&s_File, s_Buffer, sizeof(s_Buffer), &s_nCount) != FR_OK || s_nCount == 0) {
            return -1;
        }
        s_nPos = 0;
    }
    return s_Buffer[s_nPos++];
}

// The next character: '\n' ends a line (LF, CR or CR LF), a tab is a space, the
// pound and copyright signs (UTF-8 or Latin-1) are A3h / A9h; text from memory
// can also hold the SPOOL_KEY_ codes. Returns -1 at the end, for text from memory
// after a final '\n' if the last line has text (unless told not to).
static int decode_char(void)
{
    for (;;) {
        int c = read_byte();
        if (c < 0) {
            if (s_pText != nullptr && s_bLineText && s_bFinalEnter) {
                return '\n';
            }
            return -1;
        }

        if (s_pText != nullptr && (c == SPOOL_KEY_DELETE_LINE || c == SPOOL_KEY_INVERSE_VIDEO)) {
            return c;
        }

        if (c == '\r') {
            int n = read_byte();
            if (n >= 0 && n != '\n') {
                s_nUnread = n;
            }
            return '\n';
        }
        if (c == '\n') {
            return c;
        }
        if (c == '\t') {
            return ' ';
        }
        if (c >= 0xC2 && c <= 0xF4) {           // a UTF-8 character: keep the pound / copyright signs
            unsigned nMore = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            int nFirst = -1;
            for (unsigned i = 0; i < nMore; i++) {
                int n = read_byte();
                if (n < 0 || (n & 0xC0) != 0x80) {
                    s_nUnread = n;
                    break;
                }
                if (i == 0) {
                    nFirst = n;
                }
            }
            if (c == 0xC2 && (nFirst == 0xA3 || nFirst == 0xA9)) {
                return nFirst;
            }
            s_nSkipped++;
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            return c;
        }
        if (c == 0xA3 || c == 0xA9) {           // Latin-1
            return c;
        }
        s_nSkipped++;
    }
}

static unsigned short word(unsigned nAddr)
{
    return mem[nAddr] | (mem[nAddr + 1] << 8);
}

static boolean is_word(const char *pText, unsigned nLength, const char *pWord)
{
    return strlen(pWord) == nLength && strncasecmp(pText, pWord, nLength) == 0;
}

// Leave out the ( comments ) of the line that are outside definitions
static void remove_comments(boolean bInside)
{
    unsigned nOut = 0;
    unsigned i = 0;
    while (i < s_nLineLength) {
        if (s_Line[i] == ' ') {
            s_Line[nOut++] = s_Line[i++];
            continue;
        }

        unsigned nStart = i;
        while (i < s_nLineLength && s_Line[i] != ' ') {
            i++;
        }
        const char *pWord = s_Line + nStart;
        unsigned nWord = i - nStart;

        // Text after these words is not Forth: up to ')' or '"', or the next character
        unsigned nEnd = nStart + nWord;
        if (is_word(pWord, nWord, "(") || is_word(pWord, nWord, ".\"")) {
            char cClose = pWord[0] == '(' ? ')' : '"';
            while (nEnd < s_nLineLength && s_Line[nEnd] != cClose) {
                nEnd++;
            }
            if (nEnd < s_nLineLength) {
                nEnd++;
            }
        } else if (is_word(pWord, nWord, "ASCII") && nEnd + 1 < s_nLineLength) {
            nEnd += 2;
        } else if (is_word(pWord, nWord, ":") || is_word(pWord, nWord, "DEFINER")
                   || is_word(pWord, nWord, "COMPILER")) {
            bInside = TRUE;
        } else if (is_word(pWord, nWord, ";")) {
            bInside = FALSE;
        }

        if (pWord[0] == '(' && nWord == 1 && !bInside) {
            s_nComments++;                      // left out
        } else {
            memmove(s_Line + nOut, s_Line + nStart, nEnd - nStart);
            nOut += nEnd - nStart;
        }
        i = nEnd;
    }
    s_nLineLength = nOut;
}

// Read the next line of the file to type (with its '\n'); lines left empty are
// skipped. FALSE at the end of the file.
static boolean read_line(void)
{
    for (;;) {
        boolean bContinued = s_bLineContinues;
        s_nLineLength = s_nLinePos = 0;
        s_bLineContinues = FALSE;

        int c;
        while ((c = decode_char()) >= 0 && c != '\n') {
            s_Line[s_nLineLength++] = (char) c;
            if (s_nLineLength == LINE_SIZE) {
                s_bLineContinues = TRUE;        // the rest of it in the next part
                return TRUE;
            }
        }
        if (c < 0 && s_nLineLength == 0 && !bContinued) {
            return FALSE;
        }

        if (!bContinued) {
            remove_comments((mem[FLAGS] & FLAGS_COMPILING) != 0);
            boolean bEmpty = TRUE;
            for (unsigned i = 0; i < s_nLineLength && bEmpty; i++) {
                bEmpty = s_Line[i] == ' ';
            }
            if (bEmpty) {
                if (c < 0) {
                    return FALSE;
                }
                s_nLine++;                      // not typed: on to the next line
                continue;
            }
        }

        s_Line[s_nLineLength++] = '\n';
        return TRUE;
    }
}

// The next character to type: from the text in memory, or from the file a line at a time
static int next_char(void)
{
    if (s_pText != nullptr) {
        return decode_char();
    }
    if (s_nLinePos == s_nLineLength && !read_line()) {
        return -1;
    }
    return (unsigned char) s_Line[s_nLinePos++];
}

void SpoolFrame(void)
{
    if (!s_bActive) {
        return;
    }

    if (s_pText == nullptr && keyboard_check_spool_abort()) {     // text: the caller watches Esc
        stop("stopped with Esc");
        return;
    }
    if (s_bRomError) {
        s_bRomError = FALSE;
        s_nErrors++;
        CLogger::Get()->Write(FromSpool, LogWarning, "%s: line %u: ERROR %u", s_Name, s_nLine, s_nRomError);
    }

    // Hold the key, release it for a frame, then the next one
    if (s_nKeyFrames > 0) {
        if (--s_nKeyFrames == 0) {
            keyboard_clear();
        }
        return;
    }

    if (!(mem[STATIN] & STATIN_INPUT)) {
        return;                                 // the ROM is busy with the last line
    }

    if (s_bAfterEnter) {
        s_bAfterEnter = FALSE;
        if (word(ENDBUF) != word(INSCRN) + 2) {
            stop("not accepted by the Ace");
            return;
        }
        s_nLine++;
    }

    for (;;) {
        int c = next_char();
        if (c < 0) {
            stop(nullptr);
            return;
        }
        if (keyboard_spool_key((unsigned char) c)) {
            s_bLineText = c != '\n';
            s_bAfterEnter = c == '\n';
            s_nKeyFrames = KEY_FRAMES;
            return;
        }
        s_nSkipped++;
    }
}
