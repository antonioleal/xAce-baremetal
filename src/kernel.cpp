#include "kernel.h"
#include "keyboard_circle.h"
#include "tape_sd.h"
#include "ram_ace.h"
#include "spool_ace.h"
#include "save_dictionary.h"
#include "line_history.h"
#include "ace_rom.h"

#include <circle/string.h>
#include <circle/util.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/synchronize.h>
#include <string.h>

#define FB_WIDTH      640
#define FB_HEIGHT     480
#define FB_BPP        32

#define ACE_COLS      32
#define ACE_ROWS      24
#define SCALE         2
#define CHAR_WIDTH    (8 * SCALE)
#define CHAR_HEIGHT   (8 * SCALE)

#define DISP_WIDTH    (ACE_COLS * CHAR_WIDTH)   // 512
#define DISP_HEIGHT   (ACE_ROWS * CHAR_HEIGHT)  // 384
#define OFFSET_X      ((FB_WIDTH - DISP_WIDTH) / 2)   // 64
#define OFFSET_Y      ((FB_HEIGHT - DISP_HEIGHT) / 2) // 48

#define COLOR_WHITE   0x00FFFFFF
#define COLOR_BLACK   0x00000000

// The real Ace shows white characters on a black screen and border; Ctrl+F4 swaps them
#define COLOR_INK     (m_bInverted ? COLOR_BLACK : COLOR_WHITE)
#define COLOR_PAPER   (m_bInverted ? COLOR_WHITE : COLOR_BLACK)
#define COLOR_BORDER  COLOR_PAPER

// 3.25 MHz Z80, 312 lines of 208 t-states per frame (50.08 Hz)
#define FRAME_TSTATES 64896
#define FRAME_MICROS  19968

// By then the ROM has set up the character set RAM (~2 s after power-on)
#define FONT_SNAPSHOT_FRAME 100

#define HID_KEY_ENTER       0x28
#define HID_KEY_ESCAPE      0x29
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_KP_ENTER    0x58
#define HID_KEY_DOWN        0x51
#define HID_KEY_UP          0x52
#define HID_KEY_HOME        0x4A
#define HID_KEY_PAGEUP      0x4B
#define HID_KEY_END         0x4D
#define HID_KEY_PAGEDOWN    0x4E

// Z80 Memory and State definitions (accessed by z80.c)
extern "C" {
    unsigned char mem[65536];
    unsigned char *memptr[8] = {
        mem,
        mem + 0x2000,
        mem + 0x4000,
        mem + 0x6000,
        mem + 0x8000,
        mem + 0xa000,
        mem + 0xc000,
        mem + 0xe000
    };
    int memattr[8] = { 0, 1, 1, 1, 1, 1, 1, 1 };
    unsigned long tstates = 0;
    unsigned long tsmax = FRAME_TSTATES;
    int reset_ace = 0;

    void mainloop(void);
    unsigned int in(int h, int l);
    unsigned int out(int h, int l, int a);
    void fix_tstates(void);
    void do_interrupt(void);
}

CKernel *CKernel::s_pThis = nullptr;
static u64 s_nNextFrameMicros = 0;

CKernel::CKernel(void)
:   m_FrameBuffer(FB_WIDTH, FB_HEIGHT, FB_BPP),
    m_Timer(&m_Interrupt),
    m_Logger(m_Options.GetLogLevel(), &m_Timer),
    m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
    m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED),
    m_pKeyboard(nullptr),
    m_pJoystick(nullptr),
    m_nJoystickProperties(0),
    m_bScreenDirty(TRUE),
    m_bInverted(FALSE),
    m_bFontValid(FALSE),
    m_nFrameCount(0),
    m_nLastRender(0),
    m_ucRepeatKey(0),
    m_nRepeatAt(0)
{
    s_pThis = this;
    memset(m_VideoRamOld, 0xFF, sizeof(m_VideoRamOld));
    memset(m_CharsetOld, 0, sizeof(m_CharsetOld));
    m_ActLED.Blink(5);
}

CKernel::~CKernel(void)
{
    s_pThis = nullptr;
}

void CKernel::KeyStatusHandlerRaw(unsigned char ucModifiers, const unsigned char RawKeys[6])
{
    keyboard_update(ucModifiers, RawKeys);
}

// Joystick directions from the D-pad (known gamepads), the first hat switch or the left
// stick (pushed over a quarter of its range); the other buttons as they are
void CKernel::JoystickStatusHandler(unsigned nDeviceIndex, const TGamePadState *pState)
{
    if (s_pThis == nullptr || nDeviceIndex != 0) {
        return;
    }

    unsigned nState = 0;
    unsigned nButtons = pState->buttons;
    if (s_pThis->m_nJoystickProperties & GamePadPropertyIsKnown) {
        const unsigned nDPad = GamePadButtonUp | GamePadButtonDown
                             | GamePadButtonLeft | GamePadButtonRight;
        if (nButtons & GamePadButtonUp)    nState |= JOYSTICK_UP;
        if (nButtons & GamePadButtonDown)  nState |= JOYSTICK_DOWN;
        if (nButtons & GamePadButtonLeft)  nState |= JOYSTICK_LEFT;
        if (nButtons & GamePadButtonRight) nState |= JOYSTICK_RIGHT;
        nButtons &= ~nDPad;
    }

    // Hat switch: 0-7 clockwise from up, anything else centred
    if (pState->nhats > 0) {
        static const unsigned s_HatDirections[8] = {
            JOYSTICK_UP, JOYSTICK_UP | JOYSTICK_RIGHT, JOYSTICK_RIGHT,
            JOYSTICK_DOWN | JOYSTICK_RIGHT, JOYSTICK_DOWN, JOYSTICK_DOWN | JOYSTICK_LEFT,
            JOYSTICK_LEFT, JOYSTICK_UP | JOYSTICK_LEFT
        };
        int nHat = pState->hats[0];
        if (nHat >= 0 && nHat < 8) {
            nState |= s_HatDirections[nHat];
        }
    }

    // Left stick: axes 0 (X) and 1 (Y)
    for (int nAxis = 0; nAxis < 2 && nAxis < pState->naxes; nAxis++) {
        int nMin = pState->axes[nAxis].minimum;
        int nMax = pState->axes[nAxis].maximum;
        int nValue = pState->axes[nAxis].value;
        int nQuarter = (nMax - nMin) / 4;
        if (nQuarter <= 0) {
            continue;
        }
        if (nValue < nMin + nQuarter) {
            nState |= nAxis == 0 ? JOYSTICK_LEFT : JOYSTICK_UP;
        } else if (nValue > nMax - nQuarter) {
            nState |= nAxis == 0 ? JOYSTICK_RIGHT : JOYSTICK_DOWN;
        }
    }

    keyboard_joystick_update(nState, nButtons);
}

