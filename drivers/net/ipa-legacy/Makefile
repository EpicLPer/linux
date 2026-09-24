# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Qualcomm IPA driver.

IPA_VERSIONS		:=	2.0 2.5 2.6l

obj-$(CONFIG_QCOM_IPA_LEGACY)	+=	ipa-legacy.o

ipa-legacy-y			:=	ipa_main.o ipa_power.o ipa_reg.o ipa_mem.o \
				ipa_table.o ipa_interrupt.o bam.o ipa_reg.o \
				ipa_dma_trans.o ipa_uc.o \
				ipa_endpoint.o ipa_cmd.o ipa_modem.o \
				ipa_qmi.o ipa_qmi_msg.o ipa_sysfs.o

ipa-legacy-y			+=	$(IPA_VERSIONS:%=reg/ipa_reg-v%.o)
ipa-legacy-y			+=	$(IPA_VERSIONS:%=data/ipa_data-v%.o)
