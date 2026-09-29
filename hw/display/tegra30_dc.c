/*
 * nVidia Tegra 3 Display Controller (DC).
 *
 * Models enough of the display controller for the head unit to present its
 * framebuffer in a QEMU window.  Registers are RAM-backed (so any write reads
 * back), and a small subset is interpreted to locate the active overlay
 * window's framebuffer in guest RAM:
 *
 *   DC_CMD_DISPLAY_WINDOW_HEADER selects window A/B/C; subsequent DC_WIN and
 *   DC_WINBUF writes are routed to the selected window's state.  Per window we
 *   track WIN_OPTIONS (enable), COLOR_DEPTH (pixel format), SIZE
 *   (width/height), LINE_STRIDE (bytes per line) and START_ADDR (framebuffer
 *   base).  Each gfx_update we (re)wrap the guest framebuffer in a
 *   DisplaySurface so QEMU scans pixels straight out of guest RAM.
 *
 * The register indices and field layouts follow the Tegra3 register spec
 * (hwinc/t30 ardisplay.h / ardisplay_b.h); the byte offset of register index N
 * is N*4.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "ui/console.h"
#include "exec/cpu-common.h"
#include "hw/display/tegra30_dc.h"

/* Register indices (NOT byte offsets; byte offset = index * 4). */
enum {
    DC_CMD_DISPLAY_WINDOW_HEADER = 0x042,
    DC_DISP_DISP_ACTIVE          = 0x409,
    DC_WIN_WIN_OPTIONS           = 0x700,
    DC_WIN_COLOR_DEPTH           = 0x703,
    DC_WIN_POSITION              = 0x704,
    DC_WIN_SIZE                  = 0x705,
    DC_WIN_LINE_STRIDE           = 0x70a,
    DC_WINBUF_START_ADDR         = 0x800,
};

/* DC_CMD_DISPLAY_WINDOW_HEADER: window-select bits. */
#define WINDOW_A_SELECT     (1u << 4)
#define WINDOW_B_SELECT     (1u << 5)
#define WINDOW_C_SELECT     (1u << 6)

/* DC_WIN_WIN_OPTIONS. */
#define WIN_ENABLE          (1u << 30)

/* DC_WIN_SIZE / DC_DISP_DISP_ACTIVE field layout. */
#define SIZE_H_SHIFT        0
#define SIZE_V_SHIFT        16
#define SIZE_MASK           0x1fff

/* DC_WIN_LINE_STRIDE: bytes-per-line live in [15:0]. */
#define LINE_STRIDE_MASK    0xffff

/* DC_WIN_COLOR_DEPTH: format enum in [4:0]. */
#define COLOR_DEPTH_MASK    0x1f

/* DC_WIN_x_COLOR_DEPTH enum values we recognise (see ardisplay_b.h). */
enum {
    WIN_COLOR_DEPTH_B5G5R5A  = 5,
    WIN_COLOR_DEPTH_B5G6R5   = 6,
    WIN_COLOR_DEPTH_AB5G5R5  = 7,
    WIN_COLOR_DEPTH_B8G8R8A8 = 12,
    WIN_COLOR_DEPTH_R8G8B8A8 = 13,
};

/* Fallback panel geometry until the guest programs DISP_ACTIVE / window SIZE. */
#define TEGRA30_DC_DEFAULT_WIDTH    800
#define TEGRA30_DC_DEFAULT_HEIGHT   480

/*
 * Release the guest-RAM mapping backing a surface when pixman tears the image
 * down.  The mapped length is recovered from the image itself (stride*height),
 * mirroring hw/display/ramfb.c.
 */
static void tegra30_dc_unmap_surface(pixman_image_t *image, void *unused)
{
    void *data = pixman_image_get_data(image);
    hwaddr len = (hwaddr)pixman_image_get_stride(image) *
                 pixman_image_get_height(image);

    cpu_physical_memory_unmap(data, len, false, len);
}

/*
 * Translate a Tegra COLOR_DEPTH enum into a pixman format.  Tegra names the
 * format by in-memory byte order; pixman names it by the value read as a
 * native (host-endian) word, so the mapping below assumes a little-endian
 * host (which is what this emulator targets).  Returns 0 for formats we do not
 * handle yet.
 */
static pixman_format_code_t tegra30_dc_pixman_format(uint32_t color_depth)
{
    switch (color_depth & COLOR_DEPTH_MASK) {
    case WIN_COLOR_DEPTH_B8G8R8A8:
        /* bytes B,G,R,A -> LE word 0xAARRGGBB */
        return PIXMAN_a8r8g8b8;
    case WIN_COLOR_DEPTH_R8G8B8A8:
        /* bytes R,G,B,A -> LE word 0xAABBGGRR */
        return PIXMAN_a8b8g8r8;
    case WIN_COLOR_DEPTH_B5G6R5:
        return PIXMAN_r5g6b5;
    case WIN_COLOR_DEPTH_B5G5R5A:
    case WIN_COLOR_DEPTH_AB5G5R5:
        return PIXMAN_a1r5g5b5;
    default:
        return 0;
    }
}

