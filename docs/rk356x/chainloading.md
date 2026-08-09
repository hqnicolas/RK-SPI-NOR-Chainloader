# RK356x BL31/U-Boot chainloading

The optional chainloader turns the minimal RK firmware into a practical first
stage without teaching it PCIe, NVMe, filesystems, Linux boot, or EFI. U-Boot
owns those jobs:

```text
RK3568 BootROM -> Rockchip DDR blob -> board chainloader -> BL31 -> U-Boot EL2
       ^ SPI NOR, eMMC, SD, or MaskROM USB                    -> board-scoped OS targets
                                                               -> Linux or EFI app
```

This is a board-scoped variant. Normal `yy3568.bin`, `rock3a.bin`, and demo
images keep their FUEFI payload behavior. ROC3566, RK3399, and RK3588 targets
do not inherit RK3568 storage, address, GPIO, or U-Boot policy.

## Supported chainloader boards

| `BOARD` | U-Boot backend | Source pin | BL33 load / stack limit | Automatic OS targets |
| --- | --- | --- | --- | --- |
| [`yy3568`](boards/yy3568.md) | official mainline FIT | v2026.07, `ece349ade2973e220f524ce59e59711cc919263f` | `0x00800000` / `0x03f00000` | `mmc1` -> `nvme` -> `mmc0` -> `scsi` -> `usb` -> `pxe` -> `dhcp` |
| [`rock3a`](boards/rock3a.md) | official mainline FIT | v2026.04, `88dc2788777babfd6322fa655df549a019aa1e69` | `0x00800000` / `0x03f00000` | `nvme` -> `mmc1` -> `usb` -> `mmc0` |

Both use Rockchip BL31 v1.46 from rkbin commit
`ecb4fcbe954edf38b3ae037d5de6d9f5bccf81f4` and official mainline U-Boot.
ROCK 3A uses the upstream `rock-3a-rk3568_defconfig` and DTS at v2026.04.
YY3568 uses v2026.07 with board support derived from Armbian commit
`a710f6715cc06fc90dfdd69fb93d642c52f3a3b8`. Each board keeps an isolated
overlay, address contract, and boot policy.

## Building one board

Use the generic interface and always name the board:

```sh
make chainload BOARD=rock3a
make chainload-check BOARD=rock3a
make usb-chainload BOARD=rock3a
```

Replace `rock3a` with `yy3568` for that board. `usb3568-uboot` remains a
deprecated YY3568 compatibility alias; new scripts should use the qualified
command.

Each build creates:

- `<board>-u-boot.itb`: U-Boot, its control DTB, and split BL31 segments.
- `uboot_<board>.bin`: dedicated first stage followed by that FIT.
- `uboot_<board>.img`: whole-SD image with its ID block at LBA `0x40`.
- `uboot_<board>_idbloader.img`: raw RKNS v2 ID block for eMMC LBA `0x40`.
- `uboot_<board>_spi.img`: raw RKNS v2 SPI-NOR ID block for LBA `0x40`.
- `<board>-u-boot-source.tar.xz`: patched, buildable corresponding source.

YY3568 additionally creates `uboot_yy3568_sd_nvme.img`. This is a whole-SD,
firmware-only bootstrap image that automatically scans only NVMe.

The default build fetches the manifest-pinned U-Boot commit. To use an existing
Git tree containing that exact commit:

```sh
make chainload BOARD=rock3a UBOOT_SRC=/path/to/u-boot
```

The source is never patched in place. A clean snapshot is exported to
`build/chainload/<board>/source`, and only that board's overlay is applied.
Generated headers, objects, tools, source snapshots, links, and stamps stay
under the same board namespace.

For offline FIT iteration, an existing compatible FIT can build just the
combined binary:

```sh
make uboot_rock3a.bin UBOOT_ITB=/path/to/u-boot.itb
```

Persistent-media generation also needs the pinned source build's `mkimage` or
an explicitly supplied compatible one:

