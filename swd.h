/**
 * swd.h — Bit-banged ARM SWD (Serial Wire Debug) driver for Flipper Zero
 *
 * Target:  N32G031K8Q7-1 (ARM Cortex-M0) inside RAZ DC25000 vape
 * Host:    Flipper Zero (STM32WB55, 64 MHz)
 *
 * Pin wiring (Flipper GPIO header → USB-C plug on vape):
 *   Flipper PA7  (GPIO header pin 11)  →  SWDIO  →  USB-C CC1
 *   Flipper PA6  (GPIO header pin 10)  →  SWCLK  →  USB-C CC2
 *   Flipper GND  (GPIO header GND)     →  GND    →  USB-C GND shell
 *
 * The vape is self-powered; USB-C carries only the CC/SWD signals.
 *
 * Protocol:  ARM ADIv5 SWD
 * Reference: ARM IHI0031 (ADIv5 Architecture Specification)
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * SWD pin definitions
 * ------------------------------------------------------------------------- */
#include <furi_hal_gpio.h>

#define SWD_SWDIO (&gpio_ext_pa7)
#define SWD_SWCLK (&gpio_ext_pa6)

/* ---------------------------------------------------------------------------
 * Return / status codes
 *
 * SWD_ACK_OK, SWD_ACK_WAIT, SWD_ACK_FAULT match the 3-bit ACK field values
 * driven by the target on the wire.  SWD_ERR_* are host-generated errors.
 * ------------------------------------------------------------------------- */
typedef enum {
    SWD_ACK_OK        = 1,  /**< Target accepted the transaction              */
    SWD_ACK_WAIT      = 2,  /**< Target busy — caller may retry               */
    SWD_ACK_FAULT     = 4,  /**< Target flagged a fault in CTRL/STAT          */
    SWD_ERR_NO_TARGET = 8,  /**< No valid IDCODE after 3 line-reset attempts  */
    SWD_ERR_TIMEOUT   = 16, /**< Power-up ack or other poll timed out         */
} SWDAck;

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * swd_connect() — full ARM ADIv5 connect sequence.
 *
 * Steps performed:
 *   1. Line reset (56 clocks HIGH) — up to 3 retries
 *   2. JTAG-to-SWD switch (0xE79E, 16 bits LSB-first)
 *   3. Line reset again + 4 idle clocks
 *   4. Read DP IDCODE — verify bit 0 == 1
 *   5. Write DP CTRL/STAT = 0x50000000 (request system + debug power)
 *   6. Poll CTRL/STAT until CSYSPWRUPACK (bit 31) + CDBGPWRUPACK (bit 29) set
 *   7. Write DP SELECT = 0x00000000 (AP 0, bank 0)
 *   8. Write AP CSW = 0x00000002 (32-bit word transfers, AddrInc off)
 *
 * Returns SWD_ACK_OK on success, SWD_ERR_NO_TARGET or SWD_ERR_TIMEOUT on
 * failure, or the raw ACK value from a failed DP/AP access.
 */
SWDAck swd_connect(void);

/**
 * swd_disconnect() — tri-state both SWD pins and release them.
 *
 * Call this when you are finished; it returns the Flipper GPIO pins to
 * high-impedance input so they do not interfere with other use.
 */
void swd_disconnect(void);

/**
 * swd_read32() — read one 32-bit word from the target's memory map.
 *
 * Uses MEM-AP TAR/DRW with AP-read pipeline correction (result captured via
 * DP RDBUFF).
 *
 * @param addr   Target byte address (must be 32-bit aligned).
 * @param out    Receives the value read from the target.
 * @return       SWD_ACK_OK on success, or an ACK/error code on failure.
 */
SWDAck swd_read32(uint32_t addr, uint32_t* out);

/**
 * swd_write32() — write one 32-bit word to the target's memory map.
 *
 * Uses MEM-AP TAR/DRW.
 *
 * @param addr   Target byte address (must be 32-bit aligned).
 * @param val    Value to write.
 * @return       SWD_ACK_OK on success, or an ACK/error code on failure.
 */
SWDAck swd_write32(uint32_t addr, uint32_t val);

/**
 * swd_halt() — halt the Cortex-M0 core via DHCSR.
 *
 * Writes 0xA05F0003 (DBGKEY | C_HALT | C_DEBUGEN) to 0xE000EDF0.
 *
 * @return SWD_ACK_OK on success, or an ACK/error code on failure.
 */
SWDAck swd_halt(void);

/**
 * swd_reset_and_run() — trigger a system reset and release halt.
 *
 * Writes AIRCR = 0x05FA0004 (VECTKEY | SYSRESETREQ) to 0xE000ED0C.
 * The core runs from reset; no halt is re-applied.
 *
 * @return SWD_ACK_OK on success, or an ACK/error code on failure.
 */
SWDAck swd_reset_and_run(void);

#ifdef __cplusplus
}
#endif
