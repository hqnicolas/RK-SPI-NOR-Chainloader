# RK356x common bare-metal firmware

This page describes behavior shared by the normal and demo firmware variants
for the explicitly supported RK356x boards. Consult the selected [board
page](index.md#documentation-map) for its GPIOs, enabled USB controllers, DDR
loader, artifacts, and validation status.

## Boot and execution model

RK356x BootROM is responsible for reading the RKNS image from SD or receiving
it through MaskROM USB. It runs the packaged Rockchip DDR loader, copies this
project's image into RAM, and transfers control at EL3. The firmware does not
need an SD, eMMC, or USB-storage driver for this delivery step.

Every normal RK356x target ends common initialization by calling
`jump_to_payload()`. It expects a FUEFI payload header immediately after the
linked firmware image, relocates the payload to `0x00a00000` when requested,
and enters it at EL2. A normal `<board>.bin` or `<board>.img` is therefore a
developer base, not a standalone program. Without an appended payload it logs
`Bad payload magic` and halts.

A `demo_<board>.bin` or `demo_<board>.img` appends the repository's example
payload. Use the demo for standalone UART, display, input, DTB, memory-map, EL2
handoff, and reset testing. Booting a demo from SD still does not give the
payload runtime access to the card.

The optional BL31/U-Boot variant is a separately linked program and does not
call `jump_to_payload()`. See [RK356x chainloading](chainloading.md).

## Common initialization

The normal firmware initializes the selected board's:

- UART2 M0 at 1,500,000 baud for early diagnostics.
- DRAM accounting and target-specific MMU map.
- VOP2 VP0, an eSmart XRGB8888 plane, DW-HDMI, and HDMI PHY.
- Board-enabled USB2 PHY and OHCI companions for polled HID input.
- Minimal firmware DTB and FUEFI service table.
- Reset-to-MaskROM path.

It intentionally has no runtime SD, eMMC, PCIe, NVMe, filesystem, networking,
USB hub, EHCI, or xHCI implementation.

## Memory layout

The normal firmware uses these RK356x low-memory reservations:

| Range | Use | Mapping |
| --- | --- | --- |
| `0x001fe000–0x00200000` | validated DDR ATAG area | reserved |
| `0x00a00000` | requested FUEFI payload relocation | cacheable |
| `0x07ff0000–0x08000000` | EL3/EL2 split stack | cacheable |
| `0x08000000–0x08400000` | OHCI DMA, DTB, and FUEFI exchange | non-cacheable |
| `0x10000000–0x12000000` | maximum framebuffer arena | non-cacheable |
| `0xf0000000–0xffffffff` | MMIO | device |

The firmware always decodes PMUGRF DDR geometry when the registers are valid.
That result is the physical-capacity authority. A bounds-checked and
checksummed `ATAG_DDR_MEM` record supplies preferred bank topology only when
its ordered, non-overlapping ranges agree with that capacity. If the ATAG is
inconsistent, the firmware logs the mismatch and synthesizes a conservative
topology from PMUGRF. It falls back to 1 GiB only when both sources are
invalid.

FUEFI reports the loaded image, payload, stacks, USB/FUEFI arena, framebuffer,
free RAM, MMIO, and validated RAM banks above 4 GiB as disjoint ranges.
`FU_GET_MEM_CHUNK` deliberately returns only the largest free range entirely
below 4 GiB, preserving the existing pointer contract. Boot logs identify the
selected DRAM source, physical byte count, bank count, and normalized ranges.

The chainloader has a separate linker and address policy. Do not reuse these
normal-firmware reservations as a board's BL31/BL33 policy.

## HDMI and framebuffer

The firmware uses the same basic fixed-mode model as the original `rk`
display path: it configures one 1920×1080p60 RGB888 timing unconditionally.
It does not probe HPD, read EDID over DDC, select modes, or handle runtime
hotplug. This avoids treating an unreadable HPD input as a headless board.

`FU_GET_SCREEN_LIST` reports the fixed address, 1920×1080 dimensions, and
64-byte-aligned stride. The framebuffer itself is configured as XRGB8888.

## USB keyboard input

Each OHCI companion enabled by the board descriptor is initialized and polled.
The implementation supports directly attached low/full-speed HID boot
keyboards, including boot-keyboard interfaces in composite devices. It handles
detach and later reattachment without blocking firmware.

US-ANSI key-down events produce printable ASCII plus Enter, Backspace, Tab,
Escape, Shift, and Caps Lock behavior. Held-key duplicates are suppressed.
Hubs, non-boot reports, arrows, function keys, Alt, EHCI, and xHCI are outside
the implemented contract.

`FU_POLL_CHAR` and `FU_GET_CHAR` service OHCI before reading the input ring.
Polling is nonblocking and `FU_GET_CHAR` returns zero when no character is
available.

## Build and validation

Build all normal/demo artifacts without fetching U-Boot:

```sh
make all
make SHELL=/bin/bash check -j"$(nproc)"
```

Typical RAM-only and SD demo targets are listed on each board page. Hardware
acceptance requires UART evidence of board/SoC identity, DRAM bytes, HDMI
setup, USB/keyboard status, payload EL, and
reset-to-MaskROM. Host tests do not replace that per-board sign-off.

For custom payload packaging, see [Firmware images, payloads, and device
trees](../payloads.md).
