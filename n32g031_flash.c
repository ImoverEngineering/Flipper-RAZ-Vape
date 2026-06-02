/**
 * n32g031_flash.c — Flash programmer for N32G031K8Q7-1 via ARM SWD
 *
 * Target MCU : N32G031K8Q7-1 (ARM Cortex-M0)
 * Flash      : 64 KB @ 0x08000000, 1 KB pages, 64 pages
 * SRAM       : 8 KB  @ 0x20000000
 * Flash ctrl : N32G031 register-compatible subset of STM32F0 flash IP
 *
 * NV storage: pages 60-63 (0x0800F000–0x0800FFFF) are read-only from the
 * perspective of this module.  The erase loop hard-stops at page 59.
 *
 * SWD access: every target memory read/write goes through swd_read32() /
 * swd_write32() from swd.h.  A non-OK ACK from either function causes the
 * current operation to return FLASH_ERR_SWD immediately.
 *
 * BSY polling: uses furi_delay_us(100) between iterations.  Per-operation
 * maximum iteration counts are pre-computed from the timeout budget so that
 * no floating-point arithmetic or division is performed inside the loop.
 *
 * Lock on error: the flash controller is locked (best-effort) even when an
 * intermediate step fails, to leave the target in a safe state.
 *
 * Author: generated for the Flipper Zero RAZ DC25000 vape-reflash project.
 */

#include "n32g031_flash.h"
#include "swd.h"

#include <furi.h>       /* furi_delay_us()  */
#include <stddef.h>
#include <string.h>

/* ============================================================================
 * Flash controller register addresses
 * ========================================================================= */

#define FLASH_BASE    0x40022000UL
#define FLASH_AC      (FLASH_BASE + 0x00U)  /* Access control (wait states)      */
#define FLASH_KEY     (FLASH_BASE + 0x04U)  /* Unlock key register               */
#define FLASH_OPTKEY  (FLASH_BASE + 0x08U)  /* Option byte unlock key            */
#define FLASH_STS     (FLASH_BASE + 0x0CU)  /* Status register                   */
#define FLASH_CTRL    (FLASH_BASE + 0x10U)  /* Control register                  */
#define FLASH_ADD     (FLASH_BASE + 0x14U)  /* Page-erase address register       */

/* FLASH_CTRL bits */
#define FLASH_CTRL_PG    (1UL << 0)  /* Program enable                           */
#define FLASH_CTRL_PER   (1UL << 1)  /* Page erase                               */
#define FLASH_CTRL_MER   (1UL << 2)  /* Mass erase                               */
#define FLASH_CTRL_STRT  (1UL << 6)  /* Start (trigger erase)                    */
#define FLASH_CTRL_LOCK  (1UL << 7)  /* Lock bit (1 = locked; clear via keys)    */

/* FLASH_STS bits */
#define FLASH_STS_BSY    (1UL << 0)  /* Controller busy                          */
#define FLASH_STS_PGERR  (1UL << 2)  /* Program error                            */
#define FLASH_STS_WRPERR (1UL << 4)  /* Write-protection error                   */
#define FLASH_STS_EOP    (1UL << 5)  /* End of operation (write 1 to clear)      */

/* Unlock key sequence */
#define FLASH_KEY1  0x45670123UL
#define FLASH_KEY2  0xCDEF89ABUL

/* ============================================================================
 * Flash geometry
 * ========================================================================= */

#define FLASH_ORIGIN      0x08000000UL   /* First byte of flash in address space  */
#define FLASH_PAGE_SIZE   1024U          /* 1 KB per page                         */
#define FLASH_TOTAL_PAGES 64U            /* 64 pages = 64 KB                      */

/* NV region: pages 60-63 are reserved and must not be erased/written */
#define FLASH_NV_FIRST_PAGE 60U          /* First NV page (inclusive)             */
#define FLASH_MAX_USER_PAGES 60U         /* Pages 0-59 are user-writable          */
#define FLASH_MAX_BYTES      (FLASH_MAX_USER_PAGES * FLASH_PAGE_SIZE) /* 61440    */

