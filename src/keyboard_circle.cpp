#include "keyboard_circle.h"
#include "keyboard_layouts.h"
#include <string.h>

/*
 * Jupiter Ace Keyboard Matrix:
 *
 * Port 0 (A8  = 0): Shift(b0), SymShift(b1), Z(b2), X(b3), C(b4)
 * Port 1 (A9  = 0): A(b0), S(b1), D(b2), F(b3), G(b4)
 * Port 2 (A10 = 0): Q(b0), W(b1), E(b2), R(b3), T(b4)
 * Port 3 (A11 = 0): 1(b0), 2(b1), 3(b2), 4(b3), 5(b4)
 * Port 4 (A12 = 0): 0(b0), 9(b1), 8(b2), 7(b3), 6(b4)
 * Port 5 (A13 = 0): P(b0), O(b1), I(b2), U(b3), Y(b4)
 * Port 6 (A14 = 0): Enter(b0), L(b1), K(b2), J(b3), H(b4)
 * Port 7 (A15 = 0): Space(b0), M(b1), N(b2), B(b3), V(b4)
 *
 * Each bit is active low (0 = pressed, 1 = released).
 */

// An Ace key combination: matrix position plus optional Shift / Symbol Shift
#define K(port, bit)  ((unsigned short) (0x80 | ((port) << 3) | (bit)))
#define SH            0x100     // Ace Shift
#define SY            0x200     // Ace Symbol Shift
#define NONE          0

// Matrix positions
#define A_ K(1,0)
#define B_ K(7,3)
#define C_ K(0,4)
#define D_ K(1,2)
#define E_ K(2,2)
#define F_ K(1,3)
#define G_ K(1,4)
#define H_ K(6,4)
#define I_ K(5,2)
#define J_ K(6,3)
#define K_ K(6,2)
#define L_ K(6,1)
#define M_ K(7,1)
#define N_ K(7,2)
#define O_ K(5,1)
#define P_ K(5,0)
#define Q_ K(2,0)
#define R_ K(2,3)
#define S_ K(1,1)
#define T_ K(2,4)
#define U_ K(5,3)
#define V_ K(7,4)
#define W_ K(2,1)
#define X_ K(0,3)
#define Y_ K(5,4)
#define Z_ K(0,2)
#define N1 K(3,0)
#define N2 K(3,1)
#define N3 K(3,2)
#define N4 K(3,3)
#define N5 K(3,4)
#define N6 K(4,4)
#define N7 K(4,3)
#define N8 K(4,2)
#define N9 K(4,1)
#define N0 K(4,0)
#define ENTER K(6,0)
#define SPACE K(7,0)

struct TKeyMapEntry {
    unsigned char  hid;         // USB HID usage ID (keyboard page 0x07)
    unsigned short plain;       // Ace keys without host Shift
    unsigned short shifted;     // Ace keys with host Shift (NONE: plain + Ace Shift)
};

/*
 * US layout. Host symbols map to the Ace Symbol Shift legends:
 *   ! @ # $ %  1-5    & ' ( ) _  6 7 8 9 0    ~ | \ { }  A S D F G
 *   < > [ ]    R T Y U    ; "  O P    ^ - + =  H J K L    : ? / * , .  Z C V B N M
 * Host Shift is only passed to the Ace for letters and the special keys,
 * so that e.g. Shift+9 types '(' instead of entering graphics mode.
 */
