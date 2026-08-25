include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=qca8337-nss
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca8337-nss
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=QCA8337 fabric re-arm after qca8k teardown
  DEPENDS:=@TARGET_qualcommax_ipq50xx
  FILES:=$(PKG_BUILD_DIR)/qca8337-nss.ko
endef

define KernelPackage/qca8337-nss/description
  Runtime helper for the VLAN-separated NSS data path: after qca8k is
  unbound (it leaves the switch ports disabled, the front PHYs powered
  down and the Atheros CPU header on), wake the PHYs and re-enable the
  fabric with the header off, so the CPU port carries plain 802.1q
  frames the NSS firmware can parse. Loaded on demand, never at boot.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,qca8337-nss))
