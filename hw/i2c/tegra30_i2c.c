/*
 * nVidia Tegra legacy (non-packet) I2C controller.
 *
 * Models the "CMD" style transfer interface used by the bootloader: software
 * programs CMD_ADDR0/1 with the slave address (addr << 1 | R/W), the data
 * length and direction in I2C_CNFG, optionally the outgoing bytes in
 * CMD_DATA1/2, then sets the SEND bit.  The transfer is performed
 * synchronously here; BUSY is never reported set and the per-command status
 * nibbles in I2C_STATUS report success/NACK.
 *
 * Ported from the Tegra2 reference implementation by Iscle.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/tegra30_i2c.h"

/*
 * Verbose slave-path tracing, toggled at runtime via the TEGRA_I2C_DEBUG
 * environment variable so the IOC link can be observed without a rebuild.
 */
static int tegra30_i2c_debug(void)
{
    static int dbg = -1;
    if (dbg < 0) {
        dbg = getenv("TEGRA_I2C_DEBUG") != NULL;
    }
    return dbg;
}

#define I2C_DBG(s, fmt, ...)                                                \
    do {                                                                   \
        if (tegra30_i2c_debug()) {                                         \
            fprintf(stderr, "[tegra-i2c %p] " fmt "\n", (void *)(s),       \
                    ##__VA_ARGS__);                                        \
        }                                                                  \
    } while (0)

static void tegra30_i2c_update_irq(Tegra30I2CState *s)
{
    /*
     * The slave packet interrupt reaches the OS directly over the A9 GIC
     * (InterruptAttachEvent on SPI 84); the NvI2c driver never programs the
     * controller INTERRUPT_MASK (0x64), so a pending slave phase must drive the
     * line regardless of int_mask.  The legacy/master path still honours
     * int_mask.
     */
    int level = s->slv_irq_pending || !!(s->int_status & s->int_mask);
    qemu_set_irq(s->irq, level);
}

/* Pop the next word (up to 4 bytes, little-endian) from the slave RX FIFO. */
static uint32_t tegra30_i2c_slv_rx_pop(Tegra30I2CState *s)
{
    uint32_t word = 0;

    for (int i = 0; i < 4 && s->slv_rx_pos < s->slv_rx_len; i++) {
        word |= (uint32_t)s->slv_rx[s->slv_rx_pos++] << (i * 8);
    }
    I2C_DBG(s, "  SLV_RX_FIFO pop -> 0x%08x (pos %u/%u, phase %u)",
            word, s->slv_rx_pos, s->slv_rx_len, s->slv_phase);
    return word;
}

/*
 * Present phase 1: the "address" interrupt carrying the first frame byte.  The
 * driver reads one word from the RX FIFO and arms itself for the data phase.
 */
static void tegra30_i2c_slv_present_addr(Tegra30I2CState *s)
{
    s->slv_rx[0] = s->slv_frame[0];
    s->slv_rx_len = 1;
    s->slv_rx_pos = 0;
    s->sl_status = 0;                       /* RNW = 0: master is writing */
    s->sl_int_source = 0;
    s->slv_packet_status = 0;
    s->int_status = (s->int_status & ~I2C_INT_SLV_PACKET_XFER) |
                    I2C_INT_SLV_RX_BUFFER_FILLED;
    s->slv_phase = 1;
    s->slv_irq_pending = true;
    I2C_DBG(s, "phase1: present addr byte 0x%02x (frame_len %u)",
            s->slv_frame[0], s->slv_frame_len);
    tegra30_i2c_update_irq(s);
}

/*
 * Present phase 2: the "data" interrupt carrying the remaining bytes.  The
 * driver reads ceil(N/4) words and returns the N+1 byte frame from
 * NvI2cSlaveReceive.
 */
static void tegra30_i2c_slv_present_data(Tegra30I2CState *s)
{
    uint32_t n = s->slv_frame_len ? s->slv_frame_len - 1 : 0;

    memcpy(s->slv_rx, s->slv_frame + 1, n);
    s->slv_rx_len = n;
    s->slv_rx_pos = 0;
    s->sl_status = 0;
    s->sl_int_source = I2C_SL_INT_SOURCE_RCVD;
    s->slv_packet_status = (n & 0xfff) << I2C_SLV_PKT_BYTENUM_SHIFT;
    s->int_status = (s->int_status & ~I2C_INT_SLV_RX_BUFFER_FILLED) |
                    I2C_INT_SLV_PACKET_XFER;
    s->slv_phase = 2;
    s->slv_irq_pending = true;
    I2C_DBG(s, "phase2: present %u data bytes (pkt_status 0x%08x)",
            n, s->slv_packet_status);
    tegra30_i2c_update_irq(s);
}

/* The driver has consumed the data phase; retire the frame and drop the line. */
static void tegra30_i2c_slv_frame_done(Tegra30I2CState *s)
{
    I2C_DBG(s, "frame done");
    s->slv_phase = 0;
    s->slv_irq_pending = false;
    s->slv_frame_len = 0;
    s->sl_int_source = 0;
    s->int_status &= ~(I2C_INT_SLV_RX_BUFFER_FILLED | I2C_INT_SLV_PACKET_XFER);
    tegra30_i2c_update_irq(s);
}

static uint64_t tegra30_i2c_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    Tegra30I2CState *s = TEGRA30_I2C(opaque);
    uint32_t avail;

    switch (offset) {
    case I2C_CNFG_OFFSET:
        return s->cnfg;
    case I2C_CMD_ADDR0_OFFSET:
        return s->cmd_addr0;
    case I2C_CMD_ADDR1_OFFSET:
        return s->cmd_addr1;
    case I2C_CMD_DATA1_OFFSET:
        return s->cmd_data1;
    case I2C_CMD_DATA2_OFFSET:
        return s->cmd_data2;
    case I2C_STATUS_OFFSET:
        return s->status;
    case I2C_SL_CNFG_OFFSET:
        return s->sl_cnfg;
    case I2C_SL_STATUS_OFFSET:
        return s->sl_status;
    case I2C_SL_ADDR1_OFFSET:
        return s->sl_addr1;
    case I2C_SL_INT_MASK_OFFSET:
        return s->sl_int_mask;
    case I2C_SL_INT_SOURCE_OFFSET:
        return s->sl_int_source;
    case I2C_INT_MASK_OFFSET:
        return s->int_mask;
    case I2C_INT_STATUS_OFFSET:
        return s->int_status;
    case I2C_INT_SOURCE_OFFSET:
        /* Raw (unmasked) interrupt source; software polls this when the
         * interrupt mask is clear. */
        return s->int_status;
    case I2C_FIFO_STATUS_OFFSET:
        /* Slave RX FIFO fill level (in words) in bits [19:16]. */
        avail = (s->slv_rx_len - s->slv_rx_pos + 3) / 4;
        return avail << I2C_FIFO_STATUS_SLV_RX_CNT_SHIFT;
    case I2C_SLV_PACKET_STATUS_OFFSET:
        return s->slv_packet_status;
    case I2C_SLV_RX_FIFO_OFFSET:
        return tegra30_i2c_slv_rx_pop(s);
    default:
        return s->regs[offset / sizeof(uint32_t)];
    }
}

