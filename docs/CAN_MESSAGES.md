# CAN message reference - tested 2023 Ducato cluster

Cluster part number: **00468637450** (Fiat Ducato, 2023). Bench only, cluster removed from the vehicle.

Classical CAN, standard 11-bit IDs unless shown with 8 hex digits (29-bit). Hexadecimal throughout. Byte indexes are **zero-based b0...b7**. These are behaviors I observed on my cluster with a Nano + MCP2515 (8 MHz) + TJA1050, not an OEM database. Many fields are only partly decoded.

## The cluster is on two CAN buses

| Bus | Speed | Cluster pins | What lives there |
|---|---|---|---|
| Body CAN | **125 kbit/s** | 5 / 6 (same positions as the 2009 X250) | Body status 356 |
| Second CAN | **500 kbit/s** | L: 11 and 12, H: 13 and 14 | RPM, speed, fuel, ABS/ESC, airbag, engine warnings |

The 500 kbit/s bus has two CAN-L pins (11, 12) and two CAN-H pins (13, 14), the same bus: 11-12 and 13-14 measure ~0 Ω with the cluster unpowered. All 500k tests here used **pins 12 (L) and 13 (H)**.

**None of the old X250 IDs (380, 281, 2A0, 180, 286, 39A, 3C0) do anything on this cluster.**

## Frames the cluster transmits by itself

| Bus | ID | DLC | Rate | Notes |
|---|---|---:|---|---|
| 125k | 0E0 | 4 | 10 Hz | |
| 125k | 468 | 2 | 2 Hz | |
| 125k | 50E, 5BC, 5C0, 5C4, 5CD, 5CE | 8 | 1 Hz | |
| 125k | 760, 762 | 6 | 1 Hz | |
| 125k | 764 | 6 | 1 Hz | b1 counts minutes since power-up |
| 125k | 1E340003 | 2 | 0.5 Hz | `00 3E`, looks like network management, node 03 |
| 500k | 0E0 | 4 | 10 Hz | |
| 500k | 256, 259 | 8 | 10 Hz | 259 b5 changes when 0DE appears/disappears |
| 500k | 73A | 6 | 1 Hz | b1 counts minutes since power-up |
| 500k | 1E360003 | 1 | ~1.5 Hz | `0F`; changed to `07` when I sent 1E360000-1E360004 (network management) |

## "Clean cluster" set (500 kbit/s)

With these eight frames running, the cluster behaves like a running van with no warning lamps, except the coolant bar (not found yet):

```
rep 0FC 50 20 40 00 00 00 00 FF 00    RPM 2000 + engine running
rep 101 100 00 C8 00 00 00 00 00 00   speed ~100 km/h
rep 545 100 00 00 08 00 00 00 00 00   fuel 6/9 segments, bright illumination
rep 0DE 100 FF FF FF FF FF FF FF FF   steering (EPS) present -> lamp off
rep 4AF 100 00 80 00 00 00 00 00 00   ABS/ESC present -> lamps off
rep 257 100 20 00 00 00 00 00 00 00   airbag/seatbelt lamps off
rep 5AE 100 00 00 00 00 00 00 00 00   engine warnings off
rep 226 100 00 00 00 00 00 00 00 00   engine/AdBlue present -> MIL off
```

Several lamps work as "module missing" timeouts: the lamp comes on when a frame stops arriving and goes off while it keeps arriving (0DE, 4AF, 257, 226).

## 0FC - RPM and engine running (500k), every 50 ms

Example 2000 RPM: `20 40 00 00 00 00 FF 00`

| Field | Observed behavior |
|---|---|
| b0 | RPM / 64 (approx.): 10 = 1000, 20 = 2100, 30 = 3000, 40 = 4000, 50 = 5000 on the needle |
| b1 | `40` valid. `1F` dropped the needle to 0. Probably low bits of the value plus status flags |
| b6 | Must be non-zero (`FF` works). Without it the RPM needle stays asleep. Only the RPM needle: the speed needle wakes up from 101 alone. No single bit was enough; exact value not found yet |
| b2-b5, b7 | `00` is fine; meaning unknown |

The cluster's RPM dial ends at 6000.

## 101 - Vehicle speed (500k)

Example ~100 km/h: `00 C8 00 00 00 00 00 00`

| Field | Observed behavior |
|---|---|
| b0:b1 | Big-endian value = km/h x 2 (0.5 km/h per bit) |
| b2-b7 | `00`; not tested |

Measured on the needle (with 0FC running): 0040 -> 35, 0082 -> 70, 00C0 -> 100, 00FF -> 130, 0100 -> 130, 0200 -> max. The needle reads a few percent high, like a real speedometer. With all bytes `00` the needle wakes up and shows 0.

## 545 - Fuel level, illumination, aux heater (500k)

Example 6 segments, bright: `00 00 08 00 00 00 00 00`

| Field | Observed behavior |
|---|---|
| b1 | `FF`: "fuel cut-off disabled" message |
| b2 | Fuel level, coarse. Bit 20: "auxiliary heater unavailable" |
| b3 | Fuel level, fine (upper bits). `FF`: "fuel level disabled" message |
| b4 | Illumination: `00` bright, `FF` dimmed. No intermediate levels |
| b5-b7 | No reaction to `FF` |

Fuel bar (9 LCD segments), b3 = 00 unless noted:

