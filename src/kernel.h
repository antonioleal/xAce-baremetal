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
#include "web_files.h"

struct TTapeListEntry;

enum TShutdownMode {
    ShutdownNone,
    ShutdownHalt,
    ShutdownReboot
};

struct TNameScroll;             // kernel.cpp: a long name scrolling in a list

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
    void ClearTextRow(int nRow);
    void DrawBar(int row, const char *pText);
    void ShowTapeList(void);
    boolean ReadName(int nRow, char *pInput, unsigned nMax,
                     TNameScroll *pFolderScroll = nullptr);
    void DrawFileList(const TTapeListEntry *pEntries, int nFiles, int *pTop, int nSelected,
                      unsigned nOffset = 0);
    void DrawFileRow(const TTapeListEntry *pEntries, int nFiles, int nTop, int nIndex,
                     const char *pFolder, boolean bSelected, unsigned nOffset);
    void DrawHistoryRow(int nLines, int nTop, int nIndex, boolean bSelected, unsigned nOffset);
    void GetScrolledListKey(unsigned char *pKey, unsigned char *pModifiers,
                            TNameScroll *pScroll, const TTapeListEntry *pEntries,
                            int nFiles, int nTop, int nSelected,
                            const char *pTitle, TNameScroll *pTitleScroll);
    void DrawListTitle(const char *pTitle, int nFiles, TNameScroll *pScroll = nullptr);
    int ListFolder(TTapeListEntry *pEntries, const char *pSelect, int *pSelected);
    boolean OpenFolder(const char *pName, TTapeListEntry *pEntries, int *pFiles,
                       int *pSelected, int *pTop);
    void ShowDeleteTape(void);
    void ShowPowerOff(void);
    void ShowKeyboardLayout(void);
    void ShowJoystick(void);
    void JoystickControlName(int nControl, CString *pName);
    void DrawJoystickControls(int nSelected);
    void ShowRamSize(void);
    void ShowSave(void);
    void ShowSaveDictionary(void);
    void ShowSaveSnapshot(void);
    void DrawSaveFolder(TNameScroll *pScroll);
    void ShowSaveResult(void);
    void ShowHistoryList(void);
    void ShowNetwork(void);
    void ConfirmUpdate(void);
    void ShowUpdateResult(void);
    boolean ChooseNetwork(TWebNetwork *pNetwork);
    boolean EditWiFiSettings(void);
    unsigned char ReadField(int nRow, int nRows, char *pInput, unsigned nMax, boolean bUpper);
    void DrawFieldText(int nRow, int nRows, const char *pText);
    void ResetAce(void);
    void ResumeAce(void);
    unsigned char WaitForKey(void);
    void WaitForRelease(void);
    void GetKeyEvent(unsigned char *pKey, unsigned char *pModifiers, u64 nUntil = 0);
    void GetListKey(unsigned char *pKey, unsigned char *pModifiers, u64 nUntil = 0);

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
    CString             m_UpdateLine1;      // what became of a system update (F11)
    CString             m_UpdateLine2;

    static CKernel     *s_pThis;
};

#endif