static void tegra30_i2c_do_command(Tegra30I2CState *s, uint8_t addr,
                                   bool is_read, unsigned int length,
                                   uint32_t *data, uint32_t *cmd_stat)
{
    int ret;

    *cmd_stat = 0;

    ret = i2c_start_transfer(s->bus, addr, is_read);
    if (ret) {
        /* No device acknowledged the address. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: NACK from slave 0x%02x (%s)\n", __func__, addr,
                      is_read ? "read" : "write");
        *cmd_stat = 1;
        return;
    }

    if (is_read) {
        *data = 0;
        for (unsigned int i = 0; i < length; i++) {
            *data |= (uint32_t)i2c_recv(s->bus) << (i * 8);
        }
    } else {
        for (unsigned int i = 0; i < length; i++) {
            if (i2c_send(s->bus, (*data >> (i * 8)) & 0xFF)) {
                *cmd_stat = i + 1;
                break;
            }
        }
    }

    i2c_end_transfer(s->bus);
}

static void tegra30_i2c_start(Tegra30I2CState *s)
{
    unsigned int length = ((s->cnfg & I2C_CNFG_LENGTH_MASK)
                           >> I2C_CNFG_LENGTH_SHIFT) + 1;
    uint32_t cmd1_stat = 0, cmd2_stat = 0;

    tegra30_i2c_do_command(s, s->cmd_addr0 >> 1, s->cnfg & I2C_CNFG_CMD1,
                           length, &s->cmd_data1, &cmd1_stat);

    if (s->cnfg & I2C_CNFG_SLV2) {
        tegra30_i2c_do_command(s, s->cmd_addr1 >> 1, s->cnfg & I2C_CNFG_CMD2,
                               length, &s->cmd_data2, &cmd2_stat);
    }

    /* BUSY clear, command status nibbles updated. */
    s->status = (s->status & ~(I2C_STATUS_BUSY |
                               I2C_STATUS_CMD1_STAT_MASK |
                               I2C_STATUS_CMD2_STAT_MASK)) |
                (cmd1_stat & I2C_STATUS_CMD1_STAT_MASK) |
                ((cmd2_stat << 4) & I2C_STATUS_CMD2_STAT_MASK);

    s->cnfg &= ~I2C_CNFG_SEND;
}

static void tegra30_i2c_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    Tegra30I2CState *s = TEGRA30_I2C(opaque);

    switch (offset) {
    case I2C_CNFG_OFFSET:
        s->cnfg = value;
        if (s->cnfg & I2C_CNFG_SEND) {
            tegra30_i2c_start(s);
        }
        if (s->cnfg & I2C_CNFG_PACKET_MODE_EN) {
            qemu_log_mask(LOG_UNIMP, "%s: packet mode not implemented\n",
                          __func__);
        }
        break;
    case I2C_CMD_ADDR0_OFFSET:
        s->cmd_addr0 = value;
        break;
    case I2C_CMD_ADDR1_OFFSET:
        s->cmd_addr1 = value;
        break;
    case I2C_CMD_DATA1_OFFSET:
        s->cmd_data1 = value;
        break;
    case I2C_CMD_DATA2_OFFSET:
        s->cmd_data2 = value;
        break;
    case I2C_STATUS_OFFSET:
        /* read-only */
        break;
    case I2C_SL_CNFG_OFFSET:
        s->sl_cnfg = value;
        break;
    case I2C_SL_STATUS_OFFSET:
        /*
         * The driver writes RCVD (0x10) to acknowledge the data phase, after it
         * has drained the RX FIFO -- this is the point the frame is fully
         * consumed and the line can drop.
         */
        if (s->slv_phase == 2 && (value & I2C_SL_INT_SOURCE_RCVD)) {
            tegra30_i2c_slv_frame_done(s);
        } else {
            s->sl_status &= ~(uint32_t)value;
        }
        break;
    case I2C_SL_ADDR1_OFFSET:
        s->sl_addr1 = value;
        break;
    case I2C_SL_INT_MASK_OFFSET:
        s->sl_int_mask = value;
        break;
    case I2C_SL_INT_SOURCE_OFFSET:
        s->sl_int_source &= ~(uint32_t)value;   /* write-1-to-clear */
        break;
    case I2C_INT_MASK_OFFSET:
        s->int_mask = value;
        tegra30_i2c_update_irq(s);
        break;
    case I2C_INT_STATUS_OFFSET:
        s->int_status &= ~(uint32_t)value;   /* write-1-to-clear */
        I2C_DBG(s, "wr INT_STATUS clear 0x%08x -> 0x%08x (phase %u)",
                (uint32_t)value, s->int_status, s->slv_phase);
        /*
         * Clearing the addr-phase RX interrupt is the driver's signal that it
         * has taken the first byte and armed for data: advance to phase 2.
         */
        if (s->slv_phase == 1 && (value & I2C_INT_SLV_RX_BUFFER_FILLED)) {
            tegra30_i2c_slv_present_data(s);
        } else {
            tegra30_i2c_update_irq(s);
        }
        break;
    case I2C_FIFO_CONTROL_OFFSET:
        /*
         * The TX/RX FIFO flush request bits (master 0/1, slave 8/9) are
         * one-shot: the controller performs the flush and clears them.  Model
         * the flush as instantaneous so software polling for completion sees
         * them clear.
         */
        s->regs[offset / sizeof(uint32_t)] = value & ~0x303u;
        break;
    default:
        s->regs[offset / sizeof(uint32_t)] = value;
        break;
    }
}

