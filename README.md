# U-Boot Airoha-Mediatek

> [!CAUTION]
> **Warning: Flashing custom bootloaders can brick your device. Proceed with caution and at your own risk.**

## About

This repository contains the U‑Boot bootloader for Airoha SoCs, along with the necessary tools and scripts to build and flash it onto supported Airoha devices.  

Click [here](./document/support-devices.md) to view supported devices.

> [!NOTE]
> Please note that this version is maintained exclusively for the OpenWrt UBI layout/Openwrt FIT format, so be sure to select the correct firmware layout when flashing.  

It features an advanced failsafe web recovery interface, including U‑Boot environment variable management, firmware updates, a web console, UBI management, and more.  

To enter recovery mode, use the WPS/Mesh button or the BreedEnter/UbootEnter tool. Once in this mode, the device will automatically obtain an IP address via DHCP, redirect `http://failsafe.lan` to the U‑Boot IP address, and provide Telnet access for debugging.

## Quick Start

- Prepare for ARM

```bash
sudo apt install python3 gcc-aarch64-linux-gnu gcc-arm-linux-gnueabi build-essential flex bison libssl-dev device-tree-compiler qemu-user-static lzma lzma-dev nodejs npm
```

- Prepare for MIPS

```bash
sudo apt install python3 gcc-mips-linux-gnu gcc-mipsel-linux-gnu build-essential flex bison libssl-dev device-tree-compiler qemu-user-static nodejs npm
```

- Clone the repository

```bash
mkdir -p bootloaders && cd bootloaders
git clone <repository_url>
```

- Build U-Boot FIP for AIROHA(ARM)

```bash
cd uboot-airoha-mediatek
SOC=<en7523|an7563|an7581|an7583> BOARD=<board_name> ./airoha.sh
# more help: ./airoha.sh --help
```

> Due to historical reasons, this repository uses the `build_airoha` tool to build the Airoha FIP. The "legacy" binaries are sourced from the OEMs, while the open-source binaries originate from [atf-airoha](https://github.com/Yuzhii0718/atf-airoha). You can choose to build only the U-Boot file and then manually assemble the FIP using other ATF files.

> [!IMPORTANT]
> **Airoha builds need the legacy LZMA SDK encoder (`lzma`).

- Build U-Boot FIP for Mediatek(ARM)

```bash
git clone <atf_mtksoc_url> atf_mtksoc
cd uboot-airoha-mediatek
SOC=<mt7622|mt7981|mt7986|mt7987|mt7988> BOARD=<board_name> ./mediatek.sh
# more help: ./mediatek.sh --help
```

> You can get the `atf_mtksoc_url` from [mkt-openwrt](https://github.com/mtk-openwrt/arm-trusted-firmware) or [Yuzhi's Edition](https://github.com/Yuzhii0718/arm-trusted-firmware-mtksoc).
>
> I'd recommend you to use the `Yuzhi's Edition`, because it is compatible with current automated build scripts, if you choose the official version, you will need to manually compile the ATF.

- Build U-Boot for EcoNet(MIPS)

```bash
git clone <airoha_mips_dramc_url> airoha_mips_dramc
cd uboot-airoha-mediatek
SOC=<en7512|en7516|en7528|en7580> BOARD=<board_name> ./econet.sh
# more help: ./econet.sh --help
```

> Econet needs a chainload payload, just u-boot.bin is not flashable. so you need to concatenate the payload files from the vendor or use the build artifacts from the [airoha_mips_dramc](https://github.com/Sirherobrine23/airoha_mips_dramc) project.

- Build U-Boot for MTMIPS(MIPS)

```bash
SOC=<mt7620|mt7621|mt7628|mt7688> BOARD=<board_name> ./mtmips.sh
# more help: ./mtmips.sh --help
```

> [!NOTE]
> Output files will be located in the `output_<platform>` directory.

## Acknowledgement

- [u-boot](https://github.com/u-boot/u-boot)
- [mtk-openwrt](https://github.com/mtk-openwrt)
- [OpenWrt](https://github.com/openwrt/openwrt)
- [atf-airoha](https://github.com/Ansuel/atf-airoha/)
- [Sirherobrine23](https://github.com/Sirherobrine23/)
