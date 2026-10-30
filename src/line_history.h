#ifndef _LINE_HISTORY_H
#define _LINE_HISTORY_H

/*
 * Line history (TAB): every line entered at the Ace's input prompt is
 * remembered (up to HISTORY_LINES); TAB retypes the last one in its place,
 * TAB again the one before, down to the oldest; Shift+TAB goes back to newer
 * lines. The line is retyped, not entered: it can be edited before Enter.
 */

#include <circle/types.h>

#define HISTORY_LINES   100

// Once per Ace frame, after the keyboard has been updated
void LineHistoryFrame(void);

// The remembered lines, for the Ctrl+TAB list: line 0 is the newest. The text is in
// Ace character codes (bit 7 = inverse).
unsigned LineHistoryCount(void);
const unsigned char *LineHistoryLine(unsigned nLine, unsigned *pLength);

// Put line nLine back on the input line, as TAB does (TAB then goes on from it).
// FALSE if the Ace is not waiting for input.
boolean LineHistoryRecall(unsigned nLine);

#endif
