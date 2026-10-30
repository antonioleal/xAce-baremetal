#ifndef _RAM_ACE_H
#define _RAM_ACE_H

/*
 * RAM pack (Ctrl+F12). The Ace has 3K of its own (1K video, 1K character set,
 * 1K for programs); a 16K, 32K or 48K pack adds RAM from 4000h, for 19K, 35K
 * or 51K in total. Missing RAM ignores writes and reads FFh, so the ROM's RAM
 * test at power-on stops at the end of the pack.
 */

#define RAM_PACK_START  0x4000
#define RAM_PACK_MAX_KB 48

// Size of the RAM pack in KB: 0, 16, 32 or 48 (the default)
unsigned RamGetPackKB(void);

// Change the RAM pack; the ROM only notices after a reset
void RamSetPackKB(unsigned nKB);

#endif
