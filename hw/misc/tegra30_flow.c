#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"
#include "accel/tcg/cpu-loop.h"
#include "system/address-spaces.h"
#include "target/arm/arm-powerctl.h"
#include "hw/misc/tegra30_flow.h"

/*
 * nVidia Tegra Flow Controller (partial model).
 *
 * On Tegra the boot ROM and first-stage bootloader run on the AVP (a.k.a.
 * COP) processor.  After setting up clocks and programming the main CPU
 * reset vector in EVP_CPU_RESET_VECTOR (0x6000F100) the AVP starts the
 * Cortex-A9 CPU complex and then halts itself by writing FLOW_MODE_WAITEVENT
 * with no wake source (value 0x40000000) to FLOW_CTLR_HALT_COP_EVENTS.
 *
 * We model this hand-off here: when the AVP halts the COP we bring the main
 * Cortex-A9 (CPU0) out of reset at the programmed CPU reset vector and then
 * halt the AVP core that issued the write.
 */

/* Register offsets */
enum {
    REG_FLOW_CTLR_HALT_CPU_EVENTS = 0x000,
    REG_FLOW_CTLR_HALT_COP_EVENTS = 0x004,
};

/*
 * HALT_*_EVENTS mode is held in bits [31:29]; mode 2 is WAITEVENT/STOP.
 * A value of exactly 0x40000000 is WAITEVENT with no wake event and a zero
 * delay count, i.e. an unconditional, permanent halt.  The firmware also
 * pokes HALT_COP_EVENTS with 0x42xxxxxx (the usec bit plus a countdown) to
 * implement busy-wait delays; those must NOT trigger the hand-off.
 */
#define FLOW_HALT_PERMANENT    0x40000000

/* Where the AVP firmware stores the main CPU entry point (EVP block). */
#define EVP_CPU_RESET_VECTOR   0x6000F100

static uint64_t tegra30_flow_read(void *opaque, hwaddr offset, unsigned size)
{
    /* No status bits are modelled; event/CSR reads return 0. */
    return 0;
}

static void tegra30_flow_write(void *opaque, hwaddr offset,
                               uint64_t val, unsigned size)
{
    Tegra30FlowState *s = TEGRA30_FLOW(opaque);

    if (offset == REG_FLOW_CTLR_HALT_COP_EVENTS &&
        val == FLOW_HALT_PERMANENT && !s->handed_off) {
        CPUState *avp = current_cpu;
        uint32_t vec = address_space_ldl_le(&address_space_memory,
                                            EVP_CPU_RESET_VECTOR,
                                            MEMTXATTRS_UNSPECIFIED, NULL);

        if (avp != NULL && vec != 0) {
            s->handed_off = true;
            qemu_log_mask(LOG_UNIMP, "%s: COP halted, starting CPU0 at reset "
                          "vector 0x%08x and halting the AVP\n",
                          __func__, vec);

            /* Bring the main Cortex-A9 (CPU0) out of reset at the vector. */
            arm_set_cpu_on(0, vec, 0, 1 /* EL1 */, false /* AArch32 */);

            /* Halt the AVP core that issued the COP halt. */
            avp->halted = 1;
            avp->exception_index = EXCP_HLT;
            cpu_loop_exit(avp); /* does not return */
        }
    }

    /* Delay events, CPU halt/unhalt, CSR writes: accepted but not modelled. */
}

static const MemoryRegionOps tegra30_flow_ops = {
    .read = tegra30_flow_read,
    .write = tegra30_flow_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_flow_reset(DeviceState *dev)
{
    Tegra30FlowState *s = TEGRA30_FLOW(dev);

    s->handed_off = false;
}

static void tegra30_flow_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30FlowState *s = TEGRA30_FLOW(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &tegra30_flow_ops, s,
                          TYPE_TEGRA30_FLOW, TEGRA30_FLOW_IOSIZE);
    /*
     * The COP->CPU hand-off path in the write handler escapes the I/O
     * dispatch with cpu_loop_exit(), which never returns and therefore never
     * releases QEMU's per-device I/O reentrancy guard.  That would leave the
     * flow controller permanently "engaged" and make every later register
     * access fault with MEMTX_ACCESS_ERROR.  Opt out of the guard.
     */
    s->iomem.disable_reentrancy_guard = true;
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription tegra30_flow_vmstate = {
    .name = "tegra30-flow",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(handed_off, Tegra30FlowState),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_flow_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_flow_reset);
    dc->vmsd = &tegra30_flow_vmstate;
}

static const TypeInfo tegra30_flow_info = {
    .name          = TYPE_TEGRA30_FLOW,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_flow_init,
    .instance_size = sizeof(Tegra30FlowState),
    .class_init    = tegra30_flow_class_init,
};

static void tegra30_flow_register(void)
{
    type_register_static(&tegra30_flow_info);
}

type_init(tegra30_flow_register)
