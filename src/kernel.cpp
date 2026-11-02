#include "kernel.h"
#include <circle/machineinfo.h>
#include "keyboard_circle.h"
#include "tape_sd.h"
#include "ram_ace.h"
#include "spool_ace.h"
#include "save_dictionary.h"
#include "line_history.h"
#include "web_files.h"
#include "wifi_settings.h"
#include "update_sd.h"
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
#define HID_KEY_SPACE       0x2C
#define HID_KEY_PAGEUP      0x4B
#define HID_KEY_END         0x4D
#define HID_KEY_PAGEDOWN    0x4E
#define HID_KEY_DELETE      0x4C
#define HID_KEY_RIGHT       0x4F
#define HID_KEY_LEFT        0x50

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

// The Pi 400's one LED is its power light: on all the time, as in Raspberry Pi OS (whose
// device tree calls it PWR, "default-on"). The other models show SD card access on the
// activity LED; their power LED is lit by the hardware.
static boolean IsPi400(void)
{
    return CMachineInfo::Get()->GetMachineModel() == MachineModel400;
}

CKernel::CKernel(void)
:   m_FrameBuffer(FB_WIDTH, FB_HEIGHT, FB_BPP),
    m_Timer(&m_Interrupt),
    m_Logger(m_Options.GetLogLevel(), &m_Timer),
    m_USBHCI(&m_Interrupt, &m_Timer, TRUE),
    m_EMMC(&m_Interrupt, &m_Timer, IsPi400() ? nullptr : &m_ActLED),
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
    if (IsPi400()) {
        m_ActLED.On();              // power light
    } else {
        m_ActLED.Blink(5);          // show we are alive
    }
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
    m_Logger.Write("xAce", LogNotice, "xAce-baremetal %s for the Raspberry Pi %u, built %s",
                   UpdateVersion(), UpdateModel(), UpdateBuild());

    if (m_EMMC.Initialize()) {
        if (TapeInitialize()) {
            // A system update being tried (F11): keep it or undo it
            UpdateStartup(m_Options.GetAppOptionString("xacetrial") != nullptr);
        }
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
        ShowSave();
    } else if (keyboard_check_history_list()) {
        ShowHistoryList();
    } else if (keyboard_check_network()) {
        ShowNetwork();
    } else if (UpdateFrame(&m_UpdateLine1, &m_UpdateLine2)) {
        ShowUpdateResult();                     // a system update kept or undone
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
    RamReset();                     // the 96K pack: page 0, the other one cleared too
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

// Block until the next key press (with the modifiers held at the time). With nUntil
// (CTimer::GetClockTicks64() time), key 0 when it comes first.
void CKernel::GetKeyEvent(unsigned char *pKey, unsigned char *pModifiers, u64 nUntil)
{
    while (!keyboard_get_event(pKey, pModifiers)) {
        if (nUntil != 0 && CTimer::GetClockTicks64() >= nUntil) {
            *pKey = 0;
            *pModifiers = 0;
            return;
        }
        AttachKeyboard();
        CTimer::SimpleMsDelay(10);
    }
}

#define REPEAT_DELAY_MICROS 400000      // hold before a list key repeats
#define REPEAT_MICROS       60000       // then one step every 60 ms

// GetKeyEvent() for the F5 / F6 file lists and the F8 layout list: Up / Down /
// Page Up / Page Down (and Space, which scrolls a long name) held down repeat. With
// nUntil (CTimer::GetClockTicks64() time), key 0 when it comes first.
void CKernel::GetListKey(unsigned char *pKey, unsigned char *pModifiers, u64 nUntil)
{
    for (;;) {
        if (keyboard_get_event(pKey, pModifiers)) {
            boolean bRepeats =    *pKey == HID_KEY_UP || *pKey == HID_KEY_DOWN
                               || *pKey == HID_KEY_PAGEUP || *pKey == HID_KEY_PAGEDOWN
                               || *pKey == HID_KEY_SPACE;
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

        if (nUntil != 0 && CTimer::GetClockTicks64() >= nUntil) {
            *pKey = 0;
            *pModifiers = 0;
            return;
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
    keyboard_check_list();          // F3 / F5 / F6 / F7 / F8 / F10 / F11 / Ctrl+F4 / Ctrl+F12 pressed while paused are ignored
    keyboard_check_invert();
    keyboard_check_delete();
    keyboard_check_poweroff();
    keyboard_check_layout();
    keyboard_check_joystick();
    keyboard_check_ram();
    keyboard_check_save();
    keyboard_check_network();
    keyboard_check_history();
    keyboard_check_history_list();
    keyboard_check_enter();         // keys used on the screen are not the Ace's: the line
    keyboard_check_other_key();     // history must not take an Enter here for its own
    m_bScreenDirty = TRUE;
}

#define LIST_FIRST_ROW  2
#define LIST_ROWS       19      // rows 2-20; title on row 0, row 1 left blank
#define LIST_STATUS_ROW 21
#define LIST_FOOTER_ROW 22      // F5: keys on row 22, the tape in the player on the last row
#define LIST_TAPE_ROW   (ACE_ROWS - 1)
#define LIST_MAX_FILES  256
#define LIST_FOOTER     " ENTER:OPEN E:EJECT N:TAPE M:DIR"
#define NEW_TAPE_MAX_INPUT 24   // fits "Name: " + name + cursor on one row

// The letter on a key in the selected keyboard layout (F8), lower case; 0 if none
static char KeyLetter(unsigned char ucKey)
{
    char c = keyboard_hid_to_ascii(ucKey, 0);
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/*
 * A name too long for its row scrolls when it is selected (F5, F6, Ctrl+TAB): after
 * a moment it moves one character at a time to its end, waits, and starts again.
 * Space moves it one character by hand (back to the start after the end), and then
 * it stays where Space left it.
 */
#define SCROLL_START_MICROS 1000000     // still before scrolling, and back at the start
#define SCROLL_STEP_MICROS  150000      // one character
#define SCROLL_END_MICROS   1000000     // at the end

struct TNameScroll {
    int      nIndex;            // the entry, -1 for none yet
    unsigned nMax;              // characters that do not fit (0: nothing to scroll)
    unsigned nOffset;           // characters scrolled out
    u64      nNext;             // next step (CTimer::GetClockTicks64())
    boolean  bManual;           // Space pressed: no more steps by themselves
};

// The selected entry (nIndex), with nMax characters that do not fit: back to its start
// when it is another one
static void ScrollSelect(TNameScroll *pScroll, int nIndex, unsigned nMax)
{
    if (nIndex == pScroll->nIndex && nMax == pScroll->nMax) {
        return;
    }
    pScroll->nIndex = nIndex;
    pScroll->nMax = nMax;
    pScroll->nOffset = 0;
    pScroll->bManual = FALSE;
    pScroll->nNext = CTimer::GetClockTicks64() + SCROLL_START_MICROS;
}

// Until when GetListKey() waits for a key (0: for ever)
static u64 ScrollUntil(const TNameScroll *pScroll)
{
    return pScroll->nMax == 0 || pScroll->bManual ? 0 : pScroll->nNext;
}

// GetListKey() gave ucKey: TRUE if it was for the scrolling (time for a step, or Space;
// the row is then drawn again)
static boolean ScrollKey(TNameScroll *pScroll, unsigned char ucKey)
{
    if (ucKey == HID_KEY_SPACE) {
        if (pScroll->nMax > 0) {
            pScroll->bManual = TRUE;
            pScroll->nOffset = pScroll->nOffset >= pScroll->nMax ? 0 : pScroll->nOffset + 1;
        }
        return TRUE;
    }
    if (ucKey != 0) {
        return FALSE;
    }

    u64 nNow = CTimer::GetClockTicks64();
    if (pScroll->nMax > 0 && !pScroll->bManual && nNow >= pScroll->nNext) {
        unsigned nWait;
        if (pScroll->nOffset < pScroll->nMax) {
            pScroll->nOffset++;
            nWait = pScroll->nOffset == pScroll->nMax ? SCROLL_END_MICROS : SCROLL_STEP_MICROS;
        } else {
            pScroll->nOffset = 0;
            nWait = SCROLL_START_MICROS;
        }
        pScroll->nNext += nWait;
        if (pScroll->nNext < nNow) {
            pScroll->nNext = nNow + nWait;      // late (busy with a key): no catching up
        }
    }
    return TRUE;
}

/*
 * A folder path too long for its row (the F5 / F6 title bar, the F7 folder line) is
 * shown as its end, "..." first. It scrolls the same way, towards the start of the
 * path, but Space does not move it (Space types in F7). nIndex is then TextId().
 */

// The earlier of two ScrollUntil() times (0: for ever)
static u64 ScrollUntil(const TNameScroll *pScroll, const TNameScroll *pOther)
{
    u64 nUntil = ScrollUntil(pScroll), nOther = ScrollUntil(pOther);
    return nUntil == 0 || (nOther != 0 && nOther < nUntil) ? nOther : nUntil;
}

// Tells texts apart for ScrollSelect() (a hash, never -1)
static int TextId(const char *pText)
{
    unsigned nHash = 5381;
    while (*pText != '\0') {
        nHash = nHash * 33 + (unsigned char) *pText++;
    }
    return (int) (nHash & 0x7FFFFFFF);
}

// Room for a name in a list line: 24 columns (23 and '/' for a folder or .zip)
static unsigned ListNameWidth(const TTapeListEntry *pEntry)
{
    return pEntry->bFolder && strcmp(pEntry->Name, "..") != 0 ? 23 : 24;
}

// Characters of the entry's name that do not fit
static unsigned ListNameOverflow(const TTapeListEntry *pEntry)
{
    unsigned nLen = strlen(pEntry->Name), nWidth = ListNameWidth(pEntry);
    return nLen > nWidth ? nLen - nWidth : 0;
}

// One list line: '>' for the tape in the player, the name (up to 24 chars, from
// character nOffset when it scrolls; folders end in /) and the size right-aligned in the
// last 6 columns
static void FormatListLine(char *pLine, const TTapeListEntry *pEntry, const char *pFolder,
                           unsigned nOffset = 0)
{
    memset(pLine, ' ', ACE_COLS);
    pLine[ACE_COLS] = '\0';
    if (pEntry == nullptr) {
        return;
    }

    if (!pEntry->bFolder && !pEntry->bSnapshot && !pEntry->bSource) {
        char Path[TAPE_PATH_SIZE];
        if (   TapeJoinPath(Path, pFolder, pEntry->Name)
            && strcasecmp(Path, TapeGetInserted()) == 0) {
            pLine[0] = '>';
        }
    }
    if (nOffset > ListNameOverflow(pEntry)) {
        nOffset = ListNameOverflow(pEntry);
    }
    const char *pName = pEntry->Name + nOffset;
    size_t nNameLen = strlen(pName);
    size_t nWidth = ListNameWidth(pEntry);
    if (nNameLen > nWidth) {
        nNameLen = nWidth;
    }
    memcpy(pLine + 1, pName, nNameLen);
    if (pEntry->bFolder && strcmp(pEntry->Name, "..") != 0) {
        pLine[1 + nNameLen] = '/';      // opens like a folder (a .zip too)
        if (!pEntry->bZip) {
            return;
        }
    }

    CString Size;
    Size.Format("%u", pEntry->nSize);
    if (pEntry->bFolder && !pEntry->bZip) {
        Size = "";
    }
    const char *pSize = Size;
    size_t nSizeLen = strlen(pSize);
    memcpy(pLine + ACE_COLS - nSizeLen, pSize, nSizeLen);
}

// The file list of F5 / F6 on rows LIST_FIRST_ROW.., scrolled to keep the selection on screen
// (the selected name from character nOffset, when it scrolls)
void CKernel::DrawFileList(const TTapeListEntry *pEntries, int nFiles, int *pTop, int nSelected,
                           unsigned nOffset)
{
    if (nSelected < *pTop) {
        *pTop = nSelected;
    } else if (nSelected >= *pTop + LIST_ROWS) {
        *pTop = nSelected - LIST_ROWS + 1;
    }

    const char *pFolder = TapeGetFolder();      // checks the card: once, not for each row
    for (int i = 0; i < LIST_ROWS; i++) {
        int nIndex = *pTop + i;
        DrawFileRow(pEntries, nFiles, *pTop, nIndex, pFolder, nIndex == nSelected && nFiles > 0,
                    nIndex == nSelected ? nOffset : 0);
    }
    if (nFiles == 0) {
        DrawText(1, LIST_FIRST_ROW, "No tapes, snapshots or sources", FALSE);
    } else if (nFiles == 1 && strcmp(pEntries[0].Name, "..") == 0) {
        DrawText(1, LIST_FIRST_ROW + 1, "Empty folder", FALSE);
    }
}

// The row of entry nIndex in the F5 / F6 list (pFolder: TapeGetFolder())
void CKernel::DrawFileRow(const TTapeListEntry *pEntries, int nFiles, int nTop, int nIndex,
                          const char *pFolder, boolean bSelected, unsigned nOffset)
{
    char Line[ACE_COLS + 1];
    FormatListLine(Line, nIndex < nFiles ? &pEntries[nIndex] : nullptr, pFolder, nOffset);
    DrawText(0, LIST_FIRST_ROW + nIndex - nTop, Line, bSelected);
}

// GetListKey() for F5 / F6: meanwhile the selected name scrolls (and Space scrolls it),
// and the folder in the title bar (pTitle, with pTitleScroll). After another key the
// name scrolling starts again (the list may have changed); the title carries on while
// the folder stays the same.
void CKernel::GetScrolledListKey(unsigned char *pKey, unsigned char *pModifiers,
                                 TNameScroll *pScroll, const TTapeListEntry *pEntries,
                                 int nFiles, int nTop, int nSelected,
                                 const char *pTitle, TNameScroll *pTitleScroll)
{
    const char *pFolder = TapeGetFolder();
    for (;;) {
        GetListKey(pKey, pModifiers, ScrollUntil(pScroll, pTitleScroll));
        if (!ScrollKey(pScroll, *pKey)) {
            break;
        }
        if (*pKey == 0) {
            unsigned nTitleOffset = pTitleScroll->nOffset;
            ScrollKey(pTitleScroll, 0);
            if (pTitleScroll->nOffset != nTitleOffset) {
                DrawListTitle(pTitle, nFiles, pTitleScroll);
            }
        }
        if (nFiles > 0) {
            DrawFileRow(pEntries, nFiles, nTop, nSelected, pFolder, TRUE, pScroll->nOffset);
        }
    }
    pScroll->nIndex = -1;
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

// Type a name on one row after "Name:"; FALSE if Esc. Meanwhile the F7 folder line
// scrolls with pFolderScroll.
boolean CKernel::ReadName(int nRow, char *pInput, unsigned nMax, TNameScroll *pFolderScroll)
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
        for (;;) {
            GetKeyEvent(&ucKey, &ucModifiers,
                        pFolderScroll != nullptr ? ScrollUntil(pFolderScroll) : 0);
            if (ucKey != 0) {
                break;
            }
            ScrollKey(pFolderScroll, 0);
            DrawSaveFolder(pFolderScroll);
        }

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

// "programs/<folder>" (the F5 folder unless given), shortened to nWidth characters:
// its end, or nOffset characters scrolled towards its start ("..." first until there).
// Gives the most nOffset can be (0 if it fits), and if wanted a TextId() of the path.
static unsigned FolderTitle(unsigned nWidth, char *pOut, const char *pFolder = nullptr,
                            unsigned nOffset = 0, int *pId = nullptr)
{
    if (pFolder == nullptr) {
        pFolder = TapeGetFolder();
    }
    char Path[TAPE_PATH_SIZE + sizeof(TAPE_FOLDER)];
    strcpy(Path, TAPE_FOLDER);
    if (pFolder[0] != '\0') {
        strcat(Path, "/");
        strcat(Path, pFolder);
    }
    if (pId != nullptr) {
        *pId = TextId(Path);
    }

    unsigned nLen = strlen(Path);
    if (nLen <= nWidth) {
        strcpy(pOut, Path);
        return 0;
    }
    unsigned nMax = nLen - nWidth;
    if (nOffset >= nMax) {
        memcpy(pOut, Path, nWidth);                 // the start
        pOut[nWidth] = '\0';
    } else {
        unsigned nStart = nMax - nOffset;           // the end when nOffset is 0
        strcpy(pOut, "...");
        memcpy(pOut + 3, Path + nStart + 3, nWidth - 3);
        pOut[nWidth] = '\0';
    }
    return nMax;
}

// Is the F5 folder a .zip, or in one? (read only)
static boolean FolderInZip(void)
{
    return strcmp(TapeGetSaveFolder(), TapeGetFolder()) != 0;
}

// What went wrong reading a file
static const char *ReadProblem(TTapeFileResult Result, const char *pDefault)
{
    return Result == TapeFileNoCard ? "No SD card" :
           Result == TapeFileTooBig ? "Too big (4 MB at most in .zip)" :
           Result == TapeFileZipUnsupported ? "Cannot unpack it from the .zip" : pDefault;
}

// Title bar of F5 / F6: pTitle, the folder (scrolling with pScroll) and the number of
// entries
void CKernel::DrawListTitle(const char *pTitle, int nFiles, TNameScroll *pScroll)
{
    CString Count;
    Count.Format("%d", nFiles);
    unsigned nTitle = strlen(pTitle);
    unsigned nWidth = ACE_COLS - 3 - nTitle - Count.GetLength();
    char Folder[ACE_COLS + 1];
    int nId;
    unsigned nMax = FolderTitle(nWidth, Folder, nullptr, 0, &nId);
    if (pScroll != nullptr) {
        ScrollSelect(pScroll, nId, nMax);
        if (pScroll->nOffset > 0) {
            FolderTitle(nWidth, Folder, nullptr, pScroll->nOffset);
        }
    }
    CString Bar;
    Bar.Format(" %s%s", pTitle, Folder);
    DrawBar(0, Bar);
    DrawText(ACE_COLS - 1 - (int) Count.GetLength(), 0, Count, TRUE);
}

// List the F5 folder into pEntries: ".." first below programs, then its folders and
// files. *pSelected gets the entry named pSelect (if any), else 0. -1 if no SD card.
int CKernel::ListFolder(TTapeListEntry *pEntries, const char *pSelect, int *pSelected)
{
    const char *pFolder = TapeGetFolder();
    int nParent = pFolder[0] != '\0' ? 1 : 0;
    int nFiles = TapeListFiles(pFolder, pEntries + nParent, LIST_MAX_FILES - nParent, TRUE);
    if (nFiles < 0) {
        return -1;
    }
    if (nParent) {
        memset(&pEntries[0], 0, sizeof pEntries[0]);
        strcpy(pEntries[0].Name, "..");
        pEntries[0].bFolder = TRUE;
    }
    nFiles += nParent;

    *pSelected = 0;
    for (int i = 0; pSelect != nullptr && i < nFiles; i++) {
        if (strcasecmp(pEntries[i].Name, pSelect) == 0) {
            *pSelected = i;
        }
    }
    return nFiles;
}

// Open the folder pName of the F5 folder, or the one above for ".." (selecting the
// folder left); the list is read again. FALSE if it cannot be opened.
boolean CKernel::OpenFolder(const char *pName, TTapeListEntry *pEntries, int *pFiles,
                            int *pSelected, int *pTop)
{
    char Folder[TAPE_PATH_SIZE], Left[TAPE_LIST_NAME_SIZE] = "";
    if (strcmp(pName, "..") == 0) {
        if (TapeGetFolder()[0] == '\0') {
            return FALSE;
        }
        strcpy(Left, TapeBaseName(TapeGetFolder()));
        TapeParentFolder(TapeGetFolder(), Folder);
    } else if (!TapeJoinPath(Folder, TapeGetFolder(), pName)) {
        return FALSE;
    }

    char Old[TAPE_PATH_SIZE];
    strcpy(Old, TapeGetFolder());
    TapeSetFolder(Folder);
    int nFiles = ListFolder(pEntries, Left, pSelected);
    if (nFiles < 0) {
        TapeSetFolder(Old);             // cannot be read: stay
        ListFolder(pEntries, pName, pSelected);
        return FALSE;
    }
    *pFiles = nFiles;
    *pTop = 0;
    return TRUE;
}

// The path of the tape in the player, or its name if the path does not fit
static void TapeLine(CString *pLine)
{
    const char *pTape = TapeGetInserted();
    pLine->Format("Tape: %s", pTape[0] == '\0' ? "none" :
                              strlen(pTape) <= ACE_COLS - 8 ? pTape : TapeBaseName(pTape));
}

// F5: pause the Ace and list the folders, .tap, .tzx and .ace files and Forth sources
// in the F5 folder (in SD:/programs), with the tape in the player. Enter opens a folder
// (Backspace goes up), inserts the selected tape (LOAD then reads from it), spools a
// source or loads an .ace snapshot (after Y/N); E ejects the tape, N makes a new one,
// M a new folder. New files (SAVE with no tape, F7) go into the folder left open.
void CKernel::ShowTapeList(void)
{
    static TTapeListEntry Entries[LIST_MAX_FILES];
    int nSelected = 0;
    int nFiles = ListFolder(Entries, nullptr, &nSelected);

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

    int nTop = 0;
    boolean bBackToAce = FALSE;     // a tape was inserted: show it, then type LOAD / SAVE
    TNameScroll Scroll = {-1, 0, 0, 0, FALSE};
    TNameScroll TitleScroll = {-1, 0, 0, 0, FALSE};
    for (;;) {
        DrawListTitle("", nFiles, &TitleScroll);

        CString Tape;
        TapeLine(&Tape);
        DrawText(0, LIST_TAPE_ROW, "                                ", FALSE);
        DrawText(1, LIST_TAPE_ROW, Tape, FALSE);

        ScrollSelect(&Scroll, nSelected, nFiles > 0 ? ListNameOverflow(&Entries[nSelected]) : 0);
        DrawFileList(Entries, nFiles, &nTop, nSelected, Scroll.nOffset);

        if (bBackToAce) {
            CTimer::SimpleMsDelay(1000);
            break;
        }

        unsigned char ucKey, ucModifiers;
        GetScrolledListKey(&ucKey, &ucModifiers, &Scroll, Entries, nFiles, nTop, nSelected,
                           "", &TitleScroll);

        if (ucKey == HID_KEY_ESCAPE) {
            break;
        }

        if (MoveSelection(ucKey, nFiles, &nSelected)) {
            ClearTextRow(LIST_STATUS_ROW);
            continue;
        }

        if (ucKey == HID_KEY_BACKSPACE || ucKey == HID_KEY_LEFT) {
            ClearTextRow(LIST_STATUS_ROW);
            OpenFolder("..", Entries, &nFiles, &nSelected, &nTop);
            continue;
        }

        if (KeyLetter(ucKey) == 'e') {
            ClearTextRow(LIST_STATUS_ROW);
            DrawText(1, LIST_STATUS_ROW, TapeGetInserted()[0] != '\0' ? "Tape ejected" : "No tape to eject", FALSE);
            TapeEject();
            continue;
        }

        if ((KeyLetter(ucKey) == 'n' || KeyLetter(ucKey) == 'm') && FolderInZip()) {
            ClearTextRow(LIST_STATUS_ROW);
            DrawText(1, LIST_STATUS_ROW, "A .zip is read only", FALSE);
            continue;
        }

        if (KeyLetter(ucKey) == 'n' || KeyLetter(ucKey) == 'm') {
            boolean bFolder = KeyLetter(ucKey) == 'm';
            DrawBar(LIST_FOOTER_ROW, bFolder ? " NEW FOLDER: ENTER  ESC: CANCEL" : " NEW TAPE: ENTER  ESC: CANCEL");
            char Input[NEW_TAPE_MAX_INPUT + 1];
            boolean bName = ReadName(LIST_STATUS_ROW, Input, NEW_TAPE_MAX_INPUT);
            DrawBar(LIST_FOOTER_ROW, LIST_FOOTER);
            ClearTextRow(LIST_STATUS_ROW);
            if (!bName) {
                continue;
            }

            char Path[TAPE_PATH_SIZE];
            TTapeFileResult Result;
            if (bFolder) {
                Result = TapeJoinPath(Path, TapeGetFolder(), Input) ? TapeMakeFolder(Path)
                                                                    : TapeFileInvalidName;
            } else {
                Result = TapeCreate(Input, Path);
            }
            if (Result != TapeFileOK) {
                DrawText(1, LIST_STATUS_ROW,
                         Result == TapeFileExists ? "That name is already taken" :
                         Result == TapeFileInvalidName ? (bFolder ? "Invalid folder name" : "Invalid tape name") :
                         Result == TapeFileNoCard ? "No SD card" :
                         bFolder ? "Could not make the folder" : "Could not create the tape", FALSE);
                continue;
            }

            // Show the new tape or folder in the list, selected
            nFiles = ListFolder(Entries, bFolder ? Input : TapeBaseName(Path), &nSelected);
            if (nFiles < 0) {
                nFiles = 0;
            }
            if (bFolder) {
                DrawText(1, LIST_STATUS_ROW, "Folder made", FALSE);
                continue;
            }
            DrawText(1, LIST_STATUS_ROW, "New tape in: SAVE / LOAD", TRUE);
            bBackToAce = TRUE;
            continue;
        }

        if ((ucKey != HID_KEY_ENTER && ucKey != HID_KEY_KP_ENTER && ucKey != HID_KEY_RIGHT)
            || nFiles == 0) {
            continue;
        }

        ClearTextRow(LIST_STATUS_ROW);
        if (Entries[nSelected].bFolder) {
            char Name[TAPE_LIST_NAME_SIZE];
            strcpy(Name, Entries[nSelected].Name);
            if (!OpenFolder(Name, Entries, &nFiles, &nSelected, &nTop)) {
                DrawText(1, LIST_STATUS_ROW, "Could not open the folder", FALSE);
            }
            continue;
        }
        if (ucKey == HID_KEY_RIGHT) {
            continue;                   // only opens folders
        }

        char Path[TAPE_PATH_SIZE];
        if (!TapeJoinPath(Path, TapeGetFolder(), Entries[nSelected].Name)) {
            DrawText(1, LIST_STATUS_ROW, "The path is too long", FALSE);
            continue;
        }
        if (Entries[nSelected].bSource) {
            if (SpoolStart(Path) != SpoolOK) {
                DrawText(1, LIST_STATUS_ROW, "Could not open the file", FALSE);
                continue;
            }
            DrawText(1, LIST_STATUS_ROW, "Spooling: Esc stops", TRUE);
            bBackToAce = TRUE;
            continue;
        }
        if (!Entries[nSelected].bSnapshot) {
            TTapeFileResult Result = TapeInsert(Path);
            if (Result != TapeFileOK) {
                DrawText(1, LIST_STATUS_ROW, ReadProblem(Result, "Could not insert the tape"), FALSE);
                continue;
            }
            DrawText(1, LIST_STATUS_ROW, "Tape inserted: type LOAD", TRUE);
            bBackToAce = TRUE;
            continue;
        }

        CString Status;
        Status.Format("Load %s?", Entries[nSelected].Name);
        DrawText(1, LIST_STATUS_ROW, Status, TRUE);
        DrawBar(LIST_FOOTER_ROW, " Y: LOAD (UNSAVED WORK IS LOST)");

        do {
            GetKeyEvent(&ucKey, &ucModifiers);
        } while (KeyLetter(ucKey) != 'y' && KeyLetter(ucKey) != 'n' && ucKey != HID_KEY_ESCAPE);

        ClearTextRow(LIST_STATUS_ROW);
        DrawBar(LIST_FOOTER_ROW, LIST_FOOTER);
        if (KeyLetter(ucKey) != 'y') {
            DrawText(1, LIST_STATUS_ROW, "Not loaded", FALSE);
            continue;
        }

        TTapeFileResult Result = TapeLoadSnapshot(Path);
        if (Result == TapeFileOK) {
            break;                  // the Ace continues in the snapshot
        }
        DrawText(1, LIST_STATUS_ROW,
                 Result == TapeFileInvalidSnapshot ? "Not a valid .ace file" :
                 ReadProblem(Result, "Could not read the file"), FALSE);
    }

    ResumeAce();
}

void CKernel::ClearTextRow(int nRow)
{
    DrawText(0, nRow, "                                ", FALSE);
}

#define DELETE_FOOTER   " ENTER:OPEN/DEL D:DEL  BKSP:UP"
#define DELETE_INFO_ROW (LIST_STATUS_ROW + 1)

// F6: pause the Ace and list the folders, tapes, snapshots and sources in the F5 folder,
// to delete them after Y/N: Enter opens a folder (Backspace goes up) or deletes a file;
// D deletes the selected file, or folder with all its files and subfolders
void CKernel::ShowDeleteTape(void)
{
    // F6 browses from the F5 folder, but leaves it as it was (where new files go)
    static char F5Folder[TAPE_PATH_SIZE];
    strcpy(F5Folder, TapeGetFolder());

    static TTapeListEntry Entries[LIST_MAX_FILES];
    int nSelected = 0;
    int nFiles = ListFolder(Entries, nullptr, &nSelected);

    ClearText();
    DrawBar(0, " DELETE");
    if (nFiles < 0) {
        DrawText(1, LIST_FIRST_ROW, "No SD card", FALSE);
        DrawBar(ACE_ROWS - 1, " ANY KEY: CLOSE");
        WaitForKey();
        TapeSetFolder(F5Folder);
        ResumeAce();
        return;
    }

    DrawBar(ACE_ROWS - 1, DELETE_FOOTER);

    WaitForRelease();               // F6 itself
    keyboard_flush_events();

    int nTop = 0;
    TNameScroll Scroll = {-1, 0, 0, 0, FALSE};
    TNameScroll TitleScroll = {-1, 0, 0, 0, FALSE};
    for (;;) {
        DrawListTitle("DELETE ", nFiles, &TitleScroll);
        ScrollSelect(&Scroll, nSelected, nFiles > 0 ? ListNameOverflow(&Entries[nSelected]) : 0);
        DrawFileList(Entries, nFiles, &nTop, nSelected, Scroll.nOffset);

        unsigned char ucKey, ucModifiers;
        GetScrolledListKey(&ucKey, &ucModifiers, &Scroll, Entries, nFiles, nTop, nSelected,
                           "DELETE ", &TitleScroll);

        if (ucKey == HID_KEY_ESCAPE) {
            break;
        }
        if (MoveSelection(ucKey, nFiles, &nSelected)) {
            ClearTextRow(LIST_STATUS_ROW);
            ClearTextRow(DELETE_INFO_ROW);
            continue;
        }
        if (ucKey == HID_KEY_BACKSPACE || ucKey == HID_KEY_LEFT) {
            ClearTextRow(LIST_STATUS_ROW);
            ClearTextRow(DELETE_INFO_ROW);
            OpenFolder("..", Entries, &nFiles, &nSelected, &nTop);
            continue;
        }

        boolean bEnter = ucKey == HID_KEY_ENTER || ucKey == HID_KEY_KP_ENTER;
        boolean bDelete = KeyLetter(ucKey) == 'd' || ucKey == HID_KEY_DELETE;
        if (!(bEnter || bDelete || ucKey == HID_KEY_RIGHT) || nFiles == 0) {
            continue;
        }
        ClearTextRow(LIST_STATUS_ROW);
        ClearTextRow(DELETE_INFO_ROW);

        char Name[TAPE_LIST_NAME_SIZE];
        strcpy(Name, Entries[nSelected].Name);
        boolean bZip = Entries[nSelected].bZip;
        boolean bFolder = Entries[nSelected].bFolder && !bZip;   // a .zip is deleted as a file
        if (Entries[nSelected].bFolder && (!bDelete || strcmp(Name, "..") == 0)) {
            if (!OpenFolder(Name, Entries, &nFiles, &nSelected, &nTop)) {
                DrawText(1, LIST_STATUS_ROW, "Could not open the folder", FALSE);
            }
            continue;
        }
        if (ucKey == HID_KEY_RIGHT) {
            continue;
        }
        if (FolderInZip()) {
            DrawText(1, LIST_STATUS_ROW, "A .zip is read only", FALSE);
            continue;
        }

        char Path[TAPE_PATH_SIZE];
        if (!TapeJoinPath(Path, TapeGetFolder(), Name)) {
            DrawText(1, LIST_STATUS_ROW, "The path is too long", FALSE);
            continue;
        }

        CString Status;
        Status.Format(bFolder ? "Delete %s/?" : "Delete %s?", Name);
        DrawText(1, LIST_STATUS_ROW, Status, TRUE);
        if (bFolder) {
            unsigned nInFiles, nInFolders;
            TapeCountFolder(Path, &nInFiles, &nInFolders);
            CString Info;
            Info.Format("With all in it: %u file%s, %u folder%s", nInFiles, nInFiles == 1 ? "" : "s",
                        nInFolders, nInFolders == 1 ? "" : "s");
            DrawText(1, DELETE_INFO_ROW, Info, FALSE);
        }
        DrawBar(ACE_ROWS - 1, " Y: DELETE  N: KEEP");

        do {
            GetKeyEvent(&ucKey, &ucModifiers);
        } while (KeyLetter(ucKey) != 'y' && KeyLetter(ucKey) != 'n' && ucKey != HID_KEY_ESCAPE);

        ClearTextRow(LIST_STATUS_ROW);
        ClearTextRow(DELETE_INFO_ROW);
        DrawBar(ACE_ROWS - 1, DELETE_FOOTER);
        if (KeyLetter(ucKey) != 'y') {
            DrawText(1, LIST_STATUS_ROW, "Not deleted", FALSE);
            continue;
        }

        if (bFolder) {
            DrawText(1, LIST_STATUS_ROW, "Deleting...", FALSE);
        }
        TTapeFileResult Result = bFolder ? TapeDeleteFolder(Path) : TapeDeleteFile(Path);
        ClearTextRow(LIST_STATUS_ROW);
        Status.Format(Result == TapeFileOK ? "Deleted %s" : "Could not delete %s", Name);
        DrawText(1, LIST_STATUS_ROW, Status, FALSE);

        int nOld = nSelected;
        nFiles = ListFolder(Entries, nullptr, &nSelected);
        if (nFiles < 0) {
            nFiles = 0;
        }
        nSelected = nOld < nFiles ? nOld : (nFiles > 0 ? nFiles - 1 : 0);
    }

    TapeSetFolder(F5Folder);       // or the nearest above, if it was deleted
    ResumeAce();
}

#define HISTORY_FOOTER  " ENTER: RETYPE  ESC: CLOSE"

// Ctrl+TAB: pause the Ace and list the remembered lines, newest first; Enter puts the
// chosen one back on the input line (as TAB does), Esc closes
// One line of the Ctrl+TAB list on its row, as the Ace shows it (inverse characters,
// graphics), from character nOffset; a line still longer than the row is cut, with '>'
// in the last column
void CKernel::DrawHistoryRow(int nLines, int nTop, int nIndex, boolean bSelected, unsigned nOffset)
{
    const u8 *pFont = m_bFontValid ? m_Font : mem + 0x2C00;
    unsigned nLength = 0;
    const u8 *pText = nIndex < nLines ? LineHistoryLine(nIndex, &nLength) : nullptr;
    if (nOffset > nLength) {
        nOffset = nLength;
    }
    if (pText != nullptr) {
        pText += nOffset;
        nLength -= nOffset;
    }
    for (int col = 0; col < ACE_COLS; col++) {
        u8 nCode = ' ';
        if (col < (int) nLength) {
            nCode = col == ACE_COLS - 1 && nLength > ACE_COLS ? '>' : pText[col];
        }
        DrawChar(col, LIST_FIRST_ROW + nIndex - nTop, pFont + (nCode & 0x7F) * 8,
                 ((nCode & 0x80) != 0) != bSelected);
    }
}

void CKernel::ShowHistoryList(void)
{
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
    TNameScroll Scroll = {-1, 0, 0, 0, FALSE};
    for (;;) {
        if (nSelected < nTop) {
            nTop = nSelected;
        } else if (nSelected >= nTop + LIST_ROWS) {
            nTop = nSelected - LIST_ROWS + 1;
        }

        // The selected line scrolls when it is too long for the row
        unsigned nLength = 0;
        LineHistoryLine(nSelected, &nLength);
        ScrollSelect(&Scroll, nSelected, nLength > ACE_COLS ? nLength - ACE_COLS : 0);

        for (int i = 0; i < LIST_ROWS; i++) {
            int nIndex = nTop + i;
            DrawHistoryRow(nLines, nTop, nIndex, nIndex == nSelected,
                           nIndex == nSelected ? Scroll.nOffset : 0);
        }

        unsigned char ucKey, ucModifiers;
        for (;;) {
            GetListKey(&ucKey, &ucModifiers, ScrollUntil(&Scroll));
            if (!ScrollKey(&Scroll, ucKey)) {
                break;
            }
            DrawHistoryRow(nLines, nTop, nSelected, TRUE, Scroll.nOffset);
        }

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

#define SAVE_FIRST_ROW  2

// F7: pause the Ace and choose what to save: the dictionary as Forth source, or
// the whole Ace as an .ace snapshot
void CKernel::ShowSave(void)
{
    static const char *s_Choices[2] = {" Dictionary (Forth source)", " Snapshot (.ace)"};
    static const char *s_Help[2][4] = {
        {"The words you defined, as", "Forth source: spool it (F5)", "into a fresh Ace to define", "them again."},
        {"The whole Ace: memory and CPU", "registers. Loading it (F5)", "carries on from where it was", "saved."}
    };
    static int s_nSelected = 0;                 // the last choice

    ClearText();
    DrawBar(0, " SAVE");
    DrawBar(ACE_ROWS - 1, " UP/DOWN  ENTER: OK  ESC: CANCEL");

    WaitForRelease();               // F7 itself
    keyboard_flush_events();

    for (;;) {
        for (int i = 0; i < 2; i++) {
            CString Line;
            Line.Format("%-29s", s_Choices[i]);
            DrawText(1, SAVE_FIRST_ROW + i, Line, i == s_nSelected);
        }
        for (int i = 0; i < 4; i++) {
            CString Line;
            Line.Format("%-30s", s_Help[s_nSelected][i]);
            DrawText(1, SAVE_FIRST_ROW + 3 + i, Line, FALSE);
        }

        unsigned char ucKey, ucModifiers;
        GetListKey(&ucKey, &ucModifiers);

        if (ucKey == HID_KEY_ESCAPE) {
            ResumeAce();
            return;
        }
        if (ucKey == HID_KEY_UP || ucKey == HID_KEY_DOWN) {
            s_nSelected ^= 1;
        } else if (ucKey == HID_KEY_ENTER || ucKey == HID_KEY_KP_ENTER) {
            break;
        }
    }

    if (s_nSelected == 0) {
        ShowSaveDictionary();
    } else {
        ShowSaveSnapshot();
    }
}

// F7: where the file goes, the folder last open in F5 (scrolling with pScroll)
void CKernel::DrawSaveFolder(TNameScroll *pScroll)
{
    char Folder[ACE_COLS + 1];
    int nId;
    unsigned nMax = FolderTitle(ACE_COLS - 2, Folder, TapeGetSaveFolder(), 0, &nId);
    ScrollSelect(pScroll, nId, nMax);
    if (pScroll->nOffset > 0) {
        FolderTitle(ACE_COLS - 2, Folder, TapeGetSaveFolder(), pScroll->nOffset);
    }
    DrawText(1, 17, FolderInZip() ? "Next to the .zip open in F5:" : "Into the folder open in F5:", FALSE);
    DrawText(1, 18, Folder, FALSE);
}

// F7, dictionary: ask for a file name and save the dictionary to it as Forth source
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
    TNameScroll FolderScroll = {-1, 0, 0, 0, FALSE};
    DrawSaveFolder(&FolderScroll);
    DrawBar(ACE_ROWS - 1, " ENTER: SAVE  ESC: CANCEL");

    WaitForRelease();

    for (;;) {
        char Input[NEW_TAPE_MAX_INPUT + 1];
        if (!ReadName(11, Input, NEW_TAPE_MAX_INPUT, &FolderScroll)) {
            break;
        }

        char Path[TAPE_PATH_SIZE];
        unsigned nWords;
        TSaveResult Result = SaveDictionaryStart(Input, Path, &nWords);
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

// F7, snapshot: ask for a file name and save the Ace to it as an .ace snapshot
void CKernel::ShowSaveSnapshot(void)
{
    ClearText();
    DrawBar(0, " SAVE SNAPSHOT");
    DrawText(1, 2, "Saves the Ace as it is now:", FALSE);
    DrawText(1, 3, "memory (screen, words, RAM", FALSE);
    DrawText(1, 4, "pack) and CPU registers, in", FALSE);
    DrawText(1, 5, "an .ace file on the SD card", FALSE);
    DrawText(1, 6, "(.ace if no extension given).", FALSE);
    DrawText(1, 8, "Loading it (F5) carries on", FALSE);
    DrawText(1, 9, "from where it was saved.", FALSE);
    TNameScroll FolderScroll = {-1, 0, 0, 0, FALSE};
    DrawSaveFolder(&FolderScroll);
    DrawBar(ACE_ROWS - 1, " ENTER: SAVE  ESC: CANCEL");

    WaitForRelease();

    if (SpoolActive() || SaveDictionaryActive()) {
        DrawText(1, 11, "Wait for the spooling or", FALSE);
        DrawText(1, 12, "dictionary save to finish", FALSE);
        DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
        WaitForKey();
        ResumeAce();
        return;
    }

    for (;;) {
        char Input[NEW_TAPE_MAX_INPUT + 1];
        if (!ReadName(11, Input, NEW_TAPE_MAX_INPUT, &FolderScroll)) {
            break;
        }

        char Path[TAPE_PATH_SIZE];
        TTapeFileResult Result = TapeSaveSnapshot(Input, Path);
        DrawText(0, 13, "                                ", FALSE);
        DrawText(0, 14, "                                ", FALSE);
        if (Result == TapeFileOK) {
            DrawText(1, 13, "Saved to", FALSE);
            DrawText(1, 14, strlen(Path) <= ACE_COLS - 2 ? Path : TapeBaseName(Path), FALSE);
            DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
            WaitForKey();
            break;
        }
        DrawText(1, 13, Result == TapeFileExists ? "That file already exists" :
                        Result == TapeFileInvalidName ? "Invalid file name" :
                        Result == TapeFileNoCard ? "No SD card" :
                                                   "Could not write the file", FALSE);
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

#define NETWORK_ACTIVITY_ROW 13
#define NETWORK_TITLE       " FILE EXCHANGE (WEB PAGE)"
#define HID_KEY_1           0x1E
#define HID_KEY_2           0x1F
#define HID_KEY_KP_1        0x59
#define HID_KEY_KP_2        0x5A
#define HID_KEY_C           0x06
#define HID_KEY_TAB         0x2B
#define FIELD_WIDTH         30

// Edit pInput (up to nMax characters) on nRows rows from nRow; returns the key that
// ends it: Enter, Esc, Up, Down or Tab. bUpper: letters in capitals.
unsigned char CKernel::ReadField(int nRow, int nRows, char *pInput, unsigned nMax, boolean bUpper)
{
    unsigned nLen = strlen(pInput);
    for (;;) {
        DrawFieldText(nRow, nRows, pInput);
        if (nLen < nMax) {
            DrawText(1 + nLen % FIELD_WIDTH, nRow + nLen / FIELD_WIDTH, " ", TRUE);   // the cursor
        }

        unsigned char ucKey, ucModifiers;
        GetKeyEvent(&ucKey, &ucModifiers);

        if (   ucKey == HID_KEY_ENTER || ucKey == HID_KEY_KP_ENTER || ucKey == HID_KEY_ESCAPE
            || ucKey == HID_KEY_UP || ucKey == HID_KEY_DOWN || ucKey == HID_KEY_TAB) {
            DrawFieldText(nRow, nRows, pInput);     // without the cursor
            return ucKey == HID_KEY_KP_ENTER ? HID_KEY_ENTER : ucKey;
        }
        if (ucKey == HID_KEY_BACKSPACE) {
            if (nLen > 0) {
                pInput[--nLen] = '\0';
            }
        } else {
            char c = keyboard_hid_to_ascii(ucKey, ucModifiers);
            if (bUpper && c >= 'a' && c <= 'z') {
                c -= 'a' - 'A';
            }
            if (c >= ' ' && c <= '~' && nLen < nMax) {
                pInput[nLen++] = c;
                pInput[nLen] = '\0';
            }
        }
    }
}

// The text of a field without the cursor
void CKernel::DrawFieldText(int nRow, int nRows, const char *pText)
{
    unsigned nLen = strlen(pText);
    for (int i = 0; i < nRows; i++) {
        DrawText(0, nRow + i, "                                ", FALSE);
        unsigned nStart = i * FIELD_WIDTH;
        if (nStart < nLen) {
            char Chunk[FIELD_WIDTH + 1];
            unsigned n = nLen - nStart < FIELD_WIDTH ? nLen - nStart : FIELD_WIDTH;
            memcpy(Chunk, pText + nStart, n);
            Chunk[n] = '\0';
            DrawText(1, nRow + i, Chunk, FALSE);
        }
    }
}

#define WIFI_NAME_ROW       3
#define WIFI_PASSWORD_ROW   7
#define WIFI_COUNTRY_ROW    12
#define WIFI_MESSAGE_ROW    15

// The Wi-Fi network to join, kept in SD:/wpa_supplicant.conf (wifi_settings.cpp);
// FALSE if cancelled or it could not be saved
boolean CKernel::EditWiFiSettings(void)
{
    TWiFiSettings Settings;
    boolean bHadFile = WiFiReadSettings(&Settings);
    TWiFiSettings Old = Settings;

    ClearText();
    DrawBar(0, " WI-FI NETWORK");
    DrawText(1, WIFI_NAME_ROW - 1, "Network name:", FALSE);
    DrawText(1, WIFI_PASSWORD_ROW - 1, "Password (none: open network):", FALSE);
    DrawText(1, WIFI_COUNTRY_ROW - 1, "Country code (PT, GB, US...):", FALSE);
    DrawText(1, 17, "Up / Down: another field", FALSE);
    DrawText(1, 18, "Enter on the country: connect", FALSE);
    DrawText(1, 20, "Kept on the SD card in", FALSE);
    DrawText(1, 21, "wpa_supplicant.conf", FALSE);
    DrawBar(ACE_ROWS - 1, " ENTER: NEXT  ESC: CANCEL");
    DrawFieldText(WIFI_NAME_ROW, 2, Settings.SSID);
    DrawFieldText(WIFI_PASSWORD_ROW, 3, Settings.Password);
    DrawFieldText(WIFI_COUNTRY_ROW, 1, Settings.Country);

    WaitForRelease();
    keyboard_flush_events();

    int nField = Settings.SSID[0] == '\0' ? 0 : 2;  // saved before: Enter connects
    for (;;) {
        unsigned char ucKey;
        switch (nField) {
        case 0:  ucKey = ReadField(WIFI_NAME_ROW, 2, Settings.SSID, WIFI_SSID_MAX, FALSE);         break;
        case 1:  ucKey = ReadField(WIFI_PASSWORD_ROW, 3, Settings.Password, WIFI_PASSWORD_MAX, FALSE); break;
        default: ucKey = ReadField(WIFI_COUNTRY_ROW, 1, Settings.Country, 2, TRUE);              break;
        }
        DrawText(0, WIFI_MESSAGE_ROW, "                                ", FALSE);

        if (ucKey == HID_KEY_ESCAPE) {
            return FALSE;
        }
        if (ucKey == HID_KEY_UP) {
            nField = nField > 0 ? nField - 1 : 2;
            continue;
        }
        if (ucKey != HID_KEY_ENTER || nField < 2) {
            nField = nField < 2 ? nField + 1 : 0;
            continue;
        }

        int nBadField;
        const char *pProblem = WiFiCheckSettings(&Settings, &nBadField);
        if (pProblem != nullptr) {
            DrawText(1, WIFI_MESSAGE_ROW, pProblem, TRUE);
            nField = nBadField;
            continue;
        }
        break;
    }

    if (!bHadFile || memcmp(&Settings, &Old, sizeof Settings) != 0) {
        if (!WiFiWriteSettings(&Settings)) {
            DrawText(1, WIFI_MESSAGE_ROW, "Could not write the SD card", TRUE);
            DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
            WaitForKey();
            return FALSE;
        }
    }
    return TRUE;
}

// What a failed start means, on two rows
static void NetworkFailureText(TWebState State, const char **ppLine1, const char **ppLine2)
{
    *ppLine2 = "";
    switch (State) {
    case WebNoWiFi:
        *ppLine1 = "No Wi-Fi found on this Pi";
        break;
    case WebNoWiFiFirmware:
        *ppLine1 = "The Wi-Fi firmware files are";
        *ppLine2 = "not in /firmware on the card";
        break;
    default:
        *ppLine1 = "The network could not start";
        break;
    }
}

// Before the network is started: cable or Wi-Fi? Starts it; FALSE if Esc
boolean CKernel::ChooseNetwork(TWebNetwork *pNetwork)
{
    for (;;) {
        ClearText();
        DrawBar(0, NETWORK_TITLE);
        DrawText(1, 2, "Connect the Pi with:", FALSE);
        DrawText(2, 4, "1  Cable network (Ethernet)", FALSE);
        DrawText(2, 5, "2  Wi-Fi", FALSE);
        if (WebFilesWiFiFailure() != WebNotStarted) {
            DrawText(11, 5, "(not available)", FALSE);
        }
        DrawText(1, 8, "The choice stays until the Pi", FALSE);
        DrawText(1, 9, "is switched off.", FALSE);
        DrawText(1, 11, "The Ace is paused while this", FALSE);
        DrawText(1, 12, "screen is open.", FALSE);
        DrawBar(ACE_ROWS - 1, " 1 / 2: CHOOSE  ESC: BACK");

        keyboard_flush_events();
        unsigned char ucKey, ucModifiers;
        GetKeyEvent(&ucKey, &ucModifiers);
        if (ucKey == HID_KEY_ESCAPE) {
            return FALSE;
        }

        if (ucKey == HID_KEY_1 || ucKey == HID_KEY_KP_1) {
            *pNetwork = WebCable;
        } else if (ucKey == HID_KEY_2 || ucKey == HID_KEY_KP_2) {
            *pNetwork = WebWiFi;
            if (WebFilesWiFiFailure() == WebNotStarted && !EditWiFiSettings()) {
                continue;
            }
        } else {
            continue;
        }

        ClearText();
        DrawBar(0, NETWORK_TITLE);
        DrawText(1, 2, *pNetwork == WebWiFi ? "Starting the Wi-Fi..." : "Starting the network...", FALSE);

        TWebState Failure;
        if (WebFilesStart(*pNetwork, &Failure)) {
            return TRUE;
        }

        const char *pLine1, *pLine2;
        NetworkFailureText(Failure, &pLine1, &pLine2);
        DrawText(1, 4, pLine1, FALSE);
        DrawText(1, 5, pLine2, FALSE);
        DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
        WaitForKey();
        if (Failure == WebFailed) {
            return FALSE;               // nothing else to try
        }
    }
}

// F11: pause the Ace, join the cable or Wi-Fi network and serve the web page for
// exchanging files with the SD card (web_files.cpp) until Esc
void CKernel::ShowNetwork(void)
{
    ClearText();
    DrawBar(0, NETWORK_TITLE);

    WaitForRelease();               // F11 itself
    keyboard_flush_events();

    if (SpoolActive() || SaveDictionaryActive()) {
        DrawText(1, 2, "Wait for the spooling or", FALSE);
        DrawText(1, 3, "dictionary save to finish", FALSE);
        DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
        WaitForKey();
        ResumeAce();
        return;
    }

    TWebNetwork Network;
    if (!WebFilesStarted(&Network)) {
        if (!ChooseNetwork(&Network)) {
            ResumeAce();
            return;
        }
    }

    boolean bRedraw = TRUE;
    TWebState LastState = WebNotStarted;
    CString LastAddress, LastName;
    unsigned nLastActivity = (unsigned) -1;
    for (;;) {
        if (bRedraw) {
            bRedraw = FALSE;
            LastState = WebNotStarted;
            nLastActivity = (unsigned) -1;

            ClearText();
            DrawBar(0, NETWORK_TITLE);
            if (Network == WebWiFi) {
                TWiFiSettings Settings;
                WiFiReadSettings(&Settings);
                Settings.SSID[ACE_COLS - 9] = '\0';     // fits the row
                CString Line;
                Line.Format("Wi-Fi: %s", Settings.SSID);
                DrawText(1, 2, Line, FALSE);
                DrawBar(ACE_ROWS - 1, " C: WI-FI NETWORK  ESC: BACK");
            } else {
                DrawText(1, 2, "Cable network (Ethernet)", FALSE);
                DrawBar(ACE_ROWS - 1, " ESC: BACK TO THE ACE");
            }
            DrawText(1, 10, "The Ace is paused while this", FALSE);
            DrawText(1, 11, "screen is open.", FALSE);
        }

        WebFilesRun(10000);
        AttachKeyboard();

        if (UpdateIsPending()) {
            ConfirmUpdate();                    // a release .zip came from the web page
            keyboard_flush_events();
            bRedraw = TRUE;
            continue;
        }

        CString Address, Name;
        TWebState State = WebFilesGetState(&Address, &Name);
        if (State != LastState || Address.Compare(LastAddress) != 0 || Name.Compare(LastName) != 0) {
            LastState = State;
            LastAddress = Address;
            LastName = Name;
            for (int row = 4; row <= 8; row++) {
                DrawText(0, row, "                                ", FALSE);
            }
            switch (State) {
            case WebFailed:         DrawText(1, 4, "The network could not start", FALSE);   break;
            case WebNoAdapter:      DrawText(1, 4, "No Ethernet adapter found", FALSE);     break;
            case WebNoCable:        DrawText(1, 4, "Plug in a network cable", FALSE);       break;
            case WebJoining:        DrawText(1, 4, "Joining the Wi-Fi network...", FALSE);  break;
            case WebJoinTrouble:
                DrawText(1, 4, "Not joined yet. Check the", FALSE);
                DrawText(1, 5, "network name, password and", FALSE);
                DrawText(1, 6, "country code (C).", FALSE);
                break;
            case WebWiFiBadSettings:
                DrawText(1, 4, "wpa_supplicant.conf cannot be", FALSE);
                DrawText(1, 5, "read: correct it (C), then", FALSE);
                DrawText(1, 6, "restart the Pi.", FALSE);
                break;
            case WebWaitingAddress: DrawText(1, 4, "Connecting...", FALSE);                 break;
            case WebReady:
                DrawText(1, 4, "Open in a web browser:", FALSE);
                DrawText(2, 6, Address, TRUE);
                if (Name.GetLength() > 0) {
                    DrawText(2, 7, Name, FALSE);
                }
                break;
            default:
                break;
            }
        }

        if (WebFilesActivityCount() != nLastActivity) {
            nLastActivity = WebFilesActivityCount();
            DrawText(1, NETWORK_ACTIVITY_ROW, nLastActivity > 0 ? "Done on the page:" : "                 ", FALSE);
            for (unsigned i = 0; i < WEB_ACTIVITY_LINES; i++) {
                CString Line;
                Line.Format("%-30s", WebFilesActivityLine(i));
                DrawText(1, NETWORK_ACTIVITY_ROW + 1 + i, Line, FALSE);
            }
        }

        unsigned char ucKey, ucModifiers;
        if (keyboard_get_event(&ucKey, &ucModifiers)) {
            if (ucKey == HID_KEY_ESCAPE) {
                break;
            }
            if (ucKey == HID_KEY_C && Network == WebWiFi) {
                if (EditWiFiSettings() && !WebFilesWiFiReload()) {
                    ClearText();
                    DrawBar(0, " WI-FI NETWORK");
                    DrawText(1, 2, "Saved. Restart the Pi to use", FALSE);
                    DrawText(1, 3, "the new Wi-Fi settings.", FALSE);
                    DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
                    WaitForKey();
                }
                keyboard_flush_events();
                bRedraw = TRUE;
            }
        }
    }

    ResumeAce();
}

// F11: a system update came from the web page (/system): install it after Y
void CKernel::ConfirmUpdate(void)
{
    ClearText();
    DrawBar(0, " SYSTEM UPDATE");
    CString Line;
    Line.Format("Version %s was sent,", UpdatePendingVersion());
    DrawText(1, 2, Line, FALSE);
    Line.Format("built %s", UpdatePendingBuild());
    DrawText(1, 3, Line, FALSE);
    Line.Format("This is %s,", UpdateVersion());
    DrawText(1, 5, Line, FALSE);
    Line.Format("built %s", UpdateBuild());
    DrawText(1, 6, Line, FALSE);
    unsigned nFiles = UpdatePendingFiles();
    Line.Format("%u file%s on the card change.", nFiles, nFiles == 1 ? "" : "s");
    DrawText(1, 8, Line, FALSE);
    if (UpdateCanTryBoot()) {
        DrawText(1, 10, "The Pi restarts to try it. If", FALSE);
        DrawText(1, 11, "it does not start, switch the", FALSE);
        DrawText(1, 12, "Pi off and on: this version", FALSE);
        DrawText(1, 13, "comes back.", FALSE);
    } else {
        DrawText(1, 10, "This Pi cannot try it first:", FALSE);
        DrawText(1, 11, "after installing, switch it", FALSE);
        DrawText(1, 12, "off and on. If it does not", FALSE);
        DrawText(1, 13, "start, copy the files in", FALSE);
        DrawText(1, 14, "update-backup on the card back", FALSE);
        DrawText(1, 15, "to its root on a computer.", FALSE);
    }
    DrawText(1, 17, "Anything not saved is lost.", FALSE);
    DrawBar(ACE_ROWS - 1, " Y: INSTALL  N: DISCARD");

    WaitForRelease();
    keyboard_flush_events();
    unsigned char ucKey, ucModifiers;
    do {
        GetKeyEvent(&ucKey, &ucModifiers);
    } while (KeyLetter(ucKey) != 'y' && KeyLetter(ucKey) != 'n' && ucKey != HID_KEY_ESCAPE);
    if (KeyLetter(ucKey) != 'y') {
        UpdateDiscard();
        return;
    }

    for (int row = 2; row < ACE_ROWS - 1; row++) {
        ClearTextRow(row);
    }
    DrawBar(ACE_ROWS - 1, "");
    DrawText(1, 2, "Installing...", FALSE);
    CString Version(UpdatePendingVersion());
    CString Message;
    if (!UpdateInstall(&Message)) {
        DrawText(1, 4, "The update failed:", FALSE);
        DrawText(1, 5, Message, FALSE);
        DrawText(1, 7, "Nothing was changed.", FALSE);
        DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
        WaitForKey();
        return;
    }

    if (UpdateCanTryBoot()) {
        Line.Format("Restarting to try %s...", (const char *) Version);
        DrawText(1, 4, Line, FALSE);
        CTimer::SimpleMsDelay(1500);
        m_ActLED.Off();
        UpdateRestart();                    // does not return
    }

    Line.Format("Version %s installed.", (const char *) Version);
    DrawText(1, 4, Line, FALSE);
    DrawText(1, 6, "Switch the Pi off and on to", FALSE);
    DrawText(1, 7, "start it.", FALSE);
    DrawBar(ACE_ROWS - 1, " ANY KEY: CONTINUE");
    WaitForKey();
}

// At start-up: a system update was kept, or did not start
void CKernel::ShowUpdateResult(void)
{
    ClearText();
    DrawBar(0, " SYSTEM UPDATE");
    DrawText(1, 2, m_UpdateLine1, FALSE);
    DrawText(1, 3, m_UpdateLine2, FALSE);
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
    static const unsigned PackKB[] = {0, 16, 32, 48, RAM_PACK_96K};
    static const char *const Names[] = {
        "3K  (no RAM pack)",
        "19K (16K RAM pack)",
        "35K (32K RAM pack)",
        "51K (48K RAM pack)",
        "99K (96K RAM pack)",
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
        if (!RamSetPackKB(PackKB[nSelected])) {
            m_Logger.Write("xAce", LogError, "No memory for the 96K RAM pack");
        }
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

    m_ActLED.Off();                 // the Pi 400's power light goes out, as in Linux
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
