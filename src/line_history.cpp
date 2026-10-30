/*
 * Line history (TAB / Shift+TAB, and the Ctrl+TAB list in kernel.cpp).
 *
 * While the ROM waits for input (STATIN bit 0), the line being typed is in the
 * lower screen: a 00h marker at INSCRN, the characters, the cursor character
 * at CURSOR, up to ENDBUF. When Enter is pressed on the host keyboard, the line
 * is remembered: from the screen if the ROM has not taken it yet, otherwise as
 * it was at the end of the previous frame (a key pressed between the end of a
 * frame and the interrupt that reads it). Lines typed by the spooler are not
 * remembered, an empty line or a repeat of the last one neither.
 *
 * A remembered line is put back through the ROM's own editor, like spooling:
 * Delete Line, then its characters, with Inverse Video (Shift+4, which toggles)
 * before and after inverse ones, without Enter. Caps lock, graphics and inverse mode are switched off
 * while it is typed and put back afterwards. Graphics characters (codes below
 * 20h) are left out.
 */
#include "line_history.h"
#include "spool_ace.h"
#include "save_dictionary.h"
#include "keyboard_circle.h"

#include <circle/types.h>
#include <circle/util.h>

extern "C" {
#include "z80.h"
}

#define SYS_INSCRN      0x3C1E
#define SYS_CURSOR      0x3C20
#define SYS_ENDBUF      0x3C22
#define SYS_STATIN      0x3C28      // bit 0: waiting for input; bits 1-3: caps, graphics, inverse
#define STATIN_INPUT    0x01
#define STATIN_MODES    0x0E

#define MAX_LINE        255         // longer lines are not remembered

struct TLine {
    u8       nLength;
    u8       Text[MAX_LINE];        // Ace character codes, bit 7 = inverse
};

static TLine s_Lines[HISTORY_LINES];    // s_Lines[0] is the newest
static unsigned s_nLines = 0;

static TLine s_Previous;            // the input line at the end of the previous frame

static int s_nBrowsing = -1;        // the line put back by TAB, -1 if none
static boolean s_bTyping = FALSE;   // the spooler is typing a line for us
static u8 s_nOldModes;
static char s_Typed[1 + 2 * MAX_LINE + 2];

static u16 word(unsigned nAddr)
{
    return mem[nAddr] | (mem[nAddr + 1] << 8);
}

// The line being typed, without the marker and the cursor
static void read_input_line(TLine *pLine)
{
    unsigned nCursor = word(SYS_CURSOR);
    unsigned nEnd = word(SYS_ENDBUF);
    pLine->nLength = 0;
    for (unsigned a = word(SYS_INSCRN) + 1; a < nEnd && a < 0x2800; a++) {
        if (a == nCursor) {
            continue;
        }
        if (pLine->nLength == MAX_LINE) {
            pLine->nLength = 0;             // too long to remember
            return;
        }
        pLine->Text[pLine->nLength++] = mem[a];
    }
}

static void remember(const TLine *pLine)
{
    if (pLine->nLength == 0) {
        return;
    }
    if (   s_nLines > 0 && s_Lines[0].nLength == pLine->nLength
        && memcmp(s_Lines[0].Text, pLine->Text, pLine->nLength) == 0) {
        return;                             // the same as the last one
    }

    if (s_nLines < HISTORY_LINES) {
        s_nLines++;
    }
    for (unsigned i = s_nLines - 1; i > 0; i--) {
        s_Lines[i] = s_Lines[i - 1];
    }
    s_Lines[0] = *pLine;
}

// Put line nIndex (or an empty line, if -1) in place of the line being typed
static void put_back(int nIndex)
{
    unsigned n = 0;
    s_Typed[n++] = SPOOL_KEY_DELETE_LINE;

    if (nIndex >= 0) {
        const TLine *pLine = &s_Lines[nIndex];
        boolean bInverse = FALSE;
        for (unsigned i = 0; i < pLine->nLength; i++) {
            u8 nCode = pLine->Text[i] & 0x7F;
            if (nCode < 0x20) {
                continue;                   // graphics: not retyped
            }
            boolean bCharInverse = (pLine->Text[i] & 0x80) != 0;
            if (bCharInverse != bInverse) {
                s_Typed[n++] = SPOOL_KEY_INVERSE_VIDEO;
                bInverse = bCharInverse;
            }
            s_Typed[n++] = nCode == 0x60 ? (char) 0xA3      // pound
                         : nCode == 0x7F ? (char) 0xA9      // copyright
                         : (char) nCode;
        }
        if (bInverse) {
            s_Typed[n++] = SPOOL_KEY_INVERSE_VIDEO;
        }
    }
    s_Typed[n] = '\0';

    s_nOldModes = mem[SYS_STATIN] & STATIN_MODES;
    mem[SYS_STATIN] &= ~STATIN_MODES;
    keyboard_set_spooling(1);
    SpoolStartText(s_Typed, FALSE);
    s_bTyping = TRUE;
}

unsigned LineHistoryCount(void)
{
    return s_nLines;
}

const unsigned char *LineHistoryLine(unsigned nLine, unsigned *pLength)
{
    *pLength = nLine < s_nLines ? s_Lines[nLine].nLength : 0;
    return nLine < s_nLines ? s_Lines[nLine].Text : nullptr;
}

boolean LineHistoryRecall(unsigned nLine)
{
    if (   nLine >= s_nLines || s_bTyping || SpoolActive() || SaveDictionaryActive()
        || !(mem[SYS_STATIN] & STATIN_INPUT)) {
        return FALSE;
    }
    s_nBrowsing = (int) nLine;
    put_back(s_nBrowsing);
    return TRUE;
}

void LineHistoryFrame(void)
{
    int nRequest = keyboard_check_history();
    boolean bEnter = keyboard_check_enter();
    boolean bOtherKey = keyboard_check_other_key();

    if (s_bTyping) {
        if (SpoolActive()) {
            return;                         // TAB while a line is typed: ignored
        }
        mem[SYS_STATIN] = (mem[SYS_STATIN] & ~STATIN_MODES) | s_nOldModes;
        keyboard_set_spooling(0);
        s_bTyping = FALSE;
    }
    if (SpoolActive() || SaveDictionaryActive()) {
        return;
    }

    boolean bInput = (mem[SYS_STATIN] & STATIN_INPUT) != 0;

    if (bEnter) {
        TLine Line;
        if (bInput) {
            read_input_line(&Line);
        } else {
            Line.nLength = 0;
        }
        remember(Line.nLength > 0 ? &Line : &s_Previous);
        s_Previous.nLength = 0;
    }
    if (bOtherKey) {
        s_nBrowsing = -1;                   // typing or editing: TAB starts again
    }

    if (bInput) {
        read_input_line(&s_Previous);
    } else {
        s_Previous.nLength = 0;
    }

    if (nRequest == 0 || !bInput || s_nLines == 0) {
        return;
    }
    if (nRequest > 0) {
        if (s_nBrowsing + 1 >= (int) s_nLines) {
            return;                         // at the oldest line: stay there
        }
        s_nBrowsing++;
    } else {
        if (s_nBrowsing < 0) {
            return;                         // not browsing: nothing newer
        }
        s_nBrowsing--;                      // past the newest: an empty line
    }
    put_back(s_nBrowsing);
}
