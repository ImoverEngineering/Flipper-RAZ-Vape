/**
 * vape_flasher.c — Flipper Zero FAP: RAZ DC25000 Vape Firmware Flasher
 *
 * Flashes replacement firmware into the N32G031K8Q7-1 (ARM Cortex-M0) MCU
 * inside a RAZ DC25000 disposable vape via bit-banged ARM SWD on GPIO pins.
 *
 * Wiring (Flipper GPIO → USB-C cable → vape CC lines):
 *   Flipper PA7  (GPIO pin 11) → CC1 → SWDIO
 *   Flipper PA6  (GPIO pin  9) → CC2 → SWCLK
 *   Flipper GND                → GND   → USB-C GND shell
 *
 * The vape is self-powered; USB-C carries only SWD signals and GND.
 *
 * Build deps: swd.h, n32g031_flash.h (same directory)
 * Entry point: vape_flasher_app()   (matches application.fam entry_point)
 *
 * Author: Claude Code
 * License: MIT
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "swd.h"
#include "n32g031_flash.h"

/* ---------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

/** Frames of OK-held (at ~30 fps poll) required to accept the disclaimer. */
#define DISCLAIMER_HOLD_FRAMES 90

/** Maximum firmware image size: 60 KB (pages 0-59, NV region preserved). */
#define MAX_FIRMWARE_BYTES 61440

/* ---------------------------------------------------------------------------
 * Application state machine
 * ------------------------------------------------------------------------- */

typedef enum {
    StateDisclaimer,  /**< Safety warning; user must hold OK for 3 s          */
    StateWiring,      /**< Show fixed wiring diagram; OK to continue           */
    StateFilePick,    /**< Blocking file browser dialog                        */
    StateConnecting,  /**< SWD connect attempt (runs in worker thread)         */
    StateConfirm,     /**< Show filename + size; ask user to confirm           */
    StateFlashing,    /**< Erase / program / verify in progress                */
    StateSuccess,     /**< Flash succeeded                                     */
    StateError,       /**< Error — display message and wait for OK/Back        */
} AppState;

/* ---------------------------------------------------------------------------
 * Application context
 * ------------------------------------------------------------------------- */

typedef struct {
    AppState state;
    FuriMutex* mutex;
    FuriMessageQueue* queue;

    /* File info */
    FuriString* file_path;
    uint8_t* firmware;       /**< malloc'd firmware buffer, NULL until loaded  */
    uint32_t firmware_size;

    /* Flash progress (written by worker, read by draw callback, mutex-guarded) */
    const char* progress_phase;  /**< "Erasing" | "Flashing" | "Verifying"    */
    uint32_t progress_done;
    uint32_t progress_total;

    /* Results */
    char error_msg[64];
    uint32_t swd_idcode;

    /* Disclaimer hold tracking */
    uint32_t disclaimer_hold_ticks; /**< Incremented each frame while OK held  */
    bool ok_held;                   /**< True while OK button is physically held*/

    /* Worker thread */
    FuriThread* worker_thread;
    volatile bool worker_running;
} AppCtx;

/**
 * Small wrapper passed to the worker thread so it can trigger a repaint
 * without needing a global ViewPort pointer.
 */
typedef struct {
    AppCtx* app;
    ViewPort* vp;
} WorkerCtx;

/* ---------------------------------------------------------------------------
 * Helper: extract basename from a path string
 *
 * Returns a pointer into @path pointing at the filename after the last '/'
 * or '\'. Returns @path itself if no separator found.
 * ------------------------------------------------------------------------- */
