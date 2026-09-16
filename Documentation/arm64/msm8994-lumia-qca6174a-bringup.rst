.. SPDX-License-Identifier: GPL-2.0-only

=========================================================
MSM8994 Lumia: PCIe QCA6174A bring-up notes and handoff
=========================================================

Scope
=====

This documents the state of PCIe (RC1) and the discrete Qualcomm Atheros
QCA6174A (``PCI 168c:003e``) on MSM8994 Microsoft Lumia "octagon" boards
(cityman = 950 XL, talkman = 950), and hands off the work.

Status: **not enumerating under Linux, but enumerates under the vendor
Android 3.10 kernel and Windows on the same unit.** Everything below is
stated so a new engineer can resume without repeating the search.

Verified bring-up sequence (works; matches the vendor stack)
===========================================================

RC1 (``pci@fc528000``), DWC-based ``qcom,pcie-msm8994``. The sequence the
driver performs at ``qcom_pcie_init_msm8994()`` time:

1. Assert PCIe reset (PERST#, TLMM 35) low.
2. Bring up the endpoint power rail (``vddpe-3v3`` = the module's 3.3 V LDO,
   enabled by PM8994 GPIO 9), then drive the WLAN enable line up
   (``WLAN_EN`` on TLMM 113) with a startup delay of at least 10 ms.
3. Program the QMP PHY (20 nm, 19.2 MHz reference table), then the PARF
   registers: ``SYS_CTRL``, ``PHY_CTRL``, ``PHY_REFCLK_CTRL``.
4. Restore the TZ security config (``scm-dev-id``) before touching PARF.
5. Wait for PHY ready, hold at least 10 ms, release PERST#.
6. Enable link training (``PARF_LTSSM`` bit 8), poll ``ELBI_SYS_STTS`` for
   ``XMLH_LINK_UP``.

Reference-clock / reset timing the endpoint requires (device documentation):

* power valid -> ``WLAN_EN`` active:              >= 10 us
* power valid -> ``PCIE_RST_L`` asserted:         >= 10 ms
* ``PCIE_REFCLK`` stable -> ``PCIE_RST_L`` assert: >= 100 us

Rail order the endpoint requires: first ``VDDIO_AO`` and the crystal I/O
rail, then the GPIO I/O rails, then **all 3.3 V rails last**.

Pins (from the device documentation; verify against the board schematic):

* ``PCIE_RST_L`` (145): weak pull-down; the reference design additionally
  requires an **external pull-down at the host**.
* ``PCIE_WAKE_L`` (146): open-drain; needs an **external pull-up**.
* ``PCIE_CLKREQ_L`` (114): open-drain; needs an **external pull-up**.
* ``PCIE_REFCLKP/N`` (4/14): 1.1 V differential, host-supplied.
* ``GPIO6`` (92): boot strap, **must be held high during power-on reset**
  (a debug-mode strap; the module pulls it up on-board).
* 32.768 kHz sleep clock and the chip's own 48 MHz crystal are the chip's
  own; the host supplies only the 100 MHz PCIe reference clock.

What was conclusively ruled out (do not re-test)
================================================

On the failing unit the PCIe link **trains** (Gen1 x1, L0, ``DLLLA=1``,
``PARF LTSSM=0x100``) but configuration reads return ``0xffffffff`` and the
endpoint never appears on the bus; it also never asserts ``PCIE_WAKE_L``.
BT (same QCA6174 die, UART) is equally dead (``hci0: Reading QCA version
information failed (-110)``) **even with PCIe compiled out of the DT**, so
the PCIe bring-up is not what breaks the chip.

Compared register-for-register and action-for-action against the working
vendor kernel and against a live Windows capture, and tested live on the
unit: PHY/PARF/DBI/ELBI/iATU values and write order, power-up rail order and
all PCIe timings, strap/GPIO levels, PERST# two-pulse sequence, config READ
and WRITE, CRS handling, ASPM/L1SS forced to L0, deferred runtime
re-enumeration, PMIC regulator/clock *votes*, and bootloader pre-init. All
were equal to the working stack or had no effect. The RPM/PMIC regulator and
clock votes mainline issues are normal; nothing the endpoint needs is voted
off or low.

Conclusion
==========

Under identical host stimuli the endpoint answers the vendor stack and not
mainline, so the remaining difference is not in the host bring-up code or
any host-visible state. It is inside the chip (its always-on / core-clock
and power domain, or secure/strap state), which is not observable from the
host. Confirming it needs in-circuit measurement of the chip's 48 MHz /
1.1 V / 32 kHz domain, or vendor information. This is recorded here so the
next person does not repeat the register/timing/strap/PMIC work.

Adding a second RC (pcie0) for a different endpoint
==================================================

The driver, QMP PHY driver and DT support both MSM8994 RC0 and RC1; the
same ``qcom,pcie-msm8994`` and ``qcom,msm8994-qmp-pcie-phy`` compatibles
apply. Enable the desired ``pcie@`` node and its ``phy@`` node and supply
``vdda`` / ``vddpe-3v3`` per the board.
