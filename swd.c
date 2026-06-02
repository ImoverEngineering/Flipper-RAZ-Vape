/**
 * swd.c — Bit-banged ARM SWD (Serial Wire Debug) driver for Flipper Zero
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
 * Timing note:
 *   Each half-clock is separated by SWD_HALF_PERIOD_US microseconds.
 *   At 64 MHz the GPIO toggle + furi_delay_us(1) yields a safe ~200 kHz
 *   SWD clock, well within what the N32G031 debug port handles.
 *
 * Protocol:  ARM ADIv5 SWD
 * Reference: ARM IHI0031 (ADIv5 Architecture Specification)
 */

#include "swd.h"

#include <stdint.h>
#include <stdbool.h>
#include <furi_hal_gpio.h>
#include <furi.h>

/* ---------------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------------- */

/** Half-period of the SWD clock in microseconds.  2 us → ~250 kHz. */
#define SWD_HALF_PERIOD_US (2U)

/* ---------------------------------------------------------------------------
 * DP register addresses  (A[3:2] field of the 8-bit request byte)
 * The address encodes bits [3:2] only; bit 1 and bit 0 are always 0.
 * We pass the full byte-address and extract A[3:2] in swd_build_request().
 * ------------------------------------------------------------------------- */
#define DP_REG_IDCODE    (0x00U) /* read  */
#define DP_REG_ABORT     (0x00U) /* write */
#define DP_REG_CTRLSTAT  (0x04U)
#define DP_REG_SELECT    (0x08U)
#define DP_REG_RDBUFF    (0x0CU)

/* ---------------------------------------------------------------------------
 * AP (MEM-AP) register addresses — bank 0 assumed via SELECT=0
 * ------------------------------------------------------------------------- */
#define AP_REG_CSW (0x00U)
#define AP_REG_TAR (0x04U)
#define AP_REG_DRW (0x0CU)

/* ---------------------------------------------------------------------------
 * Cortex-M0 debug registers
 * ------------------------------------------------------------------------- */
#define DHCSR_ADDR  (0xE000EDF0UL)
#define AIRCR_ADDR  (0xE000ED0CUL)

#define DHCSR_HALT_VAL  (0xA05F0003UL) /* DBGKEY | C_HALT | C_DEBUGEN */
#define AIRCR_RESET_VAL (0x05FA0004UL) /* VECTKEY | SYSRESETREQ       */

/* ---------------------------------------------------------------------------
 * Power-up polling limit
 * ------------------------------------------------------------------------- */
#define PWRUP_POLL_LIMIT (1000U)

/* ===========================================================================
 * Low-level bit-bang helpers
 * ======================================================================== */

/** Drive SWCLK high. */
static inline void clk_high(void) {
    furi_hal_gpio_write(SWD_SWCLK, true);
}

/** Drive SWCLK low. */
static inline void clk_low(void) {
    furi_hal_gpio_write(SWD_SWCLK, false);
}

/** Drive SWDIO high (output mode must already be set). */
static inline void dio_high(void) {
    furi_hal_gpio_write(SWD_SWDIO, true);
}

/** Drive SWDIO low (output mode must already be set). */
static inline void dio_low(void) {
    furi_hal_gpio_write(SWD_SWDIO, false);
}

/** Switch SWDIO to output push-pull. */
static inline void dio_output(void) {
    furi_hal_gpio_init_simple(SWD_SWDIO, GpioModeOutputPushPull);
}

/** Switch SWDIO to input (high-impedance). */
static inline void dio_input(void) {
    furi_hal_gpio_init_simple(SWD_SWDIO, GpioModeInput);
}

/** Read SWDIO (input mode must already be set). */
static inline bool dio_read(void) {
    return furi_hal_gpio_read(SWD_SWDIO);
}

/** Delay one half-clock period. */
static inline void half_clk(void) {
    furi_delay_us(SWD_HALF_PERIOD_US);
}

/**
 * Clock out one bit on SWDIO (host → target, output mode assumed).
 * SWDIO is set before the rising edge and held through the falling edge.
 */
