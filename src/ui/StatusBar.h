#pragma once

#include <M5GFX.h>

class StatusBar {
public:
    void render(M5Canvas& canvas);

    void setDirtyFlag(bool* flag) { _dirty = flag; }
    void setTransportMode(const char* mode) { _transportMode = mode; if (_dirty) *_dirty = true; }
    void setLoRaOnline(bool online) { _loraOnline = online; if (_dirty) *_dirty = true; }
    void flashAnnounce() { _announceFlashUntil = millis() + 1500; if (_dirty) *_dirty = true; }

private:
    const char* _transportMode = "STANDALONE";
    bool _loraOnline = false;
    unsigned long _announceFlashUntil = 0;
    float _smoothedBattery = -1.0f;
    unsigned long _lastBatteryRead = 0;
    bool* _dirty = nullptr;
};