/* ============================================================================
 * Polling timeouts
 *
 * All delays are multiples of furi_delay_us(100) = 100 µs per iteration.
 *
 * Page erase: 50 ms budget  → 50000 µs / 100 µs = 500 iterations
 * Word write:  5 ms budget  →  5000 µs / 100 µs =  50 iterations
 * ========================================================================= */

#define POLL_DELAY_US       100U
#define POLL_MAX_ERASE      500U   /* 50 ms  */
#define POLL_MAX_PROGRAM     50U   /*  5 ms  */

/* ============================================================================
 * Cortex-M debug register used for identification
 * ========================================================================= */

#define DHCSR_ADDR      0xE000EDF0UL   /* Debug Halting Control and Status Reg  */

/* Known DP IDCODE for Cortex-M0 devices (returned by n32_flash_identify)    */
#define CORTEX_M0_DP_IDCODE  0x0BB11477UL

/* ============================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * swd_ok() — return true iff the ACK code indicates a successful transaction.
 */
static inline bool swd_ok(SWDAck ack) {
    return ack == SWD_ACK_OK;
}

/**
 * flash_write32() — write a single 32-bit word via SWD, return FlashResult.
 */
static inline FlashResult flash_write32(uint32_t addr, uint32_t val) {
    return swd_ok(swd_write32(addr, val)) ? FLASH_OK : FLASH_ERR_SWD;
}

/**
 * flash_read32() — read a single 32-bit word via SWD, return FlashResult.
 */
static inline FlashResult flash_read32(uint32_t addr, uint32_t* out) {
    return swd_ok(swd_read32(addr, out)) ? FLASH_OK : FLASH_ERR_SWD;
}

/**
 * flash_poll_bsy() — spin until FLASH_STS_BSY clears or timeout expires.
 *
 * @param max_iters  Maximum number of 100-µs polling iterations.
 * @return FLASH_OK, FLASH_ERR_TIMEOUT, or FLASH_ERR_SWD.
 */
static FlashResult flash_poll_bsy(uint32_t max_iters) {
    for(uint32_t i = 0; i < max_iters; i++) {
        uint32_t sts = 0;
        FlashResult r = flash_read32(FLASH_STS, &sts);
        if(r != FLASH_OK) return FLASH_ERR_SWD;
        if(!(sts & FLASH_STS_BSY)) return FLASH_OK;
        furi_delay_us(POLL_DELAY_US);
    }
    return FLASH_ERR_TIMEOUT;
}

/**
 * flash_check_errors() — inspect STS for PGERR / WRPERR after an operation.
 *
 * @param err_code   FlashResult to return if an error flag is set
 *                   (FLASH_ERR_ERASE or FLASH_ERR_PROGRAM).
 * @return FLASH_OK if no error flags, err_code or FLASH_ERR_PROTECTED on
 *         error, or FLASH_ERR_SWD if the register read fails.
 */
static FlashResult flash_check_errors(FlashResult err_code) {
    uint32_t sts = 0;
    FlashResult r = flash_read32(FLASH_STS, &sts);
    if(r != FLASH_OK) return FLASH_ERR_SWD;

    if(sts & FLASH_STS_WRPERR) return FLASH_ERR_PROTECTED;
    if(sts & FLASH_STS_PGERR)  return err_code;
    return FLASH_OK;
}

/**
 * flash_clear_eop() — write 1 to FLASH_STS_EOP to acknowledge end-of-op.
 *
 * This is a write-1-to-clear bit; writing EOP does not disturb other bits
 * because PGERR/WRPERR are also write-1-to-clear and we write only EOP.
 */
static FlashResult flash_clear_eop(void) {
    return flash_write32(FLASH_STS, FLASH_STS_EOP);
}

/**
 * flash_unlock() — perform the two-key unlock sequence.
 *
 * After writing both keys, reads back FLASH_CTRL to verify the LOCK bit is
 * clear.  Returns FLASH_ERR_UNLOCK if CTRL cannot be read or LOCK is still
 * set (e.g. wrong key order, or controller already in an error state).
 *
 * @return FLASH_OK on success.
 */