static void swd_write_bit(bool bit) {
    if(bit) {
        dio_high();
    } else {
        dio_low();
    }
    half_clk();
    clk_high();
    half_clk();
    clk_low();
}

/**
 * Clock in one bit from SWDIO (input mode assumed).
 * Samples on the rising edge.
 */
static bool swd_read_bit(void) {
    half_clk();
    clk_high();
    half_clk();
    bool bit = dio_read();
    clk_low();
    return bit;
}

/**
 * Execute one turnaround clock cycle.
 * The direction of SWDIO is NOT changed here — the caller must handle that
 * before and after as required by the SWD spec.
 */
static void swd_turnaround(void) {
    half_clk();
    clk_high();
    half_clk();
    clk_low();
}

/* ===========================================================================
 * Multi-bit helpers
 * ======================================================================== */

/**
 * Write 'count' bits LSB-first from 'data' (output mode assumed).
 */
static void swd_write_bits(uint32_t data, uint8_t count) {
    for(uint8_t i = 0; i < count; i++) {
        swd_write_bit((data >> i) & 1U);
    }
}

/**
 * Read 'count' bits LSB-first into *out (input mode assumed).
 */
static void swd_read_bits(uint32_t* out, uint8_t count) {
    *out = 0;
    for(uint8_t i = 0; i < count; i++) {
        if(swd_read_bit()) {
            *out |= (1U << i);
        }
    }
}

/* ===========================================================================
 * Parity
 * ======================================================================== */

/** Compute even parity (returns 1 if popcount of val is odd, 0 otherwise). */
static bool swd_parity32(uint32_t val) {
    val ^= val >> 16;
    val ^= val >> 8;
    val ^= val >> 4;
    val ^= val >> 2;
    val ^= val >> 1;
    return (bool)(val & 1U);
}

/** Even parity over the 4 bits APnDP, RnW, A2, A3. */
static bool swd_request_parity(bool apndp, bool rnw, uint8_t addr) {
    uint8_t bits = (uint8_t)apndp ^ (uint8_t)rnw ^
                   ((addr >> 2) & 1U) ^ ((addr >> 3) & 1U);
    return (bool)(bits & 1U);
}

/* ===========================================================================
 * Request byte builder
 * ======================================================================== */

/**
 * Build the 8-bit SWD request byte.
 *
 * Bit layout (LSB → MSB):
 *   [0] Start  = 1
 *   [1] APnDP  (0=DP, 1=AP)
 *   [2] RnW    (0=write, 1=read)
 *   [3] A2     (addr bit 2)
 *   [4] A3     (addr bit 3)
 *   [5] Parity (even parity over bits 1-4)
 *   [6] Stop   = 0
 *   [7] Park   = 1
 *
 * @param apndp  false=DP register, true=AP register
 * @param rnw    false=write, true=read
 * @param addr   Register address (only bits [3:2] are used)
 */
static uint8_t swd_build_request(bool apndp, bool rnw, uint8_t addr) {
    uint8_t a2 = (addr >> 2) & 1U;
    uint8_t a3 = (addr >> 3) & 1U;
    bool parity = swd_request_parity(apndp, rnw, addr);

    uint8_t req = 0;
    req |= (1U);                          /* bit 0: Start */
    req |= ((uint8_t)apndp << 1);        /* bit 1: APnDP */
    req |= ((uint8_t)rnw   << 2);        /* bit 2: RnW   */
    req |= (a2             << 3);        /* bit 3: A2    */
    req |= (a3             << 4);        /* bit 4: A3    */
    req |= ((uint8_t)parity << 5);       /* bit 5: Parity*/
    /* bit 6: Stop = 0 (already 0)                        */
    req |= (1U             << 7);        /* bit 7: Park  */
    return req;
}

/* ===========================================================================
 * Core SWD read / write transaction
 * ======================================================================== */