static const TKeyMapEntry s_KeyMap[] = {
    // Letters: host Shift becomes Ace Shift
    {0x04, A_, NONE}, {0x05, B_, NONE}, {0x06, C_, NONE}, {0x07, D_, NONE},
    {0x08, E_, NONE}, {0x09, F_, NONE}, {0x0A, G_, NONE}, {0x0B, H_, NONE},
    {0x0C, I_, NONE}, {0x0D, J_, NONE}, {0x0E, K_, NONE}, {0x0F, L_, NONE},
    {0x10, M_, NONE}, {0x11, N_, NONE}, {0x12, O_, NONE}, {0x13, P_, NONE},
    {0x14, Q_, NONE}, {0x15, R_, NONE}, {0x16, S_, NONE}, {0x17, T_, NONE},
    {0x18, U_, NONE}, {0x19, V_, NONE}, {0x1A, W_, NONE}, {0x1B, X_, NONE},
    {0x1C, Y_, NONE}, {0x1D, Z_, NONE},

    // Digit row: shifted gives the host symbol, except Shift + 5-8: the Ace's cursor
    // keys, as on the Ace (% ^ & * are Symbol Shift + 5 / H / 6 / B). Original ACE only:
    // the F8 layouts keep their symbols there
    {0x1E, N1, SY|N1},      // 1 !
    {0x1F, N2, SY|N2},      // 2 @
    {0x20, N3, SY|N3},      // 3 #
    {0x21, N4, SY|N4},      // 4 $
    {0x22, N5, NONE},       // 5, Shift: cursor left
    {0x23, N6, NONE},       // 6, Shift: cursor up (in the ROM)
    {0x24, N7, NONE},       // 7, Shift: cursor down
    {0x25, N8, NONE},       // 8, Shift: cursor right
    {0x26, N9, SY|N8},      // 9 (
    {0x27, N0, SY|N9},      // 0 )

    // Control keys
    {0x28, ENTER, ENTER},           // Return
    {0x29, SH|SPACE, SH|SPACE},     // Escape    -> Break
    {0x2A, SH|N0, SH|N0},           // Backspace -> Delete
    {0x2C, SPACE, SPACE},           // Space
    {0x39, SH|N2, SH|N2},           // Caps Lock -> Caps Lock
    {0x4C, SH|N0, SH|N0},           // Delete    -> Delete

    // Punctuation
    {0x2D, SY|J_, SY|N0},   // - _
    {0x2E, SY|L_, SY|K_},   // = +
    {0x2F, SY|Y_, SY|F_},   // [ {
    {0x30, SY|U_, SY|G_},   // ] }
    {0x31, SY|D_, SY|S_},   // \ |
    {0x33, SY|O_, SY|Z_},   // ; :
    {0x34, SY|N7, SY|P_},   // ' "
    {0x35, NONE,  SY|A_},   // ` ~
    {0x36, SY|N_, SY|R_},   // , <
    {0x37, SY|M_, SY|T_},   // . >
    {0x38, SY|V_, SY|C_},   // / ?

    // Function keys
    {0x3A, SH|N1, SH|N1},   // F1 -> Delete Line
    {0x3D, SH|N4, SH|N4},   // F4 -> Inverse Video
    {0x42, SH|N9, SH|N9},   // F9 -> Graphics

    // Cursor keys
    {0x4F, SH|N8, SH|N8},   // Right
    {0x50, SH|N5, SH|N5},   // Left
    {0x51, SH|N7, SH|N7},   // Down (in the ROM Shift+7 moves the cursor down,
    {0x52, SH|N6, SH|N6},   // Up    Shift+6 up)

    // Keypad
    {0x54, SY|V_, SY|V_},   // /
    {0x55, SY|B_, SY|B_},   // *
    {0x56, SY|J_, SY|J_},   // -
    {0x57, SY|K_, SY|K_},   // +
    {0x58, ENTER, ENTER},   // Enter
    {0x59, N1, N1}, {0x5A, N2, N2}, {0x5B, N3, N3}, {0x5C, N4, N4},
    {0x5D, N5, N5}, {0x5E, N6, N6}, {0x5F, N7, N7}, {0x60, N8, N8},
    {0x61, N9, N9}, {0x62, N0, N0},
    {0x63, SY|M_, SY|M_},   // .
};

