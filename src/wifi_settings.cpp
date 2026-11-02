#include "wifi_settings.h"

#include <circle/string.h>
#include <circle/util.h>
#include <fatfs/ff.h>

// From hostap (src/crypto/sha1-pbkdf2.c): the WPA key of a passphrase
extern "C" int pbkdf2_sha1(const char *passphrase, const u8 *ssid, size_t ssid_len,
                           int iterations, u8 *buf, size_t buflen);

// The countries Circle's WLAN driver accepts (src/drivers/driver_circle.cpp in hostap):
// it does not join a network until one is set
static const char s_Countries[][3] = {
    "AD","AE","AF","AI","AL","AM","AN","AR","AS","AT","AU","AW","AZ",
    "BA","BB","BD","BE","BF","BG","BH","BL","BM","BN","BO","BR","BS",
    "BT","BY","BZ","CA","CF","CH","CI","CL","CN","CO","CR","CU","CX",
    "CY","CZ","DE","DK","DM","DO","DZ","EC","EE","EG","ES","ET","FI",
    "FM","FR","GB","GD","GE","GF","GH","GL","GP","GR","GT","GU","GY",
    "HK","HN","HR","HT","HU","ID","IE","IL","IN","IR","IS","IT","JM",
    "JO","JP","KE","KH","KN","KP","KR","KW","KY","KZ","LB","LC","LI",
    "LK","LS","LT","LU","LV","MA","MC","MD","ME","MF","MH","MK","MN",
    "MO","MP","MQ","MR","MT","MU","MV","MW","MX","MY","NG","NI","NL",
    "NO","NP","NZ","OM","PA","PE","PF","PG","PH","PK","PL","PM","PR",
    "PT","PW","PY","QA","RE","RO","RS","RU","RW","SA","SE","SG","SI",
    "SK","SN","SR","SV","SY","TC","TD","TG","TH","TN","TR","TT","TW",
    "TZ","UA","UG","US","UY","UZ","VC","VE","VI","VN","VU","WF","WS",
    "YE","YT","ZA","ZW"
};

