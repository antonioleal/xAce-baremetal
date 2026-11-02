#ifndef _WEB_FILES_H
#define _WEB_FILES_H

#include <circle/string.h>
#include <circle/types.h>

/*
 * File exchange through a web page (F11): the Pi joins the cable network or a Wi-Fi
 * network (DHCP) and serves a page for listing, uploading, downloading (several as a .zip), renaming and
 * deleting the Ace's files in the SD card root (.tap, .tzx, .ace, .fth, .fs, .f).
 *
 * Circle's network runs as cooperative tasks, and they only run while WebFilesRun()
 * is being called: from the F11 screen, with the Ace paused. So the page never
 * touches the SD card while the Ace is using it, and the emulation timing is not
 * disturbed. The network, once started, stays up until the Pi is switched off, so
 * the choice of cable or Wi-Fi stays until then too (Circle has one network).
 *
 * Wi-Fi uses the on-board WLAN of the Pi 3, 4, 400 and Zero (2) W, with its firmware
 * files in SD:/firmware/ and the network to join in SD:/wpa_supplicant.conf
 * (wifi_settings.h).
 */

enum TWebNetwork {
    WebCable,
    WebWiFi
};

enum TWebState {
    WebNotStarted,
    WebFailed,              // the network could not be started
    WebNoAdapter,           // no Ethernet adapter found
    WebNoCable,             // link down
    WebNoWiFi,              // no Wi-Fi device found, or it could not be started
    WebNoWiFiFirmware,      // the Wi-Fi firmware files are not in SD:/firmware/
    WebWiFiBadSettings,     // the WPA supplicant could not read wpa_supplicant.conf
    WebJoining,             // joining the Wi-Fi network
    WebJoinTrouble,         // still not joined after a while: wrong settings?
    WebWaitingAddress,      // waiting for DHCP
    WebReady                // serving at the address
};

// Start the network and the web server (the first time); FALSE if they could not be
// started, with the reason in *pFailure. After a Wi-Fi failure the cable can still be
// chosen; after a start, the other network cannot.
boolean WebFilesStart(TWebNetwork Network, TWebState *pFailure);

// Started? With which network?
boolean WebFilesStarted(TWebNetwork *pNetwork);

// Wi-Fi failed to start before (WebNoWiFi...), or WebNotStarted
TWebState WebFilesWiFiFailure(void);

// Wi-Fi: wpa_supplicant.conf has changed. FALSE if it cannot be used before a restart:
// the Pi already got an address (Circle does not ask for a new one), or the WPA
// supplicant has stopped.
boolean WebFilesWiFiReload(void);

// Let the network and the web server run for about nMicros
void WebFilesRun(unsigned nMicros);

// pAddress gets "http://a.b.c.d", pName "http://xace.local" (empty until known)
TWebState WebFilesGetState(CString *pAddress, CString *pName);

// What the page has done, for the F11 screen: the last WEB_ACTIVITY_LINES lines,
// oldest first. The count goes up with every new line.
#define WEB_ACTIVITY_LINES  8
#define WEB_ACTIVITY_WIDTH  30
unsigned WebFilesActivityCount(void);
const char *WebFilesActivityLine(unsigned nIndex);

#endif
