#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/dma.h"
#include "hw/misc/tegra30_snor.h"

/*
 * nVidia Tegra 3 SNOR (synchronous NOR) controller.
 *
 * The controller exposes a small register file and a DMA engine.  Boot
 * software (nVidia QuickBoot) programs the NOR-side source address, the
 * AHB-side (system memory) destination address and a transfer length, then
 * kicks the transfer by setting the GO bit in CONFIG.  The hardware copies the
 * requested number of words from the memory-mapped NOR window into system
 * memory and clears the GO bits when finished.
 *
 * Register layout (32-bit registers):
 *   0x00 CONFIG      bit31 = GO_NOR (start; cleared when the transfer is done)
 *   0x04 STATUS      transfer status; low 16 bits read 0 when idle
 *   0x08 NOR_ADDR    NOR-side source address (points into the NOR window)
 *   0x0C AHB_ADDR    AHB/system-memory destination address
 *   0x10 TIMING0     interface timing (not modelled)
 *   0x14 TIMING1     interface timing (not modelled)
 *   0x20 DMA_CFG     bit31 = DMA_GO, bits[15:2] = (word_count - 1)
 */

enum {
    SNOR_CONFIG   = 0x00,
    SNOR_STATUS   = 0x04,
    SNOR_NOR_ADDR = 0x08,
    SNOR_AHB_ADDR = 0x0C,
    SNOR_TIMING0  = 0x10,
    SNOR_TIMING1  = 0x14,
    SNOR_DMA_CFG  = 0x20,
};

#define SNOR_CONFIG_GO          (1u << 31)
#define SNOR_DMA_CFG_GO         (1u << 31)  /* DMA_GO       */
#define SNOR_DMA_CFG_BSY        (1u << 30)  /* DMA busy     */
#define SNOR_DMA_CFG_IE_DMA_DONE (1u << 28) /* IRQ enable   */
#define SNOR_DMA_CFG_IS_DMA_DONE (1u << 27) /* IRQ status (W1C) = completion */
#define SNOR_STA_DEVICE_BSY     (1u << 31)
#define SNOR_STA_SLAVE_DONE     (1u << 30)  /* transfer-done status */

static void tegra30_snor_do_dma(Tegra30SnorState *s)
{
    uint32_t nor_addr = s->regs[SNOR_NOR_ADDR / 4];
    uint32_t ahb_addr = s->regs[SNOR_AHB_ADDR / 4];
    uint32_t dma_cfg  = s->regs[SNOR_DMA_CFG / 4];
    /* DMA_CFG[15:2] holds (word_count - 1). */
    uint32_t words = ((dma_cfg >> 2) & 0x3fff) + 1;
    uint32_t len = words * 4;
    g_autofree uint8_t *buf = g_malloc(len);

    /* NOR window -> system memory copy via the global system address space. */
    dma_memory_read(&address_space_memory, nor_addr, buf, len,
                    MEMTXATTRS_UNSPECIFIED);
    dma_memory_write(&address_space_memory, ahb_addr, buf, len,
                     MEMTXATTRS_UNSPECIFIED);

    /* Transfer completed synchronously: clear the GO/BSY bits, advance ptrs. */
    s->regs[SNOR_CONFIG / 4]  &= ~SNOR_CONFIG_GO;
    s->regs[SNOR_DMA_CFG / 4] &= ~(SNOR_DMA_CFG_GO | SNOR_DMA_CFG_BSY);
    s->regs[SNOR_NOR_ADDR / 4] = nor_addr + len;
    s->regs[SNOR_AHB_ADDR / 4] = ahb_addr + len;

    /*
     * Signal completion.  The driver polls / takes an interrupt on the
     * DMA-done status: DMA_CFG.IS_DMA_DONE (bit27, W1C) and SNOR_STA.SLAVE_DONE
     * (bit30).  Without this the block DMA reads "time out" even though the data
     * was copied (the header is read by direct NOR-window access, which is why
     * only the large block transfers appeared to fail).  Raise the SNOR
     * interrupt too when IE_DMA_DONE is enabled.
     */
    s->regs[SNOR_DMA_CFG / 4] |= SNOR_DMA_CFG_IS_DMA_DONE;
    s->regs[SNOR_STATUS / 4] = (s->regs[SNOR_STATUS / 4] & ~SNOR_STA_DEVICE_BSY)
                               | SNOR_STA_SLAVE_DONE;
    if (dma_cfg & SNOR_DMA_CFG_IE_DMA_DONE) {
        qemu_irq_raise(s->irq);
    }
}