static const char* path_basename(const char* path) {
    const char* last = path;
    for(const char* p = path; *p; ++p) {
        if(*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

/* ---------------------------------------------------------------------------
 * Progress callback — called from worker thread by n32_flash_program()
 *
 * Acquires the mutex, updates progress fields, releases mutex, then
 * requests a ViewPort repaint.  Must return quickly.
 * ------------------------------------------------------------------------- */
static void flash_progress_cb(const char* phase, uint32_t done, uint32_t total, void* ctx) {
    WorkerCtx* wctx = ctx;
    furi_mutex_acquire(wctx->app->mutex, FuriWaitForever);
    wctx->app->progress_phase = phase;
    wctx->app->progress_done = done;
    wctx->app->progress_total = total;
    furi_mutex_release(wctx->app->mutex);
    view_port_update(wctx->vp);
}

/* ---------------------------------------------------------------------------
 * Worker thread — SWD connect + flash
 * ------------------------------------------------------------------------- */
static int32_t flash_worker(void* raw_ctx) {
    WorkerCtx* wctx = raw_ctx;
    AppCtx* app = wctx->app;

    /* ---- 1. SWD connect ---- */
    SWDAck r = swd_connect();
    if(r != SWD_ACK_OK) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        snprintf(
            app->error_msg,
            sizeof(app->error_msg),
            "SWD connect failed\n(ACK=0x%02X)\nCheck cable+power",
            (unsigned)r);
        app->state = StateError;
        furi_mutex_release(app->mutex);
        view_port_update(wctx->vp);
        app->worker_running = false;
        return -1;
    }

    /* ---- 2. Halt core ---- */
    r = swd_halt();
    if(r != SWD_ACK_OK) {
        swd_disconnect();
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        snprintf(
            app->error_msg,
            sizeof(app->error_msg),
            "Core halt failed\n(ACK=0x%02X)",
            (unsigned)r);
        app->state = StateError;
        furi_mutex_release(app->mutex);
        view_port_update(wctx->vp);
        app->worker_running = false;
        return -1;
    }

    /* ---- 3. Erase / program / verify ---- */
    /* Initialise progress so the draw callback shows something right away. */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->progress_phase = "Erasing";
    app->progress_done = 0;
    app->progress_total = app->firmware_size;
    furi_mutex_release(app->mutex);
    view_port_update(wctx->vp);

    FlashResult fr = n32_flash_program(
        app->firmware,
        app->firmware_size,
        flash_progress_cb,
        wctx);

    if(fr != FLASH_OK) {
        swd_disconnect();
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        snprintf(
            app->error_msg,
            sizeof(app->error_msg),
            "Flash error:\n%s",
            n32_flash_err_str(fr));
        app->state = StateError;
        furi_mutex_release(app->mutex);
        view_port_update(wctx->vp);
        app->worker_running = false;
        return -1;
    }

    /* ---- 4. Reset and run ---- */
    swd_reset_and_run();
    swd_disconnect();

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = StateSuccess;
    furi_mutex_release(app->mutex);
    view_port_update(wctx->vp);

    app->worker_running = false;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Connect worker — used for StateConnecting (identify only, no flash)
 *
 * On success transitions to StateConfirm.
 * On failure transitions to StateError.
 * ------------------------------------------------------------------------- */
static int32_t connect_worker(void* raw_ctx) {
    WorkerCtx* wctx = raw_ctx;
    AppCtx* app = wctx->app;

    /*
     * Call swd_connect() directly (rather than n32_flash_identify) so we can
     * surface the raw ACK code in the error message for diagnosis:
     *
     *   0x08 = SWD_ERR_NO_TARGET  — IDCODE read got no valid response
     *   0x10 = SWD_ERR_TIMEOUT    — power-up ACK timed out
     *   0x02 = SWD_ACK_WAIT       — target is busy
     *   0x04 = SWD_ACK_FAULT      — target flagged a fault
     *   0x07                      — SWDIO floating (pull-up not reaching target)
     *   0x00                      — SWDIO stuck low (short / wrong pin)
     */
    SWDAck result = swd_connect();

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    if(result == SWD_ACK_OK) {
        /* Confirm live target by reading DHCSR */
        swd_halt();
        uint32_t dhcsr = 0;
        SWDAck rd = swd_read32(0xE000EDF0UL, &dhcsr);
        if(rd == SWD_ACK_OK) {
            app->swd_idcode = 0x0BB11477UL; /* Cortex-M0 DP IDCODE */
            app->state = StateConfirm;
        } else {
            snprintf(
                app->error_msg,
                sizeof(app->error_msg),
                "SWD up, DHCSR fail\nACK=0x%02X\nWrong target?",
                (unsigned)rd);
            app->state = StateError;
            swd_disconnect();
        }
    } else {
        snprintf(
            app->error_msg,
            sizeof(app->error_msg),
            "SWD fail ACK=0x%02X\nFlip USB-C, check\nvape is powered on",
            (unsigned)result);
        app->state = StateError;
    }

    furi_mutex_release(app->mutex);
    view_port_update(wctx->vp);

    app->worker_running = false;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Draw callbacks (one function per state, called from draw_cb)
 * ------------------------------------------------------------------------- */

static void draw_disclaimer(Canvas* canvas, AppCtx* app) {
    /* Inverted title bar */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_invert_color(canvas);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, "! VAPE FLASHER !");
    canvas_invert_color(canvas);

    /* Warning lines */
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 22, "Incorrect firmware can");
    canvas_draw_str(canvas, 2, 32, "damage battery or cause fire.");
    canvas_draw_str(canvas, 2, 42, "YOU assume ALL liability.");

    /* Instruction */
    canvas_draw_str(canvas, 2, 54, "Hold [OK] 3s to accept");

    /* Progress bar outline (120 px wide, centred in the last strip) */
    canvas_draw_frame(canvas, 4, 56, 120, 7);

    /* Fill proportional to hold ticks (0 to DISCLAIMER_HOLD_FRAMES) */
    uint32_t ticks = app->disclaimer_hold_ticks;
    if(ticks > DISCLAIMER_HOLD_FRAMES) ticks = DISCLAIMER_HOLD_FRAMES;
    uint32_t fill_w = (ticks * 118) / DISCLAIMER_HOLD_FRAMES;
    if(fill_w > 0) {
        canvas_draw_box(canvas, 5, 57, fill_w, 5);
    }
}

static void draw_wiring(Canvas* canvas) {
    /* Inverted title bar */
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_invert_color(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, "WIRING");
    canvas_invert_color(canvas);

    /*
     * Fixed wiring — confirmed via ST-Link on production hardware:
     *   CC1 = SWDIO,  CC2 = SWCLK
     *
     * Flipper GPIO header → USB-C breakout → Vape
     *
     * Display is 128×64.  FontSecondary ≈ 6 px/char, row height ≈ 10 px.
     */
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 23, "Flipper    USB-C   Signal");
    /* Separator */
    canvas_draw_line(canvas, 2, 25, 126, 25);
    canvas_draw_str(canvas, 2, 34, "PA7 pin11  CC1     SWDIO");
    canvas_draw_str(canvas, 2, 44, "PA6 pin 9  CC2     SWCLK");
    canvas_draw_str(canvas, 2, 54, "GND        GND shell");

    canvas_draw_str(canvas, 2, 63, "[OK] Next   [Back] Back");
}

static void draw_connecting(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 12, "Vape Flasher");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 28, "Connecting via SWD...");
    canvas_draw_str(canvas, 4, 40, "Keep vape powered on");
    canvas_draw_str(canvas, 4, 52, "Check cable orientation");
}