/* Bytes per pixel for a recognised COLOR_DEPTH, or 0 if unknown. */
static unsigned tegra30_dc_bytes_per_pixel(uint32_t color_depth)
{
    pixman_format_code_t fmt = tegra30_dc_pixman_format(color_depth);

    return fmt ? (PIXMAN_FORMAT_BPP(fmt) / 8) : 0;
}

/*
 * Work out the framebuffer geometry for the first enabled window.  Falls back
 * to a blank default panel if nothing is enabled/programmed yet.  Returns the
 * pixman format (0 means "render blank").
 */
static pixman_format_code_t tegra30_dc_active_fb(Tegra30DCState *s,
                                                 hwaddr *addr,
                                                 uint32_t *width,
                                                 uint32_t *height,
                                                 uint32_t *stride)
{
    for (int w = 0; w < TEGRA30_DC_NUM_WINDOWS; w++) {
        Tegra30DCWindow *win = &s->win[w];
        pixman_format_code_t fmt;
        unsigned bpp;
        uint32_t ww, wh, ws;

        if (!(win->win_options & WIN_ENABLE) || win->start_addr == 0) {
            continue;
        }

        fmt = tegra30_dc_pixman_format(win->color_depth);
        bpp = tegra30_dc_bytes_per_pixel(win->color_depth);
        if (fmt == 0 || bpp == 0) {
            continue;
        }

        ww = (win->size >> SIZE_H_SHIFT) & SIZE_MASK;
        wh = (win->size >> SIZE_V_SHIFT) & SIZE_MASK;
        if (ww == 0 || wh == 0) {
            continue;
        }

        ws = win->line_stride & LINE_STRIDE_MASK;
        if (ws == 0) {
            ws = ww * bpp;
        }

        *addr = win->start_addr;
        *width = ww;
        *height = wh;
        *stride = ws;
        return fmt;
    }

    /* Nothing enabled: blank panel at DISP_ACTIVE (or the default) size. */
    *addr = 0;
    *width = (s->disp_active >> SIZE_H_SHIFT) & SIZE_MASK;
    *height = (s->disp_active >> SIZE_V_SHIFT) & SIZE_MASK;
    if (*width == 0 || *height == 0) {
        *width = TEGRA30_DC_DEFAULT_WIDTH;
        *height = TEGRA30_DC_DEFAULT_HEIGHT;
    }
    *stride = 0;
    return 0;
}

/* Paint a black placeholder surface of the given size. */
static void tegra30_dc_blank(Tegra30DCState *s, uint32_t width, uint32_t height)
{
    DisplaySurface *surface = qemu_console_surface(s->con);

    /*
     * Make sure the console owns its surface: the previous frame may have
     * installed a surface aliasing guest RAM, which we must not scribble on.
     * A resize allocates a fresh QEMU-owned surface; force one if the current
     * surface is the wrong size or is not QEMU-allocated.
     */
    if (!surface ||
        surface_width(surface) != width ||
        surface_height(surface) != height ||
        !surface_is_allocated(surface)) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
    }
    if (surface && surface_is_allocated(surface)) {
        memset(surface_data(surface), 0,
               (size_t)surface_stride(surface) * surface_height(surface));
    }
    dpy_gfx_update_full(s->con);

    s->surface_valid = false;
    s->last_format = 0;
}

static void tegra30_dc_update_display(void *opaque)
{
    Tegra30DCState *s = opaque;
    pixman_format_code_t format;
    hwaddr addr, mapsize, size, linesize;
    uint32_t width, height, stride;
    DisplaySurface *surface;
    void *data;

    format = tegra30_dc_active_fb(s, &addr, &width, &height, &stride);
    if (format == 0) {
        /* No usable framebuffer yet; show a blank panel. */
        tegra30_dc_blank(s, width, height);
        return;
    }

    /*
     * If geometry/format/base are unchanged and we already installed a
     * surface, the surface still aliases guest RAM, so just push an update.
     */
    if (s->surface_valid && !s->invalidate &&
        addr == s->last_addr && width == s->last_width &&
        height == s->last_height && stride == s->last_stride &&
        format == s->last_format) {
        dpy_gfx_update_full(s->con);
        return;
    }

    /* Map the guest framebuffer and wrap it in a surface (read directly). */
    linesize = (hwaddr)width * (PIXMAN_FORMAT_BPP(format) / 8);
    size = (hwaddr)stride * (height - 1) + linesize;
    mapsize = size;
    data = cpu_physical_memory_map(addr, &mapsize, false);
    if (!data || mapsize != size) {
        if (data) {
            cpu_physical_memory_unmap(data, mapsize, 0, 0);
        }
        tegra30_dc_blank(s, width, height);
        return;
    }

    surface = qemu_create_displaysurface_from(width, height, format,
                                              stride, data);
    if (!surface) {
        cpu_physical_memory_unmap(data, mapsize, 0, 0);
        tegra30_dc_blank(s, width, height);
        return;
    }
    /* Release the guest mapping when the surface is torn down/replaced. */
    pixman_image_set_destroy_function(surface->image,
                                      tegra30_dc_unmap_surface, NULL);

    dpy_gfx_replace_surface(s->con, surface);

    s->surface_valid = true;
    s->invalidate = false;
    s->last_addr = addr;
    s->last_width = width;
    s->last_height = height;
    s->last_stride = stride;
    s->last_format = format;

    dpy_gfx_update_full(s->con);
}

