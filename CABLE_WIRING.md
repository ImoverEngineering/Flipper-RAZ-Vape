# Cable Wiring — USB-C SWD to Flipper GPIO

This document covers how to build the cable that connects a Flipper Zero's GPIO header to the
RAZ vape's SWD debug interface, which is exposed on the USB-C charge port's CC pins.

---

> **WARNING**
>
> Do **not** connect VBUS or any power rail from the Flipper to the vape's USB-C port.
> The vape is self-powered from its internal lithium battery.
> Connecting power lines risks charging circuit conflicts or damage.

---

## Overview

The N32G031K8Q7-1 MCU inside the vape has its SWD port wired internally to the USB-C connector's
CC (Configuration Channel) pins. These are normally used by USB-C for orientation detection but
are repurposed here as debug lines:

- **CC1** = SWDIO (Serial Wire Data I/O)
- **CC2** = SWCLK (Serial Wire Clock)

A short cable with a USB-C male plug on one end and female Dupont connectors on the other is all
that is needed.

---

## Parts

| Part | Description | Approximate cost |
|------|-------------|-----------------|
| USB-C male breakout plug/board | Exposes CC1, CC2, GND as individual pins or pads | ~$3 on AliExpress |
| Female Dupont connectors (×3) | Fit onto Flipper GPIO header pins | ~$1 for a strip |
| Wire | 24–28 AWG, 15–30 cm per lead | On hand |

**Search terms:** "USB-C male breakout", "USB-C plug breakout board", "USB-C test board with CC pins".

You want a board that exposes **CC1, CC2, and GND as separate pads or pins** — not just VBUS and
GND. Boards designed for USB-C sink/source testing typically do this.

A cable longer than ~30 cm can introduce noise that causes SWD errors. Keep it short.

---

## USB-C Pin Reference

Only three pins matter. Leave everything else unconnected.

| USB-C Pin | Signal | Function | Connect to |
|-----------|--------|----------|------------|
| A5 | CC1 | SWDIO | Flipper Pin 11 (PA7) |
| B5 | CC2 | SWCLK | Flipper Pin  9 (PA6) |
| A1 / A12 / B1 / B12 | GND | Ground | Flipper GND (Pin 8 or 18) |
| All others | — | Leave unconnected | — |

> **CC1 and CC2 are on opposite sides of the USB-C connector** (A5 and B5). On a standard male
> USB-C breakout these are often labeled `CC1` and `CC2` or just `CC`.

---

## Wiring Table

| USB-C Pin | Signal | Flipper GPIO Pin | Flipper Label |
|-----------|--------|-----------------|---------------|
| A5 (CC1) | SWDIO | Pin 11 | PA7 |
| B5 (CC2) | SWCLK | Pin  9 | PA6 |
| GND | GND | Pin 8 or 18 | GND |

---

## Flipper GPIO Header Diagram

Top-down view of the 18-pin GPIO header on the right edge of the Flipper Zero.
Pins 10 and 11 are SWCLK and SWDIO. Either GND pin (8 or 18) works.

```
       [left edge / back of Flipper]
       _______________________________
      |                               |
 1 ── │  5V    │  GND  │ ── 2        |
 3 ── │  3.3V  │  GND  │ ── 4        |
 5 ── │  PA4   │  PB3  │ ── 6        |
 7 ── │  PA5   │  PB2  │ ── 8  [GND] |  ← use this GND, or pin 18
 9 ── │  PA6   │  PC3  │ ── 10       |  ← pin 9 = SWCLK
11 ── │  PA7   │  PC1  │ ── 12       |  ← pin 11 = SWDIO
13 ── │  PA2   │  PC0  │ ── 14       |
15 ── │  PA3   │  PB6  │ ── 16       |
17 ── │  PB7   │  GND  │ ── 18 [GND] |  ← or use this GND
      |_______________________________|
```

Wait — pin numbering: the header runs top to bottom, odd pins on the left, even pins on the right.

```
Pin  1  [  5V   ]  [  GND  ]  Pin  2
Pin  3  [ 3.3V  ]  [  GND  ]  Pin  4
Pin  5  [  PA4  ]  [  PB3  ]  Pin  6
Pin  7  [  PA5  ]  [  PB2  ]  Pin  8   ← GND (option A)
Pin  9  [  PA6  ]  [  PC3  ]  Pin 10   ← PA6 = SWCLK
Pin 11  [  PA7  ]  [  PC1  ]  Pin 12   ← PA7 = SWDIO
Pin 13  [  PA2  ]  [  PC0  ]  Pin 14
Pin 15  [  PA3  ]  [  PB6  ]  Pin 16
Pin 17  [  PB7  ]  [  GND  ]  Pin 18   ← GND (option B)
```

Pins to connect:

- **Pin 9 (PA6)** — SWCLK — to USB-C CC2 (B5)
- **Pin 11 (PA7)** — SWDIO — to USB-C CC1 (A5)
- **Pin 8 or 18 (GND)** — GND — to USB-C GND

---

## Orientation

> **The USB-C connector is orientation-sensitive for SWD.**
>
> USB-C is physically reversible, but CC1 and CC2 swap sides when the plug is flipped.
> The correct orientation has been confirmed on production hardware via ST-Link:
> **CC1 = SWDIO (PA7, pin 11)** and **CC2 = SWCLK (PA6, pin 9)**.
>
> There is only one correct orientation. If the connection fails, flip the plug 180° and retry.
> Once you find the working side, mark it with a small dot of nail polish or a permanent marker.

---

## Do Not Connect Power

The vape is powered by its own internal LiPo battery. Do not wire:

- USB-C VBUS (A4/A9/B4/B9) to any Flipper pin
- USB-C VBUS to the Flipper's 3.3V or 5V rail
- Any Flipper power pin to the vape

Connecting VBUS can interfere with the vape's charging IC and may cause damage.

---

## Photo / Diagram

*(A photo of the completed cable and Flipper connection would go here.
Contributions welcome — open a PR with an image in `docs/`.)*

---

## Alternative: Direct Pogo-Pin Connection (No USB-C)

If you have a Flipper GPIO header breakout board and access to the vape's PCB test pads, you can
skip the USB-C cable entirely and connect pogo pins directly to the SWD pads:

| MCU pad | Signal | Flipper GPIO |
|---------|--------|-------------|
| PA13 | SWDIO | Pin 11 (PA7) |
| PA14 | SWCLK | Pin  9 (PA6) |
| GND | GND | Pin 8 or 18 |

This requires partially disassembling the vape to expose the PCB, but gives a more reliable
connection and removes any ambiguity about USB-C orientation.