static FlashResult flash_unlock(void) {
    FlashResult r;

    r = flash_write32(FLASH_KEY, FLASH_KEY1);
    if(r != FLASH_OK) return FLASH_ERR_UNLOCK;

    r = flash_write32(FLASH_KEY, FLASH_KEY2);
    if(r != FLASH_OK) return FLASH_ERR_UNLOCK;

    /* Verify LOCK bit cleared */
    uint32_t ctrl = 0;
    r = flash_read32(FLASH_CTRL, &ctrl);
    if(r != FLASH_OK) return FLASH_ERR_UNLOCK;
    if(ctrl & FLASH_CTRL_LOCK) return FLASH_ERR_UNLOCK;

    return FLASH_OK;
}

/**
 * flash_lock() — set the LOCK bit via read-modify-write.
 *
 * Called as a best-effort cleanup on both success and error paths; the
 * return value is intentionally discarded by callers that are already
 * propagating a different error.
 */
static FlashResult flash_lock(void) {
    uint32_t ctrl = 0;
    FlashResult r = flash_read32(FLASH_CTRL, &ctrl);
    if(r != FLASH_OK) return FLASH_ERR_SWD;

    ctrl |= FLASH_CTRL_LOCK;
    return flash_write32(FLASH_CTRL, ctrl);
}

/**
 * flash_erase_page() — erase one 1 KB page by its page index (0-59).
 *
 * Sequence per N32G031 reference manual:
 *   1. Set PER in CTRL.
 *   2. Load page start address into ADD.
 *   3. Set PER | STRT in CTRL to trigger erase.
 *   4. Poll BSY.
 *   5. Clear EOP.
 *   6. Check PGERR / WRPERR.
 *
 * @param page_index  0-based page number; must be < FLASH_NV_FIRST_PAGE.
 * @return FLASH_OK, FLASH_ERR_ERASE, FLASH_ERR_PROTECTED, FLASH_ERR_SWD,
 *         or FLASH_ERR_TIMEOUT.
 */
static FlashResult flash_erase_page(uint32_t page_index) {
    FlashResult r;
    uint32_t page_addr = FLASH_ORIGIN + (page_index * FLASH_PAGE_SIZE);

    /* Step 1: set PER mode */
    r = flash_write32(FLASH_CTRL, FLASH_CTRL_PER);
    if(r != FLASH_OK) return r;

    /* Step 2: write the page address to ADD */
    r = flash_write32(FLASH_ADD, page_addr);
    if(r != FLASH_OK) return r;

    /* Step 3: trigger erase */
    r = flash_write32(FLASH_CTRL, FLASH_CTRL_PER | FLASH_CTRL_STRT);
    if(r != FLASH_OK) return r;

    /* Step 4: poll busy */
    r = flash_poll_bsy(POLL_MAX_ERASE);
    if(r != FLASH_OK) return r;  /* timeout or SWD error */

    /* Step 5: clear EOP */
    r = flash_clear_eop();
    if(r != FLASH_OK) return r;

    /* Step 6: check error flags */
    return flash_check_errors(FLASH_ERR_ERASE);
}

/**
 * flash_write_word() — program one 32-bit word at a flash address.
 *
 * Sequence per N32G031 reference manual:
 *   1. Set PG in CTRL.
 *   2. Write the word directly to the target flash address via SWD.
 *   3. Poll BSY.
 *   4. Clear EOP.
 *   5. Check PGERR / WRPERR.
 *
 * @param flash_addr  Target address in flash (must be 32-bit aligned).
 * @param word        Value to write.
 * @return FLASH_OK, FLASH_ERR_PROGRAM, FLASH_ERR_PROTECTED, FLASH_ERR_SWD,
 *         or FLASH_ERR_TIMEOUT.
 */
static FlashResult flash_write_word(uint32_t flash_addr, uint32_t word) {
    FlashResult r;

    /* Step 1: set PG mode */
    r = flash_write32(FLASH_CTRL, FLASH_CTRL_PG);
    if(r != FLASH_OK) return r;

    /* Step 2: write word directly to flash address */
    r = flash_write32(flash_addr, word);
    if(r != FLASH_OK) return r;

    /* Step 3: poll busy */
    r = flash_poll_bsy(POLL_MAX_PROGRAM);
    if(r != FLASH_OK) return r;

    /* Step 4: clear EOP */
    r = flash_clear_eop();
    if(r != FLASH_OK) return r;

    /* Step 5: check error flags */
    return flash_check_errors(FLASH_ERR_PROGRAM);
}

