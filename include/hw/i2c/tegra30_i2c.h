#ifndef HW_I2C_TEGRA30_I2C_H
#define HW_I2C_TEGRA30_I2C_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"

/** Size of the register window of a Tegra I2C controller */
#define TEGRA30_I2C_IOSIZE        (0x200)

/* Register offsets (legacy, non-packet I2C controller) */
#define I2C_CNFG_OFFSET           0x00
#define I2C_CMD_ADDR0_OFFSET      0x04
#define I2C_CMD_ADDR1_OFFSET      0x08
#define I2C_CMD_DATA1_OFFSET      0x0C
#define I2C_CMD_DATA2_OFFSET      0x10
#define I2C_STATUS_OFFSET         0x1C
#define I2C_SL_CNFG_OFFSET        0x20
#define I2C_SL_STATUS_OFFSET      0x28
#define I2C_SL_ADDR1_OFFSET       0x2C
#define I2C_SL_INT_MASK_OFFSET    0x40
#define I2C_SL_INT_SOURCE_OFFSET  0x44
#define I2C_FIFO_CONTROL_OFFSET   0x5C
#define I2C_FIFO_STATUS_OFFSET    0x60
#define I2C_INT_MASK_OFFSET       0x64
#define I2C_INT_STATUS_OFFSET     0x68
#define I2C_INT_SOURCE_OFFSET     0x70
#define I2C_SLV_RX_FIFO_OFFSET    0x7C
#define I2C_SLV_PACKET_STATUS_OFFSET 0x80

/* INT_SOURCE/INT_STATUS (0x70/0x68) slave bits. */
#define I2C_INT_SLV_RX_BUFFER_FILLED  (1u << 23)
#define I2C_INT_SLV_TX_BUFFER_REQ     (1u << 24)
#define I2C_INT_SLV_PACKET_XFER       (1u << 25)  /* slave packet xfer complete */

/* SL_STATUS (0x28) bits. */
#define I2C_SL_STATUS_RNW             (1u << 1)   /* master read-not-write */

/* SL_INT_SOURCE (0x44) bits. */
#define I2C_SL_INT_SOURCE_RCVD        (1u << 4)   /* slave received data */

/* FIFO_STATUS / SLV_PACKET_STATUS fields. */
#define I2C_FIFO_STATUS_SLV_RX_CNT_SHIFT  16
#define I2C_SLV_PKT_BYTENUM_SHIFT         4       /* byte count in [15:4] */

/* I2C_CNFG bits */
#define I2C_CNFG_A_MOD            (1u << 0)   /* 10-bit address mode      */
#define I2C_CNFG_LENGTH_SHIFT     1
#define I2C_CNFG_LENGTH_MASK      (0x7u << 1) /* bytes-1 for this command */
#define I2C_CNFG_SLV2             (1u << 4)   /* second slave transaction */
#define I2C_CNFG_START            (1u << 5)
#define I2C_CNFG_CMD1             (1u << 6)   /* 1 = addr0 is a read      */
#define I2C_CNFG_CMD2             (1u << 7)   /* 1 = addr1 is a read      */
#define I2C_CNFG_NOACK            (1u << 8)
#define I2C_CNFG_SEND             (1u << 9)   /* kick off the transfer    */
#define I2C_CNFG_PACKET_MODE_EN   (1u << 10)

/* I2C_STATUS bits */
#define I2C_STATUS_CMD1_STAT_MASK 0x0000000Fu
#define I2C_STATUS_CMD2_STAT_MASK 0x000000F0u
#define I2C_STATUS_BUSY           (1u << 8)

#define TYPE_TEGRA30_I2C    "tegra30-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30I2CState, TEGRA30_I2C)

struct Tegra30I2CState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t packet_header[3];
    uint32_t packet_words;
    uint32_t packet_remaining;
    uint32_t rx_count;
    uint32_t rx_pos;
    uint8_t rx_fifo[32];
    bool packet_seen;
    bool packet_read;
    bool packet_active;

    uint32_t cnfg;
    uint32_t cmd_addr0;
    uint32_t cmd_addr1;
    uint32_t cmd_data1;
    uint32_t cmd_data2;
    uint32_t status;
    uint32_t sl_cnfg;

    /* Slave-mode receive path, used by the on-board companion (IOC) link. */
    uint32_t sl_addr1;
    uint32_t sl_status;         /* SL_STATUS         (0x28) */
    uint32_t sl_int_mask;
    uint32_t sl_int_source;     /* SL_INT_SOURCE     (0x44) */
    uint32_t int_mask;          /* INTERRUPT_MASK    (0x64) */
    uint32_t int_status;        /* INT_SOURCE/STATUS (0x70/0x68) */
    uint32_t slv_packet_status; /* SLV_PACKET_STATUS (0x80) */
    uint32_t slv_rx_len;        /* bytes available in slv_rx (current phase) */
    uint32_t slv_rx_pos;        /* next byte to be read out */
    uint8_t  slv_rx[256];       /* FIFO contents for the current phase */

    /*
     * The NvI2c slave driver consumes a master-write as a two-phase packet: an
     * "address" interrupt delivering the first byte, then a "data" interrupt
     * delivering the remainder.  We stage the whole frame and advance the phase
     * in response to the driver's register writes.
     */
    uint8_t  slv_phase;         /* 0 idle, 1 addr presented, 2 data presented */
    bool     slv_irq_pending;   /* a slave phase is awaiting service */
    uint32_t slv_frame_len;     /* length of the staged frame */
    uint8_t  slv_frame[256];    /* the staged frame */

    /* Remaining registers are accepted but otherwise inert. */
    uint32_t regs[TEGRA30_I2C_IOSIZE / sizeof(uint32_t)];
};

/*
 * Deliver a packet to the controller's slave receive path, as if an external
 * I2C master had written it to this controller's slave address.  Raises the
 * SLV_RX_BUFFER_FILLED interrupt.
 */
void tegra30_i2c_slave_deliver(Tegra30I2CState *s, const uint8_t *buf,
                               uint32_t len);

#endif /* HW_I2C_TEGRA30_I2C_H */