/**
 * swd_dp_read() — read a DP register.
 *
 * @param addr  DP register address (DP_REG_* constant)
 * @param out   Receives the 32-bit register value on SWD_ACK_OK
 * @return      ACK code
 */
static SWDAck swd_dp_read(uint8_t addr, uint32_t* out) {
    uint8_t req = swd_build_request(false, true, addr);

    /* Phase 1: send request (host drives SWDIO) */
    dio_output();
    swd_write_bits(req, 8);

    /* Phase 2: turnaround — release SWDIO, clock 1 cycle */
    dio_input();
    swd_turnaround();

    /* Phase 3: read 3-bit ACK (target drives) */
    uint32_t ack = 0;
    swd_read_bits(&ack, 3);

    if(ack == (uint32_t)SWD_ACK_OK) {
        /* Phase 4: read 32-bit data + 1 parity bit (target still drives) */
        uint32_t data = 0;
        swd_read_bits(&data, 32);
        bool parity_bit = swd_read_bit();

        /* Phase 5: turnaround — host takes SWDIO back, clock 1 cycle */
        dio_output();
        swd_turnaround();

        /* Phase 6: idle */
        swd_write_bit(false);

        /* Verify parity */
        if(parity_bit != swd_parity32(data)) {
            /* Parity error — treat as fault */
            *out = 0;
            return SWD_ACK_FAULT;
        }

        *out = data;
        return SWD_ACK_OK;
    } else {
        /*
         * Non-OK ACK (WAIT or FAULT):
         * Clock 33 bits while SWDIO is still in input mode, then reclaim it.
         */
        for(int i = 0; i < 33; i++) {
            swd_read_bit();
        }
        dio_output();
        swd_turnaround();
        swd_write_bit(false); /* idle */
        *out = 0;
        return (SWDAck)ack;
    }
}

/**
 * swd_dp_write() — write a DP register.
 *
 * @param addr  DP register address (DP_REG_* constant)
 * @param val   32-bit value to write
 * @return      ACK code
 */
static SWDAck swd_dp_write(uint8_t addr, uint32_t val) {
    uint8_t req = swd_build_request(false, false, addr);

    /* Phase 1: send request (host drives SWDIO) */
    dio_output();
    swd_write_bits(req, 8);

    /* Phase 2: turnaround — release SWDIO, clock 1 cycle */
    dio_input();
    swd_turnaround();

    /* Phase 3: read 3-bit ACK */
    uint32_t ack = 0;
    swd_read_bits(&ack, 3);

    if(ack == (uint32_t)SWD_ACK_OK) {
        /* Phase 4: turnaround — host takes SWDIO back, clock 1 cycle */
        dio_output();
        swd_turnaround();

        /* Phase 5: write 32-bit data + parity */
        swd_write_bits(val, 32);
        swd_write_bit(swd_parity32(val));

        /* Phase 6: idle */
        swd_write_bit(false);
        return SWD_ACK_OK;
    } else {
        /*
         * Non-OK ACK (WAIT or FAULT):
         * Reclaim SWDIO, clock 1 TRN, drive 33 zero bits, idle.
         */
        dio_output();
        swd_turnaround();
        swd_write_bits(0, 33);
        swd_write_bit(false); /* idle */
        return (SWDAck)ack;
    }
}

/**
 * swd_ap_read() — read an AP register.
 *
 * AP reads are pipelined; the value returned is from the PREVIOUS read.
 * Callers that need actual data must follow up with swd_dp_read(DP_REG_RDBUFF).
 *
 * @param addr  AP register address (AP_REG_* constant)
 * @param out   Receives the (pipelined) 32-bit value on SWD_ACK_OK
 * @return      ACK code
 */