/* ============================================================================
 * Helper: invoke the progress callback if one was provided
 * ========================================================================= */

static inline void report_progress(
    FlashProgressCb cb,
    void*           ctx,
    const char*     phase,
    uint32_t        done,
    uint32_t        total) {

    if(cb) cb(phase, done, total, ctx);
}

/* ============================================================================
 * Public API implementation
 * ========================================================================= */

FlashResult n32_flash_program(
    const uint8_t*  data,
    uint32_t        len,
    FlashProgressCb cb,
    void*           cb_ctx) {

    FlashResult r;

    /* ------------------------------------------------------------------
     * Parameter validation
     * ------------------------------------------------------------------ */
    if(len == 0 || len > FLASH_MAX_BYTES) {
        return FLASH_ERR_SIZE;
    }

    /* Number of pages that need to be erased to cover 'len' bytes */
    uint32_t pages_needed = (len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    /* Clamp to the writable region — the size check above already ensures
     * pages_needed <= FLASH_MAX_USER_PAGES, but be explicit. */
    if(pages_needed > FLASH_MAX_USER_PAGES) {
        pages_needed = FLASH_MAX_USER_PAGES;
    }

    /* Number of 32-bit words to write (ceiling division).
     * If len is not a multiple of 4 the last word will be zero-padded. */
    uint32_t words_to_write = (len + 3U) / 4U;

    /* Total bytes for the flash and verify progress phases.
     * Use words_to_write * 4 so the callback total is always word-aligned. */
    uint32_t prog_total = words_to_write * 4U;

    /* ------------------------------------------------------------------
     * Step 1: Unlock
     * ------------------------------------------------------------------ */
    r = flash_unlock();
    if(r != FLASH_OK) return r;  /* Never locked yet — no lock cleanup needed */

    /* ------------------------------------------------------------------
     * Step 2: Page erase (pages 0 .. pages_needed-1)
     * ------------------------------------------------------------------ */
    uint32_t erase_total = pages_needed * FLASH_PAGE_SIZE;

    for(uint32_t page = 0; page < pages_needed; page++) {
        r = flash_erase_page(page);
        if(r != FLASH_OK) {
            flash_lock();  /* best-effort */
            return r;
        }
        report_progress(cb, cb_ctx, "Erasing",
                        (page + 1U) * FLASH_PAGE_SIZE,
                        erase_total);
    }

    /* ------------------------------------------------------------------
     * Step 3: Program words
     * ------------------------------------------------------------------ */
    uint32_t progress_cb_threshold = FLASH_PAGE_SIZE; /* report every 1 KB */
    uint32_t bytes_since_last_cb   = 0;

    for(uint32_t i = 0; i < words_to_write; i++) {
        /* Build the 32-bit word from the source buffer.
         * Bytes beyond 'len' are zero-padded (last partial word only). */
        uint32_t word = 0;
        uint32_t byte_offset = i * 4U;
        uint32_t bytes_left  = len - byte_offset;
        uint32_t copy_bytes  = (bytes_left >= 4U) ? 4U : bytes_left;

        /* Use memcpy for safe unaligned byte assembly */
        memcpy(&word, data + byte_offset, copy_bytes);
        /* Any remaining bytes in 'word' are already 0 (zero-initialised) */

        uint32_t flash_addr = FLASH_ORIGIN + byte_offset;
        r = flash_write_word(flash_addr, word);
        if(r != FLASH_OK) {
            flash_lock();  /* best-effort */
            return r;
        }

        bytes_since_last_cb += 4U;
        if(bytes_since_last_cb >= progress_cb_threshold) {
            report_progress(cb, cb_ctx, "Flashing",
                            (i + 1U) * 4U,
                            prog_total);
            bytes_since_last_cb = 0;
        }
    }
    /* Final progress tick for "Flashing" if last block wasn't on a threshold */
    report_progress(cb, cb_ctx, "Flashing", prog_total, prog_total);

    /* Clear PG mode before verify reads */
    r = flash_write32(FLASH_CTRL, 0UL);
    if(r != FLASH_OK) {
        flash_lock();
        return r;
    }

    /* ------------------------------------------------------------------
     * Step 4: Verify
     * ------------------------------------------------------------------ */
    bytes_since_last_cb = 0;

    for(uint32_t i = 0; i < words_to_write; i++) {
        uint32_t byte_offset = i * 4U;

        /* Reconstruct expected word (same logic as the write loop) */
        uint32_t expected = 0;
        uint32_t bytes_left = len - byte_offset;
        uint32_t copy_bytes = (bytes_left >= 4U) ? 4U : bytes_left;
        memcpy(&expected, data + byte_offset, copy_bytes);

        uint32_t flash_addr = FLASH_ORIGIN + byte_offset;
        uint32_t actual     = 0;
        r = flash_read32(flash_addr, &actual);
        if(r != FLASH_OK) {
            flash_lock();
            return FLASH_ERR_SWD;
        }

        if(actual != expected) {
            flash_lock();
            return FLASH_ERR_VERIFY;
        }

        bytes_since_last_cb += 4U;
        if(bytes_since_last_cb >= progress_cb_threshold) {
            report_progress(cb, cb_ctx, "Verifying",
                            (i + 1U) * 4U,
                            prog_total);
            bytes_since_last_cb = 0;
        }
    }
    /* Final progress tick for "Verifying" */
    report_progress(cb, cb_ctx, "Verifying", prog_total, prog_total);

    /* ------------------------------------------------------------------
     * Step 5: Lock
     * ------------------------------------------------------------------ */
    flash_lock();  /* best-effort; ignore return value on success path */

    return FLASH_OK;
}

bool n32_flash_identify(uint32_t* idcode_out) {
    /* Attempt to bring the SWD link up.  If the target is already connected
     * from a previous call, swd_connect() re-runs the line-reset and power-up
     * sequence, which is harmless. */
    SWDAck ack = swd_connect();
    if(ack != SWD_ACK_OK) {
        if(idcode_out) *idcode_out = 0;
        return false;
    }

    /* Halt the core so DHCSR is accessible (not strictly required on M0, but
     * avoids any risk of the core interfering with the debug bus). */
    swd_halt();

    /* Read DHCSR (Debug Halting Control and Status Register @ 0xE000EDF0).
     * This register is present and readable on all ARM Cortex-M cores.
     * A successful read confirms we have a live SWD target. */
    uint32_t dhcsr = 0;
    ack = swd_read32(DHCSR_ADDR, &dhcsr);
    if(ack != SWD_ACK_OK) {
        if(idcode_out) *idcode_out = 0;
        return false;
    }

    /* DHCSR[16] (S_HALT) may or may not be set depending on core state, but
     * the register must always be readable as a non-zero value on a live
     * Cortex-M (at minimum DBGKEY bits survive reset as 0xA05F0000).
     * We treat any successful read as a valid target — a bus fault on the
     * debug bus would have returned a non-OK ACK above. */
    (void)dhcsr;  /* value not inspected further */

    /* The DP IDCODE register is not directly addressable through MEM-AP
     * swd_read32().  Return the known Cortex-M0 DP IDCODE for this family. */
    if(idcode_out) *idcode_out = CORTEX_M0_DP_IDCODE;

    return true;
}

const char* n32_flash_err_str(FlashResult r) {
    switch(r) {
    case FLASH_OK:            return "OK";
    case FLASH_ERR_UNLOCK:    return "Flash unlock failed";
    case FLASH_ERR_ERASE:     return "Page erase error";
    case FLASH_ERR_PROGRAM:   return "Programming error";
    case FLASH_ERR_VERIFY:    return "Verify mismatch";
    case FLASH_ERR_PROTECTED: return "Write protection error";
    case FLASH_ERR_SWD:       return "SWD communication error";
    case FLASH_ERR_TIMEOUT:   return "BSY poll timeout";
    case FLASH_ERR_SIZE:      return "Image too large (max 60 KB)";
    default:                  return "Unknown error";
    }
}
