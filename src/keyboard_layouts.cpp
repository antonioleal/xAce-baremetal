#include "keyboard_layouts.h"

/*
 * Each table lists the digit row and the punctuation keys in full, and the
 * letter keys that are not plain QWERTY. Columns: normal, Shift, AltGr,
 * Shift+AltGr. Dead keys give their accent when it is an ASCII character.
 * Characters the Ace does not have (accented letters, the euro sign, ...)
 * are left out.
 *
 * HID keys beyond the US layout: 0x32 is the ISO key next to Enter, 0x64 the
 * ISO key next to left Shift, 0x87 the JIS "ro" key and 0x89 the JIS yen key.
 */

#define PND LAYOUT_POUND

static const TLayoutKey s_UK[] = {
    {0x1E, '1', '!'}, {0x1F, '2', '"'}, {0x20, '3', PND}, {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '^'}, {0x24, '7', '&'}, {0x25, '8', '*'},
    {0x26, '9', '('}, {0x27, '0', ')'},
    {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'}, {0x30, ']', '}'},
    {0x31, '#', '~'}, {0x32, '#', '~'}, {0x33, ';', ':'}, {0x34, '\'', '@'},
    {0x35, '`', 0},   {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'},
    {0x64, '\\', '|'},
};

static const TLayoutKey s_US[] = {
    {0x1E, '1', '!'}, {0x1F, '2', '@'}, {0x20, '3', '#'}, {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '^'}, {0x24, '7', '&'}, {0x25, '8', '*'},
    {0x26, '9', '('}, {0x27, '0', ')'},
    {0x2D, '-', '_'}, {0x2E, '=', '+'}, {0x2F, '[', '{'}, {0x30, ']', '}'},
    {0x31, '\\', '|'}, {0x32, '\\', '|'}, {0x33, ';', ':'}, {0x34, '\'', '"'},
    {0x35, '`', '~'}, {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'},
    {0x64, '\\', '|'},
};

static const TLayoutKey s_German[] = {
    {0x14, 'q', 'Q', '@'}, {0x1C, 'z', 'Z'}, {0x1D, 'y', 'Y'},
    {0x1E, '1', '!'}, {0x1F, '2', '"'}, {0x20, '3', 0},   {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '&'}, {0x24, '7', '/', '{'}, {0x25, '8', '(', '['},
    {0x26, '9', ')', ']'}, {0x27, '0', '=', '}'},
    {0x2D, 0, '?', '\\'}, {0x2E, 0, '`'}, {0x2F, 0, 0},   {0x30, '+', '*', '~'},
    {0x31, '#', '\''}, {0x32, '#', '\''}, {0x33, 0, 0},  {0x34, 0, 0},
    {0x35, '^', 0},   {0x36, ',', ';'}, {0x37, '.', ':'}, {0x38, '-', '_'},
    {0x64, '<', '>', '|'},
};

static const TLayoutKey s_French[] = {
    {0x04, 'q', 'Q'}, {0x14, 'a', 'A'}, {0x1A, 'z', 'Z'}, {0x1D, 'w', 'W'},
    {0x33, 'm', 'M'}, {0x10, ',', '?'},
    {0x1E, '&', '1'}, {0x1F, 0, '2', '~'}, {0x20, '"', '3', '#'}, {0x21, '\'', '4', '{'},
    {0x22, '(', '5', '['}, {0x23, '-', '6', '|'}, {0x24, 0, '7', '`'}, {0x25, '_', '8', '\\'},
    {0x26, 0, '9', '^'}, {0x27, 0, '0', '@'},
    {0x2D, ')', 0, ']'}, {0x2E, '=', '+', '}'}, {0x2F, '^', 0}, {0x30, '$', PND},
    {0x31, '*', 0},   {0x32, '*', 0},   {0x34, 0, '%'},   {0x35, 0, 0},
    {0x36, ';', '.'}, {0x37, ':', '/'}, {0x38, '!', 0},
    {0x64, '<', '>'},
};

static const TLayoutKey s_Italian[] = {
    {0x1E, '1', '!'}, {0x1F, '2', '"'}, {0x20, '3', PND}, {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '&'}, {0x24, '7', '/'}, {0x25, '8', '('},
    {0x26, '9', ')'}, {0x27, '0', '='},
    {0x2D, '\'', '?'}, {0x2E, 0, '^'}, {0x2F, 0, 0, '[', '{'}, {0x30, '+', '*', ']', '}'},
    {0x31, 0, 0},     {0x32, 0, 0},     {0x33, 0, 0, '@'}, {0x34, 0, 0, '#'},
    {0x35, '\\', '|'}, {0x36, ',', ';'}, {0x37, '.', ':'}, {0x38, '-', '_'},
    {0x64, '<', '>'},
};

static const TLayoutKey s_Spanish[] = {
    {0x1E, '1', '!', '|'}, {0x1F, '2', '"', '@'}, {0x20, '3', 0, '#'}, {0x21, '4', '$', '~'},
    {0x22, '5', '%'}, {0x23, '6', '&'}, {0x24, '7', '/'}, {0x25, '8', '('},
    {0x26, '9', ')'}, {0x27, '0', '='},
    {0x2D, '\'', '?'}, {0x2E, 0, 0},  {0x2F, '`', '^', '['}, {0x30, '+', '*', ']'},
    {0x31, 0, 0, '}'}, {0x32, 0, 0, '}'}, {0x33, 0, 0}, {0x34, 0, 0, '{'},
    {0x35, 0, 0, '\\'}, {0x36, ',', ';'}, {0x37, '.', ':'}, {0x38, '-', '_'},
    {0x64, '<', '>'},
};

static const TLayoutKey s_Portuguese[] = {
    {0x1E, '1', '!'}, {0x1F, '2', '"', '@'}, {0x20, '3', '#', PND}, {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '&'}, {0x24, '7', '/', '{'}, {0x25, '8', '(', '['},
    {0x26, '9', ')', ']'}, {0x27, '0', '=', '}'},
    {0x2D, '\'', '?'}, {0x2E, 0, 0},  {0x2F, '+', '*'}, {0x30, 0, '`'},
    {0x31, '~', '^'}, {0x32, '~', '^'}, {0x33, 0, 0},  {0x34, 0, 0},
    {0x35, '\\', '|'}, {0x36, ',', ';'}, {0x37, '.', ':'}, {0x38, '-', '_'},
    {0x64, '<', '>'},
};

// Norwegian, Swedish and Danish share the digit row
#define NORDIC_DIGITS \
    {0x1E, '1', '!'}, {0x1F, '2', '"', '@'}, {0x20, '3', '#', PND}, {0x21, '4', 0, '$'}, \
    {0x22, '5', '%'}, {0x23, '6', '&'}, {0x24, '7', '/', '{'}, {0x25, '8', '(', '['}, \
    {0x26, '9', ')', ']'}, {0x27, '0', '=', '}'}

static const TLayoutKey s_Norwegian[] = {
    NORDIC_DIGITS,
    {0x2D, '+', '?'}, {0x2E, '\\', '`'}, {0x2F, 0, 0},  {0x30, 0, '^', '~'},
    {0x31, '\'', '*'}, {0x32, '\'', '*'}, {0x33, 0, 0}, {0x34, 0, 0},
    {0x35, '|', 0},   {0x36, ',', ';'}, {0x37, '.', ':'}, {0x38, '-', '_'},
    {0x64, '<', '>'},
};

static const TLayoutKey s_Swedish[] = {
    NORDIC_DIGITS,
    {0x2D, '+', '?', '\\'}, {0x2E, 0, '`'}, {0x2F, 0, 0}, {0x30, 0, '^', '~'},
    {0x31, '\'', '*'}, {0x32, '\'', '*'}, {0x33, 0, 0}, {0x34, 0, 0},
    {0x35, 0, 0},     {0x36, ',', ';'}, {0x37, '.', ':'}, {0x38, '-', '_'},
    {0x64, '<', '>', '|'},
};

static const TLayoutKey s_Danish[] = {
    NORDIC_DIGITS,
    {0x2D, '+', '?'}, {0x2E, 0, '`', '|'}, {0x2F, 0, 0}, {0x30, 0, '^', '~'},
    {0x31, '\'', '*'}, {0x32, '\'', '*'}, {0x33, 0, 0}, {0x34, 0, 0},
    {0x35, 0, 0},     {0x36, ',', ';'}, {0x37, '.', ':'}, {0x38, '-', '_'},
    {0x64, '<', '>', '\\'},
};

// JIS: the yen key types a backslash, as on Japanese computers
static const TLayoutKey s_Japanese[] = {
    {0x1E, '1', '!'}, {0x1F, '2', '"'}, {0x20, '3', '#'}, {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '&'}, {0x24, '7', '\''}, {0x25, '8', '('},
    {0x26, '9', ')'}, {0x27, '0', 0},
    {0x2D, '-', '='}, {0x2E, '^', '~'}, {0x2F, '@', '`'}, {0x30, '[', '{'},
    {0x31, ']', '}'}, {0x32, ']', '}'}, {0x33, ';', '+'}, {0x34, ':', '*'},
    {0x35, 0, 0},     {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'},
    {0x64, 0, 0},     {0x87, '\\', '_'}, {0x89, '\\', '|'},
};

#define LAYOUT(name, altgr, keys) { name, altgr, keys, sizeof(keys) / sizeof(keys[0]) }

const TKeyboardLayout g_KeyboardLayouts[LayoutCount] = {
    { "Original ACE", 0, 0, 0 },
    LAYOUT("UK",         0, s_UK),
    LAYOUT("US",         0, s_US),
    LAYOUT("German",     1, s_German),
    LAYOUT("French",     1, s_French),
    LAYOUT("Italian",    1, s_Italian),
    LAYOUT("Spanish",    1, s_Spanish),
    LAYOUT("Portuguese", 1, s_Portuguese),
    LAYOUT("Norwegian",  1, s_Norwegian),
    LAYOUT("Swedish",    1, s_Swedish),
    LAYOUT("Danish",     1, s_Danish),
    LAYOUT("Japanese",   0, s_Japanese),
};

int layout_is_char_key(unsigned char hid)
{
    return (hid >= 0x04 && hid <= 0x27)         // letters, digits
        || (hid >= 0x2D && hid <= 0x38)         // punctuation
        || hid == 0x64 || hid == 0x87 || hid == 0x89;
}

unsigned char layout_char(const TKeyboardLayout *pLayout, unsigned char hid,
                          int bShift, int bAltGr)
{
    for (unsigned i = 0; i < pLayout->nKeys; i++) {
        const TLayoutKey *pKey = &pLayout->pKeys[i];
        if (pKey->hid == hid) {
            if (bAltGr) {
                return bShift ? pKey->altgr_shifted : pKey->altgr;
            }
            return bShift ? pKey->shifted : pKey->normal;
        }
    }

    // Letters not in the table are where QWERTY has them
    if (hid >= 0x04 && hid <= 0x1D && !bAltGr) {
        return (unsigned char) ((bShift ? 'A' : 'a') + hid - 0x04);
    }

    return 0;
}
