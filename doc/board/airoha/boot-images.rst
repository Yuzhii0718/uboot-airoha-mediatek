.. SPDX-License-Identifier: GPL-2.0+

Airoha boot images
==================

The Airoha port (AN7563, AN7581, AN7583, EN7523, EN7562) combines prebuilt
BL1/BL2/BL31 binaries with the U-Boot binary that has just been compiled into a
number of different boot images.  They differ only in how the same components
are packaged:

* The **legacy** layout (``bl1-bl2-bl31-uboot.bin`` with BL1,
  ``bl2-bl31-uboot.bin`` without it) is one self-contained 512 KiB flash image
  that carries the complete boot chain.
* The **modern split FIP** layout ships BL2 and BL31+U-Boot as two separate
  images.
* **chainloader** images are U-Boot payloads that are loaded by an already
  running bootloader.
* **bootext.ram** is a minimal recovery image used by BL2.

All of them are produced by ``tools/build_airoha/Makefile``, which the top level
Makefile invokes automatically when ``CONFIG_ARCH_AIROHA`` is set.  The output
file names are chosen to match the OpenWrt artifacts so that the images can be
dropped into an OpenWrt image builder without renaming.

Boot chain
----------

Airoha SoCs boot as follows::

    BootROM (mask ROM)
      +- BL1        first stage, only present in the legacy layout with BL1
           +- BL2   TF-A preloader: DRAM + SPI-NAND init, LZMA decompressor,
                    certificate handling, dispatch of BL31/BL33
                +- BL31    EL3 runtime firmware (LZMA compressed)
                     +- BL33   U-Boot proper (LZMA compressed)

BL2, BL31 and U-Boot are always shipped inside a FIP (Firmware Image Package)
and are located through its table of contents; BL1 is a flat binary that is
prepended to the FIP in the legacy layout.  BL31 and U-Boot are always LZMA
compressed, because BL2 decompresses them into DRAM before starting them.

The FIP container
-----------------

The FIP is the TF-A Firmware Image Package as understood by the Airoha BootROM
and BL2.  It is created with ``tools/fiptool`` (built from
``tools/fiptool_src``)::

    fiptool create --align 1024 --tb-fw bl2.bin --soc-fw bl31.lzma \
                   --nt-fw u-boot.bin.lzma <output>

Its layout is:

.. code-block:: text

    +---------------------------+  offset 0
    | header: 16 bytes          |  magic u32 le = 0xAA640001
    |                           |  serial u32 le = 0x12345678
    |                           |  flags  u64   = 0
    +---------------------------+  offset 16
    | ToC entry: 40 bytes       |  uuid(16) + offset(8) + size(8) + flags(8)
    | ... one per payload ...   |  offset/size are relative to the FIP start
    +---------------------------+
    | terminating entry         |  all-zero uuid, 40 bytes
    +---------------------------+
    | payload BL2               |  aligned to --align (1024 here)
    | payload BL31              |
    | payload BL33 (U-Boot)     |
    +---------------------------+

Payloads are selected by UUID.  The UUID is stored as raw bytes, i.e. in this
byte order:

================================  ==========================================  =====================
Payload                           UUID (on-disk byte order)                   fiptool option
================================  ==========================================  =====================
BL2 (trusted boot FW)             5ff9ec0b-4d22-3e4d-a544-c39d81c73f0a        ``--tb-fw``
BL31 (EL3 runtime FW)             47d4086d-4cfe-9846-9b95-2950cbbd5a00        ``--soc-fw``
BL33 / U-Boot (non-trusted FW)    d6d0eea7-fcea-d54b-9782-9934f234b6e4        ``--nt-fw``
================================  ==========================================  =====================

With ``--align 1024`` the table of contents occupies the first 1024 bytes and
the first payload starts at FIP offset 0x400.  Inside a 512 KiB flash image,
where the FIP begins at 0x800, this puts BL2 at 0xC00 -- the offset the legacy
images use.

Image variants
--------------