static SWDAck swd_ap_read(uint8_t addr, uint32_t* out) {
    uint8_t req = swd_build_request(true, true, addr);

    /* Phase 1: send request */
    dio_output();
    swd_write_bits(req, 8);

    /* Phase 2: turnaround */
    dio_input();
    swd_turnaround();

    /* Phase 3: 3-bit ACK */
    uint32_t ack = 0;
    swd_read_bits(&ack, 3);

    if(ack == (uint32_t)SWD_ACK_OK) {
        /* Phase 4: 32-bit data + parity */
        uint32_t data = 0;
        swd_read_bits(&data, 32);
        bool parity_bit = swd_read_bit();

        /* Phase 5: turnaround */
        dio_output();
        swd_turnaround();

        /* Phase 6: idle */
        swd_write_bit(false);

        if(parity_bit != swd_parity32(data)) {
            *out = 0;
            return SWD_ACK_FAULT;
        }

        *out = data;
        return SWD_ACK_OK;
    } else {
        for(int i = 0; i < 33; i++) {
            swd_read_bit();
        }
        dio_output();
        swd_turnaround();
        swd_write_bit(false);
        *out = 0;
        return (SWDAck)ack;
    }
}

/**
 * swd_ap_write() — write an AP register.
 *
 * @param addr  AP register address (AP_REG_* constant)
 * @param val   32-bit value to write
 * @return      ACK code
 */
static SWDAck swd_ap_write(uint8_t addr, uint32_t val) {
    uint8_t req = swd_build_request(true, false, addr);

    /* Phase 1: send request */
    dio_output();
    swd_write_bits(req, 8);

    /* Phase 2: turnaround */
    dio_input();
    swd_turnaround();

    /* Phase 3: 3-bit ACK */
    uint32_t ack = 0;
    swd_read_bits(&ack, 3);

    if(ack == (uint32_t)SWD_ACK_OK) {
        /* Phase 4: turnaround */
        dio_output();
        swd_turnaround();

        /* Phase 5: 32-bit data + parity */
        swd_write_bits(val, 32);
        swd_write_bit(swd_parity32(val));

        /* Phase 6: idle */
        swd_write_bit(false);
        return SWD_ACK_OK;
    } else {
        dio_output();
        swd_turnaround();
        swd_write_bits(0, 33);
        swd_write_bit(false);
        return (SWDAck)ack;
    }
}

/* ===========================================================================
 * Line reset and JTAG-to-SWD switch
 * ======================================================================== */

/**
 * swd_line_reset() — send 56 clocks with SWDIO HIGH.
 * This resets the SWD state machine on the target.
 * Output mode is assumed; remains in output mode on return.
 */
static void swd_line_reset(void) {
    dio_output();
    dio_high();
    for(int i = 0; i < 56; i++) {
        half_clk();
        clk_high();
        half_clk();
        clk_low();
    }
}

/**
 * swd_jtag_to_swd() — send the 16-bit JTAG-to-SWD switch sequence 0xE79E
 * LSB-first, then float SWDIO back to idle.
 */
static void swd_jtag_to_swd(void) {
    dio_output();
    /* 0xE79E = 0b1110_0111_1001_1110 */
    swd_write_bits(0xE79EU, 16);
}

/**
 * swd_idle_cycles() — drive SWDIO LOW for 'n' clock cycles (idle / padding).
 * Output mode assumed.
 */
static void swd_idle_cycles(uint8_t n) {
    dio_output();
    dio_low();
    for(uint8_t i = 0; i < n; i++) {
        half_clk();
        clk_high();
        half_clk();
        clk_low();
    }
}

/* ===========================================================================
 * Public API implementation
 * ======================================================================== */

