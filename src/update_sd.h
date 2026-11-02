#ifndef _UPDATE_SD_H
#define _UPDATE_SD_H

#include <circle/string.h>
#include <circle/types.h>

/*
 * System update from the web page (F11, /system): a release .zip of xAce-baremetal
 * (xAce-baremetal-<version>-pi<model>.zip) is uploaded and checked, then installed
 * once Y is pressed on the Ace. Only the files that differ from those on the card are
 * changed: the kernel image, the boot firmware (start*.elf, fixup*.dat, bootcode.bin,
 * *.dtb), the Wi-Fi firmware (firmware/) and the licences. config.txt, cmdline.txt,
 * wpa_supplicant.conf and the programs folder are never touched.
 *
 * Pi 4 / 400: the new kernel (and boot firmware) is first tried once with the boot
 * firmware's "tryboot": tryboot.txt (config.txt with the new kernel's name) is used for
 * one restart only. If the new kernel starts and runs, it puts all the new files in
 * place (the old ones go to SD:/update-backup); if it does not, switching the Pi off and
 * on starts the old one again, which removes the new files.
 * Other models: the files are put in place at once; the Pi is then switched off and on.
 */

// This kernel: version, model and build ("0.7", 4, "Nov  1 2026 21:30:00")
const char *UpdateVersion(void);
unsigned UpdateModel(void);
const char *UpdateBuild(void);

// Can this Pi try a new kernel first (tryboot)?
boolean UpdateCanTryBoot(void);

// At start-up, once the SD card is mounted: finish or undo a tried update. bTrial: the
// command line has xacetrial=1 (only tryboot.txt's has)
void UpdateStartup(boolean bTrial);

// Each frame: after the Ace has run for a while, a tried update is put in place.
// TRUE once, with what to tell the user, when something happened (also from
// UpdateStartup())
boolean UpdateFrame(CString *pLine1, CString *pLine2);

// --- Web page --------------------------------------------------------------------

// An uploaded release .zip: saved and checked. pReport gets what was found. TRUE if it
// can be installed (it is then pending, until installed or discarded).
boolean UpdateReceive(const u8 *pZip, unsigned nLength, CString *pReport);

boolean UpdateIsPending(void);
const char *UpdatePendingVersion(void);
const char *UpdatePendingBuild(void);
unsigned UpdatePendingFiles(void);

void UpdateDiscard(void);

// {"version":..,"model":..,"build":..,"tryboot":..,"pending":{..} or null,"last":".."}
void UpdateStatusJson(CString *pJson);

// --- On the Ace (F11, after Y) ---------------------------------------------------------

// Install the pending update. FALSE on error (pMessage says why). TRUE: on a Pi 4 / 400,
// restart with UpdateRestart() to try it; on the others it is in place (switch off and on)
boolean UpdateInstall(CString *pMessage);

// Pi 4 / 400: restart into the new kernel once (tryboot)
void UpdateRestart(void);

#endif
