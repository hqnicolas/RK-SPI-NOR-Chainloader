# Chainloader board manifests

Chainloading is opt-in and board-scoped. A board is supported only when this
directory contains a manifest plus its own overlay. The manifest pins the
U-Boot and BL31 inputs, address policy, output names, and boot policy. No
fallback manifest exists: an unknown `BOARD` is rejected.

Schema 5 validates two independent mainline policies and permits only the
declared YY3568 NVMe-only SD variant:

| Board | Backend | BL33 load / stack | Automatic OS targets |
| --- | --- | --- | --- |
| `yy3568` | `mainline-fit` | `0x00800000` / `0x03f00000` | `mmc1 nvme mmc0 scsi usb pxe dhcp` |
| `rock3a` | `mainline-fit` | `0x00800000` / `0x03f00000` | `nvme mmc1 usb mmc0` |

`yy3568.variants.sd_nvme_only` adds the board-qualified
`uboot_yy3568_sd_nvme.img` artifact. It must place the variant build's complete
`u-boot-rockchip.bin` binman output at LBA 64, merge the board-owned
`yy3568-sd-nvme-only.config`, execute exactly `bootflow scan -lb nvme`, and
return to the prompt on failure. ROCK 3A has no variant and retains its
existing scan order.

YY3568 uses official U-Boot v2026.07 and imports its working board material
from Armbian commit `a710f6715cc06fc90dfdd69fb93d642c52f3a3b8`. ROCK 3A
uses the official mainline `rock-3a-rk3568_defconfig` and DTS at U-Boot
v2026.04; Armbian commit `587b6f2c0a867859ca3f323f6008bee9e3ef1553`
provides its board-selection reference.

Run `python3 tools/chainload-manifest.py validate --all` before invoking a
board build. The validator closes the schema and rejects unknown backends,
missing or extra fields, repository path escapes, cross-board overlay/media
selection, mismatched names, artifact collisions (including variants), invalid
formats/offsets/commands/failure actions, and overlapping stage/FIT/BL33
ranges.

Every manifest declares an ordered `boot_policy.automatic_scan` with a
board-specific target list for each medium or network group. The validator
rejects unsupported or repeated groups, missing or extra target mappings,
duplicate devices, and names assigned to the wrong group. On both boards
`mmc1` is removable SD, `mmc0` is eMMC, and `mmc2` SDIO is intentionally
absent. Mainline bootstd uses the unnumbered `nvme` and `usb` class targets.

Adding a board requires all of the following:

1. A new manifest and board-specific U-Boot overlay.
2. A manifest-generated platform descriptor and isolated
   `build/chainload/<board>/` linker/object namespace.
3. A reviewed FIT staging area, BL31 ranges, BL33 load/stack bounds, and TF-A
   handoff protocol.
4. Explicit boot-media entries for each physical flash type: BootROM priority,
   format, detected-capacity policy, pinmux, rkdeveloptool storage ID, write
   offset, partition-preservation policy, backup scope, and artifact name.
5. A board-qualified U-Boot OS scan mapping. Each `mmcN` must be checked
   against that board's aliases, SDIO must be excluded, and any SCSI or
   network fallback must be explicitly ordered and enabled in U-Boot.
6. Offline parser/address/media-policy tests, a chainloader CI matrix entry,
   release allowlist changes, and hardware validation of installation and
   restoration. Never reuse another board's addresses, GPIOs, storage IDs,
   artifacts, restore metadata, or DTS data by default.
