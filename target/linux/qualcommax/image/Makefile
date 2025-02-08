include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

define Device/Default
	PROFILES := Default
	KERNEL_LOADADDR := 0x41000000
	DEVICE_DTS = $$(SOC)-$(lastword $(subst _, ,$(1)))
	DEVICE_DTS_CONFIG := config@1
	DEVICE_DTS_DIR := $(DTS_DIR)/qcom
	IMAGES := sysupgrade.bin
	IMAGE/sysupgrade.bin = sysupgrade-tar | append-metadata
	IMAGE/sysupgrade.bin/squashfs :=
endef

define Device/FitImage
	KERNEL_SUFFIX := -uImage.itb
	KERNEL = kernel-bin | libdeflate-gzip | fit gzip $$(KDIR)/image-$$(DEVICE_DTS).dtb
	KERNEL_NAME := Image
endef

define Device/FitImageLzma
	KERNEL_SUFFIX := -uImage.itb
	KERNEL = kernel-bin | lzma | fit lzma $$(KDIR)/image-$$(DEVICE_DTS).dtb
	KERNEL_NAME := Image
endef

define Device/EmmcImage
	IMAGES += factory.bin
	IMAGE/factory.bin := append-rootfs | pad-rootfs | pad-to 64k
	IMAGE/sysupgrade.bin/squashfs := append-rootfs | pad-to 64k | sysupgrade-tar rootfs=$$$$@ | append-metadata
endef

define Device/UbiFit
	KERNEL_IN_UBI := 1
	IMAGES += factory.ubi
	IMAGE/factory.ubi := append-ubi
endef
# this will be removed next commit
# better things to come ...
define Build/nand-factory
  $(TOPDIR)/scripts/mkits-glinet-ipq-image.sh \
  	"$(BUILD_DIR)/linux-qualcommax_ipq50xx/tmp" \
	"$(DEVICE_IMG_PREFIX)-squashfs-nand-factory"							
  PATH=$(LINUX_DIR)/scripts/dtc:$(PATH) mkimage -f \
  	"$(BUILD_DIR)/linux-qualcommax_ipq50xx/tmp/flash.its" \
	"$(BUILD_DIR)/linux-qualcommax_ipq50xx/tmp/$(DEVICE_IMG_PREFIX)-squashfs-nand-factory.$1"
endef

include $(SUBTARGET).mk

$(eval $(call BuildImage))