512 KiB boot image (CONFIG_AIROHA_BUILD_LEGACY)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The legacy self-contained Airoha boot image: one 512 KiB flash image holding
the whole boot chain -- a 2 KiB prefix followed by a single internal FIP.  It
can be built on every Airoha platform.  ``CONFIG_AIROHA_LEGACY_BL1`` decides
what the prefix is:

* enabled (default on EN7523) -- BL1 is prepended and the artifact is
  ``bl1-bl2-bl31-uboot.bin``,
* disabled -- the prefix stays zero and the artifact is
  ``bl2-bl31-uboot.bin``, i.e. exactly the same image without BL1.  This is
  what SoCs whose BootROM loads the FIP directly use (AN7563).

.. code-block:: text

    0x00000  BL1, truncated to 0x800 bytes, or 2 KiB of zeros without BL1
             (the 2 KiB prefix region is always reserved)
    0x00800  internal FIP (BL2 + BL31.lzma + U-Boot.lzma, --align 1024)
    ...      zero padding up to the end of the container
    0x80000  end of image (512 KiB)

There is no environment region in this layout: everything after the FIP is
padding, and the FIP is free to grow up to 0x80000 - 0x800.  The classic SDK
bootloader image did reserve an optional 16 KiB environment tail at 0x7C000 (see
``tools/airoha_pack_tcboot_legacy.py``, ``--env`` / ``--no-env``), but this
packer never writes one.  Where U-Boot keeps its environment is decided by the
U-Boot configuration alone -- in the Airoha defconfigs it lives in the UBI
volumes ``ubootenv`` / ``ubootenv2``, not in the boot image.

The image is assembled by ``tools/airoha_pack_boot.sh``, which reports how much
of the container the image really uses, for example::

    bootimg:     bl1-bl2-bl31-uboot.bin (524288 bytes, 0x80000, FIP @0x800)
    bl1:         0x00000  0x7e0
    fip:         0x00800  0x76000
    used:        0x00076800  485376 bytes (474 KiB) of 512 KiB  [92.5%]
    padding:     0x00009800   38912 bytes (38 KiB) zero-filled  [7.5%]

OpenWrt names: ``bl1-bl2-bl31-uboot.bin`` (with BL1, the
upstream name for this image) and ``bl2-bl31-uboot.bin`` (without BL1); the
vendor SDK calls the same image ``mtd0-bootloader.img``.

preloader.bin and bl31-uboot.fip (modern split FIP layout)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The layout used for AN7581 / AN7583 (``CONFIG_AIROHA_BUILD_MODERN``).  BL2 is
shipped on its own so that it can be upgraded independently of BL31+U-Boot:

* ``preloader.bin`` -- a FIP with a single BL2 entry, created with
  ``fiptool create --tb-fw bl2.bin`` (also produced by the legacy build, see
  ``CONFIG_AIROHA_PRELOADER`` below).
* ``bl31-uboot.fip`` -- a FIP with BL31 (LZMA) and U-Boot (LZMA), created with
  ``fiptool create --soc-fw bl31.lzma --nt-fw u-boot.bin.lzma``.

Neither image has a fixed size or a prefix; both are exactly as large as their
contents.  The old ``bl2.fip`` / ``u-boot.fip`` spellings are kept as Makefile
aliases for ``preloader.bin`` / ``bl31-uboot.fip``.

OpenWrt names: ``openwrt-airoha-<soc>-<board>-ubi-preloader.bin`` and
``openwrt-airoha-<soc>-<board>-ubi-bl31-uboot.fip``.

preloader.bin (CONFIG_AIROHA_PRELOADER)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A standalone FIP that contains only BL2, built by a single switch that applies
to both boot image layouts (``CONFIG_AIROHA_BUILD_MODERN`` and
``CONFIG_AIROHA_BUILD_LEGACY``):

.. code-block:: text

    fiptool create --tb-fw bl2.bin preloader.bin                 # modern/split
    fiptool create --align 1024 --tb-fw bl2.bin preloader.bin    # legacy

