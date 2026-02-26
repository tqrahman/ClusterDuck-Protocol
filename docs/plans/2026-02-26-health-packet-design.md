# Health Packet Design

**Date:** 2026-02-26
**Branch:** v5-rssi
**Status:** Approved

---

## Problem

Once CDP routes are established the routing table is never refreshed. RREQ/RREP
only fires at join time (every 15 s) or on-demand when a destination goes missing
(every 30 s). This means neighbor RSSI/SNR values in the routing table can be
hours old. There is also no mechanism to surface battery voltage, ESP health
metrics, or other node-level diagnostics to the cloud.

---

## Goals

1. Give the Papa (and by extension MQTT consumers) a periodic, current snapshot
   of every node's RF neighborhood and health state.
2. Keep neighbor signal data continuously fresh without extra packets.
3. Let sketch authors inject board-specific metrics (battery, solar, sensors)
   alongside CDP-owned system metrics.

---

## Non-Goals

- Route refreshing (RREQ handles that on-demand).
- Changing the RREQ/RREP RSSI/SNR path work already shipped on this branch.
- Defining the ESP internal health metrics (next task, separate PR).

---

## Architecture

```
Every received packet
        │
        ▼
[Duck::handleReceivedPacket]
        │  reads getRSSI()/getSNR() from radio
        │  calls router.updateNeighborCache(rxPacket.sduid, rssi, snr)
        ▼
[DuckRouter::neighborCache]
  unordered_map<string duid, NeighborCacheEntry{rssi, snr, lastSeen}>
        │
        ▼  every HEALTH_INTERVAL (5 min), when PUBLIC
[Duck::sendHealthPacket()]
  builds JSON: uptime + heap + neighborCache + usr payload
        │
        ▼
  CdpPacket (topic: health) → PAPADUCK_DUID → Papa → MQTT
```

A **separate flat cache** (`neighborCache`) is used instead of the existing
`routingTable` because the routing table is keyed by destination DUID and the
same physical node can appear multiple times as a next-hop for different
destinations. The cache is keyed by sender DUID (`rxPacket.sduid`) and
represents only direct, one-hop neighbors.

---

## Data Structures

### `NeighborCacheEntry`

New struct, lives in `src/routing/` alongside `Neighbor.h`:

```cpp
struct NeighborCacheEntry {
    int8_t        rssi;     // raw RSSI in dBm
    int8_t        snr;      // raw SNR in dB
    unsigned long lastSeen; // millis() timestamp of last received packet
};
```

Raw dBm/dB values (not normalized) are used here because MQTT consumers
need human-readable RF units, not the internal [0,1] routing score.

### `DuckRouter` additions

```cpp
// new member
std::unordered_map<std::string, NeighborCacheEntry> neighborCache;

// new method
void updateNeighborCache(const Duid& sender, int8_t rssi, int8_t snr);

// new method
const std::unordered_map<std::string, NeighborCacheEntry>& getNeighborCache() const;
```

`updateNeighborCache` is called in `handleReceivedPacket()` in every duck type
(MamaDuck, PapaDuck, DuckLink) as the first action after the packet is read from
the radio, before any topic-specific handling.

---

## Health Packet Format

Sent on the already-reserved `health` topic to `PAPADUCK_DUID`.

```json
{
  "id":   "aabbccddeeff0011",
  "up":   123456,
  "heap": 45000,
  "nb": [
    {"id": "hex1", "r": -88, "s":  7, "t": 12400},
    {"id": "hex2", "r": -95, "s": -2, "t": 45000}
  ],
  "usr": "{\"batt\":3.7,\"solar\":1.1}"
}
```

| Key      | Type    | Source | Description                                      |
|----------|---------|--------|--------------------------------------------------|
| `id`     | string  | CDP    | This duck's DUID as hex string                   |
| `up`     | uint32  | CDP    | Uptime in ms (`millis()`)                        |
| `heap`   | uint32  | CDP    | Free heap bytes (`ESP.getFreeHeap()`)            |
| `nb`     | array   | CDP    | Neighbor cache entries (count capped, TBD)       |
| `nb[].id`| string  | CDP    | Neighbor DUID as hex string                      |
| `nb[].r` | int8    | CDP    | Raw RSSI in dBm                                  |
| `nb[].s` | int8    | CDP    | Raw SNR in dB                                    |
| `nb[].t` | uint32  | CDP    | ms since last heard (`millis() - lastSeen`)      |
| `usr`    | string  | Sketch | Raw JSON string injected by sketch, merged as-is |

**Neighbor cap:** exact limit TBD when health packet is implemented, based on
remaining byte budget after system fields. The `usr` field size also affects
this. Expect 4–5 neighbors with a typical `usr` payload.

### Sketch API

```cpp
// Call any time before the 5-minute timer fires.
// CDP stores the string and includes it verbatim in the next health packet.
duck.setHealthData("{\"batt\":3.7,\"solar\":1.1}");
```

The stored string is included under the `usr` key. CDP does not parse or
validate it — the sketch author is responsible for valid JSON.

---

## Timer

New members in `Duck` base class:

```cpp
unsigned long lastHealthTime = 0L;
#define HEALTH_INTERVAL 300000L  // 5 minutes
```

Checked inside `run()` when `NetworkState == PUBLIC`:

```cpp
if (millis() - lastHealthTime > HEALTH_INTERVAL) {
    sendHealthPacket();
    lastHealthTime = millis();
}
```

The first health packet fires `HEALTH_INTERVAL` after the duck enters PUBLIC
state, not immediately on join, to avoid a burst of packets during network
formation.

---

## Files to Modify

| File | Change |
|------|--------|
| `src/routing/NeighborCacheEntry.h` | New struct |
| `src/routing/DuckRouter.h` | Add `neighborCache`, `updateNeighborCache()`, `getNeighborCache()` |
| `src/routing/DuckRouter.cpp` | Implement `updateNeighborCache()` |
| `src/Ducks/Duck.h` | Add `lastHealthTime`, `HEALTH_INTERVAL`, `sendHealthPacket()`, `setHealthData()`, `userHealthData` member |
| `src/Ducks/MamaDuck.h` | Call `updateNeighborCache()` at top of `handleReceivedPacket()` |
| `src/Ducks/PapaDuck.h` | Call `updateNeighborCache()` at top of `handleReceivedPacket()` |
| `src/Ducks/DuckLink.h` | Call `updateNeighborCache()` at top of `handleReceivedPacket()` |

---

## Open Questions

- **Neighbor cap:** exact number TBD at implementation time based on byte budget.
- **ESP internal health metrics:** uptime and heap are included now; additional
  metrics (CPU freq, flash size, WiFi RSSI) are a follow-on task.
- **`usr` size budget:** sketch authors need guidance on max safe payload size.
  A comment in the API doc pointing to the 229-byte limit is sufficient for now.
