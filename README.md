# xAce Baremetal for the Raspberry Pi 4 / 400 ...
*(Using the Brilliant Circle Framework)*

This project consists of the **xAce** Jupiter Ace emulator ported to run baremetal (no operating system) on the **Raspberry Pi models 4 / 400 (default), 3, 2 and 1**, built on the original code and the [Circle](https://github.com/rsta2/circle) C++ library.

Please visit the [Jupiter ACE Archive](https://www.jupiter-ace.co.uk/index.html) to obtain software for this emulator and give your contribution if you find it useful.

I cannot claim to be the sole author of this, that little orange circled white star gives me some help :-)

Ready to use Zip files with the binaries are provided, so you can directly download them to your PI's formatted sdcard. See the **SD Card Preparation** paragraph below.

---

## Source Files

All in `src/`:

| File | Purpose |
| :--- | :--- |
| `z80.c`, `z80ops.c`, `cbops.c`, `edops.c`, `z80.h` | Z80 CPU core (from xz80 / xz81 / xAce). The `*ops.c` files are included by `z80.c`, not compiled separately. |
| `tape.h` | Declarations of the tape traps called by the Z80 core |
| `main.cpp`, `kernel.cpp`, `kernel.h` | Circle kernel: devices (keyboard, USB joystick), frame timing, screen rendering, F3 / F5 / F6 / F7 / F8 / F10 screens |
| `keyboard_circle.cpp`, `keyboard_circle.h` | USB keyboard and joystick to Jupiter Ace keyboard matrix |
| `keyboard_layouts.cpp`, `keyboard_layouts.h` | Keyboard layouts for F8 (UK, US, German, French, …) |
| `ram_ace.cpp`, `ram_ace.h` | RAM pack size (Ctrl+F12) |
| `spool_ace.cpp`, `spool_ace.h` | Spooling Forth source files (`.fth`, `.fs`, `.f`) into the Ace (F5) |
| `save_dictionary.cpp`, `save_dictionary.h` | Saving the dictionary as a Forth source file (F7) |
| `line_history.cpp`, `line_history.h` | Line history: TAB / Shift+TAB retype earlier lines, Ctrl+TAB lists them |
| `sound_ace.cpp`, `sound_ace.h` | Beeper sound |
| `tape_sd.cpp`, `tape_sd.h` | Tape LOAD / SAVE with `.tap` (and loading `.tzx`) files on the SD card; loading `.ace` snapshots |
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
* **Tape**: `LOAD` / `BLOAD` / `SAVE` / `BSAVE` use `.tap` files on the SD card; `.tzx` tapes can be loaded too (see below).
* **ROM**: The Jupiter Ace ROM is compiled into the kernel (`ace_rom.h`), so the Ace starts
  without reading any file. Its tape routines are patched at boot with emulator traps.
* **Memory**: The emulated Ace has the standard 3 KB of built-in RAM plus, by default, a
  48 KB RAM pack: **51 KB of RAM** in total (**49 KB for programs**: the 1 KB built in plus
  the 48 KB pack). **Ctrl+F12** chooses the typical sizes of the time instead:

  | RAM | RAM pack | For programs | RAMTOP |
  | :--- | :--- | :--- | :--- |
  | 3K | none (a stock Ace) | 1K | `0x4000` |
  | 19K | 16K | 17K | `0x8000` |
  | 35K | 32K | 33K | `0xC000` |
  | 51K (default) | 48K | 49K | `0x0000` (top of memory) |

  Missing RAM ignores writes and reads `0xFF`, so the ROM's RAM test at power-on finds the
  chosen size, as on the real machine. RAM is filled with `0xFF` at power-on and on F12 reset.

  | Address | Size | Contents |
  | :--- | :--- | :--- |
  | `0x0000 – 0x1FFF` | 8 KB | ROM |
  | `0x2000 – 0x27FF` | 1 KB | Video RAM (32×24 characters), mirrored twice |
  | `0x2800 – 0x2FFF` | 1 KB | Character set RAM (128 characters × 8 bytes), mirrored twice |
  | `0x3000 – 0x3FFF` | 1 KB | Built-in program RAM, mirrored four times |
  | `0x4000 – 0xFFFF` | up to 48 KB | RAM pack (16K: to `0x7FFF`, 32K: to `0xBFFF`) |

---

## Key Mappings

| Standard USB Keyboard | Jupiter Ace Action |
| :--- | :--- |
| **A – Z** | A – Z (Shift gives the other case) |
| **0 – 9**, keypad digits | 0 – 9 |
| **Symbols** (`! @ # $ ( ) - _ = + [ ] { } ; : ' " , . < > / ? \ \| ~`) | The same symbol (Symbol Shift + its key) |
| **Shift + 5 / 6 / 7 / 8** | Move the cursor left / up / down / right, as on the Ace (Original ACE keymap; the F8 layouts type their symbols) |
| `% ^ & *` | **Ctrl + 5 / H / 6 / B**, as on the Ace (Original ACE keymap, where Shift + 5-8 move the cursor); keypad `*` |
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
| **F3** | Assign Ace keys to the USB joystick's directions and buttons (pauses the Ace; see *USB joystick*) |
| **F4** | Inverse Video on / off (`Shift + 4`) |
| **Ctrl + F4** | Swap the screen colours: black on white / white on black |
| **F5** | List the tapes (`.tap`, `.tzx`), snapshots (`.ace`) and Forth sources (`.fth`, `.fs`, `.f`) and the tape in the player; Enter inserts a tape, loads a snapshot or spools a source, E ejects the tape, N makes a new one (pauses the Ace) |
| **F6** | Delete a tape, a snapshot or a Forth source from the SD card: choose it in the list, Enter, then Y (pauses the Ace) |
| **F7** | Save the dictionary as a Forth source file (`.fth`, `.fs`, `.f`): type its name (see *Saving the dictionary*) |
| **F8** | Choose the keyboard layout (pauses the Ace; see *Keyboard layouts*) |
| **F9** | Graphics Mode (`Shift + 9`) |
| **F10**, power key (**Fn + F10** on the Pi 400) | Power off the Raspberry Pi (pauses the Ace, then Y/N) |
| **F12** | Hard Reset |
| **Ctrl + F12** | Choose the RAM size: 3K, 19K, 35K or 51K (restarts the Ace) |

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
* **Shift + digits**: on the Ace they are editing keys. In the Original ACE keymap
  **Shift + 5 / 6 / 7 / 8** move the cursor, as on the Ace (the ROM moves up a row on
  Shift + 6 and down on Shift + 7), so `% ^ & *` are typed the Ace way, with Ctrl (or the
  keypad `*`). The other Shift + digits type the PC symbols (`! @ # $ ( )`); their Ace
  functions are on separate keys: Delete Line F1, Caps Lock, Inverse Video F4, Graphics F9,
  Delete Backspace. The arrow keys move the cursor in every keymap.

### Ace editing keys (F1, F4, F9)

These function keys are shortcuts for Jupiter Ace key combinations, so you don't need to
remember the Shift + digit legends:

* **F1 – Delete Line** (`Shift + 1`): clears the line being typed.
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
the screen ends with `>`). Move with **Up / Down** (**Page Up / Page Down**, **Home / End**;
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

## Tapes and snapshots (SD card)

Tapes are standard Jupiter Ace `.tap` files, or `.tzx` files, in the root folder of the SD card.

* **`.tzx`** tapes are read-only: `LOAD` / `BLOAD` read them, `SAVE` always writes `.tap`.
  The Ace's blocks can be in standard (`0x10`), turbo (`0x11`), pure data (`0x14`) or
  generalized data (`0x19`, one bit per symbol) TZX blocks; text, archive information,
  pauses and other blocks are skipped. Direct recordings (`0x15`) and CSW blocks (`0x18`),
  which hold sampled sound from a real cassette rather than bytes, are not read.

* `LOAD name` / `BLOAD name` search, in order:
  1. the rest of the current tape (then from its start): the tape inserted with F5, or the
     one the previous file was loaded from, so multi-file tapes load in sequence;
  2. `name.tap`, then `name.tzx`;
  3. every other `.tap` and `.tzx` file in the root folder.

  If nothing matches, a stand-in file is loaded that prints *Couldn't load your file!*
* `LOAD` / `BLOAD` **without a name** load the next file on the current tape (with no tape
  inserted, the first one found on the card). This is an emulator convenience: the real
  Ace compares the whole name, so a nameless `LOAD` never finds a file there.
* `SAVE name` / `BSAVE name` **add the file at the end of the tape in the player** when it
  is a `.tap`, like recording on a cassette. With no tape in the player, or a `.tzx` (which
  is read-only), they write `name.tap`, **replacing** any existing file of that name;
  characters that are not valid in file names become `_`.
* `VERIFY name` / `BVERIFY name` compare the file on the tape with memory, without changing
  memory: `OK` if they match, `ERROR 10` if they differ (or the file is not found).

Press **F5** to see the tapes (`.tap`, `.tzx`), snapshots (`.ace`) and Forth sources (`.fth`,
`.fs`, `.f`) on the card, with their sizes, sorted by name. Move with **Up / Down**
(**Page Up / Page Down**, **Home / End** for long lists; held down, Up / Down / Page Up /
Page Down repeat) and close with **Esc**.

The bottom line of the screen shows the tape in the player (`Tape: none` when it is empty).

* **Enter** on a `.tap` or `.tzx` **inserts** it: the tape goes into the player, rewound (`>`
  marks it in the list), and the Ace comes back. `LOAD` / `BLOAD` now read from it first, a
  plain `load` takes its files one after the other, and `SAVE` / `BSAVE` add files to it
  (`.tap` only). It stays in until another tape is inserted (also by loading from another
  tape by name), it is ejected, or it is deleted with F6; F12 and Ctrl+F12 rewind it.
* **E** ejects the tape.
* **N** makes a **new, blank tape**: type its name (`.tap` is added), press Enter, and the
  empty `.tap` is created and inserted, ready for `SAVE`. An existing file is never
  overwritten.

**Snapshots**: `.ace` files are memory snapshots, the format of the Ace32, EightyOne and
SpudACE emulators. Select one in the F5 list and press **Enter**, then **Y**: the Ace's
memory and CPU registers are replaced by the snapshot's, and the program carries on where
the snapshot was taken (anything not saved to tape is lost). If the snapshot's machine had
more RAM than the current size (see Ctrl+F12), the RAM pack is enlarged to match it first.
Many snapshots of 35K machines only hold the memory up to `0x7FFF`; when they were taken at
the Forth prompt, the missing return stack is rebuilt, so they work as well. A file that
is not a valid snapshot is
rejected without changing anything.

Press **F6** to delete a tape, a snapshot or a Forth source file: the same list as F5 appears. Move with
**Up / Down** (**Page Up / Page Down**, **Home / End**; held down, they repeat as in F5),
press **Enter** on the file, then
**Y** to confirm (N or Esc keeps it). The list stays open for more deletions; **Esc** closes
it. Only `.tap`, `.tzx`, `.ace`, `.fth`, `.fs` and `.f` files in the card's root folder are listed, so the boot
files are safe. Deleting the tape in the player ejects it.

Press **F10** (or the keyboard's power key, **Fn + F10** on the Pi 400) and then **Y** to power off: the SD card is unmounted and the Pi halts
(N or Esc returns to the Ace). Anything not saved to tape is lost. To start again,
unplug and replug the power.

File names are not case sensitive on the SD card, but Ace names inside a tape are:
`LOAD game` will not find a file saved as `GAME`.

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

---
### Saving the dictionary

**F7** writes every word you have defined (or loaded) to a Forth source file on the SD card,
the reverse of spooling: spooling the file into a fresh Ace (F5) defines the same words
again. Type the name, ending in `.fth`, `.fs` or `.f` (`.fth` is added if it has no
extension; other extensions are refused); an existing file is never overwritten. The Ace must be at the Forth prompt with nothing typed on the input line.

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


`build.sh` can be run from any folder. It:

1. copies `circle-config/Config.mk` to `circle/Config.mk` if it is missing;
2. if the model differs from the previous build, cleans Circle and `src/` first, because
   they are built for one model at a time (the last model is kept in `.build-model`);
3. builds the Circle libraries (only what changed, so later runs are quick);
4. builds the kernel image;
5. empties `sdcard/` and fills it with the model's boot firmware from `firmware/pi<model>`,
   the kernel image and `config.txt` (which tells each model which kernel image to load).
   Anything else you put in `sdcard/` is deleted, so keep tapes, snapshots and `cmdline.txt`
   elsewhere.

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
cd ../src
make                        # builds the kernel image (kernel7l.img for the Pi 4)
make sdcard                 # copies it and config.txt to ../sdcard/
```

### 3. Release

```bash
./release.sh 0.6                # builds every model, zips each one's sdcard/ into dist/
./release.sh 0.6 --publish      # also creates the GitHub release v0.6 with the zips
```

`release.sh` makes one zip per model with the contents of `sdcard/` for it:
`dist/xAce-baremetal-<version>-pi1.zip` to `-pi4.zip` (`pi4` is also for the Pi 400). With
`--publish` it creates the release `v<version>` on the current commit (which must be committed
and pushed), with the zips and the `<version>` section of `CHANGELOG.md` as release notes;
GitHub adds the source code zip and tar.gz. `sdcard/` is left with the Pi 4 build.

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

3. Copy the contents of the project's `sdcard/` folder to the root of the card. For the
   Pi 4 / 400 that is:
   * `start4.elf`, `fixup4.dat`, `bcm2711-*.dtb`: boot firmware (the firmware loads the
     `.dtb` for the model it runs on)
   * `bcm2711-rpi-400.dtb`: only used on a Raspberry Pi 400, which runs the same kernel.
     The Pi 400 has no headphone jack, so select HDMI sound (see *Sound*).
   * `config.txt`
   * `kernel7l.img`
4. Optionally add `cmdline.txt` (see *Sound*) and your `.tap`, `.tzx`, `.ace` and `.fth` / `.fs` / `.f` files.

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