The legacy 512 KiB layout (with or without BL1) adds ``--align 1024`` so that
BL2 starts at 0xC00 -- exactly where it sits inside the container (2 KiB prefix
plus the FIP at 0x800).  The modern split layout keeps the plain OpenWrt
container.

What the image is used for depends on the layout:

* modern split FIP (AN7581 / AN7583): the BL2 half of the boot chain, flashed
  to the ``bl2`` MTD partition (no alignment),
* legacy: ``bl1-bl2-bl31-uboot.bin`` / ``bl2-bl31-uboot.bin`` already embed BL2
  in their internal FIP, so this image is not needed to boot.  It serves the
  same purpose as ``bootext.ram`` and only differs in the packaging: BL2 inside
  a FIP container instead of the bare preloader with the trailing CRC32.  Like
  ``bootext.ram`` it is not installed into the ``bl2`` MTD partition.

bl1-preloader.bin (CONFIG_AIROHA_PRELOADER_BL1)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A flat image for the SoCs that do not boot a bare FIP (EN7523, default on for
them): the 2 KiB BL1 region followed by the very same BL2 FIP as
``preloader.bin``.  It belongs to the modern split layout
(``CONFIG_AIROHA_BUILD_MODERN``); the legacy 512 KiB images carry their own
BL1/prefix and use ``preloader.bin`` as is.

.. code-block:: text

    0x00000  BL1 from the blob set, padded/truncated to 0x800
    0x00800  BL2 FIP (a verbatim copy of preloader.bin)

Built by ``tools/build_airoha/Makefile`` right after ``preloader.bin``:

.. code-block:: sh

    cat <bl1.bin> > bl1-preloader.bin
    truncate -s 2048 bl1-preloader.bin
    cat preloader.bin >> bl1-preloader.bin

The blob set has to provide ``bl1.bin``.

chainloader images (CONFIG_AIROHA_BUILD_CHAINLOADER)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A chainloader lets a board boot a second U-Boot from its own flash partition,
without touching the primary boot chain.  Two build methods exist:

**OpenWrt method** (default, ``CONFIG_AIROHA_CHAINLOADER_METHOD_OPENWRT``)
  A plain FIT built exactly like the OpenWrt ``Build/an7581-chainloader`` step:
  kernel = ``u-boot.bin.lzma`` (fallback: the uncompressed ``u-boot.bin``) and
  fdt = ``u-boot.dtb``, generated with ``tools/mkits.sh`` + ``mkimage``.
  Load/entry address 0x80200000, FDT at 0x82000000.  Output:
  ``<board>-chainload-uboot.itb``.

**Shim method** (``CONFIG_AIROHA_CHAINLOADER_METHOD_SHIM``)
  The legacy Airoha method, packed from per-board sources in
  ``tools/build_airoha/chainloader/<soc>_<board>/`` (``shim.bin``,
  ``control.dts``, ``chainloader.its``).  Outputs:

  * ``<board>-chainloader-prefix-shim.uImage`` -- LZMA-compressed shim in a
    legacy uImage whose ``ih_os`` byte is patched to U-Boot (0x03),
  * ``<board>-chainloader-control.dtb`` -- compiled control DTB,
  * ``<board>-chainloader.itb`` -- FIT with control DTB, shim and payload,
  * ``<board>-chainloader-slot.bin`` -- slot image: prefix shim at 0x0, FIT at
    0x2100.

OpenWrt names: ``<board>-chainload-uboot.itb`` (OpenWrt method),
``<board>-chainloader.itb`` / ``<board>-chainloader-slot.bin`` (shim method).

