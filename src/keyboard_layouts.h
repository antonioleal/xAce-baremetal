#ifndef _KEYBOARD_LAYOUTS_H
#define _KEYBOARD_LAYOUTS_H

/*
 * Host keyboard layouts (F8). Each layout gives the character printed on a key,
 * which keyboard_circle.cpp then types on the Ace. Characters are ASCII, plus
 * LAYOUT_POUND for the pound sign; 0 means the key has no character the Ace has.
 */

#define LAYOUT_POUND    0xA3    // Latin-1 pound sign

enum TLayoutIndex {
    LayoutOriginalAce,          // the built-in US mapping (keyboard_circle.cpp)
    LayoutUK,
    LayoutUS,
    LayoutGerman,
    LayoutFrench,
    LayoutItalian,
    LayoutSpanish,
    LayoutPortuguese,
    LayoutNorwegian,
    LayoutSwedish,
    LayoutDanish,
    LayoutJapanese,
    LayoutCount
};

struct TLayoutKey {
    unsigned char hid;          // USB HID usage ID (keyboard page 0x07)
    unsigned char normal;
    unsigned char shifted;
    unsigned char altgr;
    unsigned char altgr_shifted;
};

struct TKeyboardLayout {
    const char       *pName;
    int               bAltGr;   // Right Alt is AltGr (otherwise it is Symbol Shift)
    const TLayoutKey *pKeys;    // keys that differ from a plain QWERTY letter
    unsigned          nKeys;
};

extern const TKeyboardLayout g_KeyboardLayouts[LayoutCount];

// Keys whose character depends on the layout (letters, digits, punctuation)
int layout_is_char_key(unsigned char hid);

// The character on a key, 0 if none
unsigned char layout_char(const TKeyboardLayout *pLayout, unsigned char hid,
                          int bShift, int bAltGr);

#endif
