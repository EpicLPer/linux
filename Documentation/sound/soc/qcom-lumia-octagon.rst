.. SPDX-License-Identifier: GPL-2.0

===================================================
Microsoft Lumia 950 family (octagon) audio
===================================================

Octagon is Microsoft's board name for both the Lumia 950 (Talkman,
MSM8992) and the Lumia 950 XL (Cityman, MSM8994). This note describes
what this kernel tree implements. It applies to any userspace that
talks ALSA to the card (UCM, PulseAudio, PipeWire, TinyALSA). It is
not specific to one distribution.

Hardware
========

::

  ADSP (LPASS)
    ├── SLIMBUS_0_RX  → WCD9330 (tomtom) → 3.5 mm jack
    └── QUATERNARY MI2S → TAS2553        → loudspeaker

Cityman firmware is ``qcadsp8994.mbn``. Talkman firmware is
``qcadsp8992.mbn``. Do not load the other phone's ADSP blob.

The WCD9330 slim enumeration is ``slim217,130`` (PGD and IFD).
WCD9335 (Tasha, ``slim217,1a0``) is a different part and must not
be bound here.

The 3.5 mm jack insert switch on this board is inverted relative
to the codec's default comparator (``qcom,mbhc-insert-detect-inverted``).

What works
==========

* ADSP remoteproc (``qcom,msm8994-adsp-pil``)
* Loudspeaker playback over QUAT MI2S (48 kHz, S16)
* Volume keys
* SLIMbus NGD after ADSP is running (register-mode SAT, then MSGQ)
* WCD9330 bind, chip id, 9.6 MHz MCLK
* Headset jack insert and remove
* Analog headphone path: HPH PAs unmute (a start pop)

The ALSA card name is still ``cityman-speaker``. Routing between
speaker and jack is userspace (the ``SLIMBUS_0_RX`` / ``QUAT_MI2S_RX``
mixers).

What does not work
==================

Headphone playback is silent after the start pop. PCM ``hw_ptr``
advances and the DAPM path to ``HEADPHONE`` is on. The WCD interface
device RX ports 16/17 stay at overflow | PORT_CLOSED after the
satellite define / connect / activate sequence matches the 3.10
tomtom path (``DEF_ACT_CHAN`` 5-bit client id, ``CONNECT_SINK``,
``RECONFIG_NOW``). ADSP acknowledges those messages but does not
keep an active IFD-to-PGD drain.

This is not a mixer-volume, digital-mute, or analog-gain problem.
The remaining hole is ADSP-side slim manager state for
``SLIMBUS_0_RX``.

Bring-up
========

Mapping the NGD MMIO at probe reboots this SoC. The driver probes
without attaching the controller. After ADSP is up, write ``1`` to
``ngd_hw`` then ``ngd_up`` on the slim-ngd platform device (sysfs).

Sequences and AFE CDC register maps were ported from the 3.10
``wcd9330`` / ``wcd9xxx`` / ``q6afe`` sources. The NGD
``DEF_ACT_CHAN`` client-id mask matches ``slim-msm-ngd.c``.

Assistance
==========

Portions of this bring-up were produced with an LLM coding assistant.
See Documentation/process/coding-assistants.rst. Commits in this
series use an ``Assisted-by: LLM`` tag. They do not carry a
machine ``Signed-off-by``.
