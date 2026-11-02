#include "ram_ace.h"
#include <string.h>

extern "C" {
#include "z80.h"
}

#define BANK_SIZE   0x2000      // memptr[] / memattr[] bank

static unsigned s_nPackKB = RAM_PACK_MAX_KB;

// The 96K pack: the page not in mem[] (allocated when first chosen), and which
// page mem[] holds
static u8 *s_pOtherPage = nullptr;
static unsigned s_nPage = 0;

int ram_paging = 0;
int ram_latch_pending = 0;

unsigned RamGetPackKB(void)
{
    return s_nPackKB;
}

unsigned RamGetEnd(void)
{
    unsigned nKB = s_nPackKB > RAM_PACK_MAX_KB ? RAM_PACK_MAX_KB : s_nPackKB;
    return RAM_PACK_START + nKB * 1024;
}

boolean RamSetPackKB(unsigned nKB)
{
    if (nKB != RAM_PACK_96K && nKB > RAM_PACK_MAX_KB) {
        nKB = RAM_PACK_MAX_KB;
    }
    if (nKB == RAM_PACK_96K && s_pOtherPage == nullptr) {
        s_pOtherPage = new u8[RAM_PAGE_SIZE];
        if (s_pOtherPage == nullptr) {
            return FALSE;
        }
        memset(s_pOtherPage, 0xFF, RAM_PAGE_SIZE);
    }

    s_nPackKB = nKB;
    unsigned nEnd = RamGetEnd();
    for (unsigned nBank = RAM_PACK_START / BANK_SIZE; nBank < 8; nBank++) {
        int bPresent = nBank * BANK_SIZE < nEnd;
        memattr[nBank] = bPresent ? 1 : 0;
        if (!bPresent) {
            memset(mem + nBank * BANK_SIZE, 0xFF, BANK_SIZE);
        }
    }

    // Without the 96K pack, what is in mem[] is all there is
    s_nPage = 0;
    ram_latch_pending = 0;
    ram_paging = nKB == RAM_PACK_96K;
    return TRUE;
}

unsigned RamGetPage(void)
{
    return s_nPage;
}

u8 *RamGetOtherPage(void)
{
    return ram_paging ? s_pOtherPage : nullptr;
}

void RamSetPage(unsigned nPage, boolean bPending)
{
    if (ram_paging) {
        s_nPage = nPage & 1;
        ram_latch_pending = bPending;
    }
}

boolean RamIsPending(void)
{
    return ram_latch_pending != 0;
}

void RamReset(void)
{
    s_nPage = 0;
    ram_latch_pending = 0;
    if (ram_paging) {
        memset(s_pOtherPage, 0xFF, RAM_PAGE_SIZE);
    }
}

// The refresh after LD R,A: the page bit 7 of R selects
void ram_latch(unsigned char ucR)
{
    ram_latch_pending = 0;
    unsigned nPage = ucR >> 7;
    if (!ram_paging || nPage == s_nPage) {
        return;
    }

    // Swap the pages' contents (memcpy(): mem[] need not be aligned for wide accesses)
    u8 Buffer[1024];
    for (unsigned i = 0; i < RAM_PAGE_SIZE; i += sizeof Buffer) {
        memcpy(Buffer, mem + RAM_PACK_START + i, sizeof Buffer);
        memcpy(mem + RAM_PACK_START + i, s_pOtherPage + i, sizeof Buffer);
        memcpy(s_pOtherPage + i, Buffer, sizeof Buffer);
    }
    s_nPage = nPage;
}
