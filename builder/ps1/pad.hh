// PS1Pad: psyqo's AdvancedPad reduced to what the game reads (docs/43 C11). The engine reads only pad 1 (Pad1a: port 1,
// or pad A of a multitap there), but AdvancedPad's Fast mode polled both ports every frame, and an empty port 2 never
// sends /ACK: each frame waited out its whole timeout (~105 us) after one byte, about half of the ~5 hblanks readPad
// cost (nocash psx-spx "Controller and Memory Card I/O Ports"; "PSX expects No-connection if not returned Acknowledge
// less than 100 usec", "Controller and Memory Card Signals"). PS1Pad polls port 1 only, every frame, with
// AdvancedPad's own byte sequence, timings and multitap handling (copied from nugget/psyqo/src/advancedpad.cpp, MIT,
// PCSX-Redux authors; nugget itself stays upstream). Same API for the calls the port makes.
#pragma once
#include <stdint.h>
#include <psyqo/advancedpad.hh>

class PS1Pad {
  public:
    using Pad    = psyqo::AdvancedPad::Pad;
    using Button = psyqo::AdvancedPad::Button;
    void initialize();
    bool isPadConnected(Pad pad) const { return (unsigned)pad < 4 && m_padData[(unsigned)pad].connected == 0; }
    bool isButtonPressed(Pad pad, Button button) const {
        return (unsigned)pad < 4 && (m_padData[(unsigned)pad].buttons & (1 << (unsigned)button)) == 0;
    }

  private:
    void readPad();
    union PadData {
        struct {
            uint8_t connected;
            uint8_t padType;
            uint16_t buttons;
            uint8_t adc[8];
        };
        uint16_t packed[6];
    };
    PadData m_padData[4]; // port 1: pads A-D (a multitap), as AdvancedPad's Pad1a-Pad1d
};
