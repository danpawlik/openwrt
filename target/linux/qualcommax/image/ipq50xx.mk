define Device/glinet_gl-b3000
  	$(call Device/FactoryImage)
  	$(call Device/FitImage)
  	$(call Device/UbiFit)
	DEVICE_VENDOR := GL.iNET
  	DEVICE_MODEL := GL-B3000
  	KERNEL_LOADADDR := 0x41000000
  	BLOCKSIZE := 128k
  	PAGESIZE := 2048
  	SOC := ipq5018
  	UBINIZE_OPTS := -E 5	# EOD marks to "hide" factory sig at EOF
  	DEVICE_DTS_CONFIG:=config@mp03.5-c1
  	SUPPORTED_DEVICES:=b3000, glinet,gl-b3000
  	IMAGES := sysupgrade.tar nand-factory.img factory.ubi factory.bin
  	IMAGE/sysupgrade.tar := sysupgrade-tar | append-metadata
  	IMAGE/nand-factory.img := append-ubi | qsdk-ipq-factory-nand | append-metadata
  	IMAGE/factory.ubi := append-ubi
  	IMAGE/factory.bin := append-ubi | append-metadata
  	DEVICE_PACKAGES := \
  	ath11k-firmware-qcn6122 \
  	ipq-wifi-glinet_gl-b3000
endef
TARGET_DEVICES += glinet_gl-b3000

define Device/linksys_mx_atlas6
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Linksys
	BLOCKSIZE := 128k
	PAGESIZE := 2048
	KERNEL_SIZE := 8192k
	IMAGE_SIZE := 83968k
	NAND_SIZE := 256m
	SOC := ipq5018
	IMAGES += factory.bin
	IMAGE/factory.bin := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-ubi | linksys-image type=$$$$(DEVICE_MODEL)
endef

define Device/linksys_mx2000
	$(call Device/linksys_ipq50xx_mx_base)
	DEVICE_MODEL := MX2000
	DEVICE_DTS_CONFIG := config@mp03.5-c1
	DEVICE_PACKAGES := ath11k-firmware-qcn6122 \
		ipq-wifi-linksys_mx2000
endef
TARGET_DEVICES += linksys_mx2000

define Device/linksys_mx5500
	$(call Device/linksys_ipq50xx_mx_base)
	DEVICE_MODEL := MX5500
	DEVICE_DTS_CONFIG := config@mp03.1
	DEVICE_PACKAGES := kmod-ath11k-pci \
		ath11k-firmware-qcn9074 \
		ipq-wifi-linksys_mx5500
endef
TARGET_DEVICES += linksys_mx5500
