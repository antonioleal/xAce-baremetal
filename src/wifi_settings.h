#ifndef _WIFI_SETTINGS_H
#define _WIFI_SETTINGS_H

#include <circle/string.h>
#include <circle/types.h>

/*
 * The Wi-Fi network to join (F11), kept in SD:/wpa_supplicant.conf, the file read by
 * Circle's WPA supplicant. The F11 screen reads the settings back from it and writes
 * it again when they change; the file can also be written by hand (only the country
 * and the first network's name and password are shown on the F11 screen then).
 */

#define WIFI_SETTINGS_FILE  "SD:/wpa_supplicant.conf"

#define WIFI_SSID_MAX       32      // network name
#define WIFI_PASSWORD_MAX   64      // 8-63 characters, or the 64 hex digits of the key

struct TWiFiSettings {
    char SSID[WIFI_SSID_MAX + 1];
    char Password[WIFI_PASSWORD_MAX + 1];   // empty: open network
    char Country[3];                        // ISO 3166 code, e.g. "PT"
};

// Read the settings from the file; FALSE if there is none, or it has no network
// (the fields are then empty, or what could be read)
boolean WiFiReadSettings(TWiFiSettings *pSettings);

// Check the settings: nullptr if fine, else what is wrong (at most 30 characters),
// with the field in *pField: 0 name, 1 password, 2 country
const char *WiFiCheckSettings(const TWiFiSettings *pSettings, int *pField);

// Write the settings to the file (replacing it); FALSE on error
boolean WiFiWriteSettings(const TWiFiSettings *pSettings);

// The text of the file for the settings
void WiFiFormatSettings(const TWiFiSettings *pSettings, CString *pText);

// Parse the text of a file
boolean WiFiParseSettings(const char *pText, TWiFiSettings *pSettings);

#endif