static void draw_confirm(Canvas* canvas, AppCtx* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 12, "Vape Flasher");

    canvas_set_font(canvas, FontSecondary);

    /* Basename of selected file */
    const char* basename = path_basename(furi_string_get_cstr(app->file_path));
    /* Truncate if too long for the display (~21 chars at FontSecondary) */
    char name_buf[24];
    snprintf(name_buf, sizeof(name_buf), "%s", basename);
    canvas_draw_str(canvas, 4, 26, name_buf);

    /* File size */
    char size_buf[32];
    snprintf(size_buf, sizeof(size_buf), "Size: %lu bytes", (unsigned long)app->firmware_size);
    canvas_draw_str(canvas, 4, 38, size_buf);

    /* IDCODE if non-zero */
    if(app->swd_idcode) {
        char id_buf[28];
        snprintf(id_buf, sizeof(id_buf), "IDCODE: 0x%08lX", (unsigned long)app->swd_idcode);
        canvas_draw_str(canvas, 4, 50, id_buf);
    }

    canvas_draw_str(canvas, 4, 62, "[OK] Flash  [Back] Cancel");
}

static void draw_flashing(Canvas* canvas, AppCtx* app) {
    /* Dynamic title showing current phase */
    canvas_set_font(canvas, FontPrimary);
    const char* title =
        (app->progress_phase && app->progress_phase[0]) ? app->progress_phase : "Working...";
    canvas_draw_str(canvas, 4, 12, title);

    /* Phase label */
    canvas_set_font(canvas, FontSecondary);
    char phase_buf[32];
    snprintf(phase_buf, sizeof(phase_buf), "Phase: %s", title);
    canvas_draw_str(canvas, 4, 26, phase_buf);

    /* Progress bar */
    canvas_draw_frame(canvas, 4, 32, 120, 10);
    if(app->progress_total > 0) {
        uint32_t fill = (uint32_t)(((uint64_t)app->progress_done * 118) / app->progress_total);
        if(fill > 118) fill = 118;
        if(fill > 0) {
            canvas_draw_box(canvas, 5, 33, fill, 8);
        }
    }

    /* Percentage */
    char pct_buf[16];
    if(app->progress_total > 0) {
        uint32_t pct =
            (uint32_t)(((uint64_t)app->progress_done * 100) / app->progress_total);
        snprintf(pct_buf, sizeof(pct_buf), "%lu%%", (unsigned long)pct);
    } else {
        snprintf(pct_buf, sizeof(pct_buf), "0%%");
    }
    canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignBottom, pct_buf);
}