// Ace keys for a character typed in one of the F8 layouts or spooled (the Symbol
// Shift legends, as in the ROM's key table; A3h is the pound sign, A9h the
// copyright sign); NONE if the Ace has no such character
static unsigned short ace_from_char(unsigned char c)
{
    static const unsigned short letters[26] = {
        A_, B_, C_, D_, E_, F_, G_, H_, I_, J_, K_, L_, M_,
        N_, O_, P_, Q_, R_, S_, T_, U_, V_, W_, X_, Y_, Z_
    };
    static const unsigned short digits[10] = {N0, N1, N2, N3, N4, N5, N6, N7, N8, N9};
    static const struct { unsigned char c; unsigned short ace; } symbols[] = {
        {'!', SY|N1}, {'@', SY|N2}, {'#', SY|N3}, {'$', SY|N4}, {'%', SY|N5},
        {'&', SY|N6}, {'\'', SY|N7}, {'(', SY|N8}, {')', SY|N9}, {'_', SY|N0},
        {'<', SY|R_}, {'>', SY|T_}, {'[', SY|Y_}, {']', SY|U_}, {';', SY|O_},
        {'"', SY|P_}, {'~', SY|A_}, {'|', SY|S_}, {'\\', SY|D_}, {'{', SY|F_},
        {'}', SY|G_}, {'^', SY|H_}, {'-', SY|J_}, {'+', SY|K_}, {'=', SY|L_},
        {':', SY|Z_}, {LAYOUT_POUND, SY|X_}, {0xA9, SY|I_}, {'?', SY|C_}, {'/', SY|V_},
        {'*', SY|B_}, {',', SY|N_}, {'.', SY|M_}, {' ', SPACE},
    };

    if (c >= 'a' && c <= 'z') return letters[c - 'a'];
    if (c >= 'A' && c <= 'Z') return SH | letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    for (unsigned i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
        if (symbols[i].c == c) return symbols[i].ace;
    }
    return NONE;
}

#define HID_KEY_F3      0x3C
#define HID_KEY_F5      0x3E
#define HID_KEY_F6      0x3F
#define HID_KEY_ENTER   0x28
#define HID_KEY_TAB     0x2B
#define HID_KEY_KP_ENTER 0x58
#define HID_KEY_F7      0x40
#define HID_KEY_F8      0x41
#define HID_KEY_F4      0x3D
#define HID_KEY_F10     0x43
#define HID_KEY_ESCAPE  0x29
#define HID_KEY_F12     0x45
#define HID_KEY_POWER   0x66        // keyboard power key, e.g. Fn+F10 on the Pi 400

