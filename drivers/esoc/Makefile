# SPDX-License-Identifier: GPL-2.0-only
# generic  external soc control support

ccflags-$(CONFIG_ESOC_DEBUG)	:= -DDEBUG
obj-$(CONFIG_ESOC)	+= esoc_bus.o
obj-$(CONFIG_ESOC_DEV)	+= esoc_dev.o
obj-$(CONFIG_ESOC_CLIENT)	+= esoc_client.o
obj-$(CONFIG_ESOC_MDM_4x)	+= esoc_mdm_4x.o
esoc_mdm_4x-objs += esoc-mdm-pon.o esoc-mdm-4x.o
obj-$(CONFIG_ESOC_MDM_DRV)	+= esoc-mdm-drv.o
obj-$(CONFIG_ESOC_MDM_DBG_ENG)	+= esoc-mdm-dbg-eng.o
