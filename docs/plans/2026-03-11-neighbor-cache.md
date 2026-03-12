# Neighbor Cache & Health Packet Implementation Plan

**Date:** 2026-03-11
**Branch:** v5-rssi
**Design doc:** `docs/plans/2026-02-26-health-packet-design.md`
**Status:** Ready to implement

---

## Overview

Implement the neighbor cache and health packet as specified in the design doc.
Every received packet updates a flat RSSI/SNR cache keyed by sender DUID.
Every 5 minutes (when PUBLIC) the duck emits a `health` packet to the Papa
containing uptime, heap, neighbor cache, and an optional sketch-injected payload.

---

## Step 1 — `src/routing/NeighborCacheEntry.h` (new file)

```cpp
#ifndef NEIGHBORCACHEENTRY_H_
#define NEIGHBORCACHEENTRY_H_

#include <cstdint>

struct NeighborCacheEntry {
    int8_t        rssi;     // raw RSSI in dBm
    int8_t        snr;      // raw SNR in dB
    unsigned long lastSeen; // millis() when last packet was received
};

#endif
```

---

## Step 2 — `src/routing/DuckRouter.h`

Add the include, the cache member, and two new methods.

After the existing `#include "Neighbor.h"` line add:

```cpp
#include "NeighborCacheEntry.h"
#include <unordered_map>
#include <string>
```

In the `public:` section add:

```cpp
/**
 * @brief Update (or insert) a neighbor's RSSI/SNR in the cache.
 * Call immediately after readReceivedData() while radio values are fresh.
 */
void updateNeighborCache(const Duid& sender, int8_t rssi, int8_t snr);

/**
 * @brief Read-only access to the neighbor cache for health packet assembly.
 */
const std::unordered_map<std::string, NeighborCacheEntry>& getNeighborCache() const;
```

In the `private:` section add after `BloomFilter filter;`:

```cpp
std::unordered_map<std::string, NeighborCacheEntry> neighborCache;
```

---

## Step 3 — `src/routing/DuckRouter.cpp`

Append the two new method implementations:

```cpp
void DuckRouter::updateNeighborCache(const Duid& sender, int8_t rssi, int8_t snr) {
    std::string key(sender.begin(), sender.end());
    neighborCache[key] = NeighborCacheEntry{rssi, snr, millis()};
}

const std::unordered_map<std::string, NeighborCacheEntry>&
DuckRouter::getNeighborCache() const {
    return neighborCache;
}
```

---

## Step 4 — `src/Ducks/Duck.h`

### 4a. Add timer constants and health members (in `protected:` section)

After `unsigned long lastRreqTime = 0L;` add:

```cpp
static constexpr unsigned long HEALTH_INTERVAL = 300000L; // 5 minutes
unsigned long lastHealthTime = 0L;
std::string userHealthData;
```

### 4b. Add `setHealthData()` to the public API

In `public:` section add:

```cpp
/**
 * @brief Supply sketch-specific JSON to include in the next health packet.
 * Call any time before the 5-minute timer fires.
 * CDP stores the string verbatim — caller is responsible for valid JSON.
 * Keep the payload under ~100 bytes to stay within the 229-byte packet limit.
 *
 * @param jsonPayload  Raw JSON string, e.g. "{\"batt\":3.7,\"solar\":1.1}"
 */
void setHealthData(const std::string& jsonPayload) {
    userHealthData = jsonPayload;
}
```

### 4c. Add `sendHealthPacket()` to `protected:`

```cpp
/**
 * @brief Build and transmit a health packet to PAPADUCK_DUID.
 *
 * JSON format:
 * {
 *   "id":   "<hex duid>",
 *   "up":   <millis>,
 *   "heap": <free bytes>,
 *   "nb":   [{"id":"<hex>","r":<rssi>,"s":<snr>,"t":<ms since heard>}, ...],
 *   "usr":  "<verbatim sketch string>"   // omitted when empty
 * }
 */
void sendHealthPacket() {
    // Allocate enough for system fields + up to 8 neighbors + usr
    StaticJsonDocument<1024> doc;

    // --- system fields ---
    std::string idStr(duid.begin(), duid.end());
    doc["id"]   = idStr;
    doc["up"]   = (uint32_t)millis();
    doc["heap"] = (uint32_t)duckesp::getFreeHeap();

    // --- neighbor cache ---
    JsonArray nb = doc.createNestedArray("nb");
    const auto& cache = router.getNeighborCache();
    int count = 0;
    for (const auto& [key, entry] : cache) {
        if (count >= 8) break; // cap
        JsonObject obj = nb.createNestedObject();
        obj["id"] = key;
        obj["r"]  = entry.rssi;
        obj["s"]  = entry.snr;
        obj["t"]  = (uint32_t)(millis() - entry.lastSeen);
        count++;
    }

    // --- optional sketch payload ---
    if (!userHealthData.empty()) {
        doc["usr"] = userHealthData;
    }

    std::string payload;
    serializeJson(doc, payload);
    sendData(topics::health, payload, PAPADUCK_DUID);
}
```