static uint64_t tegra30_snor_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra30SnorState *s = TEGRA30_SNOR(opaque);

    if (offset / 4 >= TEGRA30_SNOR_NUM_REGS) {
        qemu_log_mask(LOG_UNIMP, "%s: read offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return 0;
    }

    return s->regs[offset / 4];
}

static void tegra30_snor_write(void *opaque, hwaddr offset, uint64_t val,
                               unsigned size)
{
    Tegra30SnorState *s = TEGRA30_SNOR(opaque);

    if (offset / 4 >= TEGRA30_SNOR_NUM_REGS) {
        qemu_log_mask(LOG_UNIMP, "%s: write offset 0x%04x = 0x%08x\n",
                      __func__, (uint32_t)offset, (uint32_t)val);
        return;
    }

    if (offset == SNOR_DMA_CFG) {
        /* IS_DMA_DONE (bit27) is write-1-to-clear; other bits are R/W. */
        uint32_t cur = s->regs[SNOR_DMA_CFG / 4];
        uint32_t newv = (uint32_t)val & ~SNOR_DMA_CFG_IS_DMA_DONE;
        if (val & SNOR_DMA_CFG_IS_DMA_DONE) {
            cur &= ~SNOR_DMA_CFG_IS_DMA_DONE;     /* acknowledge */
        }
        s->regs[SNOR_DMA_CFG / 4] = newv | (cur & SNOR_DMA_CFG_IS_DMA_DONE);
        if (!(s->regs[SNOR_DMA_CFG / 4] & SNOR_DMA_CFG_IS_DMA_DONE)) {
            qemu_irq_lower(s->irq);
        }
        return;
    }

    if (offset == SNOR_STATUS) {
        /* SLAVE_DONE and the other status bits are write-1-to-clear. */
        s->regs[SNOR_STATUS / 4] &= ~((uint32_t)val & 0x5f000000u);
        return;
    }

    s->regs[offset / 4] = (uint32_t)val;

    /* Setting GO_NOR in CONFIG kicks the (synchronous) DMA. */
    if (offset == SNOR_CONFIG && (val & SNOR_CONFIG_GO)) {
        tegra30_snor_do_dma(s);
    }
}

static const MemoryRegionOps tegra30_snor_ops = {
    .read = tegra30_snor_read,
    .write = tegra30_snor_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_snor_reset(DeviceState *dev)
{
    Tegra30SnorState *s = TEGRA30_SNOR(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void tegra30_snor_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30SnorState *s = TEGRA30_SNOR(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &tegra30_snor_ops, s,
                          TYPE_TEGRA30_SNOR, TEGRA30_SNOR_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription tegra30_snor_vmstate = {
    .name = "tegra30-snor",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30SnorState, TEGRA30_SNOR_NUM_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_snor_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_snor_reset);
    dc->vmsd = &tegra30_snor_vmstate;
}

static const TypeInfo tegra30_snor_info = {
    .name          = TYPE_TEGRA30_SNOR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_snor_init,
    .instance_size = sizeof(Tegra30SnorState),
    .class_init    = tegra30_snor_class_init,
};

static void tegra30_snor_register(void)
{
    type_register_static(&tegra30_snor_info);
}

type_init(tegra30_snor_register)
