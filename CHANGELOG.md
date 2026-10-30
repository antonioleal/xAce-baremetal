## 0.6 (30th October 2026)

 * Adapted from the original xAce 0.5 code by Edward Patel (see the entries below).
 * Switch codebase to run baremetal on a Raspberry Pi 4 / 400.
 * Display white on black like the real Ace; redraw cells whose character is redefined.
 * Translate PC keyboard symbols to Ace Symbol Shift keys (Forth `:` `(` `*` etc.).
 * Service screen, keyboard and sound every frame, even with Z80 interrupts disabled.
 * Correct 3.25 MHz timing (64,896 t-states / frame) and don't race after a stall.
 * Fix Z80 instruction timings inherited from xz80: LD C/E/L/A,n (4 -> 7 t-states),
   RES / SET b,(HL) and (IX+d) / (IY+d) (3 too few), RLD (9 -> 18).
 * HALT waits for the next interrupt instead of doing nothing, and accepting an interrupt
   disables interrupts and takes 13 T-states, as on the Z80. The interrupt is the vsync
   signal, held for 8 lines as in the MiSTer core (level-triggered, not latched): VLIST now takes one frame per
   word, like the real Ace (144 frames, as on EightyOne and the MiSTer core).
 * Video waits: CPU accesses to the screen at 2400h and the character set at 2C00h wait
   while the picture is drawn, as on the real Ace (picture and interrupt timing as in the
   MiSTer core, so that VLIST takes the same number of frames every time).
 * Timing checked against the MiSTer core with Forth loops that count iterations for 50 or
   500 frames, in SLOW and FAST mode, reading normal RAM or screen memory: the counts are
   the same on both (e.g. 392 / 722 reading the screen, 5299 / 9153 over 500 frames).
 * Tape LOAD / SAVE using .tap files on the SD card.
 * VERIFY / BVERIFY compare the tape with memory (ERROR 10 if they differ): the tape trap
   loaded the block instead, so BVERIFY overwrote memory and always said OK.
 * Beeper sound on the headphone jack or HDMI.
 * Emulates an Ace with a 48 KB RAM pack: 51 KB RAM, 49 KB for programs (unchanged
   from 0.5; memory map now documented in README.md).
 * Ctrl+F12 chooses the RAM size: 3K, 19K, 35K or 51K (default), restarting the Ace.
 * LOAD / BLOAD also read .tzx tapes (TZX data blocks 10h, 11h, 14h and 19h).
 * F5 lists the .tap, .tzx and .ace files on the SD card and shows the tape in the player.
   Enter on a tape inserts it (LOAD then reads from it, LOAD without a name takes its next
   file, SAVE adds files to a .tap); E ejects it; N makes a new blank tape.
   Enter on an .ace snapshot loads it
   (memory and CPU registers, Ace32 / EightyOne format).
 * F6 deletes a .tap / .tzx tape, an .ace snapshot or a Forth source from the SD card: chosen
   from a list like F5's, with confirmation.
 * Shift + 5 / 6 / 7 / 8 move the cursor, as on the Ace (Original ACE keymap; % ^ & * are
   then Ctrl + 5 / H / 6 / B); the arrow keys still move it too.
 * The Up and Down arrow keys were swapped: in the ROM Shift+6 moves the cursor up a row
   and Shift+7 down.
 * Remove the F3 shortcut: Shift+3 has no True Video function in the Ace ROM (Shift+4,
   F4, toggles inverse video on and off).
 * TAB retypes the last line entered, TAB again the one before (up to 100 lines, stopping
   at the oldest); Shift+TAB goes back to newer lines. Ctrl+TAB lists the lines to choose one.
 * F7 saves the dictionary as a Forth source file (.fth, .fs or .f): colon / DEFINER / COMPILER words as
   LIST shows them, variables and constants with their values, other words as bytes; hidden
   and REDEFINEd words included. Spooling the file into a fresh Ace defines the same words.
 * Spooling, as in the original xAce: a Forth source file (.fth, .fs, .f) is typed into the Ace line by
   line (F5 Enter), at full speed. Comments outside definitions are left out (they would give
   ERROR 4); comments inside definitions are kept. It goes on after ERROR n and stops at a
   line the Ace rejects or with Esc.
 * Y / N answers (F5, F6, F10) follow the F8 keyboard layout.
 * Up / Down / Page Up / Page Down repeat when held down in the F5, F6 and F8 lists.
 * F8 chooses the keyboard layout: Original ACE (default), UK, US, German, French, Italian,
   Spanish, Portuguese, Norwegian, Swedish, Danish or Japanese.
 * USB joystick or gamepad: each direction and button presses an Ace key. F3 assigns them:
   move the joystick or press a button, then the key (directions default to the cursor keys).
 * Ctrl+F4 swaps the screen colours between white on black and black on white.
 * F10, or the keyboard's power key (Fn+F10 on the Pi 400), powers off the Raspberry Pi
   (with confirmation; unmounts the SD card first).
 * Circle is included as a copy of Step51.1.1 (circle/README.xAce gives the commit) instead
   of a git submodule, so the project and its source archives build on their own.
 * Add build.sh to build the Circle libraries and the kernel and update sdcard/ in one step.
 * build.sh takes the Raspberry Pi model (1, 2, 3, 4 / 400; default 4).
 * build.sh fills sdcard/ with the model's boot firmware from firmware/pi1 - pi4.
 * release.sh makes one SD card zip per model (xAce-baremetal-<version>-piN.zip) and can
   publish them as a GitHub release.

## 0.5 (22nd December 2012)

 * Switch build method from IMake to CMake.
 * Support attaching of tape files.
 * Add file spooling.
 * Create proper checksum when saving files.

## 0.4.1 (29th June 2010)

 * Adds support for 32bpp displays.
 * Frees more allocated memory when finished with.
 * Corrects assignment of strings to variables with no allocated storage.
 * Adds more file I/O error detection.
 * Improves portability.
 * Switched to just using .tap instead of .dic and .byt when loading and saving files.
 * Changes keybindings.

## 0.4 (1997)

 * Written by Edward Patel and based on a ZX81 emulator called xz81 written by Russell Marks, which was in turn based on Ian Collier's xz80, a ZX Spectrum emulator for X.