static boolean valid_country(const char *pCode)
{
    for (unsigned i = 0; i < sizeof s_Countries / sizeof s_Countries[0]; i++) {
        if (pCode[0] == s_Countries[i][0] && pCode[1] == s_Countries[i][1] && pCode[2] == '\0') {
            return TRUE;
        }
    }
    return FALSE;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static boolean is_hex_key(const char *p)
{
    unsigned n = 0;
    for (; p[n] != '\0'; n++) {
        if (hex_digit(p[n]) < 0) {
            return FALSE;
        }
    }
    return n == 64;
}

const char *WiFiCheckSettings(const TWiFiSettings *pSettings, int *pField)
{
    *pField = 0;
    if (pSettings->SSID[0] == '\0') {
        return "Type the network name";
    }
    for (const char *p = pSettings->SSID; *p != '\0'; p++) {
        if (*p < ' ' || *p > '~') {
            return "Name: plain letters only";
        }
    }

    *pField = 1;
    unsigned nPassword = strlen(pSettings->Password);
    if (nPassword > 0 && !is_hex_key(pSettings->Password)) {
        if (nPassword < 8 || nPassword > 63) {
            return "Password: 8 to 63 characters";
        }
        for (const char *p = pSettings->Password; *p != '\0'; p++) {
            if (*p < ' ' || *p > '~') {
                return "Password: plain letters only";
            }
        }
    }

    *pField = 2;
    if (!valid_country(pSettings->Country)) {
        return "Unknown country code";
    }

    return nullptr;
}

// --- Writing ----------------------------------------------------------------

static void append_hex(CString *pText, const u8 *pData, unsigned nLength)
{
    for (unsigned i = 0; i < nLength; i++) {
        CString Byte;
        Byte.Format("%02x", (unsigned) pData[i]);
        pText->Append(Byte);
    }
}

void WiFiFormatSettings(const TWiFiSettings *pSettings, CString *pText)
{
    CString Line;
    *pText = "# Wi-Fi network for xAce-baremetal (F11), read by its WPA supplicant.\n"
                  "# Written by the F11 screen; it can be edited by hand too.\n\n";
    Line.Format("country=%s\n\n", pSettings->Country);
    pText->Append(Line);

    // The name as hex: any character is fine then, even " or #
    Line.Format("network={\n"
                "\t# %s\n"
                "\tssid=", pSettings->SSID);
    pText->Append(Line);
    append_hex(pText, (const u8 *) pSettings->SSID, strlen(pSettings->SSID));
    pText->Append("\n\tscan_ssid=1\n");     // finds hidden networks too

    const char *pPassword = pSettings->Password;
    if (pPassword[0] == '\0') {
        pText->Append("\tkey_mgmt=NONE\n");
    } else {
        if (is_hex_key(pPassword)) {
            Line.Format("\tpsk=%s\n", pPassword);
            pText->Append(Line);
        } else if (strchr(pPassword, '"') == nullptr && strchr(pPassword, '#') == nullptr) {
            Line.Format("\tpsk=\"%s\"\n", pPassword);
            pText->Append(Line);
        } else {
            // " and # confuse the file's comment rule: write the key the password makes
            u8 Key[32];
            pbkdf2_sha1(pPassword, (const u8 *) pSettings->SSID, strlen(pSettings->SSID),
                        4096, Key, sizeof Key);
            pText->Append("\tpsk=");
            append_hex(pText, Key, sizeof Key);
            pText->Append("\n");
        }
        pText->Append("\tkey_mgmt=WPA-PSK\n");
    }
    pText->Append("}\n");
}

boolean WiFiWriteSettings(const TWiFiSettings *pSettings)
{
    CString Text;
    WiFiFormatSettings(pSettings, &Text);

    FIL File;
    if (f_open(&File, WIFI_SETTINGS_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return FALSE;
    }
    UINT nWritten;
    unsigned nLength = Text.GetLength();
    boolean bOK = f_write(&File, (const char *) Text, nLength, &nWritten) == FR_OK && nWritten == nLength;
    if (f_close(&File) != FR_OK) {
        bOK = FALSE;
    }
    return bOK;
}

// --- Reading ----------------------------------------------------------------

// Value of "key=value" on a trimmed line, or nullptr
static const char *value_of(const char *pLine, const char *pKey)
{
    unsigned n = strlen(pKey);
    if (strncmp(pLine, pKey, n) != 0) {
        return nullptr;
    }
    const char *p = pLine + n;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') {
        return nullptr;
    }
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// A string value: "text" (up to the last "), or hex digits when bHex; FALSE if neither
static boolean parse_string(const char *pValue, boolean bHex, char *pOut, unsigned nMax)
{
    if (*pValue == '"') {
        const char *pEnd = nullptr;     // the last "
        for (const char *p = pValue + 1; *p != '\0'; p++) {
            if (*p == '"') {
                pEnd = p;
            }
        }
        if (pEnd == nullptr || (unsigned) (pEnd - pValue - 1) > nMax) {
            return FALSE;
        }
        memcpy(pOut, pValue + 1, pEnd - pValue - 1);
        pOut[pEnd - pValue - 1] = '\0';
        return TRUE;
    }

    if (!bHex) {
        return FALSE;
    }
    unsigned n = 0;
    while (hex_digit(pValue[0]) >= 0 && hex_digit(pValue[1]) >= 0) {
        if (n >= nMax) {
            return FALSE;
        }
        pOut[n++] = (char) (hex_digit(pValue[0]) << 4 | hex_digit(pValue[1]));
        pValue += 2;
    }
    pOut[n] = '\0';
    return n > 0 && *pValue == '\0';
}

boolean WiFiParseSettings(const char *pText, TWiFiSettings *pSettings)
{
    memset(pSettings, 0, sizeof *pSettings);

    int nNetwork = 0;               // 1 inside the first network block, 2 after it
    boolean bSSID = FALSE;
    while (*pText != '\0') {
        // One line, without the leading and trailing blanks
        char Line[256];
        unsigned n = 0;
        while (*pText != '\0' && *pText != '\n') {
            if (n < sizeof Line - 1) {
                Line[n++] = *pText;
            }
            pText++;
        }
        if (*pText == '\n') {
            pText++;
        }
        while (n > 0 && (Line[n - 1] == ' ' || Line[n - 1] == '\t' || Line[n - 1] == '\r')) {
            n--;
        }
        Line[n] = '\0';
        const char *p = Line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') {
            continue;
        }

        const char *pValue;
        if (nNetwork != 1) {
            if ((pValue = value_of(p, "country")) != nullptr) {
                if (strlen(pValue) == 2) {
                    strcpy(pSettings->Country, pValue);
                }
            } else if (nNetwork == 0 && strncmp(p, "network", 7) == 0 && strchr(p, '{') != nullptr) {
                nNetwork = 1;
            }
        } else {
            if (*p == '}') {
                nNetwork = 2;
            } else if ((pValue = value_of(p, "ssid")) != nullptr) {
                bSSID = parse_string(pValue, TRUE, pSettings->SSID, WIFI_SSID_MAX);
            } else if ((pValue = value_of(p, "psk")) != nullptr) {
                if (!parse_string(pValue, FALSE, pSettings->Password, WIFI_PASSWORD_MAX)
                    && is_hex_key(pValue)) {
                    strcpy(pSettings->Password, pValue);
                }
            }
        }
    }

    return bSSID;
}

boolean WiFiReadSettings(TWiFiSettings *pSettings)
{
    static char Text[4096];
    memset(pSettings, 0, sizeof *pSettings);

    FIL File;
    if (f_open(&File, WIFI_SETTINGS_FILE, FA_READ) != FR_OK) {
        return FALSE;
    }
    UINT nRead = 0;
    FRESULT Result = f_read(&File, Text, sizeof Text - 1, &nRead);
    f_close(&File);
    if (Result != FR_OK) {
        return FALSE;
    }
    Text[nRead] = '\0';

    return WiFiParseSettings(Text, pSettings);
}