static void draw_success(Canvas* canvas) {
    /* Inverted title bar */
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_invert_color(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, "  DONE!  ");
    canvas_invert_color(canvas);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 28, "Unplug cable and");
    canvas_draw_str(canvas, 4, 40, "power-cycle the vape.");
    canvas_draw_str(canvas, 4, 56, "[OK] to exit");
}

static void draw_error(Canvas* canvas, AppCtx* app) {
    /* Inverted title bar */
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_invert_color(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, "Error");
    canvas_invert_color(canvas);

    canvas_set_font(canvas, FontSecondary);

    /*
     * Split error_msg on '\n' and print up to 3 lines.
     * We copy to a local buffer to avoid modifying app->error_msg.
     */
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", app->error_msg);

    char* line = buf;
    int y_positions[] = {28, 40, 52};
    int line_idx = 0;
    while(line && line_idx < 3) {
        char* nl = strchr(line, '\n');
        if(nl) *nl = '\0';
        canvas_draw_str(canvas, 4, y_positions[line_idx], line);
        line = nl ? nl + 1 : NULL;
        line_idx++;
    }

    canvas_draw_str(canvas, 4, 62, "[OK] to exit");
}

/* ---------------------------------------------------------------------------
 * ViewPort draw callback (called on the GUI thread)
 * ------------------------------------------------------------------------- */
static void draw_cb(Canvas* canvas, void* raw_ctx) {
    AppCtx* app = raw_ctx;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    AppState state = app->state;

    canvas_clear(canvas);

    switch(state) {
    case StateDisclaimer:
        draw_disclaimer(canvas, app);
        break;
    case StateWiring:
        draw_wiring(canvas);
        break;
    case StateFilePick:
        /* Show a simple "loading" screen while the blocking dialog runs. */
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 4, 12, "Vape Flasher");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 4, 30, "Select firmware file...");
        break;
    case StateConnecting:
        draw_connecting(canvas);
        break;
    case StateConfirm:
        draw_confirm(canvas, app);
        break;
    case StateFlashing:
        draw_flashing(canvas, app);
        break;
    case StateSuccess:
        draw_success(canvas);
        break;
    case StateError:
        draw_error(canvas, app);
        break;
    }

    furi_mutex_release(app->mutex);
}

/* ---------------------------------------------------------------------------
 * ViewPort input callback (queues raw InputEvents to main loop)
 * ------------------------------------------------------------------------- */
static void input_cb(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, 0);
}

/* ---------------------------------------------------------------------------
 * File loading helper
 *
 * Opens the file at @path_str, reads it into a malloc'd buffer, stores the
 * pointer and size in @app.  Returns true on success, false on failure
 * (sets app->error_msg on failure).
 * ------------------------------------------------------------------------- */