void CKernel::JoystickRemovedHandler(CDevice *pDevice, void *pContext)
{
    if (s_pThis != nullptr) {
        s_pThis->m_pJoystick = nullptr;
        keyboard_joystick_update(0, 0);
        CLogger::Get()->Write("xAce", LogNotice, "USB joystick disconnected");
    }
}

void CKernel::KeyboardRemovedHandler(CDevice *pDevice, void *pContext)
{
    if (s_pThis != nullptr) {
        s_pThis->m_pKeyboard = nullptr;
        keyboard_clear();
        CLogger::Get()->Write("xAce", LogNotice, "USB Keyboard disconnected");
    }
}

boolean CKernel::Initialize(void)
{
    boolean bOK = TRUE;

    if (bOK) bOK = m_FrameBuffer.Initialize();
    if (bOK) bOK = m_Serial.Initialize(115200);
    if (bOK) bOK = m_Logger.Initialize(&m_Serial);
    if (bOK) bOK = m_Interrupt.Initialize();
    if (bOK) bOK = m_Timer.Initialize();
    if (bOK) bOK = m_USBHCI.Initialize();

    if (!bOK) {
        return FALSE;
    }

    // Tape and sound are optional: the Ace still runs without them
    if (m_EMMC.Initialize()) {
        TapeInitialize();
    } else {
        m_Logger.Write("xAce", LogWarning, "SD card not available, tape disabled");
    }

    m_Sound.Initialize(&m_Interrupt, m_Options.GetSoundDevice());

    keyboard_init();

    ClearBorder(COLOR_BORDER);

    // Load Jupiter Ace ROM into memory (0x0000 - 0x1FFF) with the tape and error traps
    memcpy(mem, ace_rom, sizeof(ace_rom));
    TapePatchROM(mem);
    SpoolPatchROM(mem);

    // Clear RAM (0x2000 - 0xFFFF) to 0xFF like real Ace power-on
    memset(mem + 8192, 0xFF, 57344);

    return TRUE;
}

TShutdownMode CKernel::Run(void)
{
    m_Logger.Write("xAce", LogNotice, "Starting Jupiter Ace Z80 core...");

    s_nNextFrameMicros = m_Timer.GetClockTicks64() + FRAME_MICROS;

    // Enter Z80 CPU execution loop (runs forever)
    mainloop();

    return ShutdownHalt;
}

void CKernel::ClearBorder(u32 nColor)
{
    u32 *pBuffer = (u32 *) m_FrameBuffer.GetBuffer();
    unsigned nPitchWords = m_FrameBuffer.GetPitch() / sizeof(u32);

    for (unsigned y = 0; y < FB_HEIGHT; y++) {
        for (unsigned x = 0; x < FB_WIDTH; x++) {
            pBuffer[y * nPitchWords + x] = nColor;
        }
    }
}

void CKernel::Frame(unsigned long nFrameTStates)
{
    m_Sound.EndFrame(nFrameTStates);

    if (!m_bFontValid && ++m_nFrameCount >= FONT_SNAPSHOT_FRAME) {
        memcpy(m_Font, mem + 0x2C00, sizeof(m_Font));
        m_bFontValid = TRUE;
    }

    UpdateKeyboard();
    SpoolFrame();
    SaveDictionaryFrame();
    LineHistoryFrame();

    if (keyboard_check_list()) {
        ShowTapeList();
    } else if (keyboard_check_delete()) {
        ShowDeleteTape();
    } else if (keyboard_check_poweroff()) {
        ShowPowerOff();
    } else if (keyboard_check_layout()) {
        ShowKeyboardLayout();
    } else if (keyboard_check_joystick()) {
        ShowJoystick();
    } else if (keyboard_check_ram()) {
        ShowRamSize();
    } else if (keyboard_check_save()) {
        ShowSaveDictionary();
    } else if (keyboard_check_history_list()) {
        ShowHistoryList();
    } else if (!SaveDictionaryActive()) {
        ShowSaveResult();                       // once a save has finished
    }

    // Spooling runs the Ace faster than real time: draw at most 50 times a second
    u64 nNow = CTimer::GetClockTicks64();
    if (!(SpoolActive() || SaveDictionaryActive()) || nNow - m_nLastRender >= FRAME_MICROS) {
        m_nLastRender = nNow;
        RenderScreen();
    }
}

void CKernel::AttachKeyboard(void)
{
    boolean bUpdated = m_USBHCI.UpdatePlugAndPlay();

    if (bUpdated && m_pKeyboard == nullptr) {
        m_pKeyboard = (CUSBKeyboardDevice *) m_DeviceNameService.GetDevice("ukbd1", FALSE);
        if (m_pKeyboard != nullptr) {
            m_pKeyboard->RegisterRemovedHandler(KeyboardRemovedHandler);
            m_pKeyboard->RegisterKeyStatusHandlerRaw(KeyStatusHandlerRaw);
            m_Logger.Write("xAce", LogNotice, "USB Keyboard attached");
        }
    }
    if (bUpdated) {
        AttachJoystick();
    }
}

void CKernel::AttachJoystick(void)
{
    if (m_pJoystick != nullptr) {
        return;
    }
    CUSBGamePadDevice *pJoystick =
        (CUSBGamePadDevice *) m_DeviceNameService.GetDevice("upad", 1, FALSE);
    if (pJoystick == nullptr) {
        return;
    }
    m_nJoystickProperties = pJoystick->GetProperties();
    const TGamePadState *pState = pJoystick->GetInitialState();
    m_Logger.Write("xAce", LogNotice, "USB joystick attached: %d button(s), %d hat(s), %d axes",
                   pState->nbuttons, pState->nhats, pState->naxes);
    pJoystick->RegisterRemovedHandler(JoystickRemovedHandler);
    m_pJoystick = pJoystick;
    pJoystick->RegisterStatusHandler(JoystickStatusHandler);
}

void CKernel::UpdateKeyboard(void)
{
    AttachKeyboard();

    if (keyboard_check_reset()) {
        ResetAce();
    }

    if (keyboard_check_invert()) {
        m_bInverted = !m_bInverted;
        ClearBorder(COLOR_BORDER);
        m_bScreenDirty = TRUE;
    }
}

// Hard reset (F12): RAM cleared like at power-on, the ROM measures it again
void CKernel::ResetAce(void)
{
    reset_ace = 1;
    memset(mem + 8192, 0xFF, 57344);
    memset(m_VideoRamOld, 0xFF, sizeof(m_VideoRamOld));
    m_bScreenDirty = TRUE;
    keyboard_clear();
    TapeReset();
    m_Logger.Write("xAce", LogNotice, "Reset Ace (%uK RAM)", 3 + RamGetPackKB());
}

