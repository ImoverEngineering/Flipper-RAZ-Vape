# Flipper-RAZ-Vape

Flash custom firmware onto RAZ DC25000 disposable vapes (and compatible devices) directly from a Flipper Zero, using the USB-C charge port as an SWD debug interface.

---

> **WARNING — READ BEFORE PROCEEDING**
>
> This project modifies the firmware of a lithium battery-powered device.
> Incorrect use can cause **device damage, battery over-discharge, thermal runaway, or fire**.
> The vape contains a lithium polymer cell — treat it with the same respect as any LiPo battery.
>
> - You proceed **entirely at your own risk**.
> - The authors accept no liability for damaged hardware, voided warranties, injuries, or any other consequence.
> - Do not leave a device unattended during or after flashing.
> - Do not flash a device with a visibly damaged, swollen, or leaking battery.

---

## What It Does

Flipper-RAZ-Vape is a Flipper Zero FAP (Flipper Application Package) that bit-bangs ARM SWD
over the Flipper's GPIO header to flash a `.bin` firmware image onto the vape's
**N32G031K8Q7-1** microcontroller. The SWD lines are accessible without disassembly — they are
routed to the USB-C charge port's CC pins.

Custom firmware is built with the companion **Vaporware SDK**
([ImoverEngineering/Vaporware](https://github.com/ImoverEngineering/Vaporware)), included here as
a git submodule under `vaporware/`. Pre-built binaries for the bundled apps are attached to each
GitHub release.

---

## Requirements

| Item | Notes |
|------|-------|
| Flipper Zero | Stock firmware; no jailbreak needed |
| USB-C SWD cable | DIY — see [CABLE_WIRING.md](CABLE_WIRING.md) |
| Target device | RAZ DC25000, GV2024 V1/V8, or any N32G031K8Q7-1 + GC9107 device |
| Firmware `.bin` | From releases or built from Vaporware SDK |

---

## Supported Devices

| Device | MCU | Notes |
|--------|-----|-------|
| RAZ DC25000 | N32G031K8Q7-1 | Primary target |
| GV2024 V1 | N32G031K8Q7-1 | Compatible |
| GV2024 V8 | N32G031K8Q7-1 | Compatible |
| Any N32G031K8Q7-1 + GC9107 device | N32G031K8Q7-1 | Likely compatible |

---

## Installation

### Install the FAP

1. Download the latest `.fap` file from the [Releases](../../releases) page.
2. Copy it to your Flipper's SD card at:
   ```
   SD:/apps/Tools/flipper_raz_vape.fap
   ```
3. Eject the SD card safely, or use qFlipper / Flipper Mobile to transfer the file.

### Get Firmware Binaries

**Option A — Pre-built (recommended):** Download `.bin` files from the [Releases](../../releases)
page. Available apps: `flappy` (Flappy Bird), `slots` (slot machine), `template` (skeleton).
Copy the `.bin` to your Flipper SD card anywhere convenient (e.g., `SD:/vape/`).

**Option B — Build from source:** See [Building Firmware Binaries](#building-firmware-binaries) below.

---

## Usage

1. **Wire the cable** — follow [CABLE_WIRING.md](CABLE_WIRING.md) to build and connect the USB-C
   SWD cable between your Flipper and the vape.
2. **Plug in the cable** — insert the USB-C end into the vape in the correct orientation
   (see cable wiring doc). Do not connect VBUS to the Flipper.
3. **Power the vape on** — the vape must be active; SWD is inaccessible when it is asleep or
   powered off.
4. **Open the app** on the Flipper: `Apps → Tools → Flipper RAZ Vape`
5. **Browse** to the `.bin` file using the file picker.
6. **Hold OK** to acknowledge the disclaimer.
7. **Confirm** — the Flipper erases and flashes the target. This takes a few seconds.
8. **Done** — the vape reboots into the new firmware automatically.

> If "SWD connect failed" appears, unplug the USB-C connector, flip it 180°, and try again.
> See [Troubleshooting](#troubleshooting).

---

## GPIO Pin Mapping

Connect the Flipper GPIO header to the USB-C SWD cable as follows:

```
Flipper GPIO Header (top view, left edge of device)
Pin 1  [ 5V    ] [ GND   ] Pin 2   ← do not use 5V
Pin 3  [ 3.3V  ] [ GND   ] Pin 4   ← do not use 3.3V
Pin 5  [ PA4   ] [ PB3   ] Pin 6
Pin 7  [ PA5   ] [ PB2   ] Pin 8
Pin 9  [ PA6   ] [ PC3   ] Pin 10  ← SWCLK (PA6)
Pin 11 [ PA7   ] [ PC1   ] Pin 12  ← SWDIO (PA7)
Pin 13 [ PA2   ] [ PC0   ] Pin 14
Pin 15 [ PA3   ] [ PB6   ] Pin 16
Pin 17 [ PB7   ] [ GND   ] Pin 18  ← GND
```

| Flipper GPIO | Flipper Label | Signal | USB-C Pin |
|-------------|---------------|--------|-----------|
| Pin 11 | PA7 | SWDIO | A5 (CC1) |
| Pin 10 | PA6 | SWCLK | B5 (CC2) |
| Pin 8 or 18 | GND | GND | GND |

> **Do not connect VBUS or 3.3V.** The vape is self-powered from its internal battery.

---

## Building the FAP from Source

```bash
# Install ufbt
pip install ufbt

# Clone the repo with submodules
git clone --recurse-submodules https://github.com/ImoverEngineering/Flipper-RAZ-Vape.git
cd Flipper-RAZ-Vape

# Build
ufbt build
```

Output `.fap` will be in `dist/`.

To deploy directly to a connected Flipper:

```bash
ufbt launch
```

---

## Building Firmware Binaries

Firmware source lives in the [Vaporware SDK](https://github.com/ImoverEngineering/Vaporware)
submodule at `vaporware/`.

**Linux / CI:**

```bash
make flappy    # build Flappy Bird firmware
make slots     # build slot machine firmware
make all       # build all apps
```

**Windows:**

Run the provided batch scripts from inside `vaporware/`:

```bat
build_flappy.bat
```

Output `.bin` files land in `vaporware/dist/` (or per-app output dirs — check the Vaporware docs).

---

## How It Works

The RAZ DC25000 uses an **N32G031K8Q7-1** ARM Cortex-M0+ microcontroller. Its SWD debug port is
connected internally to the USB-C charge connector's CC pins:

- **CC1 (USB-C pin A5)** = SWDIO
- **CC2 (USB-C pin B5)** = SWCLK

The Flipper Zero bit-bangs the **ARM ADIv5** SWD protocol on PA7/PA6 to reach the CPU's Debug
Port (DP), then accesses the AHB-AP to read/write flash via the **N32G031 flash controller**
(unlock sequence, page erase, half-word program). No proprietary tools or OpenOCD instance is
needed — everything runs on the Flipper.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| "SWD connect failed" | Wrong USB-C orientation | Unplug, flip 180°, retry |
| "SWD connect failed" | Vape is asleep/off | Power the vape on first |
| "SWD connect failed" | Loose wiring | Check all three connections (SWDIO, SWCLK, GND) |
| Flash stalls partway | Cable too long or noisy | Use a shorter cable (15-30 cm) |
| Vape unresponsive after flash | Bad firmware image | Re-flash the OEM or a known-good `.bin` |

---

## License

MIT — see [LICENSE](LICENSE).

Vaporware SDK is a separate project with its own license; see `vaporware/LICENSE`.
