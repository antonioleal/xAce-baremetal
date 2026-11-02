#ifndef _RAM_ACE_H
#define _RAM_ACE_H

#include <circle/types.h>

/*
 * RAM pack (Ctrl+F12). The Ace has 3K of its own (1K video, 1K character set,
 * 1K for programs); a 16K, 32K or 48K pack adds RAM from 4000h, for 19K, 35K
 * or 51K in total. Missing RAM ignores writes and reads FFh, so the ROM's RAM
 * test at power-on stops at the end of the pack.
 *
 * The 96K pack (Edwin Blink's design, jupiter-ace.co.uk) has two 48K pages at
 * 4000h - FFFFh, 99K in all. Bit 7 of the Z80's R register selects the page: a
 * latch takes address line A7 in every refresh cycle, where the Z80 puts R, and
 * only LD R,A changes that bit. So after LD R,A the page changes at the next
 * opcode fetch's refresh (that opcode still comes from the old page) or at the
 * next interrupt acknowledge. It starts on page 0, and a reset (R = 0) goes back
 * there. The page in use is always in mem[]; a change swaps the other one in.
 */

#define RAM_PACK_START  0x4000
#define RAM_PACK_MAX_KB 48              // RAM that can be seen at once
#define RAM_PACK_96K    96              // RamSetPackKB(): the 96K pack
#define RAM_PAGE_SIZE   0xC000          // a page of the 96K pack: 4000h - FFFFh

// Size of the RAM pack in KB: 0, 16, 32, 48 (the default) or 96
unsigned RamGetPackKB(void);

// End of the RAM (one page of the 96K pack): 4000h - 10000h
unsigned RamGetEnd(void);

// Change the RAM pack; the ROM only notices after a reset. FALSE if there is no
// memory for the 96K pack's second page.
boolean RamSetPackKB(unsigned nKB);

// The 96K pack: its page in use (0 or 1), and the other one (RAM_PAGE_SIZE bytes,
// from 4000h); nullptr without the 96K pack
unsigned RamGetPage(void);
u8 *RamGetOtherPage(void);

// The memory in mem[] is page nPage (a snapshot loaded), without swapping; with
// bPending a latch change (to the other page) is still due, as after LD R,A
void RamSetPage(unsigned nPage, boolean bPending);

// Is a page change due (an LD R,A not yet followed by a refresh)?
boolean RamIsPending(void);

// Power-on (F12): the second page cleared like the first one, page 0
void RamReset(void);

extern "C" {
// For the Z80 (z80.c): set by LD R,A with the 96K pack; the latch then takes bit 7
// of R at the next refresh
extern int ram_paging;
extern int ram_latch_pending;
void ram_latch(unsigned char ucR);
}

#endif