void CKernel::RenderScreen(void)
{
    unsigned char *video_ram = mem + 0x2400;
    unsigned char *charset   = mem + 0x2C00;

    // Characters redefined since the last frame must be redrawn wherever they appear
    boolean bCharChanged[128];
    for (int code = 0; code < 128; code++) {
        unsigned char *pNew = charset + code * 8;
        unsigned char *pOld = m_CharsetOld + code * 8;
        bCharChanged[code] = memcmp(pNew, pOld, 8) != 0;
        if (bCharChanged[code]) {
            memcpy(pOld, pNew, 8);
        }
    }

    int cell_idx = 0;
    for (int row = 0; row < ACE_ROWS; row++) {
        for (int col = 0; col < ACE_COLS; col++, cell_idx++) {
            unsigned char c = video_ram[cell_idx];
            unsigned char char_code = c & 0x7F;

            if (c != m_VideoRamOld[cell_idx] || bCharChanged[char_code] || m_bScreenDirty) {
                m_VideoRamOld[cell_idx] = c;

                DrawChar(col, row, charset + (char_code * 8), (c & 0x80) ? TRUE : FALSE);
            }
        }
    }

    m_bScreenDirty = FALSE;
}

void CKernel::DrawChar(int col, int row, const u8 *pBitmap, boolean bInvert)
{
    u32 *pBuffer = (u32 *) m_FrameBuffer.GetBuffer();
    unsigned nPitchWords = m_FrameBuffer.GetPitch() / sizeof(u32);

    int screen_x = OFFSET_X + (col * CHAR_WIDTH);
    int screen_y = OFFSET_Y + (row * CHAR_HEIGHT);

    for (int py = 0; py < 8; py++) {
        unsigned char line_bits = pBitmap[py];
        if (bInvert) {
            line_bits ^= 0xFF;
        }

        for (int px = 0; px < 8; px++) {
            u32 pixel_color = (line_bits & (0x80 >> px)) ? COLOR_INK : COLOR_PAPER;

            int px0 = screen_x + (px * SCALE);
            int py0 = screen_y + (py * SCALE);

            pBuffer[py0 * nPitchWords + px0]           = pixel_color;
            pBuffer[py0 * nPitchWords + px0 + 1]       = pixel_color;
            pBuffer[(py0 + 1) * nPitchWords + px0]     = pixel_color;
            pBuffer[(py0 + 1) * nPitchWords + px0 + 1] = pixel_color;
        }
    }
}

// Draw ASCII text in Ace character cells, clipped to the 32 column screen
void CKernel::DrawText(int col, int row, const char *pText, boolean bInvert)
{
    const u8 *pFont = m_bFontValid ? m_Font : mem + 0x2C00;

    for (; *pText != '\0' && col < ACE_COLS; pText++, col++) {
        unsigned char c = (unsigned char) *pText;
        if (c < 0x20 || c > 0x7E) {
            c = '?';
        }
        DrawChar(col, row, pFont + c * 8, bInvert);
    }
}

// Block until a key is pressed and released; returns its USB HID code
unsigned char CKernel::WaitForKey(void)
{
    WaitForRelease();

    unsigned char ucKey;
    while ((ucKey = keyboard_held_key()) == 0) {
        AttachKeyboard();
        CTimer::SimpleMsDelay(10);
    }

    WaitForRelease();

    return ucKey;
}

void CKernel::WaitForRelease(void)
{
    while (keyboard_held_key() != 0) {
        AttachKeyboard();
        CTimer::SimpleMsDelay(10);
    }
}

// Block until the next key press (with the modifiers held at the time)
void CKernel::GetKeyEvent(unsigned char *pKey, unsigned char *pModifiers)
{
    while (!keyboard_get_event(pKey, pModifiers)) {
        AttachKeyboard();
        CTimer::SimpleMsDelay(10);
    }
}

#define REPEAT_DELAY_MICROS 400000      // hold before a list key repeats
#define REPEAT_MICROS       60000       // then one step every 60 ms

// GetKeyEvent() for the F5 / F6 file lists and the F8 layout list: Up / Down /
// Page Up / Page Down held down repeat
void CKernel::GetListKey(unsigned char *pKey, unsigned char *pModifiers)
{
    for (;;) {
        if (keyboard_get_event(pKey, pModifiers)) {
            boolean bRepeats =    *pKey == HID_KEY_UP || *pKey == HID_KEY_DOWN
                               || *pKey == HID_KEY_PAGEUP || *pKey == HID_KEY_PAGEDOWN;
            m_ucRepeatKey = bRepeats ? *pKey : 0;
            m_nRepeatAt = CTimer::GetClockTicks64() + REPEAT_DELAY_MICROS;
            return;
        }

        if (m_ucRepeatKey != 0) {
            if (keyboard_held_key() != m_ucRepeatKey) {
                m_ucRepeatKey = 0;                  // let go, or another key pressed
            } else if (CTimer::GetClockTicks64() >= m_nRepeatAt) {
                m_nRepeatAt += REPEAT_MICROS;
                *pKey = m_ucRepeatKey;
                *pModifiers = 0;
                return;
            }
        }

        AttachKeyboard();
        CTimer::SimpleMsDelay(10);
    }
}

void CKernel::ClearText(void)
{
    for (int row = 0; row < ACE_ROWS; row++) {
        DrawText(0, row, "                                ", FALSE);
    }
}

// A full-width inverse bar, used for titles and footers
void CKernel::DrawBar(int row, const char *pText)
{
    DrawText(0, row, "                                ", TRUE);
    DrawText(0, row, pText, TRUE);
}

// Leave a host screen: wait for the keys to be let go, then redraw the Ace
void CKernel::ResumeAce(void)
{
    WaitForRelease();
    keyboard_clear();
    keyboard_flush_events();
    keyboard_check_list();          // F3 / F5 / F6 / F7 / F8 / F10 / Ctrl+F4 / Ctrl+F12 pressed while paused are ignored
    keyboard_check_invert();
    keyboard_check_delete();
    keyboard_check_poweroff();
    keyboard_check_layout();
    keyboard_check_joystick();
    keyboard_check_ram();
    keyboard_check_save();
    keyboard_check_history();
    keyboard_check_history_list();
    keyboard_check_enter();         // keys used on the screen are not the Ace's: the line
    keyboard_check_other_key();     // history must not take an Enter here for its own
    m_bScreenDirty = TRUE;
}