// Written from USB interrupt context, read by the Z80 core
static volatile unsigned char s_keyboard_ports[8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static volatile int s_reset_requested = 0;
static volatile int s_invert_requested = 0;
static volatile int s_list_requested = 0;
static volatile int s_delete_requested = 0;
static volatile int s_poweroff_requested = 0;
static volatile int s_layout_requested = 0;
static volatile int s_ram_requested = 0;
static volatile int s_save_requested = 0;           // F7
static volatile int s_history_request = 0;          // TAB: 1 older line, Shift+TAB: -1 newer
static volatile int s_history_list_requested = 0;   // Ctrl+TAB
static volatile int s_enter_pressed = 0;            // for the line history
static volatile int s_other_key_pressed = 0;        // a key other than TAB, ends browsing
static volatile int s_spooling = 0;                 // keys come from the spooler
static volatile int s_spool_abort = 0;              // Esc while spooling
static volatile int s_layout = LayoutOriginalAce;     // F8
static volatile int s_joystick_requested = 0;       // F3
static volatile unsigned char s_held_key = 0;
static unsigned char s_prev_keys[6];        // previous report, to detect presses

// Single producer (USB interrupt), single consumer (host UI)
#define EVENT_QUEUE_SIZE 32
static struct { unsigned char key, mods; } s_events[EVENT_QUEUE_SIZE];
static volatile unsigned s_event_head = 0;
static volatile unsigned s_event_tail = 0;

void keyboard_init(void)
{
    keyboard_clear();
    s_reset_requested = 0;
    s_invert_requested = 0;
    s_list_requested = 0;
    s_delete_requested = 0;
    s_poweroff_requested = 0;
    s_layout_requested = 0;
    s_ram_requested = 0;
    s_save_requested = 0;
    s_joystick_requested = 0;
    s_history_request = 0;
    s_history_list_requested = 0;
    s_enter_pressed = 0;
    s_other_key_pressed = 0;
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    keyboard_flush_events();
}

void keyboard_clear(void)
{
    for (int i = 0; i < 8; i++) {
        s_keyboard_ports[i] = 0xFF;
    }
}

static volatile unsigned char s_joystick_ports[8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

unsigned char keyboard_get_keyport(int port)
{
    if (port >= 0 && port < 8) {
        if (s_spooling) {
            return s_keyboard_ports[port];      // the spooler's keys only
        }
        return s_keyboard_ports[port] & s_joystick_ports[port];
    }
    return 0xFF;
}

int keyboard_check_reset(void)
{
    int reset = s_reset_requested;
    s_reset_requested = 0;
    return reset;
}

int keyboard_check_invert(void)
{
    int invert = s_invert_requested;
    s_invert_requested = 0;
    return invert;
}

int keyboard_check_list(void)
{
    int list = s_list_requested;
    s_list_requested = 0;
    return list;
}

int keyboard_check_delete(void)
{
    int del = s_delete_requested;
    s_delete_requested = 0;
    return del;
}

int keyboard_check_poweroff(void)
{
    int off = s_poweroff_requested;
    s_poweroff_requested = 0;
    return off;
}

static inline void press(unsigned char ports[8], unsigned short key)
{
    if (key & 0x80) {
        ports[(key >> 3) & 7] &= ~(1 << (key & 7));
    }
    if (key & SH) {
        ports[0] &= ~0x01;
    }
    if (key & SY) {
        ports[0] &= ~0x02;
    }
}

int keyboard_check_history(void)
{
    int request = s_history_request;
    s_history_request = 0;
    return request;
}

int keyboard_check_history_list(void)
{
    int list = s_history_list_requested;
    s_history_list_requested = 0;
    return list;
}

int keyboard_check_enter(void)
{
    int enter = s_enter_pressed;
    s_enter_pressed = 0;
    return enter;
}

int keyboard_check_other_key(void)
{
    int other = s_other_key_pressed;
    s_other_key_pressed = 0;
    return other;
}

int keyboard_check_save(void)
{
    int save = s_save_requested;
    s_save_requested = 0;
    return save;
}

// Spooling: the host keyboard is off (only Esc is watched) and the spooler
// presses the Ace keys itself
void keyboard_set_spooling(int bOn)
{
    s_spooling = bOn;
    s_spool_abort = 0;
    keyboard_clear();
}

int keyboard_check_spool_abort(void)
{
    int abort = s_spool_abort;
    s_spool_abort = 0;
    return abort;
}

int keyboard_spool_key(unsigned char c)
{
    unsigned short ace = c == '\n' ? ENTER
                       : c == 0x01 ? SH | N1            // Delete Line (SPOOL_KEY_ codes)
                       : c == 0x02 ? SH | N4            // Inverse Video (toggles)
                       : ace_from_char(c);
    if (ace == NONE) {
        return 0;
    }

    unsigned char ports[8];
    memset(ports, 0xFF, sizeof(ports));
    press(ports, ace);
    for (int i = 0; i < 8; i++) {
        s_keyboard_ports[i] = ports[i];
    }
    return 1;
}

int keyboard_check_ram(void)
{
    int ram = s_ram_requested;
    s_ram_requested = 0;
    return ram;
}

int keyboard_check_layout(void)
{
    int layout = s_layout_requested;
    s_layout_requested = 0;
    return layout;
}

int keyboard_layout_count(void)
{
    return LayoutCount;
}

const char *keyboard_layout_name(int nLayout)
{
    return g_KeyboardLayouts[nLayout].pName;
}

int keyboard_get_layout(void)
{
    return s_layout;
}

void keyboard_set_layout(int nLayout)
{
    if (nLayout >= 0 && nLayout < LayoutCount) {
        s_layout = nLayout;
    }
}

unsigned char keyboard_held_key(void)
{
    return s_held_key;
}

void keyboard_flush_events(void)
{
    s_event_tail = s_event_head;
}

int keyboard_get_event(unsigned char *pKey, unsigned char *pModifiers)
{
    unsigned tail = s_event_tail;
    if (tail == s_event_head) {
        return 0;
    }
    *pKey = s_events[tail].key;
    *pModifiers = s_events[tail].mods;
    s_event_tail = (tail + 1) % EVENT_QUEUE_SIZE;
    return 1;
}

static void push_event(unsigned char key, unsigned char mods)
{
    unsigned next = (s_event_head + 1) % EVENT_QUEUE_SIZE;
    if (next != s_event_tail) {             // drop when full
        s_events[s_event_head].key = key;
        s_events[s_event_head].mods = mods;
        s_event_head = next;
    }
}

// Text entry on the host screens: the selected layout (US for Original ACE)
char keyboard_hid_to_ascii(unsigned char key, unsigned char ucModifiers)
{
    if (key == 0x2C) {
        return ' ';
    }
    if (key >= 0x59 && key <= 0x62) {       // keypad digits
        return "1234567890"[key - 0x59];
    }

    int nLayout = s_layout;
    const TKeyboardLayout *pLayout =
        &g_KeyboardLayouts[nLayout != LayoutOriginalAce ? nLayout : LayoutUS];
    int bShift = (ucModifiers & 0x22) != 0;
    int bAltGr = pLayout->bAltGr && (ucModifiers & 0x40) != 0;

    unsigned char c = layout_char(pLayout, key, bShift, bAltGr);
    return (c >= 0x20 && c < 0x7F) ? (char) c : 0;
}

static int was_down(unsigned char key)
{
    for (int i = 0; i < 6; i++) {
        if (s_prev_keys[i] == key) {
            return 1;
        }
    }
    return 0;
}

static const TKeyMapEntry *lookup(unsigned char hid)
{
    for (unsigned i = 0; i < sizeof(s_KeyMap) / sizeof(s_KeyMap[0]); i++) {
        if (s_KeyMap[i].hid == hid) {
            return &s_KeyMap[i];
        }
    }
    return 0;
}

// Map a USB HID boot report to the Ace keyboard matrix
void keyboard_update(unsigned char ucModifiers, const unsigned char rawKeys[6])
{
    unsigned char ports[8];
    memset(ports, 0xFF, sizeof(ports));

    // F8 layout; Original ACE uses s_KeyMap only
    int nLayout = s_layout;
    const TKeyboardLayout *pLayout =
        nLayout != LayoutOriginalAce ? &g_KeyboardLayouts[nLayout] : 0;
    int bHasAltGr = pLayout != 0 && pLayout->bAltGr;

    int bShift = (ucModifiers & 0x22) != 0;     // Left / Right Shift
    int bAltGr = bHasAltGr && (ucModifiers & 0x40) != 0;   // Right Alt as AltGr
    // Left / Right Ctrl or Alt (Right Alt only when it is not AltGr)
    int bSym   = (ucModifiers & (bHasAltGr ? 0x15 : 0x55)) != 0;

    if (bSym) {
        // Ctrl/Alt held: positional mode, so every Ace Symbol Shift
        // combination (including the (c) and pound legends) is reachable
        press(ports, SY | (bShift ? SH : 0));
    }

    if (s_spooling) {
        // Spooling: the Ace's keys belong to the spooler; Esc stops it
        unsigned char held = 0;
        for (int i = 0; i < 6; i++) {
            if (rawKeys[i] == 0) continue;
            if (held == 0) held = rawKeys[i];
            if (rawKeys[i] == HID_KEY_ESCAPE && !was_down(rawKeys[i])) s_spool_abort = 1;
        }
        memcpy(s_prev_keys, rawKeys, sizeof(s_prev_keys));
        s_held_key = held;
        return;
    }

    int bKeyDown = 0;
    unsigned char held = 0;
    for (int i = 0; i < 6; i++) {
        unsigned char key = rawKeys[i];
        if (key == 0) continue;
        if (held == 0) held = key;

        // Act on presses only: some keyboards repeat reports while a key is held
        int bPressed = !was_down(key);
        if (bPressed) {
            push_event(key, ucModifiers);
        }

        if (key == HID_KEY_TAB) {           // line history (line_history.cpp)
            if (bPressed) {
                if (ucModifiers & 0x11) {           // Left / Right Ctrl: the list
                    s_history_list_requested = 1;
                } else {
                    s_history_request = bShift ? -1 : 1;
                }
            }
            continue;
        }
        if (bPressed) {
            s_other_key_pressed = 1;
            if (key == HID_KEY_ENTER || key == HID_KEY_KP_ENTER) s_enter_pressed = 1;
        }

        if (key == HID_KEY_F12) {
            if (bPressed) {
                if (ucModifiers & 0x11) {       // Left / Right Ctrl
                    s_ram_requested = 1;
                } else {
                    s_reset_requested = 1;
                }
            }
            continue;
        }

        if (key == HID_KEY_F4 && (ucModifiers & 0x11)) {    // Ctrl+F4 (F4 alone: Inverse Video)
            if (bPressed) s_invert_requested = 1;
            continue;
        }

        if (key == HID_KEY_F5) {
            if (bPressed) s_list_requested = 1;
            continue;
        }

        if (key == HID_KEY_F6) {
            if (bPressed) s_delete_requested = 1;
            continue;
        }

        if (key == HID_KEY_F10 || key == HID_KEY_POWER) {
            if (bPressed) s_poweroff_requested = 1;
            continue;
        }

        if (key == HID_KEY_F7) {
            if (bPressed) s_save_requested = 1;
            continue;
        }

        if (key == HID_KEY_F8) {
            if (bPressed) s_layout_requested = 1;
            continue;
        }

        if (key == HID_KEY_F3) {
            if (bPressed) s_joystick_requested = 1;
            continue;
        }

        // Layouts: type the character printed on the key
        if (pLayout != 0 && layout_is_char_key(key)) {
            unsigned short ace;
            if (bSym) {
                // Symbol Shift + the key as labelled (matrix position only)
                ace = ace_from_char(layout_char(pLayout, key, 0, 0)) & ~(SH | SY);
            } else {
                ace = ace_from_char(layout_char(pLayout, key, bShift, bAltGr));
            }
            if (ace == NONE) continue;
            bKeyDown = 1;
            press(ports, ace);
            continue;
        }

        const TKeyMapEntry *pEntry = lookup(key);
        if (pEntry == 0) continue;
        bKeyDown = 1;

        unsigned short ace;
        if (bSym) {
            ace = pEntry->plain & ~(SH | SY);   // matrix position only
            if (!(ace & 0x80)) ace = pEntry->shifted & ~(SH | SY);
        } else if (bShift) {
            ace = pEntry->shifted != NONE ? pEntry->shifted : (pEntry->plain | SH);
        } else {
            ace = pEntry->plain;
        }

        press(ports, ace);
    }

    // Shift on its own (or with an unmapped key) still reaches the Ace
    if (bShift && !bKeyDown) {
        press(ports, SH);
    }

    memcpy(s_prev_keys, rawKeys, sizeof(s_prev_keys));
    s_held_key = held;

    // Publish the whole matrix at once so the Z80 never sees a half-built state
    for (int i = 0; i < 8; i++) {
        s_keyboard_ports[i] = ports[i];
    }
}

// USB joystick (F3): each direction and button presses the Ace key assigned to it.
// Controls 0-3 are up, down, left and right, 4 onwards the buttons (bit 0 onwards).
// By default the directions are the cursor keys (in the ROM Shift+6 moves up, Shift+7
// down) and the buttons press nothing
static volatile unsigned short s_joystick_keys[JOYSTICK_CONTROLS] = {
    SH|N6, SH|N7, SH|N5, SH|N8
};
static volatile unsigned s_joystick_directions = 0;     // JOYSTICK_ bits
static volatile unsigned s_joystick_buttons = 0;

static void joystick_publish(void)
{
    unsigned nDirections = s_joystick_directions;
    unsigned nButtons = s_joystick_buttons;

    unsigned char ports[8];
    memset(ports, 0xFF, sizeof(ports));
    for (int i = 0; i < JOYSTICK_BUTTON0; i++) {
        if (nDirections & (1u << i)) {
            press(ports, s_joystick_keys[i]);
        }
    }
    for (int i = 0; i < 32; i++) {
        if (nButtons & (1u << i)) {
            press(ports, s_joystick_keys[JOYSTICK_BUTTON0 + i]);
        }
    }
    for (int i = 0; i < 8; i++) {
        s_joystick_ports[i] = ports[i];
    }
}

void keyboard_joystick_update(unsigned nDirections, unsigned nButtons)
{
    s_joystick_directions = nDirections;
    s_joystick_buttons = nButtons;
    joystick_publish();
}

unsigned keyboard_joystick_directions(void)
{
    return s_joystick_directions;
}

unsigned keyboard_joystick_buttons(void)
{
    return s_joystick_buttons;
}

int keyboard_check_joystick(void)
{
    int joystick = s_joystick_requested;
    s_joystick_requested = 0;
    return joystick;
}

int keyboard_joystick_get_key(int nControl)
{
    return nControl >= 0 && nControl < JOYSTICK_CONTROLS ? s_joystick_keys[nControl] : NONE;
}

void keyboard_joystick_set_key(int nControl, int nAceKey)
{
    if (nControl >= 0 && nControl < JOYSTICK_CONTROLS) {
        s_joystick_keys[nControl] = (unsigned short) nAceKey;
        joystick_publish();
    }
}

// The Ace key a host key press stands for, as keyboard_update() maps it (selected
// layout, Shift, Ctrl / Alt as Symbol Shift); NONE for the host's own keys
int keyboard_ace_key_from_hid(unsigned char key, unsigned char ucModifiers)
{
    int nLayout = s_layout;
    const TKeyboardLayout *pLayout =
        nLayout != LayoutOriginalAce ? &g_KeyboardLayouts[nLayout] : 0;
    int bHasAltGr = pLayout != 0 && pLayout->bAltGr;
    int bShift = (ucModifiers & 0x22) != 0;
    int bAltGr = bHasAltGr && (ucModifiers & 0x40) != 0;
    int bSym   = (ucModifiers & (bHasAltGr ? 0x15 : 0x55)) != 0;
    unsigned short sym = bSym ? (SY | (bShift ? SH : 0)) : 0;

    if (pLayout != 0 && layout_is_char_key(key)) {
        if (bSym) {
            unsigned short ace = ace_from_char(layout_char(pLayout, key, 0, 0)) & ~(SH | SY);
            return ace != NONE ? (ace | sym) : NONE;
        }
        return ace_from_char(layout_char(pLayout, key, bShift, bAltGr));
    }

    const TKeyMapEntry *pEntry = lookup(key);
    if (pEntry == 0) {
        return NONE;
    }
    if (bSym) {
        unsigned short ace = pEntry->plain & ~(SH | SY);
        if (!(ace & 0x80)) ace = pEntry->shifted & ~(SH | SY);
        return (ace & 0x80) ? (ace | sym) : NONE;
    }
    if (bShift) {
        return pEntry->shifted != NONE ? pEntry->shifted : (pEntry->plain | SH);
    }
    return pEntry->plain;
}

// e.g. "Q", "Shift+6", "Sym+P", "Space"; "-" for none
void keyboard_ace_key_name(int nAceKey, char *pName)
{
    static const char *s_Names[8][5] = {
        {"Shift", "Sym", "Z", "X", "C"},
        {"A", "S", "D", "F", "G"},
        {"Q", "W", "E", "R", "T"},
        {"1", "2", "3", "4", "5"},
        {"0", "9", "8", "7", "6"},
        {"P", "O", "I", "U", "Y"},
        {"Enter", "L", "K", "J", "H"},
        {"Space", "M", "N", "B", "V"},
    };

    pName[0] = '\0';
    if (nAceKey == NONE) {
        strcpy(pName, "-");
        return;
    }
    if (nAceKey & SH) {
        strcat(pName, (nAceKey & (SY | 0x80)) ? "Shift+" : "Shift");
    }
    if (nAceKey & SY) {
        strcat(pName, (nAceKey & 0x80) ? "Sym+" : "Sym");
    }
    if (nAceKey & 0x80) {
        strcat(pName, s_Names[(nAceKey >> 3) & 7][nAceKey & 7]);
    }
}
