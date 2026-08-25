include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=qca-dwmac-nss
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-dwmac-nss
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS data-plane glue for the IPQ5018 dwmac stack
  DEPENDS:=@TARGET_qualcommax_ipq50xx
  FILES:=$(PKG_BUILD_DIR)/qca-dwmac-nss.ko
endef

define KernelPackage/qca-dwmac-nss/description
  Implements the nss-dp data-plane override API on top of the dwmac-ipq5018
  ethernet stack, so qca-nss-drv can attach the NSS firmware fast path on
  IPQ5018, which has neither the PPE nor the EDMA that qca-ppe-nss assumes.
  Arming a GMAC (echo 2 > /sys/kernel/debug/qca-dwmac-nss/fw_mask, then
  loading qca-nss-drv) parks the host DMA and hands the controller to the
  firmware; DSA on the external switch stays transparent.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/qca-ppe-nss
	$(CP) $(PKG_BUILD_DIR)/exports/nss_dp_api_if.h $(1)/usr/include/qca-ppe-nss/
endef

$(eval $(call KernelPackage,qca-dwmac-nss))
