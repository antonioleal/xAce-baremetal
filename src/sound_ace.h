#ifndef _SOUND_ACE_H
#define _SOUND_ACE_H

#include <circle/interrupt.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/types.h>

/*
 * Jupiter Ace beeper: an IN from port 0xFE pulls the speaker low,
 * an OUT to port 0xFE drives it high. Edges are timestamped in Z80
 * t-states during a frame and resampled at the end of each frame.
 */
class CAceSound {
public:
    CAceSound(void);
    ~CAceSound(void);

    // pDevice: "sndpwm" (headphone jack, default) or "sndhdmi"
    boolean Initialize(CInterruptSystem *pInterrupt, const char *pDevice);

    void Speaker(unsigned nLevel, unsigned long nTStates)
    {
        if (nLevel != m_nLevel && m_nEvents < MaxEvents) {
            m_Events[m_nEvents].nTStates = nTStates;
            m_Events[m_nEvents].nLevel   = nLevel;
            m_nEvents++;
            m_nLevel = nLevel;
        }
    }

    // Render one emulated frame of nFrameTStates and queue it for playback
    void EndFrame(unsigned long nFrameTStates);

private:
    static const unsigned MaxEvents = 8192;
    static const unsigned MaxFrameSamples = 2048;

    struct TEvent {
        unsigned long nTStates;
        unsigned      nLevel;
    };

    CSoundBaseDevice *m_pSound;

    TEvent   m_Events[MaxEvents];
    unsigned m_nEvents;
    unsigned m_nLevel;          // current speaker level (last recorded)
    unsigned m_nFrameLevel;     // speaker level at the start of the frame

    float    m_fSamplePos;      // start of the next sample, in t-states from frame start
    float    m_fFilterIn;       // DC blocker state
    float    m_fFilterOut;

    s16      m_Buffer[MaxFrameSamples];
};

#endif