| b2 | Segments |
|---|---:|
| 00, 01 | 3 |
| 04 | 4 |
| 06 | 5 |
| 08 | 6 |
| 08 with b3 = 80 | 7 |
| 0A | 8 |
| 0C and above | 9 (full) |

00/01 showing 3 segments instead of empty is unresolved.

## 4AF - ABS / ESC (500k)

Sending the frame turns the ABS/traction lamps off; single bits on a `00` base:

| Field | Observed behavior |
|---|---|
| b0 mask 80 | "ABS disabled" message, traction lamps off |
| b0 mask 40 | "ABS disabled" message, ABS + traction lamps off |
| b0 mask 10 | **EBD fault**: red brake lamp (shared with the parking brake symbol), sometimes "Brake system fault, see handbook" + one chime. Not the parking brake |
| b0 mask 08 | Traction control lamp flashing (wheel slip) |
| b0 mask 01 | "ESC disabled" + lamp |
| b1 mask 80 | "ESC active", lamps off |
| b1 mask 40 | "Hill Hold Control disabled" |
| other bits | No reaction |

b0 bits combine (e.g. `11` = EBD + ESC off, `90` = EBD + ABS off). The red brake lamp lit at power-up only goes off when 4AF **and 101 (speed)** are both being sent. The real parking brake signal was not found; on the X250 it was in the body frame, so it is probably on the 125k bus here.

## 257 - Airbag and seatbelts (500k)

| Field | Observed behavior |
|---|---|
| b0 mask 80 | "Have airbag checked", seatbelt lamp off |
| b0 mask 20 | Airbag and seatbelt lamps both off (normal state) |
| b0 mask 40 | Airbag lamp flashing |
| b4 mask 80, b4 mask 04, b5 mask 20, b5 mask 01 | Seatbelt symbols on the LCD (different seat positions; not mapped) |

## 226 - Engine status: start-stop, AdBlue, driver messages (500k)

Sending `00 00 00 00 00 00 00 00` turns the **engine warning lamp (MIL) off**; when 226 is missing the cluster lights the MIL. `FF FF FF FF 06 FF FF FF` lights the MIL as a side effect of the AdBlue "engine will not restart" fault.

| Field | Observed behavior |
|---|---|
| b0 mask 80 | **AdBlue lamp** (confirmed with the handbook) |
| b2 mask 80 | "Start-stop disabled, restart" |
| b3 mask 01 | "Press the clutch fully" |
| b4 mask 04 | "Engine will stop in -- km, check AdBlue system" |
| b4, b5 (several low bits) | AdBlue warnings; looks like a number (remaining km), not decoded |
| b5 = 64 | "Engine will not restart, refill AdBlue" |

## 5AE - Engine warnings and oil service (500k)

| Field | Observed behavior |
|---|---|
| b3 = FF | Glow plug lamp |
| b4 mask 10 | **Engine warning lamp (MIL)**, no text (once seen together with an "Oil change done" message) |
| b4 mask 20 | Red coolant temperature lamp |
| b4 mask 80 | Fuel filter |
| b4 = FF | Red coolant + DPF + fuel filter (DPF bit not isolated) |
| `00 82 40 00 ...` | "Have the engine oil changed" |
| `00 82 10 00 ...` | "Oil change done" |
| `00 00 82 40 ...` | Oil warning symbol on the LCD |

## Other IDs confirmed with all-FF data (500k), not decoded

| ID | Reaction |
|---|---|
| 0DE | Steering / EPS. Present: lamp off. Missing: "Have steering checked" |
| 0FF | "Fuel cut-off activated" (crash signal) |
| 384 | ECO |
| 4B1 | "Brake fluid low" |
| 4B2 | Oil level messages ("oil max", "check oil level") |
| 5A5 | "Speed limiter disabled" |

## 356 - Body status (125k)

Single bits on a `00` base:

| Field | Observed behavior |
|---|---|
| b0 mask 20 | Immobilizer lamp flashing |
| b0 mask 01 | Immobilizer lamp flashing |
| b1 mask 08 | Immobilizer lamp steady |
| b3 mask 80 | Door open |
| b3 mask 04 | Bonnet open |
| b4 mask 40 | Door open (another door) |
| b4 mask 08, b4 mask 02 | Reaction seen, not identified |
| b5 mask 40 | "Key lock disabled" |
| b6 mask 80 | Start-stop active |

All 256 values of b0 were tried; none woke the needles.

## Not found yet

- **Coolant temperature bar** (9 LCD segments). Not found on 500k with FF, 82, 80 or C0 fills, in 0FC/5AE/101 spare bytes, or around 0F0-0FF and 5A0-5BF. Next: the 125k bus while 500k runs from a second controller.
- Parking brake, turn signals, headlights, cruise control (probably 125k body bus); exact b6 value in 0FC, DPF bit in 5AE, individual seats in 257, low-fuel lamp, oil pressure lamp.
- Lane departure / driver-assist symbols never appeared; probably disabled in this cluster's configuration.

## Things that did nothing

Old X250 IDs; 1E34xxxx, 1E344000, 0E094000 and 0A18A000 network-management/ignition guesses; 350, 384 (as ignition), 1DC, 3B0, 3B4 (suggested by an AI, no source); 4B4, 4B5, 050-060.
