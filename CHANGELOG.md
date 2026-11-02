## 0.7 (2nd November 2026)

 * F7 can also save the whole Ace as an .ace snapshot (memory and CPU registers, at any
   time): F7 now asks whether to save the dictionary or a snapshot.
 * Loading an .ace snapshot of a 51K Ace said it was not a valid snapshot (the end marker
   after FFFFh was not read).
 * SAVE / BSAVE also write .tzx tapes: they add the file to the tape in the player when it
   is a .tzx too (standard speed data blocks), and F5 N makes a new .tzx when the name ends
   in .tzx. With no tape in the player they still write name.tap.
 * LOAD / BLOAD / VERIFY only read the tape in the player, like a cassette: with no tape
   inserted they give ERROR 10 (they used to search every tape on the SD card, and the tape
   found became the current one, so a later SAVE was added to it), and a file that is not
   on the tape gives ERROR 10 (instead of a stand-in file). The files passed while
   searching are shown, as on a real Ace.
 * LOAD / BLOAD without a name list the files on the tape in the player and load nothing.
 * Tape blocks are read as the ROM's own routine (18A7h) reads them: the next block, header
   or data as asked (the .tzx flag byte is used), with its length and checksum checked, so
   damaged tapes give ERROR 10 as on a real Ace, and programs that load their own blocks
   through the ROM routine work.
 * Shift + 1 / 2 / 4 / 9 / 0 are the Ace's Delete Line / Caps Lock / Inverse Video /
   Graphics / Delete, as on the Ace (Original ACE keymap; `! @ $ ( )` are then Ctrl + 1 /
   2 / 4 / 8 / 9, as on the Ace). Shift + 3 still types #.
 * F2 is Caps Lock (Shift + 2), like F1 / F4 / F9 for the other Ace editing keys.
 * The Ace's files (.tap, .tzx, .ace, .fth, .fs, .f) are now kept in a "programs" folder
   on the SD card, in any subfolders, leaving the root to the boot files. It is made at
   start-up, and Ace files in the root of the card are moved into it.
 * F5 browses the folders (Enter opens one, Backspace goes up, M makes one); new files
   (SAVE with no tape, F5 N, F7) go into the folder last open in F5. F6 also deletes
   folders, with everything in them, after confirming.
 * Long names in the F5 and F6 lists, and long lines in the Ctrl+TAB list, scroll when
   selected (after a second, one character every 150 ms, a second's wait at the end);
   Space scrolls them one character by hand. Long folder paths in the F5 / F6 title bar
   and on the F7 save screens scroll the same way, towards their start.
 * Ctrl+F12 offers 99K: Edwin Blink's 96K RAM pack, two 48K pages at 4000h - FFFFh
   selected by bit 7 of the Z80's R register (LD R,A), latched at the next refresh as on
   the pack. F7 snapshots keep both pages in the one .ace file: the other page follows the
   end marker, where other emulators stop reading. Loading such a snapshot chooses the 96K
   pack.
 * F5 opens .zip files like folders (read only): their tapes, snapshots and Forth sources
   are inserted, loaded or spooled straight from the .zip (stored or deflate, up to 4 MB
   unpacked; puff.c by Mark Adler does the unpacking). SAVE with a tape from a .zip in
   the player writes a new .tap next to the .zip. A .zip inside the .zip opens like a
   folder too (unpacked into memory, up to 4 MB), as in collections that keep each
   program in its own .zip. The web page handles .zip files as files.
 * System update from the F11 web page (/system): upload a release .zip, confirm with Y on
   the Ace, and only the changed files (kernel, boot and Wi-Fi firmware) are replaced. A
   Pi 4 / 400 tries the new kernel once first (tryboot) and keeps the old one if it does
   not start; other models are switched off and on by hand. The kernel carries its
   version (src/version.h), shown on the page and in the log.
 * build.sh also zips sdcard/ into dist/xAce-baremetal-<version>-pi<model>.zip (the
   version from src/version.h). release.sh is merged into it: build.sh --all builds every
   model, build.sh --all --publish also creates the GitHub release.
 * Pi 400: its LED (the power light) stays on while the Ace runs, as in Raspberry Pi OS,
   and goes out at power off (F10). Other models still show SD card access on the
   activity LED.
 * F11 opens a file exchange web page: the Pi joins the cable network or a Wi-Fi network
   (DHCP) and serves a page (at its address or http://xace.local) for uploading several
   files or whole folders at once, downloading files or folders (as a .zip), and making,
   renaming, moving and deleting files and folders in the programs folder. The Ace is
   paused while F11 is open. The Wi-Fi network name, password and country are typed on
   the F11 screen and kept in wpa_supplicant.conf; the SD card gets the Wi-Fi firmware
   in /firmware.

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
