#ifndef HW_DISPLAY_TEGRA30_DC_H
#define HW_DISPLAY_TEGRA30_DC_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qemu/timer.h"
#include "hw/misc/tegra30_host1x.h"

/*
 * nVidia Tegra 3 Display Controller (DC).
 *
 * Each controller exposes a 0x40000-byte MMIO window of 32-bit registers
 * (register index N lives at byte offset N*4).  Software selects one of the
 * three overlay windows (A/B/C) through DC_CMD_DISPLAY_WINDOW_HEADER and then
 * programs the DC_WIN and DC_WINBUF registers, which apply to the currently
 * selected window(s).  We model the registers as a flat RAM-backed array and
 * only interpret the handful needed to locate and blit window A's framebuffer
 * out of guest RAM into a QemuConsole.
 */

/* Number of 32-bit registers backing the MMIO window (covers up to 0x800). */
#define TEGRA30_DC_NUM_REGS     0x1000
#define TEGRA30_DC_IOSIZE       0x40000

/* Overlay windows selectable through DISPLAY_WINDOW_HEADER. */
#define TEGRA30_DC_NUM_WINDOWS  3

#define TYPE_TEGRA30_DC "tegra30-dc"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30DCState, TEGRA30_DC)

/* Per-window programmable state (the subset we interpret). */
typedef struct Tegra30DCWindow {
    uint32_t win_options;       /* DC_WIN_x_WIN_OPTIONS  (WIN_ENABLE bit 30) */
    uint32_t color_depth;       /* DC_WIN_x_COLOR_DEPTH  (field [4:0])       */
    uint32_t position;          /* DC_WIN_x_POSITION                         */
    uint32_t size;              /* DC_WIN_x_SIZE  (H [12:0], V [28:16])      */
    uint32_t line_stride;       /* DC_WIN_x_LINE_STRIDE  (bytes, [15:0])     */
    uint32_t start_addr;        /* DC_WINBUF_x_START_ADDR (guest phys addr)  */
} Tegra30DCWindow;

struct Tegra30DCState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    QemuConsole *con;
    qemu_irq irq;
    QEMUTimer *frame_timer;
    Tegra30Host1xState *host1x;
    uint32_t pending_sync[32];

    /* Flat backing store so any unmodelled register reads back what was
     * written; the interpreted registers are mirrored into the fields below. */
    uint32_t regs[TEGRA30_DC_NUM_REGS];

    /* Currently selected window (from DISPLAY_WINDOW_HEADER), 0..2 or -1. */
    int cur_window;

    Tegra30DCWindow win[TEGRA30_DC_NUM_WINDOWS];

    uint32_t disp_active;       /* DC_DISP_DISP_ACTIVE (panel resolution) */

    /* Cached parameters of the surface currently handed to the console, so we
     * only rebuild it when the guest changes the framebuffer geometry. */
    bool surface_valid;
    hwaddr last_addr;
    uint32_t last_width;
    uint32_t last_height;
    uint32_t last_stride;
    uint32_t last_format;       /* pixman_format_code_t, 0 == none */
    bool invalidate;
};

#endif /* HW_DISPLAY_TEGRA30_DC_H */
