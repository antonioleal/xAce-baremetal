# xAce Baremetal for the Raspberry Pi 4 / 400 ...
*(Using the Brilliant Circle Framework)*

This project consists of the [xAce](https://lawrencewoodman.github.io/xAce) Jupiter Ace emulator ported to run baremetal (no operating system) on the **Raspberry Pi models 4 / 400 (default), 3, 2 and 1**, built on the original code and the [Circle](https://github.com/rsta2/circle) C++ library.

Please visit the [Jupiter ACE Archive](https://www.jupiter-ace.co.uk/index.html) to obtain software for this emulator and give your contribution if you find it useful.

Ready to use Zip files with the binaries are provided, so you can directly download them to your PI's formatted sdcard. See the **SD Card Preparation** paragraph below.

---

## Source Files

All in `src/`:

| File | Purpose |
| :--- | :--- |
| `z80.c`, `z80ops.c`, `cbops.c`, `edops.c`, `z80.h` | Z80 CPU core (from xz80 / xz81 / xAce). The `*ops.c` files are included by `z80.c`, not compiled separately. |
| `tape.h` | Declarations of the tape traps called by the Z80 core |
| `main.cpp`, `kernel.cpp`, `kernel.h` | Circle kernel: devices (keyboard, USB joystick), frame timing, screen rendering, F3 / F5 / F6 / F7 / F8 / F10 / F11 screens |
| `keyboard_circle.cpp`, `keyboard_circle.h` | USB keyboard and joystick to Jupiter Ace keyboard matrix |
| `keyboard_layouts.cpp`, `keyboard_layouts.h` | Keyboard layouts for F8 (UK, US, German, French, …) |
| `ram_ace.cpp`, `ram_ace.h` | RAM pack size (Ctrl+F12), and the 96K pack's two pages (switched by the Z80 core on `LD R,A`) |
| `spool_ace.cpp`, `spool_ace.h` | Spooling Forth source files (`.fth`, `.fs`, `.f`) into the Ace (F5) |
| `save_dictionary.cpp`, `save_dictionary.h` | Saving the dictionary as a Forth source file (F7) |
| `line_history.cpp`, `line_history.h` | Line history: TAB / Shift+TAB retype earlier lines, Ctrl+TAB lists them |
| `sound_ace.cpp`, `sound_ace.h` | Beeper sound |
| `web_files.cpp`, `web_files.h` | File exchange web page (F11): cable or Wi-Fi network, web server, the page itself |
| `wifi_settings.cpp`, `wifi_settings.h` | The Wi-Fi network to join (F11), kept in `wpa_supplicant.conf` on the SD card |
| `update_sd.cpp`, `update_sd.h` | System update from the web page (F11, `/system`): checking a release `.zip`, installing it, trying a new kernel once with the Pi 4's tryboot; the kernel's version marker |
| `version.h` | The version number, built into the kernel and used by `build.sh` for the zips and releases |
| `zip_sd.cpp`, `zip_sd.h` | Reading `.zip` files on the SD card (F5 opens them like folders) |
| `puff.c`, `puff.h` | Unpacking deflate data (`.zip` files): *puff* 2.3 by Mark Adler, from zlib's `contrib/puff` (zlib 1.3.1), zlib licence, unchanged |
| `tape_sd.cpp`, `tape_sd.h` | Tape LOAD / SAVE with `.tap` and `.tzx` files on the SD card; loading and saving (F7) `.ace` snapshots; the `programs` folder and its subfolders (paths, listing, making, moving and deleting folders) |
| `ace_rom.h` | The 8 KB Jupiter Ace ROM (`ace.rom` in the project root) as a C array |
| `config.txt` | Raspberry Pi boot configuration |
| `Makefile` | Build rules (uses Circle's `Rules.mk`) |

---

## Architecture Overview

* **Target Hardware**: Raspberry Pi 4 Model B or Raspberry Pi 400 (Broadcom BCM2711,
  Cortex-A72), 32-bit mode. Both run the same kernel, and it has been tested on both.
* **Display**: 640×480 @ 60 Hz HDMI framebuffer.
  * Jupiter Ace resolution (256×192) scaled 2× to 512×384, centred.
  * White characters on a black screen and border, like the real Ace.
  * Only changed 8×8 character cells are redrawn, including cells whose character was
    redefined in the character set RAM (`0x2C00`).
* **Input**: Standard USB keyboard plugged into any USB port, or the Pi 400's built-in
  keyboard (which is connected over USB inside the case).
  * Uses Circle's USB host controller driver (`CUSBHCIDevice`, xHCI on the Pi 4 / 400) and
    `CUSBKeyboardDevice` in raw mode.
  * US layout by default ("Original ACE"): symbols are typed as on the PC keyboard and
    translated to the Ace Symbol Shift combinations (see below). F8 selects another
    layout (see *Keyboard layouts*).
  * The keyboard can be unplugged and plugged in again while running.
  * A USB joystick or gamepad (`CUSBGamePadDevice`) presses the Ace keys assigned to its
    directions and buttons with F3 (see *USB joystick*).
* **CPU Timing**:
  * Z80 at 3.25 MHz: 64,896 t-states per frame (312 lines × 208 t-states), 50.08 Hz.
  * Instruction timings match the Zilog figures (all unprefixed and `CB` instructions,
    the documented `ED` ones and the main `DD` / `FD` ones are checked). `HALT` waits for
    the next interrupt, as on the real Z80: the ROM's `VLIST` uses it to print one word per
    frame (144 frames for the ROM's own words, as on EightyOne and the MiSTer Ace core).
  * Video waits as on the real Ace: the copies of the screen (`0x2400`) and character set
    (`0x2C00`) that the ROM uses give the video circuit priority, so a CPU access while
    the picture is drawn waits until the end of that line's picture. The copies at
    `0x2000` and `0x2800` give the CPU priority. The picture timing is the MiSTer Ace
    core's: the first 128 t-states of lines 0-191 of 312, with the interrupt (vsync) at
    t-state 154 of line 248. Forth text output is a little slower for it, tight loops
    writing to the screen up to about 30%.
  * The interrupt is the vsync signal, as in the MiSTer core: it is held for 8 lines
    (1,664 t-states), and can only be taken then. A program that keeps interrupts
    disabled through it misses that frame's interrupt; a handler that re-enables them
    while it is still held is entered again (the ROM's handler is longer, so the Ace
    gets one interrupt per frame).
  * Checked against the MiSTer Ace core with Forth loops that count how many times they
    run in 50 or 500 frames. After a fresh boot, both give the same counts:

    ```
    0 variable a
    : tst a ! 0 15403 @ 50 + begin swap 1+ swap a @ c@ drop dup 15403 @ - 0< until drop . ;
    : t2 0 15403 @ 50 + begin swap 1+ swap dup 15403 @ - 0< until drop . ;
    : t3 0 15403 @ 500 + begin swap 1+ swap dup 15403 @ - 0< until drop . ;
    ```

    | Test | SLOW (after boot) | FAST |
    | :--- | ---: | ---: |
    | `9216 tst` (reads the screen at `0x2400`) | 392 | 722 |
    | `16384 tst` (reads RAM at `0x4000`) | 400 | 724 |
    | `t2` | 539 | 931 |
    | `t3` (500 frames) | 5299 | 9153 |

    The emulator still treats each memory access as happening at the end of its instruction,
    rather than at its own t-state as on the real Z80. Only machine code that times screen
    accesses to the exact t-state could show a difference.
  * Paced with the ARM microsecond timer (`CTimer`). If the emulator falls more than a
    frame behind (SD card access, USB enumeration) it resynchronises instead of racing.
  * Screen, keyboard and sound are serviced every frame, even while the Z80 has
    interrupts disabled. The 50 Hz interrupt (`RST 38h`) is taken when they are enabled.
* **Sound**: The Ace beeper (IN from port `0xFE` = speaker low, OUT = speaker high) is
  timestamped in t-states, resampled to 48 kHz and played on the headphone jack (default)
  or HDMI. The Pi 400 has no headphone jack, so it needs HDMI sound.
* **Tape**: `LOAD` / `BLOAD` / `SAVE` / `BSAVE` use `.tap` or `.tzx` files on the SD card (see below).
* **ROM**: The Jupiter Ace ROM is compiled into the kernel (`ace_rom.h`), so the Ace starts
  without reading any file. Its tape block read (`0x18A7`) and write (`0x1820`) routines are
  patched at boot with emulator traps that do the same with tape files; the rest of `LOAD`,
  `SAVE`, `VERIFY` (headers, names, checks) is the ROM's own.
* **Memory**: The emulated Ace has the standard 3 KB of built-in RAM plus, by default, a
  48 KB RAM pack: **51 KB of RAM** in total (**49 KB for programs**: the 1 KB built in plus
  the 48 KB pack). **Ctrl+F12** chooses the typical sizes of the time instead:

  | RAM | RAM pack | For programs | RAMTOP |
  | :--- | :--- | :--- | :--- |
  | 3K | none (a stock Ace) | 1K | `0x4000` |
  | 19K | 16K | 17K | `0x8000` |
  | 35K | 32K | 33K | `0xC000` |
  | 51K (default) | 48K | 49K | `0x0000` (top of memory) |
  | 99K | 96K (two 48K pages) | 49K at a time | `0x0000` (top of memory) |

  Missing RAM ignores writes and reads `0xFF`, so the ROM's RAM test at power-on finds the
  chosen size, as on the real machine. RAM is filled with `0xFF` at power-on and on F12 reset.

  **The 96K RAM pack** is Edwin Blink's design ([jupiter-ace.co.uk](https://jupiter-ace.co.uk/hardware_96kram_pack.html)):
  two 48K pages, both at `0x4000 – 0xFFFF`; the Ace's own 3K is never paged. It starts on
  page 0 and then works as a 48K pack (the ROM finds 51K). Bit 7 of the Z80's R register
  selects the page: the pack latches it in every refresh cycle, and only `LD R,A` changes
  it, so `LD A,128 : LD R,A` gives page 1 and `XOR A : LD R,A` page 0. As on the pack, the
  new page takes over at the refresh after the next opcode fetch (that opcode still comes
  from the old page) or at an interrupt acknowledge, so code that switches pages should
  run below `0x4000`. A reset (R = 0) goes back to page 0. With the other sizes `LD R,A`
  does nothing to the memory.

  | Address | Size | Contents |
  | :--- | :--- | :--- |
  | `0x0000 – 0x1FFF` | 8 KB | ROM |
  | `0x2000 – 0x27FF` | 1 KB | Video RAM (32×24 characters), mirrored twice |
  | `0x2800 – 0x2FFF` | 1 KB | Character set RAM (128 characters × 8 bytes), mirrored twice |
  | `0x3000 – 0x3FFF` | 1 KB | Built-in program RAM, mirrored four times |
  | `0x4000 – 0xFFFF` | up to 48 KB | RAM pack (16K: to `0x7FFF`, 32K: to `0xBFFF`; 96K: one of its two pages) |

---

## Key Mappings

| Standard USB Keyboard | Jupiter Ace Action |
| :--- | :--- |
| **A – Z** | A – Z (Shift gives the other case) |
| **0 – 9**, keypad digits | 0 – 9 |
| **Symbols** (`# - _ = + [ ] { } ; : ' " , . < > / ? \ \| ~`) | The same symbol (Symbol Shift + its key) |
| **Shift + 1 / 2 / 4 / 9 / 0** | Delete Line / Caps Lock / Inverse Video / Graphics / Delete, as on the Ace (Original ACE keymap; the F8 layouts type their symbols) |
| **Shift + 5 / 6 / 7 / 8** | Move the cursor left / up / down / right, as on the Ace (Original ACE keymap; the F8 layouts type their symbols) |
| `! @ $ % & ( ) ^ *` | **Ctrl + 1 / 2 / 4 / 5 / 6 / 8 / 9 / H / B**, as on the Ace (Original ACE keymap, where Shift + digits are the Ace's editing keys); keypad `*` |
| Keypad `/ * - + .` | The same symbol |
| **Enter** / keypad Enter | ENTER |
| **Space** | SPACE |
| **Tab** / **Shift + Tab** | Retype an earlier line / a newer one (see *Line history*) |
| **Ctrl + Tab** | List the remembered lines and choose one to retype (pauses the Ace) |
| **Backspace** / **Delete** | Delete (`Shift + 0`) |
| **Escape** | Break (`Shift + Space`) |
| **Left / Right / Up / Down** | Move the cursor too, in every keymap (`Shift + 5 / 8 / 6 / 7`: in the ROM `Shift + 6` moves up a row, `Shift + 7` down) |
| **Caps Lock** | Caps Lock (`Shift + 2`) |
| **Ctrl / Alt** + key | Ace Symbol Shift + that key position (reaches every legend, e.g. © and £) |
| **F1** | Delete Line (`Shift + 1`) |
| **F2** | Caps Lock (`Shift + 2`) |
| **F3** | Assign Ace keys to the USB joystick's directions and buttons (pauses the Ace; see *USB joystick*) |
| **F4** | Inverse Video on / off (`Shift + 4`) |
| **Ctrl + F4** | Swap the screen colours: black on white / white on black |
| **F5** | Browse the `programs` folder of the SD card: its folders, tapes (`.tap`, `.tzx`), snapshots (`.ace`) and Forth sources (`.fth`, `.fs`, `.f`), and the tape in the player; Enter opens a folder (Backspace goes up), inserts a tape, loads a snapshot or spools a source, E ejects the tape, N makes a new tape, M a new folder (pauses the Ace) |
| **F6** | Delete a tape, a snapshot, a Forth source, or a folder with everything in it: choose it in the list, Enter (D for a folder), then Y (pauses the Ace) |
| **F7** | Save the dictionary as a Forth source file (`.fth`, `.fs`, `.f`), or the whole Ace as an `.ace` snapshot: choose which, then type the name (see *Saving the dictionary* and *Snapshots*; pauses the Ace) |
| **F8** | Choose the keyboard layout (pauses the Ace; see *Keyboard layouts*) |
| **F9** | Graphics Mode (`Shift + 9`) |
| **F10**, power key (**Fn + F10** on the Pi 400) | Power off the Raspberry Pi (pauses the Ace, then Y/N) |
| **F11** | File exchange: join the cable or Wi-Fi network and serve a web page for copying files to and from the SD card (pauses the Ace; see *File exchange*) |
| **F12** | Hard Reset |
| **Ctrl + F12** | Choose the RAM size: 3K, 19K, 35K, 51K or 99K (96K pack); changing these restarts the Ace |

### Shift and Symbol Shift

The Ace has two shift keys. On the PC keyboard:

| Jupiter Ace | PC / Pi 400 keyboard |
| :--- | :--- |
| **SHIFT** | **Shift** (left or right) |
| **SYMBOL SHIFT** | **Ctrl** (left or right) or **Alt**; in the layouts with AltGr (German, French, Italian, Spanish, Portuguese, Norwegian, Swedish, Danish) only **Left Alt**, as Right Alt is AltGr there |

* **Ctrl / Alt + key** is the Ace's Symbol Shift + the key in the same position, so every
  legend on the Ace keyboard can be reached: `Ctrl + X` = `£`, `Ctrl + I` = `©`,
  `Ctrl + A` = `~`, `Ctrl + B` = `*`, `Ctrl + 5` = `%`.
* **Symbols** can also be typed as on a PC (Shift + the key with the symbol): they are
  translated to the Ace's Symbol Shift combination.
* **Shift + digits**: on the Ace they are editing keys, and so they are in the Original
  ACE keymap: **Shift + 1** Delete Line, **Shift + 2** Caps Lock, **Shift + 4** Inverse
  Video, **Shift + 5 / 6 / 7 / 8** move the cursor (the ROM moves up a row on Shift + 6 and
  down on Shift + 7), **Shift + 9** Graphics and **Shift + 0** Delete. Shift + 3 has no
  function in the ROM, so it types `#`. The symbols on the digits (`! @ $ % & ( )`) are
  typed the Ace way, with Ctrl (or the keypad `*`). The F8 layouts type the PC symbols
  there; the editing functions are also on separate keys in every keymap: Delete Line F1,
  Caps Lock F2 (or Caps Lock), Inverse Video F4, Graphics F9, Delete Backspace, and the arrow keys.

### Ace editing keys (F1, F2, F4, F9)

These function keys are shortcuts for Jupiter Ace key combinations, so you don't need to
remember the Shift + digit legends:

* **F1 – Delete Line** (`Shift + 1`): clears the line being typed.
* **F2 – Caps Lock** (`Shift + 2`): toggles capitals, like the PC's Caps Lock key.
* **F4 – Inverse Video** (`Shift + 4`): switches inverse video on, and pressed again off:
  characters typed while it is on are stored with their top bit set and show inverted
  (black on white), e.g. to highlight a word inside `." HELLO"`.
* **F9 – Graphics** (`Shift + 9`): toggles graphics mode for typing the Ace's block graphics characters.

F4 only affects the characters typed next; text already on the screen does not change.

**Ctrl + F4** is different: it is not an Ace key. It swaps the colours of the whole display (screen
and border) between white on black, like the real Ace, and black on white. Inverse video characters
swap too, so they still stand out. Press Ctrl + F4 again to go back.

### Line history (TAB)

The last 100 lines entered (with Enter, at the Forth prompt or wherever a program asks for
input) are remembered. **TAB** puts the last one back on the input line, in place of what
is being typed; each further TAB goes one line further back, stopping at the oldest.
**Shift + TAB** goes back to newer lines, and past the newest to an empty line. Any other
key ends the browsing, so the next TAB starts again from the last line. The line is only
retyped, not entered: edit it if you like and press Enter.

**Ctrl + TAB** shows all the remembered lines, newest first, one per row (a line longer than
the screen ends with `>`; the selected one scrolls to show the rest, as long names do in
F5). Move with **Up / Down** (**Page Up / Page Down**, **Home / End**;
held down, they repeat) and press **Enter** to put the chosen line back on the input line;
**Esc** closes the list. TAB and Shift + TAB then go on from the chosen line.

* A line is not remembered twice in a row; empty lines and spooled lines are not
  remembered. The history lasts until the Pi is switched off (F12 keeps it).
* Inverse video characters and `£` / `©` are retyped as they were; graphics characters
  are left out.
  
 This was not part of the original Ace, it was done because I'm a lazy typer.  If you dislike it just don't use it :-)

### Keyboard layouts (F8)

Press **F8** to choose the layout of your keyboard, e.g. the national layout of a
Raspberry Pi 400. Use **Up / Down** (held down, they repeat) and **Enter** (**Esc** keeps the
current one; `*` marks it). The choice lasts until the Pi is switched off.

| Layout | Notes |
| :--- | :--- |
| **Original ACE** (default) | The mapping in the table above: a US keyboard, with Right Alt as Symbol Shift |
| UK, US, Japanese | Right Alt is Symbol Shift, like Ctrl / Left Alt. Japanese is JIS: `¥` types `\` |
| German, French, Italian, Spanish, Portuguese, Norwegian, Swedish, Danish | **Right Alt is AltGr**, for the symbols printed on the keys' third position (e.g. `AltGr + 2` = `@` in Portuguese) |

With a layout chosen, each key types the character printed on it. Letters follow the layout
too (QWERTZ in German, AZERTY in French). For example in Portuguese, `Shift + 3` types `#`,
`AltGr + 3` types `£`, the `+` key types `+` and the `~` key types `~`.

* Characters the Ace does not have type nothing: accented letters (é, ç, ñ, ü, ø, å, …),
  `€`, `§`, `¤`, `°`, `«»`, `¡¿` and the backtick. Accent keys (dead keys) type the
  accent itself when the Ace has it (`^`, `~`).
* Every Ace symbol can be reached in every layout; where a layout has no key for one
  (e.g. `~` in Italian, `©` everywhere), use **Ctrl / Left Alt + the key labelled with the
  Ace letter**: `Ctrl + A` = `~`, `Ctrl + I` = `©`, `Ctrl + X` = `£` (see the Ace keyboard
  legends).
* The tape name typed after F5 N (new tape) follows the layout too.

### USB joystick (F3)

The Jupiter Ace had no joystick port: its games read the keyboard. A USB joystick or
gamepad plugged into the Raspberry Pi presses Ace keys instead, as if they were typed, so it
works with any game: each direction and each button presses the Ace key assigned to it.

By default the directions are the Ace's cursor keys (`Shift + 6 / 7 / 5 / 8`: in the ROM
`Shift + 6` moves up and `Shift + 7` down) and the buttons press nothing. To assign a key:

1. Press **F3** (the Ace pauses). The screen lists the directions and the buttons that
   have a key, and says whether a joystick is connected.
2. Move the joystick or press one of its buttons: it is highlighted.
3. Press the key on the keyboard, typed as for the Ace: e.g. `Q`, `Space`, `Enter`,
   `Shift + 6`, or `Ctrl + P` for Symbol Shift + P. **Backspace** removes the key, **Esc**
   leaves it unchanged.
4. Repeat for the other controls; **Esc** goes back to the Ace.

* Two controls can press the same key, and a direction and a button pressed together press
  both their keys (with `Shift` in one of them, the other is shifted too, as on the Ace).
* Directions come from the D-pad, the hat switch or the left stick. Buttons are numbered,
  or named (A, B, X, Y, LB, Start, ...) on gamepads Circle knows.
* The keys are not remembered after power off.
* Generic USB HID joysticks and gamepads work, as well as PS3, PS4, Xbox 360, Xbox One,
  Switch Pro and 8BitDo controllers (Circle's drivers). The first one plugged in is used,
  and it can be plugged in or out while running.
* The joystick and the keyboard can be used together; while a Forth source is spooled
  the joystick is ignored.

---
## SD Card Operations

### The programs folder
The Ace's files (tapes `.tap` / `.tzx`, snapshots `.ace` and Forth sources `.fth` / `.fs` /
`.f`) are kept in the **`programs`** folder of the SD card, in any folders you like inside
it (for example: `programs/games/arcade/invaders.tap`). The root of the card is left to the boot files.
`programs` is made at start-up if it is missing, and any Ace files in the root of the card
(where they were kept before version 0.7, or copied there from a computer), and `.zip`
files, are moved into
it then; a file whose name is already taken in `programs` stays in the root.


### Tapes (.tap and .tzx files)

Tapes are standard Jupiter Ace `.tap` files, or `.tzx` files.

Press **F5** to see the folder you are in: its folders first (shown with a `/` after the
name), then its tapes, snapshots and Forth sources, with their sizes, each sorted by name.
The title bar shows the folder's path. Move with **Up / Down** (**Page Up / Page Down**,
**Home / End** for long lists; held down, Up / Down / Page Up / Page Down repeat) and close
with **Esc**.

* **Enter** (or **Right**) on a folder opens it; **Backspace** (or **Left**), or Enter on
  `..`, goes back up.
* **M** makes a new folder here: type its name and press Enter.
* **Long names** (more than 24 characters, 23 for folders and `.zip` files) are cut in the
  list. The selected one scrolls: after a second it moves one character at a time to its
  end, waits a second there and starts again. **Space** moves it one character by hand
  (held down, it repeats; after the end it goes back to the start), and it then stays
  where Space left it until another entry is selected. The same goes for F6 and for the
  lines of Ctrl + TAB.
* **Long folder paths** in the title bar (F5 and F6) show their end, `...` first. They
  scroll the same way, towards the start of the path, and back to the end; Space does not
  move them. So does the folder line of F7, while you type the name.
* **`.zip` files** are listed with the folders (`collection.zip/`, with their size) and open
  like them: Enter shows the tapes, snapshots and Forth sources in the `.zip`, with the
  folders it has, and they are inserted, loaded or spooled as usual. A `.zip` inside the
  `.zip` opens the same way (collections often keep each program in its own `.zip`): it
  is unpacked into memory, so it can be at most 4 MB. A `.zip` is read only: no new tapes (N) or folders (M) in it, and files in it cannot be deleted (F6
  deletes the whole `.zip`). Each file is unpacked into memory when it is used, so it can
  be at most 4 MB unpacked. Files stored as they are or compressed with *deflate* (what
  nearly every zip tool does) can be read; other methods (bzip2, LZMA, ...), encrypted
  files and ZIP64 archives cannot.
* The folder you leave F5 in is remembered (until the Pi is switched off): new files go
  into it — `SAVE` with no tape in the player, new tapes (N) and F7's dictionaries and
  snapshots. If it is in a `.zip`, they go into the folder the `.zip` is in.

The bottom line of the screen shows the tape in the player (`Tape: none` when it is empty).

* **Enter** on a `.tap` or `.tzx` **inserts** it: the tape goes into the player, rewound (`>`
  marks it in the list), and the Ace comes back. `LOAD` / `BLOAD` / `VERIFY` read from it,
  a plain `load` lists its files, and `SAVE` / `BSAVE` add files to it.
  It stays in until another tape is inserted, it is ejected, or it is deleted with F6;
  F12 and Ctrl+F12 rewind it.
* **E** ejects the tape.
* **N** makes a **new, blank tape** in the folder shown: type its name ending in `.tap` or
  `.tzx` (`.tap` is added if it ends in neither), press Enter, and the empty tape is
  created and inserted, ready for `SAVE`. An existing file is never overwritten.

* **`.tzx`** tapes: `LOAD` / `BLOAD` read them, and `SAVE` / `BSAVE` add to one in the player
  as standard speed data blocks (`0x10`), with a one second pause after each. The Ace's
  blocks can be in standard (`0x10`), turbo (`0x11`), pure data (`0x14`) or
  generalized data (`0x19`, one bit per symbol) TZX blocks; text, archive information,
  pauses and other blocks are skipped. Direct recordings (`0x15`) and CSW blocks (`0x18`),
  which hold sampled sound from a real cassette rather than bytes, are not read.

* `LOAD name` / `BLOAD name` read **only the tape in the player** (inserted with F5), like a
  cassette: they search it from where it is (after the last file loaded, so multi-file tapes
  load in sequence) to its end, then from its start. The files passed on the way are shown
  (`Dict: name` / `Bytes: name`), as on a real Ace. If the file is not on the tape they
  give `ERROR 10`.
* `LOAD` / `BLOAD` **without a name** list the files on the tape in the player, from its
  start, and load nothing (ending with `ERROR 10`, as no file matches).
* With **no tape in the player**, `LOAD`, `BLOAD`, `VERIFY` and `BVERIFY`, with or without a
  name, give `ERROR 10` at once.
* `SAVE name` / `BSAVE name` **add the file at the end of the tape in the player** (`.tap`
  or `.tzx`), like recording on a cassette. With no tape in the player, they write
  `name.tap` in the folder last open in F5, **replacing** any existing file of that name;
  characters that are not valid in file names become `_`. A tape from a `.zip` cannot be
  added to: with one in the player, `SAVE` writes `name.tap` as with no tape (next to the
  `.zip`), and the tape from the `.zip` stays in.
* `VERIFY name` / `BVERIFY name` compare the file on the tape with memory, without changing
  memory: `OK` if they match, `ERROR 10` if they differ (or the file is not on the tape).
* Blocks are read as the Ace's ROM reads them from a cassette: the next block on the tape,
  which must be a header or a data block as asked (`.tzx` files keep the Ace's flag byte
  for this; in a `.tap` a block is a header if it has the header's size and does not
  follow a header), with exactly the length asked for and the right checksum. As on a
  real Ace, a damaged header is skipped and damaged data gives `ERROR 10`. Programs that
  read their own blocks through the ROM's tape routine (`0x18A7`), such as multi-part
  games, get the next blocks of the tape; at its end the routine fails, and holding
  **Space** gives BREAK (`ERROR 3`), as on the Ace.


### Snapshots (.ace files)
`.ace` files are memory snapshots, the format of the Ace32, EightyOne and
SpudACE emulators. Select one in the F5 list and press **Enter**, then **Y**: the Ace's
memory and CPU registers are replaced by the snapshot's, and the program carries on where
the snapshot was taken (anything not saved to tape is lost). If the snapshot's machine had
more RAM than the current size (see Ctrl+F12), the RAM pack is enlarged to match it first;
snapshots of 51K machines hold all the memory, up to `0xFFFF`.
Many snapshots of 35K machines only hold the memory up to `0x7FFF`; when they were taken at
the Forth prompt, the missing return stack is rebuilt, so they work as well. A file that
is not a valid snapshot is
rejected without changing anything.

To save a snapshot, press **F7**, choose *Snapshot (.ace)* and type a name (`.ace` is added
if it has no extension; other extensions are refused, and an existing file is never
overwritten). It goes into the folder last open in F5, shown on the screen. It holds the Ace exactly as it was when F7 was pressed: all the RAM there is
(up to the end of the RAM pack), with the screen, the character set and the Z80
registers, so a game or program can be saved in the middle and loaded later with F5 to
carry on from there. Unlike saving the dictionary, it works at any time, not only at the
Forth prompt (but not while a file is being spooled). The format is the one above, so
other emulators can load the file; MAME only loads snapshots of machines up to 19K
(memory up to `0x7FFF`).

With the 96K RAM pack the snapshot is still one `.ace` file: the page in use is saved as
usual, and the other page follows the snapshot's end marker, with which page was in use
(the block starts with `XACE96K`). Loading it chooses the 96K pack and restores both
pages. Other emulators stop reading at the end marker (EightyOne does), so they load it
as a 51K snapshot of the page in use. A snapshot without that block loaded with the 96K
pack goes into the page that bit 7 of its R register selects; the other page is kept.

### Spooling Forth source files

A Forth source file (`.fth`, `.fs` or `.f`, plain text) on the SD card can be typed into the Ace as if from
the keyboard, as `xAce`'s spooling did: select it in the **F5** list and press **Enter**.

* The text goes through the Ace's own editor, line by line: each line is entered and run
  (or compiled) before the next one is typed. Definitions can span several lines.
* While spooling, the Ace runs as fast as the Pi can, without its 50 Hz pacing, and the
  keyboard is ignored except **Esc**, which stops spooling.
* Spooling stops at the end of the file and at the first line the Ace does not accept: it is
  left in the editor with `?`, to be corrected. After an `ERROR n` (which only loses the rest
  of its line) spooling goes on, as in the original xAce.
* Line ends can be LF, CR LF or CR; tabs become spaces. `£` and `©` (UTF-8 or Latin-1) are
  typed as the Ace's own characters; other characters the Ace does not have (accented
  letters, ...) are skipped. Caps Lock, graphics and inverse mode are switched off first.
* The file must be written for Ace Forth.
* `( ... )` comments **outside definitions are left out** (the Ace would answer them with
  `ERROR 4`), as are the lines left empty. Comments **inside** definitions are typed: the Ace
  keeps them in the word, so `LIST` and `EDIT` show them. Text in `." ..."` strings and the
  character after `ASCII` are never taken for comments.


### Saving the dictionary

**F7**, *Dictionary (Forth source)*, writes every word you have defined (or loaded) to a Forth source file on the SD card,
the reverse of spooling: spooling the file into a fresh Ace (F5) defines the same words
again. Type the name, ending in `.fth`, `.fs` or `.f` (`.fth` is added if it has no
extension; other extensions are refused); an existing file is never overwritten. It goes
into the folder last open in F5, shown on the screen. The Ace must be at the Forth prompt with nothing typed on the input line.

* Colon, `DEFINER` and `COMPILER` words are written as the Ace's own `LIST` shows them.
* Variables and constants are written with their **current values** (`99 variable SCORE`).
* Other words (made with `CREATE`, by a defining word, or holding machine code) are written
  as `create NAME` with their bytes, followed by their code field when needed.
* Hidden words (a program can hide its words by clearing a link) are saved too, and hidden
  again at the end of the file.
* Words are saved oldest first. A word that uses a newer one (after `REDEFINE`) is moved
  after it, so the file can be spooled; the dictionary is then laid out differently, but
  the words work the same.
* Saving runs at spooling speed; **Esc** stops it (the file is then deleted). A screen at the
  end says how many words were saved. The Ace is left exactly as it was.

Words in vocabularies other than `FORTH`, and addresses stored in variables or data (e.g. a
variable holding the address of a word), are saved as they are and may not work after
spooling into a different dictionary.

### File deletion
Press **F6** to delete a tape, a snapshot, a Forth source file or a folder: the same list as
F5 appears, starting in the folder last open in F5. Move with **Up / Down** (**Page Up /
Page Down**, **Home / End**; held down, they repeat as in F5); **Enter** (or **Right**)
opens a folder and **Backspace** (or **Left**) goes up, as in F5.

* **Enter** on a file, or **D** (or **Delete**) on a file or folder, asks to delete it;
  **Y** confirms (N or Esc keeps it).
* Deleting a **folder deletes everything in it**, files and subfolders: the question says
  how many files and folders that is.
* A `.zip` opens like a folder (Enter); **D** deletes the whole `.zip`. Nothing inside a
  `.zip` can be deleted.

The list stays open for more deletions; **Esc** closes it, leaving F5's folder as it was.
Only the `programs` folder is shown, so the boot files are safe. Deleting the tape in the
player (or its folder) ejects it.

Folder and file names typed on the Ace (F5 N and M, F7) are plain names: no `/ \ : * ? " < > |`,
and not starting with `.` or a space.

Note: File names are not case sensitive on the SD card, but Ace names inside a tape are:
`LOAD game` will not find a file saved as `GAME`.

### File exchange (F11)
Press **F11** to copy files between a computer, tablet or phone and the SD card, without
taking the card out. The first time, choose how the Pi joins your network:

* **1  Cable network (Ethernet)**: the Pi 4 / 400's Ethernet port, or a USB Ethernet
  adapter on the other models.
* **2  Wi-Fi**: the on-board Wi-Fi of the Pi 3, 4, 400, Zero W and Zero 2 W. Type the
  network name, the password (none for an open network) and your country's two-letter
  code (`PT`, `GB`, `US`, ...: Wi-Fi channels differ by country, and the Wi-Fi will not
  join without it). **Up** / **Down** move between them; **Enter** on the country
  connects. They are kept on the SD card in `wpa_supplicant.conf`, so next time they are
  already filled in, with the cursor on the country: just press Enter. WPA2 and WPA (personal) and open
  networks work; WPA3-only networks do not. The Wi-Fi firmware files must be in the
  card's `firmware` folder (`build.sh` puts them in `sdcard/firmware`).

The choice stays until the Pi is switched off. The address comes from your router (DHCP)
and the screen shows the page's address, e.g. `http://192.168.1.23`, and also
`http://xace.local` where the browser can find it by name. Open it in a web browser on
the same network:

* The page shows the `programs` folder (see *The programs folder*): the path at the top
  (click a part to go back to it), then the folders, then the `.tap`, `.tzx`, `.ace`,
  `.fth`, `.fs`, `.f` and `.zip` files, and the tape in the player. On the page a `.zip` is
  a file like the others (uploaded, downloaded, renamed, moved and deleted whole); its
  contents are seen in F5. Click a folder to open it; the
  browser's Back button works too.
* **Upload** into the folder shown: drop files on the page or choose them, several at once;
  or drop a **whole folder**, or choose one with *A folder*: it is copied with all its
  subfolders, keeping their structure (only the Ace's file types are copied; the others are
  listed as left out). Large uploads are sent in parts of up to 30 MB. A file with the same
  name as one on the card is kept unless **Replace files with the same name** is ticked.
* **Download**: click a name for one file, or tick several files and folders and use
  **Download selected** for a `.zip` with all of them, folders with everything in them
  (up to 8 MB and 1024 files at once).
* **New folder** makes one in the folder shown.
* **Rename** a file or a folder; **Move selected** moves the ticked files and folders to
  another folder (type its path in `programs`, e.g. `games/arcade`, or nothing for
  `programs` itself).
* **Delete** a file, or a folder with **everything in it** (files and subfolders), after
  confirming; or the ticked ones.
* Renaming or moving the tape in the player (or its folder) keeps it in the player;
  deleting it ejects it; replacing it rewinds it.
* Nothing outside `programs` can be reached, so the boot files are safe.

The Ace is paused while the F11 screen is open, and the page only answers then: the
network is not run while the Ace runs, so its timing is not disturbed. The screen lists
what has been done on the page. **Esc** goes back to the Ace. The network stays connected
until the Pi is switched off, so F11 again is ready at once (a Wi-Fi network may have
dropped the Pi meanwhile: it joins again by itself). Only plain ASCII file names are
handled. The page has no password: use it on a home network only.

On Wi-Fi, **C** changes the network settings. If the Pi has not joined after 30 seconds
the screen says so: correct the settings with C and it tries again with them. Once the Pi
has an address, new settings are only used after switching it off and on (Circle does not
ask the router for a new address). `wpa_supplicant.conf` can also be written by hand
(see Circle's [sample](circle/addon/wlan/sample/hello_wlan/wpa_supplicant.conf)); the
password is kept in it as typed, unless it has `"` or `#` (then as the key made from it).

### System update (F11)
The web page of F11 has a **System update** page (link at the bottom, or `/system`). It
shows the version running, and installs a new release of xAce-baremetal from its `.zip`
(`xAce-baremetal-<version>-pi<model>.zip`, the one for your Raspberry Pi):

1. Choose the `.zip` on the page. The Ace checks it: it must hold the kernel for this model
   with the version marker of an xAce-baremetal kernel (0.7 or later). Only the files that
   differ from those on the card are to change: the kernel, the boot firmware
   (`start*.elf`, `fixup*.dat`, `bootcode.bin`, `*.dtb`), the Wi-Fi firmware and the
   licences. `config.txt`, `cmdline.txt`, `wpa_supplicant.conf` and the `programs` folder
   are never touched. The page lists what will change.
2. The F11 screen on the Ace asks to install it: **Y** installs, **N** discards it. Nothing
   changes on the card before Y, so nobody on the network can install a version without
   someone at the Ace.
3. **Pi 4 / 400**: the Pi restarts to **try the new version once**, with the boot firmware's
   *tryboot* (`tryboot.txt`, a copy of `config.txt` naming the new kernel and boot
   firmware, is used for that one start only). When the new version has run for a few
   seconds it puts all the new files in place and says so; the replaced files are kept in
   `update-backup` on the card. If it does not start, **switch the Pi off and on**: the
   old version starts, removes the new files and says that the update did not start.
   Tryboot needs a Pi 4 bootloader (EEPROM) from 2021 or later; with an older one the
   update is reported as not started (update the bootloader with Raspberry Pi OS's
   `rpi-eeprom-update`).
4. **Pi 1, 2, 3, Zero**: they cannot try a version first. The files are put in place at
   once, and the screen asks to switch the Pi off and on. If it then does not start, copy
   the files in `update-backup` back to the root of the card on a computer.

While a new version is tried, the Wi-Fi firmware and `.dtb` files of the old one are used;
they are replaced with the rest once it has started.

### Power down
Press **F10** (or the keyboard's power key, **Fn + F10** on the Pi 400) and then **Y** to power off: the SD card is unmounted and the Pi halts
(N or Esc returns to the Ace). Anything not saved to tape is lost. To start again,
unplug and replug the power.

---
## Sound

The beeper plays on the 3.5 mm headphone jack by default. To use HDMI audio instead
(always on the Pi 400, which has no headphone jack), create `cmdline.txt` on the SD card containing:

```text
sounddev=sndhdmi
```

---

## Build Instructions

All commands are run from the project root (the folder containing `src/`, `circle/`
and `sdcard/`). The project folder can have any name and be anywhere.

The program is developed with Slackware x86_64 Version 15.0, but you should be able to build it with other distros, otherwise just use the provided binaries.

### 0. Get the source

```bash
git clone https://github.com/antonioleal/xAce-baremetal
```

Everything needed is in the repository, including Circle (in `circle/`), so the source
archives of a release build as well.

### 1. Toolchain

```bash
./download_toolchain.sh
```

This downloads the Arm GNU Toolchain 14.2 (`arm-none-eabi`) into `toolchain/`. Circle's
`Config.mk` uses it automatically, and falls back to an `arm-none-eabi-gcc` on the `PATH`
if `toolchain/` is missing.

### 2. Build

```bash
./build.sh [model]
```

| `model` | Raspberry Pi | Kernel image |
| :--- | :--- | :--- |
| `4` or `400` (default) | Pi 4, Pi 400 | `kernel7l.img` |
| `3` | Pi 3, Zero 2 W | `kernel8-32.img` |
| `2` | Pi 2 | `kernel7.img` |
| `1` | Pi 1, Zero, Zero W | `kernel.img` |

The Pi 4 and Pi 400 have been tested on hardware; the other models build but are untested.
The Pi 5 is not supported yet: it needs a 64-bit build, and F10 power off uses registers
the Pi 5 does not have.

*Note: Since I don't own a Pi5 (or 500) so I didn't develop for it. If many of you ask I could grab one and update the program...*


`build.sh` can be run from any folder. It first empties `dist/`, then for the model (or
each model with `--all`):

1. copies `circle-config/Config.mk` to `circle/Config.mk` if it is missing;
2. if the model differs from the previous build, cleans Circle and `src/` first, because
   they are built for one model at a time (the last model is kept in `.build-model`);
3. builds the Circle libraries (only what changed, so later runs are quick);
4. builds the kernel image, and stops if it is too big to boot (Circle gives the kernel,
   with its variables, 2 MB of memory; past that the Pi stays on the rainbow screen);
5. empties `sdcard/` and fills it with the model's boot firmware from `firmware/pi<model>`,
   its Wi-Fi firmware from `firmware/wlan` (into `sdcard/firmware`), the kernel image and
   `config.txt` (which tells each model which kernel image to load).
   Anything else you put in `sdcard/` is deleted, so keep tapes, snapshots and `cmdline.txt`
   elsewhere.
6. zips the files of `sdcard/` into `dist/xAce-baremetal-<version>-pi<model>.zip` (the
   version from `src/version.h`): the release zip of the model, which the system update
   page (F11) installs.

Circle is included in `circle/`: a copy of Circle Step51.1.1, the tested version (the exact
commit is in `circle/README.xAce`). Its configuration for the Raspberry Pi in 32-bit mode with
the project toolchain is kept in `circle-config/Config.mk`, because Circle's `.gitignore`
ignores `Config.mk`. Don't run
Circle's `./configure`: it would replace `Config.mk` and lose the toolchain selection.

The same steps by hand (for another model, `export RASPPI=<model>` first and run
`./makeall clean` and `make clean` in each folder when switching):

```bash
cp circle-config/Config.mk circle/
cd circle
./makeall                   # Circle core, USB, input, file system, scheduler, sound
make -C addon/SDCard        # SD card driver (tape)
make -C addon/fatfs         # FAT file system (tape)
make -C addon/wlan          # Wi-Fi driver (F11)
make -C addon/wlan/hostap/wpa_supplicant -f Makefile.circle   # WPA supplicant (F11)
cd ../src
make                        # builds the kernel image (kernel7l.img for the Pi 4)
make sdcard                 # copies it and config.txt to ../sdcard/
```

### 3. Release

This paragraphsis mostly for the me, as I need a simple way to publish things along in GitHub as they get developed and tested.

```bash
./build.sh --all                # builds every model, with each one's zip in dist/
./build.sh --all --publish      # also creates the GitHub release v<version> with the zips
```

`--all` builds the models one after the other, the Pi 4 last (so `sdcard/` is left with
it), and `dist/` ends with all four zips: `dist/xAce-baremetal-<version>-pi1.zip` to
`-pi4.zip` (`pi4` is also for the Pi 400). The version is `XACE_VERSION` in
`src/version.h`: change it there for a new release. With `--publish` it creates the
release `v<version>` on the current commit (which must be committed and pushed, and the
release must not exist yet), with the zips and the `<version>` section of `CHANGELOG.md` as
release notes; GitHub adds the source code zip and tar.gz.

---

## SD Card Preparation

1. Format a micro-SD card with a single **FAT32** partition (MBR partition table).
2. Run `./build.sh [model]` (see *Build*). It fills `sdcard/` with everything the card
   needs for that model. The boot firmware comes from `firmware/`, which has one folder per
   model, taken from the [Raspberry Pi firmware repository](https://github.com/raspberrypi/firmware/tree/master/boot)
   at the version Circle's `boot/Makefile` uses (`0641c5b`):

   | Folder | Models | Files |
   | :--- | :--- | :--- |
   | `firmware/pi1` | Pi 1 A / B / A+ / B+, CM1, Zero, Zero W | `bootcode.bin`, `start.elf`, `fixup.dat`, `bcm2708-*.dtb` |
   | `firmware/pi2` | Pi 2 (including v1.2), CM2 | `bootcode.bin`, `start.elf`, `fixup.dat`, `bcm2709-*.dtb`, `bcm2710-rpi-2-b.dtb` |
   | `firmware/pi3` | Pi 3 B / B+ / A+, CM3, Zero 2 W, CM0 | `bootcode.bin`, `start.elf`, `fixup.dat`, `bcm2710-*.dtb` |
   | `firmware/pi4` | Pi 4, Pi 400, CM4, CM4S | `start4.elf`, `fixup4.dat`, `bcm2711-*.dtb` |

   Each folder also has the firmware licences, `LICENCE.broadcom` and `COPYING.linux`.

   The Wi-Fi firmware (F11) is in `firmware/wlan`, from the
   [Raspberry Pi firmware-nonfree repository](https://github.com/RPi-Distro/firmware-nonfree)
   at the version Circle's `addon/wlan/firmware/Makefile` uses (`c9d3ae6`), with its
   licences in `LICENCE.wlan`. `build.sh` copies the files of the model's Wi-Fi chips to
   `sdcard/firmware`: `brcmfmac43430-*` (Pi 3 B, Zero W), `brcmfmac43436-*` (Zero 2 W),
   `brcmfmac43455-*` (Pi 3 B+ / A+, Pi 4, CM4), `brcmfmac43456-*` (Pi 400). The Pi 2 has
   no Wi-Fi.

3. Copy the contents of the project's `sdcard/` folder to the root of the card. For the
   Pi 4 / 400 that is:
   * `start4.elf`, `fixup4.dat`, `bcm2711-*.dtb`: boot firmware (the firmware loads the
     `.dtb` for the model it runs on)
   * `bcm2711-rpi-400.dtb`: only used on a Raspberry Pi 400, which runs the same kernel.
     The Pi 400 has no headphone jack, so select HDMI sound (see *Sound*).
   * `config.txt`
   * `kernel7l.img`
   * `firmware/`: the Wi-Fi firmware (F11), `brcmfmac43455-*` for the Pi 4 and
     `brcmfmac43456-*` for the Pi 400
4. Optionally add `cmdline.txt` (see *Sound*), and your `.tap`, `.tzx`, `.ace` and `.fth` /
   `.fs` / `.f` files, and `.zip` files of them, in a folder called `programs`, in any
   folders you like inside it (files put in the root of the card are moved into `programs`
   at start-up).

---

## Running

1. Insert the micro-SD card into your Raspberry Pi 4 or Pi 400.
2. Connect an HDMI monitor to **HDMI0**: on the Pi 4 the port closest to the USB-C power
   port, on the Pi 400 the port next to the micro-SD slot.
3. Plug in a standard USB keyboard (the Pi 400 uses its own keyboard).
4. Optionally connect headphones or a speaker to the 3.5 mm jack (Pi 4 only; on the
   Pi 400 sound goes over HDMI, see *Sound*).
5. Power on the Pi.
6. After a few seconds the Ace screen appears: black, with the cursor at the bottom left,
   just like a real Jupiter Ace at power-on.
7. Start typing Forth words, e.g. `2 3 + .` and Enter.

### Debug log

Circle writes a log (USB keyboard attached, SD card mounted, tape LOAD / SAVE, sound
device) to the serial port at **115200 baud, 8N1**, on GPIO 14 (TXD, pin 8) and GPIO 15
(RXD, pin 10). Connect a 3.3 V USB-serial adapter to read it.