void tegra30_i2c_slave_deliver(Tegra30I2CState *s, const uint8_t *buf,
                               uint32_t len)
{
    if (s->slv_phase != 0) {
        /* A frame is still being consumed by the driver; drop this one. */
        I2C_DBG(s, "deliver dropped: phase %u busy", s->slv_phase);
        return;
    }
    if (len == 0) {
        return;
    }

    len = MIN(len, sizeof(s->slv_frame));
    memcpy(s->slv_frame, buf, len);
    s->slv_frame_len = len;
    tegra30_i2c_slv_present_addr(s);
}

static const MemoryRegionOps tegra30_i2c_ops = {
    .read = tegra30_i2c_read,
    .write = tegra30_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_i2c_reset(DeviceState *dev)
{
    Tegra30I2CState *s = TEGRA30_I2C(dev);

    s->cnfg = 0x00000800;   /* new_master_fsm set out of reset */
    s->cmd_addr0 = 0;
    s->cmd_addr1 = 0;
    s->cmd_data1 = 0;
    s->cmd_data2 = 0;
    s->status = 0;
    s->sl_cnfg = 0x00000004;
    s->sl_status = 0;
    s->sl_addr1 = 0;
    s->sl_int_mask = 0;
    s->sl_int_source = 0;
    s->int_mask = 0;
    s->int_status = 0;
    s->slv_packet_status = 0;
    s->slv_rx_len = 0;
    s->slv_rx_pos = 0;
    memset(s->slv_rx, 0, sizeof(s->slv_rx));
    s->slv_phase = 0;
    s->slv_irq_pending = false;
    s->slv_frame_len = 0;
    memset(s->slv_frame, 0, sizeof(s->slv_frame));
    memset(s->regs, 0, sizeof(s->regs));
}

static void tegra30_i2c_realize(DeviceState *dev, Error **errp)
{
    Tegra30I2CState *s = TEGRA30_I2C(dev);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    memory_region_init_io(&s->iomem, OBJECT(dev), &tegra30_i2c_ops, s,
                          TYPE_TEGRA30_I2C, TEGRA30_I2C_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    s->bus = i2c_init_bus(dev, "i2c");
}

static const VMStateDescription tegra30_i2c_vmstate = {
    .name = "tegra30-i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cnfg, Tegra30I2CState),
        VMSTATE_UINT32(cmd_addr0, Tegra30I2CState),
        VMSTATE_UINT32(cmd_addr1, Tegra30I2CState),
        VMSTATE_UINT32(cmd_data1, Tegra30I2CState),
        VMSTATE_UINT32(cmd_data2, Tegra30I2CState),
        VMSTATE_UINT32(status, Tegra30I2CState),
        VMSTATE_UINT32(sl_cnfg, Tegra30I2CState),
        VMSTATE_UINT32(sl_status, Tegra30I2CState),
        VMSTATE_UINT32(sl_addr1, Tegra30I2CState),
        VMSTATE_UINT32(sl_int_mask, Tegra30I2CState),
        VMSTATE_UINT32(sl_int_source, Tegra30I2CState),
        VMSTATE_UINT32(int_mask, Tegra30I2CState),
        VMSTATE_UINT32(int_status, Tegra30I2CState),
        VMSTATE_UINT32(slv_packet_status, Tegra30I2CState),
        VMSTATE_UINT32(slv_rx_len, Tegra30I2CState),
        VMSTATE_UINT32(slv_rx_pos, Tegra30I2CState),
        VMSTATE_UINT8_ARRAY(slv_rx, Tegra30I2CState, 256),
        VMSTATE_UINT8(slv_phase, Tegra30I2CState),
        VMSTATE_BOOL(slv_irq_pending, Tegra30I2CState),
        VMSTATE_UINT32(slv_frame_len, Tegra30I2CState),
        VMSTATE_UINT8_ARRAY(slv_frame, Tegra30I2CState, 256),
        VMSTATE_UINT32_ARRAY(regs, Tegra30I2CState,
                             TEGRA30_I2C_IOSIZE / sizeof(uint32_t)),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_i2c_reset);
    dc->realize = tegra30_i2c_realize;
    dc->vmsd = &tegra30_i2c_vmstate;
}

static const TypeInfo tegra30_i2c_info = {
    .name          = TYPE_TEGRA30_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30I2CState),
    .class_init    = tegra30_i2c_class_init,
};

static void tegra30_i2c_register(void)
{
    type_register_static(&tegra30_i2c_info);
}

type_init(tegra30_i2c_register)