static bool load_firmware_file(AppCtx* app, const char* path_str) {
    /* Free any previously loaded buffer */
    if(app->firmware) {
        free(app->firmware);
        app->firmware = NULL;
        app->firmware_size = 0;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    bool ok = false;

    if(!storage_file_open(file, path_str, FSAM_READ, FSOM_OPEN_EXISTING)) {
        snprintf(app->error_msg, sizeof(app->error_msg), "Cannot open file:\n%s", path_str);
        goto done;
    }

    uint64_t size = storage_file_size(file);
    if(size == 0) {
        snprintf(app->error_msg, sizeof(app->error_msg), "File is empty");
        goto done;
    }
    if(size > MAX_FIRMWARE_BYTES) {
        snprintf(
            app->error_msg,
            sizeof(app->error_msg),
            "File too large\n(%lu > %u bytes)",
            (unsigned long)size,
            MAX_FIRMWARE_BYTES);
        goto done;
    }
    if(size % 4 != 0) {
        /* n32_flash_program zero-pads sub-word tails, but warn if odd */
        /* We allow it; just round up the buffer so we can zero-pad here. */
    }

    uint32_t rounded = (uint32_t)((size + 3) & ~3UL);
    app->firmware = malloc(rounded);
    if(!app->firmware) {
        snprintf(app->error_msg, sizeof(app->error_msg), "Out of memory");
        goto done;
    }
    memset(app->firmware, 0xFF, rounded);

    uint16_t read_bytes = storage_file_read(file, app->firmware, (uint16_t)size);
    if(read_bytes != (uint16_t)size) {
        snprintf(
            app->error_msg,
            sizeof(app->error_msg),
            "Read error\n(got %u of %lu bytes)",
            (unsigned)read_bytes,
            (unsigned long)size);
        free(app->firmware);
        app->firmware = NULL;
        goto done;
    }

    app->firmware_size = rounded;
    ok = true;

done:
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
int32_t vape_flasher_app(void* p) {
    UNUSED(p);

    /* ---- Allocate application context ---- */
    AppCtx* app = malloc(sizeof(AppCtx));
    memset(app, 0, sizeof(AppCtx));

    app->state = StateDisclaimer;
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->file_path = furi_string_alloc();
    /*
     * Start the file browser at the SD card root so the user can navigate to
     * wherever they placed their .bin files.  After the first selection,
     * app->file_path holds the full file path and the browser reopens with
     * that file already highlighted — avoiding the Flipper quirk where
     * entering a bare directory path fails to apply the extension filter on
     * the first render (requiring a back-out/re-enter to see files).
     */
    furi_string_set(app->file_path, STORAGE_EXT_PATH_PREFIX);

    /* ---- Allocate ViewPort ---- */
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, app);
    view_port_input_callback_set(vp, input_cb, app->queue);

    /* ---- Register with GUI ---- */
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    /*
     * WorkerCtx is allocated once and reused for both the connect worker and
     * the flash worker.  It lives for the lifetime of the app.
     */
    WorkerCtx wctx = {.app = app, .vp = vp};

    /* ---- Main loop ---- */
    InputEvent event;
    bool running = true;

    while(running) {
        bool got_event =
            (furi_message_queue_get(app->queue, &event, 33) == FuriStatusOk);

        /* ----------------------------------------------------------------
         * StateFilePick: blocking dialog must run OUTSIDE the mutex and
         * OUTSIDE any draw callback.  Handle it before the mutex section.
         * -------------------------------------------------------------- */
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool do_file_pick = (app->state == StateFilePick);
        furi_mutex_release(app->mutex);

        if(do_file_pick) {
            /* Flush pending input from the queue before entering dialog */
            InputEvent discard;
            while(furi_message_queue_get(app->queue, &discard, 0) == FuriStatusOk) {
                /* drain */
            }

            /* Show file browser */
            DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
            DialogsFileBrowserOptions browser_options;
            dialog_file_browser_set_basic_options(&browser_options, ".bin", NULL);
            browser_options.hide_ext = false;

            bool selected =
                dialog_file_browser_show(dialogs, app->file_path, app->file_path, &browser_options);
            furi_record_close(RECORD_DIALOGS);

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(!selected) {
                /* User pressed back in the file browser — return to wiring screen */
                app->state = StateWiring;
            } else {
                /* Load the firmware file, then move to connecting */
                bool file_ok = load_firmware_file(app, furi_string_get_cstr(app->file_path));
                if(!file_ok) {
                    app->state = StateError;
                } else {
                    /* Kick off the connect worker */
                    app->state = StateConnecting;
                    app->worker_running = true;
                    app->worker_thread =
                        furi_thread_alloc_ex("VapeConnect", 4096, connect_worker, &wctx);
                    furi_thread_start(app->worker_thread);
                }
            }
            furi_mutex_release(app->mutex);

            view_port_update(vp);
            continue; /* skip normal event handling this iteration */
        }

        /* ---- Normal mutex-guarded state handling ---- */
        furi_mutex_acquire(app->mutex, FuriWaitForever);

        switch(app->state) {

        /* -------------------------------------------------------------- */
        case StateDisclaimer:
            if(got_event) {
                if(event.key == InputKeyBack &&
                   (event.type == InputTypeShort || event.type == InputTypePress)) {
                    running = false;
                } else if(event.key == InputKeyOk && event.type == InputTypePress) {
                    app->ok_held = true;
                } else if(event.key == InputKeyOk && event.type == InputTypeRelease) {
                    app->ok_held = false;
                    app->disclaimer_hold_ticks = 0;
                }
            }

            /* Advance hold counter each iteration while OK is held */
            if(app->ok_held) {
                app->disclaimer_hold_ticks++;
                if(app->disclaimer_hold_ticks >= DISCLAIMER_HOLD_FRAMES) {
                    app->state = StateWiring;
                    app->ok_held = false;
                    app->disclaimer_hold_ticks = 0;
                }
            }
            break;

        /* -------------------------------------------------------------- */
        case StateWiring:
            if(got_event &&
               (event.type == InputTypeShort || event.type == InputTypePress)) {
                if(event.key == InputKeyOk) {
                    app->state = StateFilePick;
                } else if(event.key == InputKeyBack) {
                    app->state = StateDisclaimer;
                    app->disclaimer_hold_ticks = 0;
                }
            }
            break;

        /* -------------------------------------------------------------- */
        case StateFilePick:
            /* Handled above, outside the mutex. Should not arrive here. */
            break;

        /* -------------------------------------------------------------- */
        case StateConnecting:
            /* Worker thread is running; no input accepted.
             * Worker sets state to StateConfirm or StateError when done.
             * Check if thread finished so we can join it. */
            if(!app->worker_running && app->worker_thread) {
                furi_mutex_release(app->mutex);
                furi_thread_join(app->worker_thread);
                furi_thread_free(app->worker_thread);
                app->worker_thread = NULL;
                furi_mutex_acquire(app->mutex, FuriWaitForever);
            }
            break;

        /* -------------------------------------------------------------- */
        case StateConfirm:
            if(got_event &&
               (event.type == InputTypeShort || event.type == InputTypePress)) {
                if(event.key == InputKeyOk) {
                    /* Launch flash worker */
                    app->state = StateFlashing;
                    app->progress_phase = "Erasing";
                    app->progress_done = 0;
                    app->progress_total = app->firmware_size;
                    app->worker_running = true;
                    app->worker_thread =
                        furi_thread_alloc_ex("VapeFlash", 4096, flash_worker, &wctx);
                    furi_thread_start(app->worker_thread);
                } else if(event.key == InputKeyBack) {
                    /* Go back to file picker */
                    app->state = StateFilePick;
                }
            }
            break;

        /* -------------------------------------------------------------- */
        case StateFlashing:
            /* Input ignored while flashing — just check for worker completion. */
            if(!app->worker_running && app->worker_thread) {
                furi_mutex_release(app->mutex);
                furi_thread_join(app->worker_thread);
                furi_thread_free(app->worker_thread);
                app->worker_thread = NULL;
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                /* State was already updated by the worker to Success or Error. */
            }
            break;

        /* -------------------------------------------------------------- */
        case StateSuccess:
            if(got_event &&
               (event.type == InputTypeShort || event.type == InputTypePress)) {
                if(event.key == InputKeyOk || event.key == InputKeyBack) {
                    running = false;
                }
            }
            break;

        /* -------------------------------------------------------------- */
        case StateError:
            /* Join any lingering worker thread */
            if(!app->worker_running && app->worker_thread) {
                furi_mutex_release(app->mutex);
                furi_thread_join(app->worker_thread);
                furi_thread_free(app->worker_thread);
                app->worker_thread = NULL;
                furi_mutex_acquire(app->mutex, FuriWaitForever);
            }
            if(got_event &&
               (event.type == InputTypeShort || event.type == InputTypePress)) {
                if(event.key == InputKeyOk || event.key == InputKeyBack) {
                    running = false;
                }
            }
            break;
        }

        furi_mutex_release(app->mutex);
        view_port_update(vp);
    }

    /* ---- Cleanup ---- */

    /* Make sure any worker thread has finished */
    if(app->worker_thread) {
        furi_thread_join(app->worker_thread);
        furi_thread_free(app->worker_thread);
        app->worker_thread = NULL;
    }

    /* Detach from GUI */
    gui_remove_view_port(gui, vp);
    view_port_free(vp);
    furi_record_close(RECORD_GUI);

    /* Release primitives */
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    furi_string_free(app->file_path);

    /* Free firmware buffer (only dynamic allocation in the app) */
    if(app->firmware) {
        free(app->firmware);
        app->firmware = NULL;
    }

    free(app);
    return 0;
}