SWDAck swd_connect(void) {
    /* Initialise both pins; start with CLK low, SWDIO high-impedance. */
    furi_hal_gpio_init_simple(SWD_SWCLK, GpioModeOutputPushPull);
    furi_hal_gpio_write(SWD_SWCLK, false);
    furi_hal_gpio_init_simple(SWD_SWDIO, GpioModeOutputPushPull);
    furi_hal_gpio_write(SWD_SWDIO, true);

    SWDAck result = SWD_ERR_NO_TARGET;

    /* Try up to 3 times to get a valid IDCODE */
    for(int attempt = 0; attempt < 3; attempt++) {
        /* Step 1: Line reset */
        swd_line_reset();

        /* Step 2: JTAG-to-SWD switch sequence */
        swd_jtag_to_swd();

        /* Step 3: Line reset again */
        swd_line_reset();

        /* Step 4: 4 idle clocks LOW */
        swd_idle_cycles(4);

        /* Step 5: Read DP IDCODE */
        uint32_t idcode = 0;
        result = swd_dp_read(DP_REG_IDCODE, &idcode);
        if(result != SWD_ACK_OK) {
            /* No response — retry */
            continue;
        }

        /* Validate: bit 0 of IDCODE must be 1 per ARM spec */
        if((idcode & 1U) == 0) {
            result = SWD_ERR_NO_TARGET;
            continue;
        }

        /* Got a valid IDCODE — proceed with power-up */
        result = SWD_ACK_OK;
        break;
    }

    if(result != SWD_ACK_OK) {
        return (result == SWD_ACK_OK) ? SWD_ACK_OK : SWD_ERR_NO_TARGET;
    }

    /* Step 6: Request system power-up and debug power-up */
    /* CSYSPWRUPREQ (bit 30) | CDBGPWRUPREQ (bit 28) = 0x50000000 */
    result = swd_dp_write(DP_REG_CTRLSTAT, 0x50000000UL);
    if(result != SWD_ACK_OK) return result;

    /* Step 7: Poll until CSYSPWRUPACK (bit 31) and CDBGPWRUPACK (bit 29) */
    uint32_t stat = 0;
    for(uint32_t i = 0; i < PWRUP_POLL_LIMIT; i++) {
        result = swd_dp_read(DP_REG_CTRLSTAT, &stat);
        if(result != SWD_ACK_OK) return result;
        if((stat & 0xA0000000UL) == 0xA0000000UL) break;
        furi_delay_us(100);
        if(i == PWRUP_POLL_LIMIT - 1) return SWD_ERR_TIMEOUT;
    }

    /* Step 8: Select AP 0, bank 0 */
    result = swd_dp_write(DP_REG_SELECT, 0x00000000UL);
    if(result != SWD_ACK_OK) return result;

    /* Step 9: Configure MEM-AP CSW for 32-bit word transfers, AddrInc off */
    /* Size[2:0]=010 (32-bit), AddrInc[5:4]=00 (off) = 0x00000002        */
    result = swd_ap_write(AP_REG_CSW, 0x00000002UL);
    return result;
}

void swd_disconnect(void) {
    /* Float both pins back to input (high-impedance) */
    furi_hal_gpio_init_simple(SWD_SWDIO, GpioModeInput);
    furi_hal_gpio_init_simple(SWD_SWCLK, GpioModeInput);
}

SWDAck swd_read32(uint32_t addr, uint32_t* out) {
    SWDAck result;

    /* Write TAR with the target address */
    result = swd_ap_write(AP_REG_TAR, addr);
    if(result != SWD_ACK_OK) return result;

    /*
     * AP reads are pipelined:
     *   1. Initiate the read of DRW — result is from previous read (stale).
     *   2. Read DP RDBUFF — this captures the actual DRW data.
     */
    uint32_t discard = 0;
    result = swd_ap_read(AP_REG_DRW, &discard);
    if(result != SWD_ACK_OK) return result;

    result = swd_dp_read(DP_REG_RDBUFF, out);
    return result;
}

SWDAck swd_write32(uint32_t addr, uint32_t val) {
    SWDAck result;

    /* Write TAR */
    result = swd_ap_write(AP_REG_TAR, addr);
    if(result != SWD_ACK_OK) return result;

    /* Write DRW — data goes straight through to the target */
    result = swd_ap_write(AP_REG_DRW, val);
    return result;
}

SWDAck swd_halt(void) {
    return swd_write32(DHCSR_ADDR, DHCSR_HALT_VAL);
}

SWDAck swd_reset_and_run(void) {
    return swd_write32(AIRCR_ADDR, AIRCR_RESET_VAL);
}
