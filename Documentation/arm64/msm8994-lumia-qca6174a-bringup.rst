.. SPDX-License-Identifier: GPL-2.0-only

==========================================================
MSM8994 Lumia: PCIe RC1 and the QCA6174A combo chip
==========================================================

This describes the PCIe (RC1) controller and the discrete Qualcomm
Atheros QCA6174A (``PCI 168c:003e``) on MSM8994 Microsoft Lumia
"octagon" boards (cityman = 950 XL, talkman = 950): the WiFi part is a
PCIe endpoint, the Bluetooth part is a separate UART-attached function
of the same die.

Hardware description
====================

RC1 (``pci@fc528000``) is a DWC-based controller described by the
``qcom,pcie-msm8994`` compatible. The endpoint is powered from a module
3.3 V LDO (``vddpe-3v3``) and a separate ``WLAN_EN`` enable line; its
1.8 V I/O rails are always-on. A 100 MHz reference clock is supplied by
the host; the chip's own crystal and sleep clock are internal.

Bring-up sequence performed by the driver at
``qcom_pcie_init_msm8994()`` time:

1. Assert PERST# (TLMM 35) low.
2. Power the endpoint rail (``vddpe-3v3``), then drive ``WLAN_EN``
   (TLMM 113) high while PERST# is still asserted.
3. Toggle the PCIe1 block reset (``GCC_PCIE_1_BCR``). The board firmware
   does this as part of its PCIe power-up; without it the PHY trains the
   link but the endpoint never answers configuration space.
4. Program the QMP PHY (20 nm, 19.2 MHz reference) and the PARF
   registers (``SYS_CTRL``, ``PHY_CTRL``, ``PHY_REFCLK_CTRL``), restoring
   the TZ security config (``scm-dev-id``) first.
5. Release PERST#, enable link training (``PARF_LTSSM`` bit 8) and poll
   ``ELBI_SYS_STTS`` for ``XMLH_LINK_UP``.

Firmware
========

The QCA6174A firmware is supplied from userspace (``linux-firmware``),
not by the kernel:

* Bluetooth: ``qca/rampatch_00440302.bin``, ``qca/nvm_00440302.bin``
* WiFi:      ``ath10k/QCA6174/hw3.0/{firmware-6.bin,board-2.bin}``

Notes
=====

The Bluetooth UART (``blsp2_uart2``) intentionally has no ``dmas``
properties: attaching the BLSP2 DMA channels to that UART holds the
QCA6174 in reset, which prevents both the Bluetooth and PCIe functions
from coming up.

Adding a second RC (pcie0) for a different endpoint
===================================================

The driver, QMP PHY driver and DT support both MSM8994 RC0 and RC1; the
same ``qcom,pcie-msm8994`` and ``qcom,msm8994-qmp-pcie-phy`` compatibles
apply. Enable the desired ``pcie@`` node and its ``phy@`` node and supply
``vdda`` / ``vddpe-3v3`` per the board.
