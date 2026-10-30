#include "ram_ace.h"
#include <string.h>

extern "C" {
#include "z80.h"
}

#define BANK_SIZE   0x2000      // memptr[] / memattr[] bank

static unsigned s_nPackKB = RAM_PACK_MAX_KB;

unsigned RamGetPackKB(void)
{
    return s_nPackKB;
}

void RamSetPackKB(unsigned nKB)
{
    if (nKB > RAM_PACK_MAX_KB) {
        nKB = RAM_PACK_MAX_KB;
    }

    unsigned nEnd = RAM_PACK_START + nKB * 1024;
    for (unsigned nBank = RAM_PACK_START / BANK_SIZE; nBank < 8; nBank++) {
        int bPresent = nBank * BANK_SIZE < nEnd;
        memattr[nBank] = bPresent ? 1 : 0;
        if (!bPresent) {
            memset(mem + nBank * BANK_SIZE, 0xFF, BANK_SIZE);
        }
    }

    s_nPackKB = nKB;
}
