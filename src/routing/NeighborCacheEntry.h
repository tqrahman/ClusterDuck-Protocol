#ifndef NEIGHBORCACHEENTRY_H_
#define NEIGHBORCACHEENTRY_H_

#include <cstdint>

struct NeighborCacheEntry {
    int8_t        rssi;     // raw RSSI in dBm
    int8_t        snr;      // raw SNR in dB
    unsigned long lastSeen; // millis() when last packet was received
};

#endif