#define LIST_FIRST_ROW  1
#define LIST_ROWS       20      // rows 1-20; title on row 0
#define LIST_STATUS_ROW 21
#define LIST_FOOTER_ROW 22      // F5: keys on row 22, the tape in the player on the last row
#define LIST_TAPE_ROW   (ACE_ROWS - 1)
#define LIST_MAX_FILES  256
#define LIST_FOOTER     " ENTER:INSERT/LOAD E:EJECT N:NEW"
#define NEW_TAPE_MAX_INPUT 24   // fits "Name: " + name + cursor on one row

// The letter on a key in the selected keyboard layout (F8), lower case; 0 if none
static char KeyLetter(unsigned char ucKey)
{
    char c = keyboard_hid_to_ascii(ucKey, 0);
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

// One list line: '>' for the tape in the player, the name (up to 24 chars) and the
// size right-aligned in the last 6 columns
static void FormatListLine(char *pLine, const TTapeListEntry *pEntry)
{
    memset(pLine, ' ', ACE_COLS);
    pLine[ACE_COLS] = '\0';
    if (pEntry == nullptr) {
        return;
    }

    if (!pEntry->bSnapshot && !pEntry->bSource && strcasecmp(pEntry->Name, TapeGetInserted()) == 0) {
        pLine[0] = '>';
    }
    size_t nNameLen = strlen(pEntry->Name);
    memcpy(pLine + 1, pEntry->Name, nNameLen < 24 ? nNameLen : 24);

    CString Size;
    Size.Format("%u", pEntry->nSize);
    const char *pSize = Size;
    size_t nSizeLen = strlen(pSize);
    memcpy(pLine + ACE_COLS - nSizeLen, pSize, nSizeLen);
}

// The file list of F5 / F6 on rows LIST_FIRST_ROW.., scrolled to keep the selection on screen
void CKernel::DrawFileList(const TTapeListEntry *pEntries, int nFiles, int *pTop, int nSelected)
{
    if (nSelected < *pTop) {
        *pTop = nSelected;
    } else if (nSelected >= *pTop + LIST_ROWS) {
        *pTop = nSelected - LIST_ROWS + 1;
    }

    for (int i = 0; i < LIST_ROWS; i++) {
        int nIndex = *pTop + i;
        char Line[ACE_COLS + 1];
        FormatListLine(Line, nIndex < nFiles ? &pEntries[nIndex] : nullptr);
        DrawText(0, LIST_FIRST_ROW + i, Line, nIndex == nSelected && nFiles > 0);
    }
    if (nFiles == 0) {
        DrawText(1, LIST_FIRST_ROW, "No tapes, snapshots or sources", FALSE);
    }
}

// Up / Down / Page Up / Page Down / Home / End in a list; FALSE for other keys
static boolean MoveSelection(unsigned char ucKey, int nFiles, int *pSelected)
{
    int nMove = 0;
    switch (ucKey) {
    case HID_KEY_UP:        nMove = -1;             break;
    case HID_KEY_DOWN:      nMove = 1;              break;
    case HID_KEY_PAGEUP:    nMove = -LIST_ROWS;     break;
    case HID_KEY_PAGEDOWN:  nMove = LIST_ROWS;      break;
    case HID_KEY_HOME:      nMove = -nFiles;        break;
    case HID_KEY_END:       nMove = nFiles;         break;
    default:                return FALSE;
    }

    *pSelected += nMove;
    if (*pSelected >= nFiles) *pSelected = nFiles - 1;
    if (*pSelected < 0) *pSelected = 0;

    return TRUE;
}

// Type a name on one row after "Name:"; FALSE if Esc
boolean CKernel::ReadName(int nRow, char *pInput, unsigned nMax)
{
    unsigned nLen = 0;
    pInput[0] = '\0';
    keyboard_flush_events();

    for (;;) {
        DrawText(0, nRow, "                                ", FALSE);
        DrawText(1, nRow, "Name:", FALSE);
        DrawText(7, nRow, pInput, FALSE);
        DrawText(7 + nLen, nRow, " ", TRUE);

        unsigned char ucKey, ucModifiers;
        GetKeyEvent(&ucKey, &ucModifiers);

        if (ucKey == HID_KEY_ESCAPE) {
            return FALSE;
        }
        if (ucKey == HID_KEY_BACKSPACE) {
            if (nLen > 0) {
                pInput[--nLen] = '\0';
            }
        } else if (ucKey == HID_KEY_ENTER || ucKey == HID_KEY_KP_ENTER) {
            if (nLen > 0) {
                return TRUE;
            }
        } else {
            char c = keyboard_hid_to_ascii(ucKey, ucModifiers);
            if (c != 0 && nLen < nMax) {
                pInput[nLen++] = c;
                pInput[nLen] = '\0';
            }
        }
    }
}

// F5: pause the Ace and list the .tap, .tzx and .ace files on the SD card, with the
// tape in the player. Enter inserts the selected tape (LOAD then reads from it) or
// loads the selected .ace snapshot (after Y/N); E ejects the tape, N makes a new one.
void CKernel::ShowTapeList(void)
{
    static TTapeListEntry Entries[LIST_MAX_FILES];
    int nFiles = TapeListFiles(Entries, LIST_MAX_FILES, TRUE);

    ClearText();
    DrawBar(0, " FILES ON SD CARD");
    if (nFiles < 0) {
        DrawText(1, LIST_FIRST_ROW, "No SD card", FALSE);
        DrawBar(ACE_ROWS - 1, " ANY KEY: CLOSE");
        WaitForKey();
        ResumeAce();
        return;
    }

    DrawBar(LIST_FOOTER_ROW, LIST_FOOTER);

    WaitForRelease();               // F5 itself
    keyboard_flush_events();

    int nSelected = 0;
    int nTop = 0;
    boolean bBackToAce = FALSE;     // a tape was inserted: show it, then type LOAD / SAVE
    for (;;) {
        CString Count;
        Count.Format("%d", nFiles);
        DrawBar(0, " FILES ON SD CARD");
        DrawText(ACE_COLS - 1 - (int) Count.GetLength(), 0, Count, TRUE);

        const char *pTape = TapeGetInserted();
        CString Tape;
        Tape.Format("Tape: %s", pTape[0] != '\0' ? pTape : "none");
        DrawText(0, LIST_TAPE_ROW, "                                ", FALSE);
        DrawText(1, LIST_TAPE_ROW, Tape, FALSE);

        DrawFileList(Entries, nFiles, &nTop, nSelected);

        if (bBackToAce) {
            CTimer::SimpleMsDelay(1000);
            break;
        }

        unsigned char ucKey, ucModifiers;
        GetListKey(&ucKey, &ucModifiers);

        if (ucKey == HID_KEY_ESCAPE) {
            break;
        }

        if (MoveSelection(ucKey, nFiles, &nSelected)) {
            DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
            continue;
        }

        if (KeyLetter(ucKey) == 'e') {
            DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
            DrawText(1, LIST_STATUS_ROW, pTape[0] != '\0' ? "Tape ejected" : "No tape to eject", FALSE);
            TapeEject();
            continue;
        }

        if (KeyLetter(ucKey) == 'n') {
            DrawBar(LIST_FOOTER_ROW, " ENTER: CREATE  ESC: CANCEL");
            char Input[NEW_TAPE_MAX_INPUT + 1];
            boolean bName = ReadName(LIST_STATUS_ROW, Input, NEW_TAPE_MAX_INPUT);
            DrawBar(LIST_FOOTER_ROW, LIST_FOOTER);
            DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
            if (!bName) {
                continue;
            }

            char Name[TAPE_LIST_NAME_SIZE];
            TTapeFileResult Result = TapeCreate(Input, Name, sizeof(Name));
            if (Result != TapeFileOK) {
                DrawText(1, LIST_STATUS_ROW,
                         Result == TapeFileExists ? "That file already exists" :
                         Result == TapeFileInvalidName ? "Invalid tape name" :
                         Result == TapeFileNoCard ? "No SD card" : "Could not create the tape", FALSE);
                continue;
            }

            // Show the new tape in the list, selected
            nFiles = TapeListFiles(Entries, LIST_MAX_FILES, TRUE);
            for (int i = 0; i < nFiles; i++) {
                if (strcasecmp(Entries[i].Name, Name) == 0) {
                    nSelected = i;
                }
            }
            DrawText(1, LIST_STATUS_ROW, "New tape in: SAVE / LOAD", TRUE);
            bBackToAce = TRUE;
            continue;
        }

        if ((ucKey != HID_KEY_ENTER && ucKey != HID_KEY_KP_ENTER) || nFiles == 0) {
            continue;
        }

        const char *pName = Entries[nSelected].Name;
        DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
        if (Entries[nSelected].bSource) {
            if (SpoolStart(pName) != SpoolOK) {
                DrawText(1, LIST_STATUS_ROW, "Could not open the file", FALSE);
                continue;
            }
            DrawText(1, LIST_STATUS_ROW, "Spooling: Esc stops", TRUE);
            bBackToAce = TRUE;
            continue;
        }
        if (!Entries[nSelected].bSnapshot) {
            TTapeFileResult Result = TapeInsert(pName);
            if (Result != TapeFileOK) {
                DrawText(1, LIST_STATUS_ROW, Result == TapeFileNoCard ? "No SD card" : "Could not insert the tape", FALSE);
                continue;
            }
            DrawText(1, LIST_STATUS_ROW, "Tape inserted: type LOAD", TRUE);
            bBackToAce = TRUE;
            continue;
        }

        CString Status;
        Status.Format("Load %s?", pName);
        DrawText(1, LIST_STATUS_ROW, Status, TRUE);
        DrawBar(LIST_FOOTER_ROW, " Y: LOAD (UNSAVED WORK IS LOST)");

        do {
            GetKeyEvent(&ucKey, &ucModifiers);
        } while (KeyLetter(ucKey) != 'y' && KeyLetter(ucKey) != 'n' && ucKey != HID_KEY_ESCAPE);

        DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
        DrawBar(LIST_FOOTER_ROW, LIST_FOOTER);
        if (KeyLetter(ucKey) != 'y') {
            DrawText(1, LIST_STATUS_ROW, "Not loaded", FALSE);
            continue;
        }

        TTapeFileResult Result = TapeLoadSnapshot(pName);
        if (Result == TapeFileOK) {
            break;                  // the Ace continues in the snapshot
        }
        DrawText(1, LIST_STATUS_ROW,
                 Result == TapeFileInvalidSnapshot ? "Not a valid .ace file" :
                 Result == TapeFileNoCard ? "No SD card" : "Could not read the file", FALSE);
    }

    ResumeAce();
}

#define DELETE_FOOTER   " ENTER: DELETE  ESC: CLOSE"

// F6: pause the Ace and list the tapes and .ace snapshots on the SD card; Enter
// deletes the selected file after Y/N
void CKernel::ShowDeleteTape(void)
{
    static TTapeListEntry Entries[LIST_MAX_FILES];
    int nFiles = TapeListFiles(Entries, LIST_MAX_FILES, TRUE);

    ClearText();
    DrawBar(0, " DELETE FILE");
    if (nFiles < 0) {
        DrawText(1, LIST_FIRST_ROW, "No SD card", FALSE);
        DrawBar(ACE_ROWS - 1, " ANY KEY: CLOSE");
        WaitForKey();
        ResumeAce();
        return;
    }

    DrawBar(ACE_ROWS - 1, DELETE_FOOTER);

    WaitForRelease();               // F6 itself
    keyboard_flush_events();

    int nSelected = 0;
    int nTop = 0;
    for (;;) {
        CString Count;
        Count.Format("%d", nFiles);
        DrawBar(0, " DELETE FILE");
        DrawText(ACE_COLS - 1 - (int) Count.GetLength(), 0, Count, TRUE);

        DrawFileList(Entries, nFiles, &nTop, nSelected);

        unsigned char ucKey, ucModifiers;
        GetListKey(&ucKey, &ucModifiers);

        if (ucKey == HID_KEY_ESCAPE) {
            break;
        }
        if (MoveSelection(ucKey, nFiles, &nSelected)) {
            DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
            continue;
        }
        if ((ucKey != HID_KEY_ENTER && ucKey != HID_KEY_KP_ENTER) || nFiles == 0) {
            continue;
        }

        char Name[TAPE_LIST_NAME_SIZE];
        strcpy(Name, Entries[nSelected].Name);

        CString Status;
        Status.Format("Delete %s?", Name);
        DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
        DrawText(1, LIST_STATUS_ROW, Status, TRUE);
        DrawBar(ACE_ROWS - 1, " Y: DELETE  N: KEEP");

        do {
            GetKeyEvent(&ucKey, &ucModifiers);
        } while (KeyLetter(ucKey) != 'y' && KeyLetter(ucKey) != 'n' && ucKey != HID_KEY_ESCAPE);

        DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
        DrawBar(ACE_ROWS - 1, DELETE_FOOTER);
        if (KeyLetter(ucKey) != 'y') {
            DrawText(1, LIST_STATUS_ROW, "Not deleted", FALSE);
            continue;
        }

        if (TapeDeleteFile(Name) != TapeFileOK) {
            Status.Format("Could not delete %s", Name);
            DrawText(1, LIST_STATUS_ROW, Status, FALSE);
            continue;
        }

        Status.Format("Deleted %s", Name);
        DrawText(1, LIST_STATUS_ROW, Status, FALSE);
        nFiles = TapeListFiles(Entries, LIST_MAX_FILES, TRUE);
        if (nFiles < 0) {
            nFiles = 0;
        }
        if (nSelected >= nFiles) {
            nSelected = nFiles > 0 ? nFiles - 1 : 0;
        }
    }

    ResumeAce();
}

#define HISTORY_FOOTER  " ENTER: RETYPE  ESC: CLOSE"

// Ctrl+TAB: pause the Ace and list the remembered lines, newest first; Enter puts the
// chosen one back on the input line (as TAB does), Esc closes
void CKernel::ShowHistoryList(void)
{
    const u8 *pFont = m_bFontValid ? m_Font : mem + 0x2C00;
    int nLines = (int) LineHistoryCount();

    ClearText();
    CString Count;
    Count.Format("%d", nLines);
    DrawBar(0, " COMMAND HISTORY");
    DrawText(ACE_COLS - 1 - (int) Count.GetLength(), 0, Count, TRUE);

    if (nLines == 0) {
        DrawText(1, LIST_FIRST_ROW, "No lines entered yet", FALSE);
        DrawBar(ACE_ROWS - 1, " ANY KEY: CLOSE");
        WaitForKey();
        ResumeAce();
        return;
    }

    DrawBar(ACE_ROWS - 1, HISTORY_FOOTER);

    WaitForRelease();               // Ctrl+TAB itself
    keyboard_flush_events();

    int nSelected = 0;
    int nTop = 0;
    int nChosen = -1;
    for (;;) {
        if (nSelected < nTop) {
            nTop = nSelected;
        } else if (nSelected >= nTop + LIST_ROWS) {
            nTop = nSelected - LIST_ROWS + 1;
        }

        // One line per row, as the Ace shows it (inverse characters, graphics); a longer
        // line is cut, with '>' in the last column
        for (int i = 0; i < LIST_ROWS; i++) {
            int nIndex = nTop + i;
            boolean bSelected = nIndex == nSelected;
            unsigned nLength = 0;
            const u8 *pText = nIndex < nLines ? LineHistoryLine(nIndex, &nLength) : nullptr;
            for (int col = 0; col < ACE_COLS; col++) {
                u8 nCode = ' ';
                if (col < (int) nLength) {
                    nCode = col == ACE_COLS - 1 && nLength > ACE_COLS ? '>' : pText[col];
                }
                DrawChar(col, LIST_FIRST_ROW + i, pFont + (nCode & 0x7F) * 8,
                         ((nCode & 0x80) != 0) != bSelected);
            }
        }

        unsigned char ucKey, ucModifiers;
        GetListKey(&ucKey, &ucModifiers);

        if (ucKey == HID_KEY_ESCAPE) {
            break;
        }
        if (MoveSelection(ucKey, nLines, &nSelected)) {
            DrawText(0, LIST_STATUS_ROW, "                                ", FALSE);
            continue;
        }
        if (ucKey == HID_KEY_ENTER || ucKey == HID_KEY_KP_ENTER) {
            nChosen = nSelected;
            break;
        }
    }

    ResumeAce();

    if (nChosen >= 0 && !LineHistoryRecall(nChosen)) {
        m_Logger.Write("xAce", LogWarning, "History: the Ace is not waiting for input");
    }
}

// F7: pause the Ace, ask for a file name and save the dictionary to it as Forth source
void CKernel::ShowSaveDictionary(void)
{
    ClearText();
    DrawBar(0, " SAVE DICTIONARY");
    DrawText(1, 2, "Saves the words you defined", FALSE);
    DrawText(1, 3, "as Forth source in a file on", FALSE);
    DrawText(1, 4, "the SD card: .fth, .fs or .f", FALSE);
    DrawText(1, 5, "(.fth if no extension given).", FALSE);
    DrawText(1, 7, "Spooling the file (F5) into", FALSE);
    DrawText(1, 8, "a fresh Ace defines the same", FALSE);
    DrawText(1, 9, "words again.", FALSE);
    DrawBar(ACE_ROWS - 1, " ENTER: SAVE  ESC: CANCEL");

    WaitForRelease();               // F7 itself

    for (;;) {
        char Input[NEW_TAPE_MAX_INPUT + 1];
        if (!ReadName(11, Input, NEW_TAPE_MAX_INPUT)) {
            break;
        }

        char Name[TAPE_LIST_NAME_SIZE];
        unsigned nWords;
        TSaveResult Result = SaveDictionaryStart(Input, Name, sizeof(Name), &nWords);
        DrawText(0, 13, "                                ", FALSE);
        DrawText(0, 14, "                                ", FALSE);
        if (Result == SaveOK) {
            CString Status;
            Status.Format("Saving %u words: Esc stops", nWords);
            DrawText(1, 13, Status, TRUE);
            CTimer::SimpleMsDelay(1000);
            break;
        }
        DrawText(1, 13, Result == SaveExists ? "That file already exists" :
                        Result == SaveInvalidName ? "Invalid file name" :
                        Result == SaveNoWords ? "No words to save" :
                        Result == SaveNoCard ? "No SD card" :
                        Result == SaveNotAtPrompt ? "The Ace must be at the prompt" :
                                                    "Could not create the file", FALSE);
        if (Result == SaveNotAtPrompt || Result == SaveNoWords) {
            DrawText(1, 14, "Esc: back to the Ace", FALSE);
        }
    }

    ResumeAce();
}

// When a save has finished: say how it went
void CKernel::ShowSaveResult(void)
{
    char Line1[ACE_COLS + 1], Line2[ACE_COLS + 1];
    if (!SaveDictionaryResult(Line1, Line2, sizeof(Line1))) {
        return;
    }

    ClearText();
    DrawBar(0, " SAVE DICTIONARY");
    DrawText(1, 3, Line1, FALSE);
    DrawText(1, 4, Line2, FALSE);
    DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
    WaitForKey();
    ResumeAce();
}

// Like Linux on the Pi 4: watchdog reset with boot partition 63, which the
// firmware treats as "halt" and stays in its low power state until replugged
static void PowerOff(void)
{
#define PM_RSTS_PARTITION_HALT  0x555       // partition 63, spread over the even bits

    PeripheralEntry();

    write32(ARM_PM_RSTS, ARM_PM_PASSWD | (read32(ARM_PM_RSTS) & ARM_PM_RSTS_PART_CLEAR)
                         | PM_RSTS_PARTITION_HALT);
    write32(ARM_PM_WDOG, ARM_PM_PASSWD | 10);
    write32(ARM_PM_RSTC, ARM_PM_PASSWD | (read32(ARM_PM_RSTC) & ARM_PM_RSTC_CLEAR)
                         | ARM_PM_RSTC_REBOOT);

    for (;;);                       // wait for the watchdog
}

#define LAYOUT_FIRST_ROW    2

// F8: pause the Ace and choose the host keyboard layout
void CKernel::ShowKeyboardLayout(void)
{
    int nCount = keyboard_layout_count();
    int nCurrent = keyboard_get_layout();
    int nSelected = nCurrent;

    ClearText();
    DrawBar(0, " KEYBOARD LAYOUT");
    DrawText(1, LAYOUT_FIRST_ROW + nCount + 1, "* = layout in use", FALSE);
    DrawBar(ACE_ROWS - 1, " UP/DOWN  ENTER: OK  ESC: CANCEL");

    WaitForRelease();               // F8 itself
    keyboard_flush_events();

    for (;;) {
        for (int i = 0; i < nCount; i++) {
            CString Line;
            Line.Format(" %c %-14s", i == nCurrent ? '*' : ' ', keyboard_layout_name(i));
            DrawText(1, LAYOUT_FIRST_ROW + i, Line, i == nSelected);
        }

        unsigned char ucKey, ucModifiers;
        GetListKey(&ucKey, &ucModifiers);

        if (ucKey == HID_KEY_ESCAPE) {
            break;
        }
        if (ucKey == HID_KEY_UP) {
            nSelected = (nSelected + nCount - 1) % nCount;
        } else if (ucKey == HID_KEY_DOWN) {
            nSelected = (nSelected + 1) % nCount;
        } else if (ucKey == HID_KEY_ENTER || ucKey == HID_KEY_KP_ENTER) {
            keyboard_set_layout(nSelected);
            m_Logger.Write("xAce", LogNotice, "Keyboard layout: %s", keyboard_layout_name(nSelected));
            break;
        }
    }

    ResumeAce();
}

#define JOYSTICK_FIRST_ROW  2
#define JOYSTICK_ROWS       16      // rows 2-17
#define JOYSTICK_PROMPT_ROW 19

// "Up", "Button 3", or the button's name on a known gamepad ("A", "LB", ...)
void CKernel::JoystickControlName(int nControl, CString *pName)
{
    static const char *s_Directions[4] = {"Up", "Down", "Left", "Right"};
    static const char *s_KnownButtons[32] = {
        "Home", 0, 0, "LT", "RT", "LB", "RB", "Y", "B", "A", "X", "Select",
        "L3", "R3", "Start", 0, 0, 0, 0, "Plus", "Minus", "Touchpad"
    };

    if (nControl < JOYSTICK_BUTTON0) {
        *pName = s_Directions[nControl];
        return;
    }
    int nButton = nControl - JOYSTICK_BUTTON0;
    if ((m_nJoystickProperties & GamePadPropertyIsKnown) && s_KnownButtons[nButton] != 0) {
        pName->Format("Button %s", s_KnownButtons[nButton]);
    } else {
        pName->Format("Button %d", nButton + 1);
    }
}

// The directions, then the buttons with a key (and nSelected, while it gets one)
void CKernel::DrawJoystickControls(int nSelected)
{
    int nRow = JOYSTICK_FIRST_ROW;
    for (int i = 0; i < JOYSTICK_CONTROLS && nRow < JOYSTICK_FIRST_ROW + JOYSTICK_ROWS; i++) {
        int nAceKey = keyboard_joystick_get_key(i);
        if (i >= JOYSTICK_BUTTON0 && nAceKey == 0 && i != nSelected) {
            continue;
        }
        CString Name;
        JoystickControlName(i, &Name);
        char KeyName[16];
        keyboard_ace_key_name(nAceKey, KeyName);
        CString Line;
        Line.Format(" %-15s %-12s", (const char *) Name, KeyName);
        DrawText(1, nRow++, Line, i == nSelected);
    }
    while (nRow < JOYSTICK_FIRST_ROW + JOYSTICK_ROWS) {
        DrawText(0, nRow++, "                                ", FALSE);
    }
}

// The joystick control just pressed or moved, -1 if none
static int JoystickControlPressed(unsigned *pDirections, unsigned *pButtons)
{
    unsigned nDirections = keyboard_joystick_directions();
    unsigned nButtons = keyboard_joystick_buttons();
    unsigned nNewDirections = nDirections & ~*pDirections;
    unsigned nNewButtons = nButtons & ~*pButtons;
    *pDirections = nDirections;
    *pButtons = nButtons;

    for (int i = 0; i < JOYSTICK_BUTTON0; i++) {
        if (nNewDirections & (1u << i)) return i;
    }
    for (int i = 0; i < 32; i++) {
        if (nNewButtons & (1u << i)) return JOYSTICK_BUTTON0 + i;
    }
    return -1;
}

// F3: move the joystick or press one of its buttons, then press the Ace key it is to press
void CKernel::ShowJoystick(void)
{
    ClearText();
    DrawBar(0, " JOYSTICK");

    WaitForRelease();               // F3 itself
    keyboard_flush_events();

    unsigned nDirections = keyboard_joystick_directions();
    unsigned nButtons = keyboard_joystick_buttons();
    int nSelected = -1;

    for (;;) {
        DrawJoystickControls(nSelected);
        if (nSelected < 0) {
            DrawText(1, JOYSTICK_PROMPT_ROW,
                     m_pJoystick != nullptr ? "Move the joystick or press    " : "No joystick connected         ",
                     FALSE);
            DrawText(1, JOYSTICK_PROMPT_ROW + 1,
                     m_pJoystick != nullptr ? "a button to assign a key to it" : "                              ",
                     FALSE);
            DrawBar(ACE_ROWS - 1, " ESC: DONE");
        } else {
            CString Name;
            JoystickControlName(nSelected, &Name);
            CString Line;
            Line.Format("%-30s", (const char *) Name);
            DrawText(1, JOYSTICK_PROMPT_ROW, "Press the Ace key for         ", FALSE);
            DrawText(1, JOYSTICK_PROMPT_ROW + 1, Line, FALSE);
            DrawBar(ACE_ROWS - 1, " BACKSPACE: NONE  ESC: CANCEL");
        }

        // A key, or a joystick control
        unsigned char ucKey = 0, ucModifiers = 0;
        int nControl = -1;
        while (!keyboard_get_event(&ucKey, &ucModifiers)) {
            nControl = JoystickControlPressed(&nDirections, &nButtons);
            if (nControl >= 0) {
                break;
            }
            AttachKeyboard();
            CTimer::SimpleMsDelay(10);
        }

        if (nControl >= 0) {
            nSelected = nControl;
            continue;
        }
        if (nSelected < 0) {
            if (ucKey == HID_KEY_ESCAPE) {
                break;
            }
            continue;
        }

        if (ucKey == HID_KEY_ESCAPE) {
            nSelected = -1;
        } else if (ucKey == HID_KEY_BACKSPACE) {
            keyboard_joystick_set_key(nSelected, 0);
            nSelected = -1;
        } else {
            int nAceKey = keyboard_ace_key_from_hid(ucKey, ucModifiers);
            if (nAceKey != 0) {
                keyboard_joystick_set_key(nSelected, nAceKey);
                char KeyName[16];
                keyboard_ace_key_name(nAceKey, KeyName);
                CString Name;
                JoystickControlName(nSelected, &Name);
                m_Logger.Write("xAce", LogNotice, "Joystick: %s = %s", (const char *) Name, KeyName);
                nSelected = -1;
            }
        }
    }

    ResumeAce();
}

// Ctrl+F12: choose the RAM size; the Ace restarts with it
void CKernel::ShowRamSize(void)
{
    static const unsigned PackKB[] = {0, 16, 32, 48};
    static const char *const Names[] = {
        "3K  (no RAM pack)",
        "19K (16K RAM pack)",
        "35K (32K RAM pack)",
        "51K (48K RAM pack)",
    };
    const int nCount = sizeof(PackKB) / sizeof(PackKB[0]);

    int nCurrent = 0;
    for (int i = 0; i < nCount; i++) {
        if (PackKB[i] == RamGetPackKB()) {
            nCurrent = i;
        }
    }
    int nSelected = nCurrent;

    ClearText();
    DrawBar(0, " RAM SIZE");
    DrawText(1, LAYOUT_FIRST_ROW + nCount + 1, "* = RAM in use", FALSE);
    DrawText(1, LAYOUT_FIRST_ROW + nCount + 3, "Enter restarts the Ace:", FALSE);
    DrawText(1, LAYOUT_FIRST_ROW + nCount + 4, "anything not saved is lost.", FALSE);
    DrawBar(ACE_ROWS - 1, " UP/DOWN  ENTER: OK  ESC: CANCEL");

    WaitForRelease();               // Ctrl+F12 itself
    keyboard_flush_events();

    boolean bChosen = FALSE;
    for (;;) {
        for (int i = 0; i < nCount; i++) {
            CString Line;
            Line.Format(" %c %-20s", i == nCurrent ? '*' : ' ', Names[i]);
            DrawText(1, LAYOUT_FIRST_ROW + i, Line, i == nSelected);
        }

        unsigned char ucKey, ucModifiers;
        GetKeyEvent(&ucKey, &ucModifiers);

        if (ucKey == HID_KEY_ESCAPE) {
            break;
        }
        if (ucKey == HID_KEY_UP) {
            nSelected = (nSelected + nCount - 1) % nCount;
        } else if (ucKey == HID_KEY_DOWN) {
            nSelected = (nSelected + 1) % nCount;
        } else if (ucKey == HID_KEY_ENTER || ucKey == HID_KEY_KP_ENTER) {
            bChosen = TRUE;
            break;
        }
    }

    ResumeAce();

    if (bChosen) {
        RamSetPackKB(PackKB[nSelected]);
        ResetAce();
    }
}

// F10: pause the Ace and power off the Raspberry Pi after confirmation
void CKernel::ShowPowerOff(void)
{
    ClearText();
    DrawBar(0, " POWER OFF");
    DrawText(1, 3, "Power off the Raspberry Pi?", FALSE);
    DrawText(1, 5, "Anything not saved to tape", FALSE);
    DrawText(1, 6, "will be lost.", FALSE);
    DrawBar(ACE_ROWS - 1, " Y: POWER OFF  N: CANCEL");

    WaitForRelease();               // F10 itself
    keyboard_flush_events();

    unsigned char ucKey, ucModifiers;
    do {
        GetKeyEvent(&ucKey, &ucModifiers);
    } while (KeyLetter(ucKey) != 'y' && KeyLetter(ucKey) != 'n' && ucKey != HID_KEY_ESCAPE);

    if (KeyLetter(ucKey) != 'y') {
        ResumeAce();
        return;
    }

    m_Logger.Write("xAce", LogNotice, "Power off");
    TapeShutdown();

    ClearText();
    DrawText(1, 11, "It is now safe to unplug", FALSE);
    DrawText(1, 12, "the Raspberry Pi.", FALSE);
    CTimer::SimpleMsDelay(1000);    // let the message be seen before the screen goes

    PowerOff();
}

// Z80 Hooks Implementation
extern "C" {

// The Ace decodes port 0xFE on A0 alone
unsigned int in(int h, int l)
{
    if (!(l & 0x01)) {
        // Reading the keyboard also pulls the speaker low
        if (CKernel::Get() != nullptr) {
            CKernel::Get()->GetSound()->Speaker(0, tstates);
        }

        unsigned char result = 0xFF;
        for (int i = 0; i < 8; i++) {
            if (!(h & (1 << i))) {
                result &= keyboard_get_keyport(i);
            }
        }
        return result;
    }
    return 0xFF;
}

unsigned int out(int h, int l, int a)
{
    // Any write to port 0xFE drives the speaker high
    if (!(l & 0x01) && CKernel::Get() != nullptr) {
        CKernel::Get()->GetSound()->Speaker(1, tstates);
    }
    return 0;
}

void fix_tstates(void)
{
    // Keep the overshoot of the last instruction in the next frame
    tstates -= tsmax;

    if (CKernel::Get() != nullptr) {
        CKernel::Get()->Frame(tsmax);
    }

    // Pace execution to the Ace frame rate (not while spooling or saving: they run flat out)
    u64 nCurrent = CTimer::GetClockTicks64();
    if (SpoolActive() || SaveDictionaryActive()) {
        s_nNextFrameMicros = nCurrent + FRAME_MICROS;
    } else if (nCurrent < s_nNextFrameMicros) {
        CTimer::SimpleusDelay(s_nNextFrameMicros - nCurrent);
        s_nNextFrameMicros += FRAME_MICROS;
    } else if (nCurrent - s_nNextFrameMicros > FRAME_MICROS) {
        // Fell more than a frame behind (SD access, USB enumeration): don't race to catch up
        s_nNextFrameMicros = nCurrent + FRAME_MICROS;
    } else {
        s_nNextFrameMicros += FRAME_MICROS;
    }

    // The new frame starts with the vsync interrupt (see z80.c)
}

void do_interrupt(void)
{
    // Host work happens in fix_tstates() every frame, even with interrupts disabled
}

}
