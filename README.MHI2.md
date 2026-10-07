# MHI2 emulator fork

This experimental fork adds the MHI2 Tegra3 machine, RCC/IOC services,
host graphics/audio bridges, MOST cluster transport and Android USB fixtures.
The primary branch is `main`. Start with the
[Audi, Volkswagen and Porsche macOS guide](README.md), which supports source
ZIPs and shallow clones without a head unit, Pico or QNX SDK.

`main` is based on official QEMU, with a linear MHI2-only patch series.
Upstream ROM and test submodule entries are retained. Build dependencies are
fetched at pinned revisions rather than committed as third-party snapshots.
See [history reconstruction and validation](tools/mhi2/CONSOLIDATION.md).

This fork includes AI-assisted experiments and is not intended as an upstream
QEMU contribution. Existing copyright and license notices remain applicable.
Firmware updates, navigation maps and captured boot data are separate assets,
not covered by the emulator source license.

Some QNX launch scripts still default to the original workstation's media paths.
The source snapshot includes those scripts and their documented overrides;
it does not include the large map database or all intermediate emulator disks.

Linux bring-up additionally exercises native PCIe RCC Ethernet (DMA/MSI),
all five I2C masters, all three EHCI controllers, read-only NOR, GPIO and RTC.
The `pmic=tps65911` and `pmic=max20024` machine options expose experimental
power-register models. Their OTP defaults and interrupt connection are test
fixtures, not production board wiring. The default `pmic=legacy` and 13 MHz
oscillator retain the previous QNX configuration. Linux test launchers select
`-global tegra30-clk.oscillator-12mhz=on` explicitly for the current PLLE table.
These Linux fixtures are separate from the QNX boot workflow in the main guide.