bootext.ram (CONFIG_AIROHA_BOOTEXT_RAM)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A minimal FIP used by BL2 for XMODEM firmware recovery ("Press x to update
firmware"): BL2 only, optionally followed by the TF-A trusted boot
certificates extracted from ``certificates.bin``
(``CONFIG_AIROHA_BOOTEXT_EMBED_CERTS``, count set by
``CONFIG_AIROHA_BOOTEXT_CERT_COUNT`` -- 6 for AN7563/AN7581/AN7583, 1
otherwise).  Packed by ``tools/airoha_pack_bootext.py``.

u-boot.bin.lzma
~~~~~~~~~~~~~~~~~~~~

When neither ``CONFIG_AIROHA_BUILD_MODERN`` nor ``CONFIG_AIROHA_BUILD_LEGACY`` is
set, the build simply produces ``u-boot.bin.lzma`` and leaves the packaging to
an external build system.

Summary
-------

======================  ==============================  ==========================================
Image                   Selected by                     OpenWrt artifact name
======================  ==============================  ==========================================
``bl1-bl2-bl31-uboot.bin``  ``AIROHA_BUILD_LEGACY``     ``<soc>-<board>-bl1-bl2-bl31-uboot.bin``
                        + ``AIROHA_LEGACY_BL1``
``bl2-bl31-uboot.bin``  ``AIROHA_BUILD_LEGACY``         ``<soc>-<board>-bl2-bl31-uboot.bin``
                        without ``AIROHA_LEGACY_BL1``
``preloader.bin``       ``AIROHA_PRELOADER``            ``<soc>-<board>-ubi-preloader.bin``
``bl1-preloader.bin``   ``AIROHA_PRELOADER_BL1``        ``<soc>-<board>-bl1-preloader.bin``
``bl31-uboot.fip``      ``AIROHA_BUILD_MODERN``            ``<soc>-<board>-ubi-bl31-uboot.fip``
chainloader images      ``AIROHA_BUILD_CHAINLOADER``    ``<board>-chainload-uboot.itb``
``bootext.ram``         ``AIROHA_BOOTEXT_RAM``          ``<soc>-bootext.ram``
======================  ==============================  ==========================================

Building
--------

The images are built as part of the normal build::

    make <soc>_<board>_defconfig
    make

``make`` alone uses whichever blob source ``.config`` selected; with the default
``source`` layout the blobs have to be staged first, so use the ``airoha.sh``
wrapper, which also collects the results into
``output_airoha/<soc>_<board>-<image>`` together with their checksums::

    SOC=an7581 BOARD=evb ./airoha.sh                     # ATF + U-Boot + FIP
    SOC=an7581 BOARD=evb STAGE=atf ./airoha.sh           # stage the ATF blobs
    SOC=an7581 BOARD=evb BLOBS=opensource ./airoha.sh    # checked-in blobs

The tools involved are:

================================================  ==================================================
Tool                                              Purpose
================================================  ==================================================
``tools/fiptool_src``                             builds ``fiptool`` (TF-A FIP packer)
``tools/airoha_pack_boot.sh``                     prefix + FIP -> 512 KiB legacy boot image
``tools/airoha_pack_chainloader_openwrt.sh``      OpenWrt-style chainload FIT
``tools/airoha_pack_chainloader_shim.sh``         shim-based chainloader images
``tools/airoha_pack_bootext.py``                  bootext.ram recovery FIP
``tools/airoha_extract_certificates.py``          splits certificates.bin into DER certificates
``tools/airoha_info_preloader.py``                prints BL2 version information
``tools/airoha_info_bl31.py``                     prints BL31 version information
``tools/build_airoha/gen_build_info_log.py``      writes firmware_build_log.txt for the blobs
``tools/build_airoha/update_bl2_flash_table.py``  patches the SPI-NAND flash table inside BL2
================================================  ==================================================

Note that ``u-boot.bin`` is compressed with the LZMA SDK encoder (``lzma -c``)
in preference to ``xz``: BL2 needs the real uncompressed size in the 8-byte
LZMA-Alone header field, which ``xz`` leaves unset.

Bootloader binaries
-------------------

BL1, BL2 and BL31 come from one of three sources, selected by the
``CONFIG_AIROHA_BLOBS_*`` choice in ``board/airoha/Kconfig``:

``CONFIG_AIROHA_BLOBS_SOURCE`` (default for the modern build)
  Build them from the ATF source tree.  ``airoha.sh`` compiles ``../atf-airoha``
  (override the location with ``ATF_DIR=<path>``) and stages the artifacts under
  ``tools/build_airoha/source/<variant>/`` before
  ``tools/build_airoha/Makefile`` packs the FIP, so the packaged bootloader
  always matches the source tree it was built from::

      SOC=an7581 BOARD=evb ./airoha.sh             # ATF + U-Boot + FIP
      SOC=an7581 BOARD=evb STAGE=atf ./airoha.sh   # stage the ATF blobs only
      SOC=an7581 BOARD=evb STAGE=uboot ./airoha.sh # reuse the staged blobs

  ``source/`` is generated, not checked in, and a build that cannot find it
  fails with the commands above.  The ATF platform is taken from the resolved
  ``CONFIG_SYS_SOC``.  The ATF platforms are named after the whole SoC family,
  and a family covers several package variants
  (``include/soc/airoha/pkgids.h``)::

      EN7523 family   en7523 / en7529 / en7562           -> atf en7523
      EN7581 family   an7581 / an7566 / an7551           -> atf an7581
      AN7583 family   an7583 / an9510 / an7553 / an7567  -> atf an7583
      AN7552 family   an7552 / an7563                    -> atf an7552

  The variants already collapse before the mapping runs
  (``arch/arm/mach-airoha/Kconfig`` gives ``CONFIG_SYS_SOC`` one default per
  ``TARGET_*`` symbol), so the EN7562 boards select ``CONFIG_TARGET_EN7523`` and
  build the EN7523 binaries; the package ID read at runtime selects the exact
  variant.  AN7563 is the only family member that keeps its own name in
  ``.config`` while being built from the AN7552 platform.

  This needs the AArch32/AArch64 toolchains and the ATF tree; the ATF
  ``build.sh`` fetches its own host archives when they are missing.

``CONFIG_AIROHA_BLOBS_OPENSOURCE`` (deprecated)
  Prebuilt open-source binaries from ``opensource/<variant>/``: ``bl2.bin``,
  ``bl31.lzma``.  Superseded by ``CONFIG_AIROHA_BLOBS_SOURCE``; kept for one
  transition release to reproduce a firmware that was still packaged from the
  checked-in blobs.

``CONFIG_AIROHA_BLOBS_LEGACY`` (default for the legacy build)
  SDK binaries from ``legacy/<variant>/``: ``bl1.bin``, ``bl2.bin``,
  ``bl31.lzma``, ``certificates.bin``, ``key_area.bin``.  Only these carry the
  certificates and key area, so they are the only ones that can build a legacy
  image with BL1 or a ``bootext.ram`` with certificates.

``<variant>`` defaults to ``<soc>_default`` and can be overridden per board with
``CONFIG_AIROHA_FIP_BLOBS_DIR``.  Any build can override the selection for a
single run with ``BLOBS=source|opensource|legacy`` -- e.g. ``BLOBS=opensource``
on a machine without the ATF tree.

Every blob is structure-checked right before it is packed (``_validate_bl2`` /
``_validate_bl31`` in ``tools/build_airoha/Makefile``); a corrupted or truncated
image aborts the build instead of producing a broken FIP.

Flash placement
---------------

======================  ==============================================
Image                   Where it is installed
======================  ==============================================
``bl1-bl2-bl31-uboot.bin``  ``u-boot`` MTD partition, offset 0
``bl2-bl31-uboot.bin``  ``u-boot`` MTD partition, offset 0
``preloader.bin``       ``bl2`` MTD partition
``bl31-uboot.fip``      ``fip`` static UBI volume
chainloader images      ``chainloader`` MTD partition
``bootext.ram``         loaded by BL2 over XMODEM, not flashed
======================  ==============================================