static void tegra30_dc_invalidate_display(void *opaque)
{
    Tegra30DCState *s = opaque;

    s->invalidate = true;
    s->surface_valid = false;
}

/* Route a window register write to the currently selected window(s). */
static void tegra30_dc_window_write(Tegra30DCState *s, uint32_t index,
                                    uint32_t value)
{
    uint32_t sel = s->regs[DC_CMD_DISPLAY_WINDOW_HEADER];
    bool any = false;

    for (int w = 0; w < TEGRA30_DC_NUM_WINDOWS; w++) {
        if (!(sel & (WINDOW_A_SELECT << w))) {
            continue;
        }
        any = true;
        switch (index) {
        case DC_WIN_WIN_OPTIONS:  s->win[w].win_options = value; break;
        case DC_WIN_COLOR_DEPTH:  s->win[w].color_depth = value; break;
        case DC_WIN_POSITION:     s->win[w].position    = value; break;
        case DC_WIN_SIZE:         s->win[w].size        = value; break;
        case DC_WIN_LINE_STRIDE:  s->win[w].line_stride = value; break;
        case DC_WINBUF_START_ADDR: s->win[w].start_addr = value; break;
        default: break;
        }
    }

    /* Re-evaluate the framebuffer on the next refresh. */
    if (any) {
        s->surface_valid = false;
    }
}

static uint64_t tegra30_dc_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra30DCState *s = TEGRA30_DC(opaque);
    uint32_t index = offset >> 2;

    if (index >= TEGRA30_DC_NUM_REGS) {
        return 0;
    }
    return s->regs[index];
}

static void tegra30_dc_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    Tegra30DCState *s = TEGRA30_DC(opaque);
    uint32_t index = offset >> 2;

    if (index >= TEGRA30_DC_NUM_REGS) {
        return;
    }

    /* RAM-backed: keep the raw value so any register reads back. */
    s->regs[index] = value;

    switch (index) {
    case DC_DISP_DISP_ACTIVE:
        s->disp_active = value;
        s->surface_valid = false;
        break;
    case DC_WIN_WIN_OPTIONS:
    case DC_WIN_COLOR_DEPTH:
    case DC_WIN_POSITION:
    case DC_WIN_SIZE:
    case DC_WIN_LINE_STRIDE:
    case DC_WINBUF_START_ADDR:
        tegra30_dc_window_write(s, index, value);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps tegra30_dc_ops = {
    .read = tegra30_dc_read,
    .write = tegra30_dc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const GraphicHwOps tegra30_dc_gfx_ops = {
    .invalidate = tegra30_dc_invalidate_display,
    .gfx_update = tegra30_dc_update_display,
};

static void tegra30_dc_reset(DeviceState *dev)
{
    Tegra30DCState *s = TEGRA30_DC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->win, 0, sizeof(s->win));
    s->cur_window = -1;
    s->disp_active = 0;
    s->surface_valid = false;
    s->invalidate = true;
    s->last_addr = 0;
    s->last_width = 0;
    s->last_height = 0;
    s->last_stride = 0;
    s->last_format = 0;
}

static void tegra30_dc_realize(DeviceState *dev, Error **errp)
{
    Tegra30DCState *s = TEGRA30_DC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &tegra30_dc_ops, s,
                          TYPE_TEGRA30_DC, TEGRA30_DC_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    s->con = graphic_console_init(dev, 0, &tegra30_dc_gfx_ops, s);
    qemu_console_resize(s->con, TEGRA30_DC_DEFAULT_WIDTH,
                        TEGRA30_DC_DEFAULT_HEIGHT);
}

static const VMStateDescription tegra30_dc_vmstate = {
    .name = "tegra30-dc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30DCState, TEGRA30_DC_NUM_REGS),
        VMSTATE_INT32(cur_window, Tegra30DCState),
        VMSTATE_UINT32(disp_active, Tegra30DCState),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_dc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_legacy_reset(dc, tegra30_dc_reset);
    dc->vmsd = &tegra30_dc_vmstate;
    dc->realize = tegra30_dc_realize;
}

static const TypeInfo tegra30_dc_info = {
    .name          = TYPE_TEGRA30_DC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30DCState),
    .class_init    = tegra30_dc_class_init,
};

static void tegra30_dc_register(void)
{
    type_register_static(&tegra30_dc_info);
}

type_init(tegra30_dc_register)