```sh
make chainload-media BOARD=rock3a MEDIA=emmc MKIMAGE=/path/to/tools/mkimage
make chainload-media BOARD=rock3a MEDIA=spi-nor MKIMAGE=/path/to/tools/mkimage
make chainload-media BOARD=yy3568 MEDIA=sd-nvme MKIMAGE=/path/to/tools/mkimage
```

U-Boot `rksd` creates the SHA-256 RKNS v2 ID block used by SD, eMMC, and
RK3568 SPI NOR. The SD convenience image carries 64 leading sectors; the raw
eMMC and SPI artifacts are written by their installers at LBA `0x40`. RK3568
does not use the generic first-2-KiB-of-each-4-KiB `rkspi` layout here. These
rules do not use or modify the normal firmware's `makeboot.out` path.

### YY3568 NVMe-only SD bootstrap

`make chainload BOARD=yy3568` builds both YY3568 SD images. To build only the
NVMe bootstrap and its prerequisites, run:

```sh
make chainload-media BOARD=yy3568 MEDIA=sd-nvme
```

`uboot_yy3568_sd_nvme.img` contains 64 zeroed sectors followed byte-for-byte by
the variant build's complete `u-boot-rockchip.bin`. That binman image contains
the RKNS ID block, DDR/TPL, mainline SPL, BL31, and U-Boot FIT in the offsets
defined by upstream U-Boot. The image does not wrap the repository's rk first
stage plus FIT in a second custom RKNS container. This follows
[U-Boot's Rockchip LBA-64 firmware layout](https://docs.u-boot.org/en/stable/board/rockchip/rockchip.html)
and [Armbian's `bs=32k seek=1` binman writer](https://github.com/armbian/build/blob/a710f6715cc06fc90dfdd69fb93d642c52f3a3b8/config/sources/families/include/rockchip64_common.inc).
It has no partition table, kernel, initramfs, DTB, or root filesystem. Write it
to the whole SD device, not a partition:

```sh
sudo dd if=uboot_yy3568_sd_nvme.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Replace `/dev/sdX` with the verified SD device. The NVMe drive must contain a
complete bootable OS using extlinux, `boot.scr`, or
`EFI/BOOT/BOOTAA64.EFI`, including the Linux YY3568 DTB.

After the normal three-second interruption window, this variant executes
exactly `bootflow scan -lb nvme`, using U-Boot's [targeted standard-boot
scan](https://docs.u-boot.org/en/v2025.01/usage/cmd/bootflow.html). Failure
returns to the interactive U-Boot prompt; it does not automatically scan the
firmware SD card, eMMC, SCSI, USB, PXE, or DHCP. Those devices and commands
remain available interactively.
Because RK3568 BootROM chooses firmware before U-Boot starts, higher-priority
valid SPI-NOR or eMMC firmware must be removed, invalidated, or bypassed when
verifying that UART reports `source=sd`.

## BootROM priority and source evidence

RK3568 BootROM searches immutable media in this order:

```text
SPI NOR -> SPI NAND -> parallel NAND -> eMMC -> SD -> MaskROM USB
```

This project supports SPI NOR and the eMMC user area for the two boards above;
SPI NAND, parallel NAND, and eMMC boot partitions are out of scope. Valid SPI
firmware wins over valid eMMC or SD firmware and prevents normal USB fallback.
Restore or invalidate SPI before testing eMMC fallback.

The common RK3568 stage reads the BootROM source word at `0xfdcc0010` before
BL31 can reuse that SRAM and logs `source=spi-nor`, `source=emmc`, `source=sd`,
or `source=usb` over UART2 at 1.5 Mbaud.

Direct MaskROM loading remains the safest development and recovery path:

```sh
make usb-chainload BOARD=yy3568
make usb-chainload BOARD=rock3a
```

Use the selected board's documented recovery control when valid persistent
firmware prevents ordinary USB discovery. Confirm exactly one RK356x device
before continuing.

## Guarded eMMC and SPI-NOR installation

The installer is compatible with xrock commit
`b90d3ba8f0a48320e3888701f7e66e0e4e038bbb` and rkdeveloptool commit
`304f073752fd25c854e1bcf05d8e7f925b1f4e14`. Install host tools separately;
release archives never bundle them. Follow
[Prepare a Linux programming host](../intro.md#prepare-a-linux-programming-host)
to build the pinned tools, configure restricted udev access, and verify the
MaskROM-to-loader transition before attempting a persistent write.

### SPI NOR installation

SPI NOR contains the RK3568 first-stage firmware, BL31, and U-Boot. It does
not contain Linux or a root filesystem. The authoritative
[visual SPI-NOR tutorial](spi-nor.md) covers both supported boards, MaskROM
entry, board-qualified image validation, mandatory complete-chip backup,
verified installation, UART evidence, NVMe diagnosis, and restoration.

Use only that guarded procedure. Raw `rkdeveloptool wl` writes and
`rkdeveloptool ef` bypass the installer protections and are unsupported.

The installer writes the flat RKNS v2 image at SPI sector `0x40` (byte offset
`0x8000`), matching the RK3568/Radxa layout while preserving sectors 0-63.

### eMMC installation

Use a new, empty, absolute backup directory and an exact board-qualified
confirmation. For ROCK 3A:

```sh
make flash-chainload BOARD=rock3a MEDIA=emmc \
  BACKUP_DIR=/absolute/path/rock3a-emmc-backup CONFIRM=rock3a:emmc
```

The same command works for `BOARD=yy3568` with a matching path and
`CONFIRM=yy3568:emmc`. From a release archive, call
`install/flash-chainload.sh flash` with the same four arguments.

The utility loads the manifest-selected DDR and USB-plug helpers, requires
exactly one RK356x device, and selects eMMC with `rkdeveloptool cs 1`.

It backs up LBA 0-63 and the complete destination range, parses MBR and GPT
metadata, rejects a partition crossing the ID block, and writes only the raw
ID block at LBA `0x40`. It never replaces the partition table. Every write is
read back sector-for-sector and compared byte-for-byte before reset.

### Restoring a backup

Restore using the board and medium recorded in `backup.json`:

```sh
make restore-chainload BACKUP=/absolute/path/rock3a-spi-backup \
  CONFIRM=restore:rock3a:spi-nor
```

Restoration rejects a backup whose board, medium, manifest SHA-256, storage ID,
write range, or detected capacity differs from the selected target. This stops
a YY3568 backup from being restored to ROCK 3A, or vice versa.

## FIT and handoff policy

The common first stage copies the FIT to `0x08000000-0x08400000` before
loading a segment. It requires inline, uncompressed ARM64 images with SHA-256,
rejects external data and BL32/OP-TEE, and permits only:

- BL31 DRAM segments in `0x00040000-0x00200000`.
- BL31 SRAM segments in `0xfdcc0000-0xfdcf0000`.
- BL33 at the selected board's manifest address, ending before that board's
  initial stack.

The stage reproduces U-Boot FIT metadata and control-DTB placement, constructs
TF-A v1 parameters for a non-secure AArch64 EL2 BL33, cleans loaded ranges,
tears down EL3 caches/MMU, and branches to BL31. Validation or preparation
errors are logged and reset to MaskROM.

It deliberately omits HDMI, framebuffer, OHCI/HID, FUEFI, storage, and demo
code. SHA-256 detects accidental corruption; this is not authenticated boot.

## U-Boot automatic OS discovery

Each board gets a three-second UART interruption window. YY3568 follows the
Armbian image order:

```text
removable SD -> NVMe -> eMMC -> SCSI -> USB -> PXE -> DHCP
```

ROCK 3A retains `nvme mmc1 usb mmc0`. Mainline bootstd's unnumbered `nvme` and
`usb` targets scan all devices in those classes. On both boards `mmc1` is the
removable SD slot and `mmc0` is eMMC. `mmc2` is onboard SDIO/Wi-Fi and is
intentionally not treated as boot storage.

Local targets search supported partitions for extlinux, `boot.scr`, or
`EFI/BOOT/BOOTAA64.EFI` and fall through when no valid bootflow succeeds.
YY3568 then permits PXE and DHCP network fallback. USB mass-storage support is
enabled for both boards. SPI commands remain interactive.

This U-Boot OS order is separate from the immutable BootROM firmware-source
order described above. For example, SPI NOR may supply the chainloader while
U-Boot subsequently loads Linux from another configured target. Both boards
retain board-specific PCIe supplies/resets, JEDEC NOR through SFC, and
8-bit/HS200 eMMC definitions. YY3568 additionally retains its verified PCIe
3.0 x2 topology and its active-low GPIO3_A7 PCIe clock-buffer enable. Armbian's
YY3568 build additionally backports the upstream clock-before-PHY and balanced
clock-cleanup fixes missing from the pinned v2026.07 source. Armbian's ROCK 3A
SATA variants remain excluded.

The artifacts do not contain Linux or a root filesystem. Prepare any automatic
medium with a kernel `Image`, the Linux DTB for the actual board, and optionally
an initramfs plus `/extlinux/extlinux.conf`, or place an AArch64 EFI application
at `EFI/BOOT/BOOTAA64.EFI` on a FAT EFI System Partition. Never substitute the
minimal bare-metal DTB or U-Boot control DTB for the Linux board DTB.

Useful prompt checks are:

```text
pci enum
nvme scan
nvme info
part list nvme 0
sf probe
mmc list
mmc info
```

## Future EDK2 and OP-TEE integration

EDK2 and OP-TEE have different roles in the trusted-firmware boot model. EDK2
is normal-world boot firmware and would replace U-Boot as BL33. OP-TEE is an
optional secure-world operating system loaded as BL32 beneath BL31:

```text
RK3568 BootROM
  -> Rockchip DDR blob
  -> board-isolated rk chainloader
  -> BL31 at EL3
       |-> optional OP-TEE BL32 at Secure EL1
       `-> EDK2 or U-Boot BL33 at Non-secure EL2
             -> EFI application or Linux
```

Neither option is implemented by the current chainloader. Its FIT policy
rejects BL32, constructs TF-A parameters with BL32 absent, and accepts only the
manifest-selected U-Boot as BL33. The normal firmware's `jump_to_payload()` is
also not the integration point for either component: it is the FUEFI EL2
payload path, not a complete TF-A secure-world handoff.

### EDK2 as an alternative BL33

An EDK2 port would be a new, explicitly selected chainloader backend, not a
change to the existing U-Boot variants. It would need its own board manifest,
firmware-volume artifacts, load and stack policy, memory map, and nonvolatile
variable-storage policy. At minimum, a useful port needs UART, PSCI through
BL31, a full board DTB, PCIe/NVMe Block I/O, FAT, EFI boot management, and
reset services. USB, MMC, SPI, and HDMI GOP remain separate board-driver work.

The community [Quartz64 UEFI project](https://github.com/jaredmcneill/quartz64_uefi)
is a useful RK3566/RK3568 reference and includes ROC-RK3566-PC, but an image or
GPIO policy from one board must never be inherited by YY3568, ROCK 3A, or
another RK356x board. The official
[TianoCore platform collection](https://github.com/tianocore/edk2-platforms/tree/master/Platform)
does not currently provide a generic RK3568 target.

U-Boot's EFI Loader already supports the repository's practical requirement of
starting `EFI/BOOT/BOOTAA64.EFI`. EDK2 is most useful when a native UEFI
environment, EFI Shell, persistent variables, or a future UEFI Secure Boot
policy is required; U-Boot remains the smaller path for NVMe Linux boot.

### OP-TEE as optional BL32

OP-TEE must run in the secure world and cannot be appended as a normal FUEFI
payload or substituted for BL33. A board port would require all of the
following:

- A manifest opt-in that pins the OP-TEE source or reviewed binary, hash,
  entry point, secure DRAM carveout, and shared-memory range.
- BL31 configured with a compatible OP-TEE dispatcher, plus TF-A parameters
  that describe BL32 and the normal-world BL33 independently.
- RK3568 security-controller and memory-firewall programming that protects
  secure RAM, with those ranges excluded from the chainloader, EDK2/U-Boot,
  DTB, and Linux memory maps.
- A Linux `/firmware/optee` DT node, `CONFIG_TEE` and `CONFIG_OPTEE`; userspace
  trusted applications additionally need the OP-TEE client and supplicant.
- Per-board tests for secure/non-secure entry state, SMC behavior, address
  overlap, reset, suspend, and failure recovery.

The official [OP-TEE Rockchip platform](https://github.com/OP-TEE/optee_os/tree/master/core/arch/arm/plat-rockchip)
does not currently include an RK3568 platform flavor. The rkbin collection has
an `rk3568_bl32_v2.16.bin`, but its implementation, source correspondence,
license, ABI, and memory policy must be established before it can be treated as
a supported OP-TEE input. Addresses from RK3399 or RK3588 OP-TEE ports must not
be reused on RK3568.

### Trust boundary and recommended order

FIT SHA-256 hashes detect corruption but do not authenticate firmware. Loading
OP-TEE therefore does not by itself establish a trusted device: an attacker
who can replace unauthenticated boot media can replace BL31, BL32, or BL33.
Authenticated BootROM loading, signed manifests or FITs, rollback protection,
and secure storage are distinct future features.

The preferred development order is:

1. Retain U-Boot as the default BL33.
2. Add and validate EDK2 as a board-scoped alternative BL33.
3. Add an independently selectable RK3568 OP-TEE port and secure-memory policy.
4. Add authenticated boot only when an actual security boundary is required.

Any EDK2 or OP-TEE implementation must preserve the existing board/variant
object namespaces and require explicit manifest policy. Enabling it for one
board must not change another board's U-Boot, address map, GPIOs, storage, or
release contents. TF-A defines the relevant
[BL31/BL32/BL33 model](https://trustedfirmware-a.readthedocs.io/en/latest/design/firmware-design.html)
and [secure-partition interfaces](https://trustedfirmware-a.readthedocs.io/en/latest/components/secure-partition-manager.html).

## Adding another board without collisions

A new board is unsupported until it provides a schema-validated manifest and
board overlay. The manifest must pin its U-Boot backend/repository/ref/commit,
BL31, artifact names, FIT staging, TF-A parameters, BL31 ranges, BL33 load and
stack boundary, boot targets, physical flash, pinmux, storage IDs, offsets,
capacity policy, and backup behavior.

Its automatic OS policy must map every declared local or network target in the
required order. MMC indices must be verified from that board's U-Boot aliases;
an SDIO/Wi-Fi controller must never be copied into the SD-card group. SCSI,
PXE, or DHCP must be backed by the matching U-Boot configuration.

The validator rejects unknown boards/backends, missing or extra policy fields,
path escapes, cross-board overlays/artifacts, mismatched names, and overlapping
memory ranges. Builds must use `build/chainload/<board>/`; copying another
board's DTS, GPIOs, addresses, storage IDs, or restore metadata is forbidden.
A port also needs CI matrix coverage, a deliberate release decision, memory-map
review, and hardware sign-off.

Hardware acceptance for each board covers USB and SD entry, eMMC boot with SPI
blank, SPI priority, eMMC fallback, source logging, storage probes, BL31/U-Boot,
PCIe/NVMe, and automatic OS discovery across every target in the declared
fallback order. Test both extlinux Linux and `BOOTAA64.EFI`, plus
invalid-FIT recovery, backup restore, and complete UART transcripts. Optional
ROCK 3A eMMC must abort safely when it is not populated.
