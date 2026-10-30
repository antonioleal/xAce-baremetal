#ifndef _KEYBOARD_CIRCLE_H
#define _KEYBOARD_CIRCLE_H

#ifdef __cplusplus
extern "C" {
#endif

void keyboard_init(void);
void keyboard_clear(void);
unsigned char keyboard_get_keyport(int port);
void keyboard_update(unsigned char ucModifiers, const unsigned char rawKeys[6]);
int keyboard_check_reset(void);
int keyboard_check_invert(void);    // Ctrl+F4 pressed since the last call
int keyboard_check_list(void);      // F5 pressed since the last call
int keyboard_check_delete(void);    // F6 pressed since the last call
int keyboard_check_poweroff(void);  // F10 or the power key (Fn+F10) pressed since the last call
int keyboard_check_layout(void);    // F8 pressed since the last call
int keyboard_check_ram(void);       // Ctrl+F12 pressed since the last call
int keyboard_check_save(void);      // F7 pressed since the last call

// Line history (line_history.cpp), since the last call: TAB (1, an older line) or
// Shift+TAB (-1, a newer one); Enter pressed; any other key pressed
int keyboard_check_history(void);
int keyboard_check_history_list(void);     // Ctrl+TAB
int keyboard_check_enter(void);
int keyboard_check_other_key(void);

// Spooling (spool_ace.cpp): host keys off except Esc; the spooler types on the Ace
void keyboard_set_spooling(int bOn);
int keyboard_check_spool_abort(void);   // Esc pressed while spooling
int keyboard_spool_key(unsigned char c);    // press c ('\n' = Enter, 1-2 = SPOOL_KEY_) alone; 0 if no Ace key

// Host keyboard layouts (F8); layout 0 is Original ACE, the default
int keyboard_layout_count(void);
const char *keyboard_layout_name(int nLayout);
int keyboard_get_layout(void);
void keyboard_set_layout(int nLayout);
unsigned char keyboard_held_key(void);  // a USB HID key currently held, 0 if none

// USB joystick (F3): each control presses the Ace key assigned to it. Controls 0-3 are
// the directions (JOYSTICK_ bits), 4 onwards the buttons; Ace keys as returned by
// keyboard_ace_key_from_hid(), 0 for none
#define JOYSTICK_UP         0x01
#define JOYSTICK_DOWN       0x02
#define JOYSTICK_LEFT       0x04
#define JOYSTICK_RIGHT      0x08
#define JOYSTICK_BUTTON0    4       // control of button bit 0
#define JOYSTICK_CONTROLS   (JOYSTICK_BUTTON0 + 32)
void keyboard_joystick_update(unsigned nDirections, unsigned nButtons);
unsigned keyboard_joystick_directions(void);
unsigned keyboard_joystick_buttons(void);
int keyboard_check_joystick(void);  // F3 pressed since the last call
int keyboard_joystick_get_key(int nControl);
void keyboard_joystick_set_key(int nControl, int nAceKey);
int keyboard_ace_key_from_hid(unsigned char key, unsigned char ucModifiers);
void keyboard_ace_key_name(int nAceKey, char *pName);  // pName: at least 16 characters

// Key presses queued from the USB interrupt, for host-side text entry
void keyboard_flush_events(void);
int keyboard_get_event(unsigned char *pKey, unsigned char *pModifiers);
char keyboard_hid_to_ascii(unsigned char key, unsigned char ucModifiers);  // selected layout, 0 if none

#ifdef __cplusplus
}
#endif

#endif