### 4d. Fire the timer in `run()`

Inside `run()`, in the `if(router.getNetworkState() == NetworkState::PUBLIC)` block,
**after** the existing receive-flag check, add:

```cpp
if (millis() - lastHealthTime > HEALTH_INTERVAL) {
    sendHealthPacket();
    lastHealthTime = millis();
}
```

The relevant section of `run()` becomes:

```cpp
if(router.getNetworkState() == NetworkState::PUBLIC) {
    if(duckRadio.getReceiveFlag()){
        handleReceivedPacket();
    }
    if (millis() - lastHealthTime > HEALTH_INTERVAL) {
        sendHealthPacket();
        lastHealthTime = millis();
    }
}
```

---

## Step 5 — Update `handleReceivedPacket()` in all three duck types

In **each** of `MamaDuck.h`, `PapaDuck.h`, and `DuckLink.h`, immediately after the
`CdpPacket rxPacket(rxData.value());` line, insert:

```cpp
// Update neighbor cache while radio RSSI/SNR are still fresh
this->router.updateNeighborCache(
    rxPacket.sduid,
    (int8_t)this->duckRadio.getRSSI(),
    (int8_t)this->duckRadio.getSNR()
);
```

### MamaDuck.h — around line 49

```cpp
CdpPacket rxPacket(rxData.value());
logdbg_ln("Got data from radio. size: %d",rxPacket.size());
// >>> INSERT HERE <<<
this->router.updateNeighborCache(
    rxPacket.sduid,
    (int8_t)this->duckRadio.getRSSI(),
    (int8_t)this->duckRadio.getSNR()
);
```

### PapaDuck.h — around line 50

Same pattern, same insertion point.

### DuckLink.h — around line 37

Same pattern, same insertion point.

---

## Step 6 — `sendData()` health-topic bypass

`sendData()` currently blocks topics below `reservedTopic::max_reserved`.
`topics::health` (`0x15`) is above that threshold, so no change is needed —
`sendHealthPacket()` can call the existing public `sendData()`.

Verify: `reservedTopic::max_reserved` is `0x10`; `topics::health` is `0x15`. ✓

---

## Step 7 — `duckesp::getFreeHeap()`

Confirm `getFreeHeap()` exists in `DuckEsp.h`/`DuckEsp.cpp`, or replace with
`ESP.getFreeHeap()` directly in `sendHealthPacket()` if the wrapper is absent.

---

## Verification checklist

- [ ] Builds cleanly (no missing includes, no linker errors)
- [ ] `updateNeighborCache` called immediately after `readReceivedData()` in all 3 duck types
- [ ] `sendHealthPacket()` fires every 5 min when PUBLIC, not before first `HEALTH_INTERVAL`
- [ ] Health JSON appears on MQTT with correct keys (`id`, `up`, `heap`, `nb`, optional `usr`)
- [ ] `nb[].t` is ms-since-last-heard (not a raw timestamp)
- [ ] Cache entries do not exceed 8 in the packet (cap enforced)
- [ ] `setHealthData()` string appears under `usr` key, omitted when empty

---

## Notes

- The cache is **unbounded** — in a dense network with many transient senders this could grow. Cap it later if needed (max 8 entries, evict by `lastSeen`).
- RSSI/SNR are read immediately after `readReceivedData()` while the radio values are still fresh. Do not move the `updateNeighborCache` call later in the handler.
- The `"t"` field in the health packet is milliseconds since last heard, not a timestamp — easier to reason about on the consumer side.
