#ifndef _KERNEL_H
#define _KERNEL_H

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/bcmframebuffer.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/usb/usbgamepad.h>
#include <circle/string.h>
#include <circle/types.h>
#include <SDCard/emmc.h>

#include "sound_ace.h"

struct TTapeListEntry;

enum TShutdownMode {
    ShutdownNone,
    ShutdownHalt,
    ShutdownReboot
};

class CKernel {
public:
    CKernel(void);
    ~CKernel(void);

    boolean Initialize(void);
    TShutdownMode Run(void);

    // Host side work done once per emulated frame
    void Frame(unsigned long nFrameTStates);

    CAceSound *GetSound(void) { return &m_Sound; }

    static CKernel *Get(void) { return s_pThis; }

private:
    void RenderScreen(void);
    void DrawChar(int col, int row, const u8 *pBitmap, boolean bInvert);
    void DrawText(int col, int row, const char *pText, boolean bInvert);
    void AttachKeyboard(void);
    void AttachJoystick(void);
    void UpdateKeyboard(void);
    void ClearText(void);
    void DrawBar(int row, const char *pText);
    void ShowTapeList(void);
    boolean ReadName(int nRow, char *pInput, unsigned nMax);
    void DrawFileList(const TTapeListEntry *pEntries, int nFiles, int *pTop, int nSelected);
    void ShowDeleteTape(void);
    void ShowPowerOff(void);
    void ShowKeyboardLayout(void);
    void ShowJoystick(void);
    void JoystickControlName(int nControl, CString *pName);
    void DrawJoystickControls(int nSelected);
    void ShowRamSize(void);
    void ShowSaveDictionary(void);
    void ShowSaveResult(void);
    void ShowHistoryList(void);
    void ResetAce(void);
    void ResumeAce(void);
    unsigned char WaitForKey(void);
    void WaitForRelease(void);
    void GetKeyEvent(unsigned char *pKey, unsigned char *pModifiers);
    void GetListKey(unsigned char *pKey, unsigned char *pModifiers);

    static void KeyStatusHandlerRaw(unsigned char ucModifiers, const unsigned char RawKeys[6]);
    static void KeyboardRemovedHandler(CDevice *pDevice, void *pContext);
    static void JoystickStatusHandler(unsigned nDeviceIndex, const TGamePadState *pState);
    static void JoystickRemovedHandler(CDevice *pDevice, void *pContext);
    void ClearBorder(u32 nColor);

private:
    CActLED             m_ActLED;
    CKernelOptions      m_Options;
    CDeviceNameService  m_DeviceNameService;
    CBcmFrameBuffer     m_FrameBuffer;
    CSerialDevice       m_Serial;
    CExceptionHandler   m_ExceptionHandler;
    CInterruptSystem    m_Interrupt;
    CTimer              m_Timer;
    CLogger             m_Logger;
    CUSBHCIDevice       m_USBHCI;
    CEMMCDevice         m_EMMC;
    CAceSound           m_Sound;

    CUSBKeyboardDevice * volatile m_pKeyboard;
    CUSBGamePadDevice * volatile m_pJoystick;  // the first USB gamepad / joystick (F3)
    unsigned            m_nJoystickProperties;  // TGamePadProperty bits

    u8                  m_VideoRamOld[32 * 24];
    u8                  m_CharsetOld[128 * 8];
    boolean             m_bScreenDirty;
    boolean             m_bInverted;        // Ctrl+F4: black on white

    u8                  m_Font[128 * 8];    // ROM character set, for the tape list
    boolean             m_bFontValid;
    unsigned            m_nFrameCount;
    u64                 m_nLastRender;      // while spooling

    unsigned char       m_ucRepeatKey;      // list key repeating while held (F5, F6, F8)
    u64                 m_nRepeatAt;

    static CKernel     *s_pThis;
};

#endif
