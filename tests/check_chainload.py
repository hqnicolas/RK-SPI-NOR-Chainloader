#!/usr/bin/env python3
"""Offline isolation/provenance checks and built RK356x chainload checks."""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PINNED_YY3568_UBOOT = "ece349ade2973e220f524ce59e59711cc919263f"
PINNED_ROCK3A_UBOOT = "88dc2788777babfd6322fa655df549a019aa1e69"
PINNED_YY3568_ARMBIAN = "a710f6715cc06fc90dfdd69fb93d642c52f3a3b8"
PINNED_ROCK3A_ARMBIAN = "587b6f2c0a867859ca3f323f6008bee9e3ef1553"
PINNED_RKBIN = "ecb4fcbe954edf38b3ae037d5de6d9f5bccf81f4"
PINNED_XROCK = "b90d3ba8f0a48320e3888701f7e66e0e4e038bbb"
PINNED_RKDEVELOPTOOL = "304f073752fd25c854e1bcf05d8e7f925b1f4e14"
BL31_HASH = "c81ac7e8e1fd727cf7f0db62a9aaea760bde2b270e34d98eb264a264b86df749"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_load_segments(data: bytes) -> list[tuple[int, int, int]]:
    require(data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 1,
            "BL31 is not a little-endian ELF64 image")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    segments = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        p_type, _, _, _, paddr, filesz, memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", data, offset)
        if p_type == 1 and memsz:
            segments.append((paddr, filesz, memsz))
    return segments


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def check_rkns_v2(raw: bytes, ddr: bytes, payload: bytes) -> None:
    """Validate the pinned U-Boot RKNS v2 rksd byte layout."""
    ddr_size = align(len(ddr), 2048)
    payload_size = align(len(payload), 2048)
    require(len(raw) == 2048 + ddr_size + payload_size,
            "RKNS v2 ID block has an unexpected size")
    require(len(raw) % 2048 == 0, "RKNS v2 ID block is not 2-KiB aligned")
    magic, reserved, size_images, boot_flag = struct.unpack_from("<IIII", raw, 0)
    require(magic == 0x534E4B52, "RKNS v2 magic mismatch")
    require(reserved == 0, "RKNS v2 reserved header word is nonzero")
    require(size_images == (2 << 16) | 384,
            "RKNS v2 header hash offset/image count mismatch")
    require(boot_flag == 1, "RKNS v2 must select SHA-256")
    require(raw[16:120] == bytes(104), "RKNS v2 reserved header bytes are nonzero")

    expected = ((4, ddr_size, ddr, 1),
                (4 + ddr_size // 512, payload_size, payload, 2))
    for index, (sector_offset, padded_size, source, counter) in enumerate(expected):
        entry_offset = 120 + index * 88
        size_offset, address, flag, actual_counter = struct.unpack_from(
            "<IIII", raw, entry_offset)
        require(size_offset == ((padded_size // 512) << 16) | sector_offset,
                f"RKNS v2 image {index} size/offset mismatch")
        require(address == 0xFFFFFFFF and flag == 0 and actual_counter == counter,
                f"RKNS v2 image {index} metadata mismatch")
        require(raw[entry_offset + 16:entry_offset + 24] == bytes(8),
                f"RKNS v2 image {index} reserved bytes are nonzero")
        padded = source + bytes(padded_size - len(source))
        require(raw[entry_offset + 24:entry_offset + 56] == hashlib.sha256(padded).digest(),
                f"RKNS v2 image {index} payload hash mismatch")
        require(raw[entry_offset + 56:entry_offset + 88] == bytes(32),
                f"RKNS v2 image {index} SHA-512 tail is nonzero")
        data_offset = sector_offset * 512
        require(raw[data_offset:data_offset + padded_size] == padded,
                f"RKNS v2 image {index} payload/padding mismatch")

    require(raw[120 + 2 * 88:0x600] == bytes(0x600 - (120 + 2 * 88)),
            "unused RKNS v2 entries/reserved area are nonzero")
    require(raw[0x600:0x620] == hashlib.sha256(raw[:0x600]).digest(),
            "RKNS v2 header hash mismatch")
    require(raw[0x620:0x800] == bytes(0x800 - 0x620),
            "RKNS v2 header signature padding is nonzero")


def rkns_v2_padded_payload(raw: bytes, index: int) -> bytes:
    """Return one RKNS v2 entry exactly as hashed by its image header."""
    require(index in (0, 1), "RKNS v2 payload index is out of range")
    entry_offset = 120 + index * 88
    size_offset = struct.unpack_from("<I", raw, entry_offset)[0]
    padded_size = (size_offset >> 16) * 512
    data_offset = (size_offset & 0xFFFF) * 512
    data_end = data_offset + padded_size
    require(padded_size != 0 and data_end <= len(raw),
            f"RKNS v2 image {index} payload is outside the ID block")
    return raw[data_offset:data_end]


def check_media_images(board: str, manifest: dict[str, object]) -> None:
    ddr = (ROOT / "img" / "rk3568_ddr_1560MHz_v1.25.bin").read_bytes()
    artifacts = manifest["artifacts"]
    assert isinstance(artifacts, dict)
    payload = (ROOT / str(artifacts["binary"])).read_bytes()
    idblock = (ROOT / str(artifacts["idblock"])).read_bytes()
    spi = (ROOT / str(artifacts["spi_nor"])).read_bytes()
    sd = (ROOT / str(artifacts["image"])).read_bytes()
    check_rkns_v2(idblock, ddr, payload)
    require(sd[:64 * 512] == bytes(64 * 512),
            f"{board}: SD convenience image does not leave LBA 0-63 empty")
    require(sd[64 * 512:] == idblock,
            "SD convenience image does not contain the validated ID block at LBA 0x40")
    require(spi == idblock,
            f"{board}: SPI NOR image is not the flat RKNS ID block for LBA 0x40")

    variants = manifest.get("variants", {})
    assert isinstance(variants, dict)
    for name, entry in variants.items():
        assert isinstance(entry, dict)
        variant_payload = (ROOT / "build" / "chainload" / board / "variants" /
                           name / str(entry["payload"])).read_bytes()
        variant_sd = (ROOT / str(entry["artifact"])).read_bytes()
        require(variant_sd[:64 * 512] == bytes(64 * 512),
                f"{board}: {name} does not leave LBA 0-63 empty")
        require(variant_sd[64 * 512:] == variant_payload,
                f"{board}: {name} is not the exact binman image at LBA 64")
        require(variant_payload[:4] == b"RKNS",
                f"{board}: {name} binman payload lacks an RKNS ID block")


def load_manifests() -> dict[str, dict[str, object]]:
    subprocess.run(
        [sys.executable, str(ROOT / "tools" / "chainload-manifest.py"),
         "validate", "--all"], check=True
    )
    paths = sorted((ROOT / "config" / "chainload").glob("*.json"))
    require([item.name for item in paths] == ["rock3a.json", "yy3568.json"],
            "chainloader board list changed without CI/release review")
    unknown = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "chainload-manifest.py"),
         "validate", "not-a-board"], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    require(unknown.returncode != 0 and "has no chainload manifest" in unknown.stderr,
            "unknown chainloader board was accepted")
    return {path.stem: json.loads(path.read_text(encoding="utf-8")) for path in paths}


def check_manifests(manifests: dict[str, dict[str, object]]) -> None:
    expected = {
        "yy3568": (
            PINNED_YY3568_UBOOT, "v2026.07", PINNED_YY3568_ARMBIAN,
            "patch/u-boot/v2026.07/board_yy3568/"
            "0001-board-rockchip-add-Youyeetoo-YY3568-support.patch",
            "0x00800000", "0x03f00000",
            ["sd", "nvme", "emmc", "scsi", "usb", "network"], {
                "sd": ["mmc1"], "nvme": ["nvme"], "emmc": ["mmc0"],
                "scsi": ["scsi"], "usb": ["usb"],
                "network": ["pxe", "dhcp"],
            }),
        "rock3a": (
            PINNED_ROCK3A_UBOOT, "v2026.04", PINNED_ROCK3A_ARMBIAN,
            "config/boards/rock-3a.conf", "0x00800000", "0x03f00000",
            ["nvme", "sd", "usb", "emmc"], {
                "nvme": ["nvme"], "sd": ["mmc1"],
                "usb": ["usb"], "emmc": ["mmc0"],
            }),
    }
    artifacts: set[str] = set()
    for board, values in expected.items():
        manifest = manifests[board]
        commit, ref, armbian_commit, armbian_path, load, stack, order, targets = values
        require(manifest["schema"] == 5 and manifest["board"] == board and
                manifest["soc"] == "rk3568" and manifest["platform"] == "rk3568",
                f"{board}: manifest identity/schema mismatch")
        uboot = manifest["uboot"]
        assert isinstance(uboot, dict)
        require(uboot["backend"] == "mainline-fit" and
                uboot["repository"] == "https://github.com/u-boot/u-boot.git" and
                uboot["commit"] == commit and uboot["ref"] == ref and
                uboot["armbian_commit"] == armbian_commit and
                uboot["armbian_path"] == armbian_path,
                f"{board}: U-Boot/Armbian provenance changed")
        bl31 = manifest["bl31"]
        assert isinstance(bl31, dict)
        require(bl31["rkbin_commit"] == PINNED_RKBIN and
                bl31["sha256"] == BL31_HASH, f"{board}: BL31 provenance changed")
        layout = manifest["layout"]
        assert isinstance(layout, dict)
        require(layout["stage_limit"] == "0x00040000" and
                layout["bl33_load"] == load and layout["bl33_stack"] == stack and
                layout["expected_bl31_segments"] == 6,
                f"{board}: memory policy changed without review")
        policy = manifest["boot_policy"]
        assert isinstance(policy, dict)
        scan = policy["automatic_scan"]
        assert isinstance(scan, dict)
        require(scan["order"] == order and
                scan["targets"] == targets and
                policy["interactive_only"] == ["spi"] and
                policy["baud_rate"] == 1500000,
                f"{board}: automatic scan policy is not board-scoped")
        host_tools = manifest["host_tools"]
        assert isinstance(host_tools, dict)
        require(host_tools["xrock"]["commit"] == PINNED_XROCK and
                host_tools["rkdeveloptool"]["commit"] == PINNED_RKDEVELOPTOOL,
                f"{board}: host-tool compatibility pin changed")
        board_artifacts = manifest["artifacts"]
        assert isinstance(board_artifacts, dict)
        variants = manifest.get("variants", {})
        assert isinstance(variants, dict)
        if board == "yy3568":
            require(variants == {
                "sd_nvme_only": {
                    "media": "sd-nvme",
                    "artifact": "uboot_yy3568_sd_nvme.img",
                    "config_fragment": (
                        "config/chainload/yy3568/overlay/configs/"
                        "yy3568-sd-nvme-only.config"
                    ),
                    "format": "rockchip-binman",
                    "payload": "u-boot-rockchip.bin",
                    "idblock_lba": 64,
                    "boot_command": "bootflow scan -lb nvme",
                    "on_failure": "prompt",
                }
            }, "YY3568 NVMe-only SD variant contract changed")
        else:
            require(not variants, "ROCK 3A unexpectedly gained a media variant")
        all_artifacts = list(board_artifacts.values())
        all_artifacts.extend(
            item["artifact"] for item in variants.values()
            if isinstance(item, dict)
        )
        for artifact in all_artifacts:
            require(str(artifact) not in artifacts,
                    f"cross-board artifact collision: {artifact}")
            artifacts.add(str(artifact))

    tool = str(ROOT / "tools" / "chainload-manifest.py")
    yy_artifacts = subprocess.run(
        [sys.executable, tool, "artifacts", "yy3568"], check=True, text=True,
        stdout=subprocess.PIPE,
    ).stdout.split()
    yy_media = subprocess.run(
        [sys.executable, tool, "media-artifacts", "yy3568"], check=True,
        text=True, stdout=subprocess.PIPE,
    ).stdout.split()
    rock_media = subprocess.run(
        [sys.executable, tool, "media-artifacts", "rock3a"], check=True,
        text=True, stdout=subprocess.PIPE,
    ).stdout.split()
    yy_patches = subprocess.run(
        [sys.executable, tool, "patches", "yy3568"], check=True,
        text=True, stdout=subprocess.PIPE,
    ).stdout.split()
    rock_patch_result = subprocess.run(
        [sys.executable, tool, "patches", "rock3a"], check=True,
        text=True, stdout=subprocess.PIPE,
    )
    rock_patches = rock_patch_result.stdout.split()
    require(yy_artifacts[-1] == "uboot_yy3568_sd_nvme.img" and
            yy_media[-1] == "uboot_yy3568_sd_nvme.img",
            "YY3568 variant is absent from artifact enumeration")
    require("uboot_yy3568_sd_nvme.img" not in rock_media and
            len(rock_media) == 3,
            "ROCK 3A inherited the YY3568 media variant")
    require(len(yy_patches) == 2 and
            all(path.startswith("config/chainload/yy3568/patches/")
                for path in yy_patches) and not rock_patches and
            rock_patch_result.stdout == "",
            "board-scoped U-Boot patch enumeration is not isolated")
    unsupported = subprocess.run(
        [sys.executable, tool, "media-artifact", "rock3a", "sd-nvme"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    require(unsupported.returncode != 0,
            "ROCK 3A accepted the YY3568-only media target")


def check_manifest_rejections(manifests: dict[str, dict[str, object]]) -> None:
    spec = importlib.util.spec_from_file_location(
        "chainload_manifest_test", ROOT / "tools" / "chainload-manifest.py")
    require(spec is not None and spec.loader is not None,
            "cannot import chainload manifest validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    with tempfile.TemporaryDirectory(prefix="rk-chainload-manifest-") as temporary:
        manifest_dir = pathlib.Path(temporary)
        module.MANIFEST_DIR = manifest_dir

        def rejected(change, description: str) -> None:
            fixtures = copy.deepcopy(manifests)
            change(fixtures)
            for board, data in fixtures.items():
                (manifest_dir / f"{board}.json").write_text(
                    json.dumps(data), encoding="utf-8")
            try:
                module.validate_all()
            except module.ManifestError:
                return
            raise AssertionError(f"manifest validator accepted {description}")

        rejected(lambda items: items["rock3a"].update(board="yy3568"),
                 "a mismatched board name")
        rejected(lambda items: items["rock3a"]["uboot"].update(backend="unknown"),
                 "an unsupported U-Boot backend")
        rejected(lambda items: items["rock3a"]["uboot"].update(
            overlay="config/chainload/yy3568/overlay"),
            "another board's overlay")
        rejected(lambda items: items["rock3a"]["uboot"].update(
            config_fragment="../yy3568.config"), "a path escape")
        rejected(lambda items: items["rock3a"]["uboot"].update(
            patches=[items["yy3568"]["uboot"]["patches"][0]]),
            "another board's U-Boot patch")
        rejected(lambda items: items["yy3568"]["uboot"]["patches"].append(
            items["yy3568"]["uboot"]["patches"][0]),
            "a duplicate U-Boot patch")
        rejected(lambda items: items["yy3568"]["uboot"].update(
            patches="config/chainload/yy3568/patches/fix.patch"),
            "a non-list U-Boot patch contract")
        rejected(lambda items: items["rock3a"]["artifacts"].update(
            binary="uboot_yy3568.bin"), "a cross-board artifact")
        rejected(lambda items: items["rock3a"]["layout"].update(
            fit_stage_start="0x00800000"), "overlapping memory ranges")
        rejected(lambda items: items["rock3a"]["boot_media"]["emmc"].update(
            artifact="uboot_yy3568_idbloader.img"), "cross-board media")
        rejected(lambda items: items["rock3a"]["boot_media"]["spi-nor"].update(
            unknown_policy=True), "an unknown media policy field")
        rejected(lambda items: items["rock3a"]["boot_media"]["spi-nor"].update(
            format="rkspi"), "an RK3568 SPI image with striped rkspi layout")
        rejected(lambda items: items["rock3a"]["boot_media"]["spi-nor"].update(
            write_lba=0), "an RK3568 SPI ID block at LBA zero")
        rejected(lambda items: items["rock3a"]["boot_policy"]["automatic_scan"].update(
            order=["nvme", "nvme", "usb", "emmc"]), "a repeated OS target group")
        rejected(lambda items: items["rock3a"]["boot_policy"]["automatic_scan"].update(
            order=["nvme", "sata", "usb", "emmc"]), "an unsupported OS target group")
        rejected(lambda items: items["rock3a"]["boot_policy"]["automatic_scan"]
                 ["targets"].update(emmc=["mmc1"]),
                 "one MMC target assigned to both SD and eMMC")
        rejected(lambda items: items["rock3a"]["boot_policy"]["automatic_scan"]
                 ["targets"].update(sd=["usb"]),
                 "a USB device assigned to the SD group")
        rejected(lambda items: items["yy3568"]["boot_policy"]["automatic_scan"]
                 ["targets"].update(network=["tftp"]),
                 "an unsupported network boot target")
        rejected(lambda items: items["rock3a"].pop("boot_policy"),
                 "a missing required policy")
        rejected(lambda items: items["yy3568"]["variants"]["sd_nvme_only"].update(
            config_fragment="../yy3568-sd-nvme-only.config"),
            "an NVMe variant path escape")
        rejected(lambda items: items["yy3568"]["variants"]["sd_nvme_only"].update(
            artifact="uboot_rock3a_sd_nvme.img"),
            "a wrong-board NVMe variant artifact")
        rejected(lambda items: items["yy3568"]["variants"]["sd_nvme_only"].update(
            artifact="uboot_yy3568.img"),
            "a duplicate in-board artifact")
        rejected(lambda items: items["yy3568"]["variants"]["sd_nvme_only"].update(
            format="rksd"), "a custom-wrapped NVMe variant format")
        rejected(lambda items: items["yy3568"]["variants"]["sd_nvme_only"].update(
            payload="uboot_yy3568.bin"),
            "a non-binman NVMe variant payload")
        rejected(lambda items: items["yy3568"]["variants"]["sd_nvme_only"].update(
            idblock_lba=32), "an unsupported NVMe variant LBA")
        rejected(lambda items: items["yy3568"]["variants"]["sd_nvme_only"].update(
            boot_command="bootflow scan"), "an untargeted NVMe boot command")
        rejected(lambda items: items["yy3568"]["variants"]["sd_nvme_only"].update(
            on_failure="reset"), "an unsupported NVMe failure action")
        rejected(lambda items: items["yy3568"]["variants"].update(
            unknown=copy.deepcopy(items["yy3568"]["variants"]["sd_nvme_only"])),
            "an unknown media variant")
        rejected(lambda items: items["yy3568"].pop("variants"),
                 "a missing YY3568 media variant")


def check_bl31() -> None:
    blob = (ROOT / "img" / "rk3568_bl31_v1.46.elf").read_bytes()
    require(len(blob) == 402376, "BL31 size mismatch")
    require(hashlib.sha256(blob).hexdigest() == BL31_HASH, "BL31 hash mismatch")
    ranges = ((0x40000, 0x200000), (0xFDCC0000, 0xFDCF0000))
    segments = parse_load_segments(blob)
    require(len(segments) == 6, "expected exactly six split BL31 load segments")
    for address, _, memory_size in segments:
        end = address + memory_size
        require(any(address >= start and end <= limit for start, limit in ranges),
                f"BL31 segment 0x{address:x}-0x{end:x} violates manifest policy")


def check_isolation() -> None:
    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    normal_board = (ROOT / "src" / "rk356x" / "board.c").read_text(encoding="utf-8")
    loader = (ROOT / "src" / "chainload" / "loader.c").read_text(encoding="utf-8")
    linker = (ROOT / "Chainload-rk356x.ld").read_text(encoding="utf-8")
    builder = (ROOT / "tools" / "build-chainload-uboot.sh").read_text(encoding="utf-8")
    manifest_validator = (ROOT / "tools" / "chainload-manifest.py").read_text(
        encoding="utf-8")
    compat = (ROOT / "src" / "chainload" / "compat.c").read_text(encoding="utf-8")
    media_builder = (ROOT / "tools" / "build-chainload-media.sh").read_text(encoding="utf-8")
    flasher = (ROOT / "tools" / "flash-chainload.sh").read_text(encoding="utf-8")
    chainfit = (ROOT / "tools" / "chainfit.c").read_text(encoding="utf-8")
    libfdt_readme = (ROOT / "external" / "libfdt" / "README.md").read_text(
        encoding="utf-8")
    require("build/chainload/$(1)/obj/" in makefile and
            "CHAINLOAD_BOARDS := yy3568 rock3a" in makefile,
            "chainloader objects lack generated board/variant namespaces")
    for symbol in ("YY3568_OBJ :=", "ROCK3A_OBJ :="):
        line = next(item for item in makefile.splitlines() if item.startswith(symbol))
        require("chain" not in line.lower(), f"normal {symbol} firmware inherited chainloader objects")
    require("chainload" not in normal_board and "FIT" not in normal_board,
            "normal RK356x entry gained FIT auto-detection")
    common_list = makefile[makefile.index("CHAINLOAD_SRC :="):makefile.index("CHAINLOAD_CFLAGS :=")]
    chainfit_rule = makefile[makefile.index("tools/chainfit.out:"):
                             makefile.index("\nchainload:")]
    for excluded in ("firmware.c", "ohci.c", "hid_keyboard.c", "hdmi.c", "vop2.c", "demo/"):
        require(excluded not in common_list, f"dedicated chainloader includes {excluded}")
    require("ASSERT(_end_of_image < 0x00040000" in linker,
            "chainloader link limit is missing")
    require("first BL31 load address\");" not in linker,
            "chainloader ASSERT uses GNU ld-incompatible trailing semicolon")
    require("size_t strnlen(const char *s, size_t maxlen)" in compat,
            "chainloader libfdt compatibility layer lacks strnlen")
    require("char *strrchr(const char *s, int c)" in compat,
            "chainloader libfdt compatibility layer lacks strrchr")
    require("-D_POSIX_C_SOURCE=200809L" in makefile and
            "src/chainload/loader.c src/chainload/compat.c" in makefile,
            "host chainloader tests do not exercise the string compatibility layer")
    require("-D_POSIX_C_SOURCE=200809L" in chainfit_rule and
            "src/chainload/compat.c" in chainfit_rule,
            "chainfit host tool lacks the string compatibility contract")
    require(loader.index("dcache_clean") < loader.index("chain_jump_bl31"),
            "loaded ranges are not cleaned before the BL31 branch")
    require("fdt_check_header" in loader and "fit_stage_start" in loader,
            "FIT is not staged before loading")
    require('BACKENDS = {"mainline-fit"}' in manifest_validator and
            '[[ "$uboot_backend" == "mainline-fit" ]]' in builder and
            "scripts/kconfig/merge_config.sh" in builder and
            'patch --batch --forward -d "$target" -p1' in builder and
            'make -C "$target" CROSS_COMPILE="$cross" -j"$jobs" all' in builder and
            'export_snapshot "$variant_snapshot"' in builder and
            'build_snapshot "$variant_snapshot" "$variant_fragment"' in builder and
            '[[ -x "$snapshot/tools/mkimage" ]]' in builder and
            "all tools/mkimage" not in builder,
            "U-Boot builder is not restricted to the mainline FIT backend")
    require(PINNED_YY3568_UBOOT in libfdt_readme and
            "official U-Boot v2026.07" in libfdt_readme,
            "libfdt provenance does not match the pinned mainline source")
    require(media_builder.count('"$mkimage" -n "$soc" -T rksd') >= 3 and
            '-T rkspi' not in media_builder and
            'flash LBA 0x40' in media_builder,
            "chainload media is not generated by pinned U-Boot mkimage")
    require('media-artifact "$(BOARD)" "$(MEDIA)"' in makefile and
            'artifacts "$(BOARD)"' in makefile and
            'build_variant sd_nvme_only' in media_builder and
            'build_binman_sd_image' in media_builder and
            'u-boot-rockchip.bin' in builder,
            "NVMe-only media is not wired through manifest-scoped targets")
    require("makeboot.out" not in media_builder,
            "chainload media accidentally uses the normal firmware packer")
    media_docs = "\n".join(
        (ROOT / "docs" / "rk356x" / name).read_text(encoding="utf-8")
        for name in ("chainloading.md", "spi-nor.md")
    )
    for marker in ("cs 1", "cs 9", "check-partition-overlap.py", "cmp \"$artifact\"",
                   "complete-spi-nor.bin", "emmc-lba0-63.bin"):
        require(marker in media_docs or marker in flasher,
                f"guarded media installer lacks contract marker: {marker}")
    platform = (ROOT / "src" / "chainload" / "rk3568.c").read_text()
    require("0xfdcc0010" in platform and 'return "source=spi-nor"' in platform and
            'return "source=emmc"' in platform and 'return "source=sd"' in platform and
            'return "source=usb"' in platform,
            "RK3568 chainloader lacks BootROM-source decoding")
    require("CHAIN_EXPECTED_BL33_ENTRY" in platform and
            "CHAIN_BL31_RANGE_INITIALIZER" in platform,
            "chainloader platform does not consume the generated board descriptor")
    require("BootROM download marker" in platform,
            "validation recovery does not document MaskROM reset")
    require("deprecated alias" in makefile and "usb-chainload" in makefile,
            "ambiguous RK3568 USB aliases were not replaced safely")
    require("chainfit-args" in makefile and "yy3568" not in chainfit and
            "rock3a" not in chainfit,
            "host FIT validation bypasses manifest-derived board policy")


def check_overlays() -> None:
    base = ROOT / "config" / "chainload" / "yy3568" / "overlay"
    patches = base.parent / "patches"
    defconfig = (base / "configs" /
                 "youyeetoo-yy3568-rk3568_defconfig").read_text()
    fragment = (base / "configs" / "yy3568-chainload.config").read_text()
    nvme_fragment = (base / "configs" /
                     "yy3568-sd-nvme-only.config").read_text()
    dts = (base / "dts" / "upstream" / "src" / "arm64" / "rockchip" /
           "rk3568-youyeetoo-yy3568.dts").read_text()
    uboot_dtsi = (base / "arch" / "arm" / "dts" /
                  "rk3568-youyeetoo-yy3568-u-boot.dtsi").read_text()
    header = (base / "include" / "configs" / "evb_rk3568.h").read_text()
    source_readme = (base / "README.rk-chainload").read_text()
    clock_patch = (patches /
                   "0001-pci-pcie_dw_rockchip-enable-clocks-before-PHY-init.patch").read_text()
    cleanup_patch = (patches /
                     "0002-pci-pcie_dw_rockchip-drop-clk_release_bulk-calls.patch").read_text()
    for obsolete in (
        base / "configs" / "yy3568-rk3568_defconfig",
        base / "arch" / "arm" / "dts" / "rk3568-yy3568.dts",
        base / "include" / "configs" / "yy3568.h",
        base / "board" / "rockchip" / "yy3568" / "Makefile",
        base / "board" / "rockchip" / "yy3568" / "yy3568.c",
        base / "arch" / "arm" / "mach-rockchip" / "decode_bl31.py",
    ):
        require(not obsolete.exists(), f"obsolete YY3568 overlay remains: {obsolete}")
    for setting in (
        'CONFIG_DEFAULT_DEVICE_TREE="rockchip/rk3568-youyeetoo-yy3568"',
        "CONFIG_ROCKCHIP_RK3568=y", "CONFIG_PCI=y", "CONFIG_NVME_PCI=y",
        "CONFIG_PCIE_DW_ROCKCHIP=y", "CONFIG_BAUDRATE=1500000",
        "CONFIG_ROCKCHIP_SFC=y", "CONFIG_MMC_SDHCI_ROCKCHIP=y",
        "CONFIG_USB_DWC3_GENERIC=y", "CONFIG_DWC_ETH_QOS_ROCKCHIP=y",
    ):
        require(setting in defconfig, f"YY3568 U-Boot defconfig lacks {setting}")
    for setting in (
        "CONFIG_BOOTDELAY=3", "CONFIG_BOOTSTD_DEFAULTS=y",
        "CONFIG_BOOTMETH_EXTLINUX=y", "CONFIG_BOOTMETH_SCRIPT=y",
        "CONFIG_CMD_NVME=y", "CONFIG_CMD_SCSI=y", "CONFIG_CMD_DHCP=y",
        "CONFIG_CMD_PXE=y", "CONFIG_CMD_NET=y", "CONFIG_CMD_TFTPBOOT=y",
        "CONFIG_BOOTMETH_EXTLINUX_PXE=y",
        "CONFIG_CMD_EXT4=y", "CONFIG_CMD_FAT=y",
        "CONFIG_CMD_SF=y", "CONFIG_CMD_MMC=y", "CONFIG_CMD_USB=y",
        "CONFIG_USB_STORAGE=y", "CONFIG_NVME_PCI=y", "CONFIG_EFI_LOADER=y",
        'CONFIG_SYS_CONFIG_NAME="evb_rk3568"',
        'CONFIG_DEFAULT_FDT_FILE="rockchip/rk3568-yy3568.dtb"',
    ):
        require(setting in fragment, f"YY3568 config fragment lacks {setting}")
    require(nvme_fragment.splitlines() == [
        "CONFIG_USE_BOOTCOMMAND=y",
        'CONFIG_BOOTCOMMAND="bootflow scan -lb nvme"',
    ], "YY3568 NVMe-only config fragment changed")
    require('#define BOOT_TARGETS "mmc1 nvme mmc0 scsi usb pxe dhcp"' in header and
            "#include <configs/rk3568_common.h>" in header and
            "ROCKCHIP_DEVICE_SETTINGS" in header,
            "YY3568 board header does not enforce its automatic scan order")
    for alias in ("mmc0 = &sdhci;", "mmc1 = &sdmmc0;", "mmc2 = &sdmmc2;"):
        require(alias in dts, f"YY3568 U-Boot DTS lacks board alias {alias}")
    for node in ("&pcie2x1", "&pcie30phy", "&pcie3x2", "vcc3v3_pcie",
                 "reset-gpios", "num-lanes = <2>"):
        require(node in dts, f"YY3568 U-Boot DTS lacks {node}")
    require("data-lanes =" not in dts,
            "YY3568 U-Boot DTS incorrectly bifurcates the PCIe 3 x2 PHY")
    pcie_oe = re.search(
        r"pcie_oe_regulator:\s*pcie-oe-regulator\s*\{(.*?)^\s*\};",
        dts, re.MULTILINE | re.DOTALL)
    require(pcie_oe is not None, "YY3568 U-Boot DTS lacks PCIe clock enable")
    require("gpio = <&gpio3 RK_PA7 GPIO_ACTIVE_HIGH>;" in pcie_oe.group(1),
            "YY3568 U-Boot DTS has the wrong PCIe clock-enable GPIO")
    require("enable-active-high" not in pcie_oe.group(1),
            "YY3568 U-Boot DTS drives its active-low PCIe clock enable high")
    require("clk_enable_bulk(&priv->clks)" in clock_patch and
            "goto err_disable_clks" in clock_patch and
            "619272.html" in clock_patch,
            "YY3568 lacks the upstream PCIe clock-before-PHY backport")
    require("clk_release_bulk(&priv->clks)" in cleanup_patch and
            "msg574897.html" in cleanup_patch,
            "YY3568 lacks the upstream PCIe clock cleanup backport")
    for marker in ("&sdhci", "&emmc_bus8", "bus-width = <8>"):
        require(marker in dts, f"YY3568 storage DTS lacks {marker}")
    require("youyeetoo,yy3568" in dts and "rockchip,rk3568" in dts,
            "YY3568 U-Boot DTS identity mismatch")
    for marker in ("/delete-property/ fit,external-offset", "&sfc",
                   "pinctrl-0 = <&fspi_pins>", 'compatible = "jedec,spi-nor"',
                   "spi-max-frequency = <100000000>",
                   "spi-rx-bus-width = <4>", "spi-tx-bus-width = <1>",
                   "<&pcie30x2m1_pins &pcie30x2_reset_h>", "&vdd_npu",
                   "regulator-always-on"):
        require(marker in uboot_dtsi, f"YY3568 U-Boot DTSI lacks {marker}")
    require("v2026.07" in source_readme and
            "youyeetoo-yy3568-rk3568_defconfig" in source_readme and
            "configs/yy3568-chainload.config" in source_readme and
            "configs/yy3568-sd-nvme-only.config" in source_readme and
            "include/configs/evb_rk3568.h" in source_readme,
            "YY3568 corresponding source lacks reproducible build instructions")

    rock = ROOT / "config" / "chainload" / "rock3a" / "overlay"
    fragment = (rock / "configs" / "rock3a-chainload.config").read_text()
    config_header = (rock / "include" / "configs" /
                     "evb_rk3568.h").read_text()
    uboot_dtsi = (rock / "arch" / "arm" / "dts" /
                  "rk3568-rock-3a-u-boot.dtsi").read_text()
    source_readme = (rock / "README.rk-chainload").read_text()
    for setting in (
        "CONFIG_BOOTDELAY=3", "CONFIG_BAUDRATE=1500000",
        "CONFIG_BOOTSTD_DEFAULTS=y",
        "CONFIG_BOOTMETH_EXTLINUX=y", "CONFIG_BOOTMETH_SCRIPT=y",
        "CONFIG_CMD_NVME=y", "CONFIG_CMD_EXT4=y", "CONFIG_CMD_FAT=y",
        "CONFIG_CMD_SF=y", "CONFIG_CMD_MMC=y", "CONFIG_CMD_USB=y",
        "CONFIG_USB_STORAGE=y", "CONFIG_NVME_PCI=y", "CONFIG_EFI_LOADER=y",
    ):
        require(setting in fragment, f"ROCK 3A config fragment lacks {setting}")
    require('#define BOOT_TARGETS "nvme mmc1 usb mmc0"' in config_header and
            "#include <configs/rk3568_common.h>" in config_header and
            "ROCKCHIP_DEVICE_SETTINGS" in config_header,
            "ROCK 3A board header does not enforce its automatic scan order")
    require("/delete-property/ fit,external-offset" in uboot_dtsi and
            '#include "rk356x-u-boot.dtsi"' in uboot_dtsi,
            "ROCK 3A does not preserve upstream DTS while forcing inline FIT data")
    require("rock-3a-rk3568_defconfig" in source_readme and
            "configs/rock3a-chainload.config" in source_readme and
            "include/configs/evb_rk3568.h" in source_readme,
            "ROCK 3A corresponding source lacks reproducible build instructions")


def check_built(board: str) -> None:
    manifests = load_manifests()
    require(board in manifests, "an unsupported board reached chainload checks")
    manifest = manifests[board]
    artifacts = manifest["artifacts"]
    uboot_policy = manifest["uboot"]
    assert isinstance(artifacts, dict) and isinstance(uboot_policy, dict)
    stage = ROOT / "build" / "chainload" / board / "stage.bin"
    require(stage.is_file() and stage.stat().st_size < 0x40000,
            f"{board}: chainloader stage exceeds its dedicated low-memory limit")
    fit = ROOT / str(artifacts["fit"])
    binary = ROOT / str(artifacts["binary"])
    require(fit.is_file() and binary.is_file(), "chainload outputs are incomplete")
    require(binary.read_bytes() == stage.read_bytes() + fit.read_bytes(),
            f"{board}: combined image is not stage + pinned FIT")
    fit_args = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "chainload-manifest.py"),
         "chainfit-args", board], check=True, text=True,
        stdout=subprocess.PIPE,
    ).stdout.split()
    subprocess.run([str(ROOT / "tools" / "chainfit.out"), board, str(fit),
                    *fit_args], check=True)
    if os.environ.get("UBOOT_ITB"):
        return
    source = ROOT / "build" / "chainload" / board / "source"
    config = (source / ".config").read_text(encoding="utf-8")
    common_settings = (
        "CONFIG_BOOTDELAY=3", "CONFIG_BAUDRATE=1500000", "CONFIG_CMD_NVME=y",
        "CONFIG_CMD_MMC=y", "CONFIG_CMD_USB=y", "CONFIG_USB_STORAGE=y",
        "CONFIG_EFI_LOADER=y", "CONFIG_CMD_BOOTEFI=y",
        "CONFIG_BOOTSTD_FULL=y", "CONFIG_CMD_BOOTFLOW_FULL=y",
        "CONFIG_BOOTMETH_SCRIPT=y", "CONFIG_CMD_SOURCE=y",
        "CONFIG_TEXT_BASE=0x00800000",
        "CONFIG_CUSTOM_SYS_INIT_SP_ADDR=0x03f00000", "CONFIG_NVME_PCI=y",
        'CONFIG_SYS_CONFIG_NAME="evb_rk3568"',
        "CONFIG_BOOTMETH_EXTLINUX=y", "CONFIG_BOOTMETH_EFILOADER=y",
    )
    board_settings = {
        "yy3568": ('CONFIG_DEFAULT_FDT_FILE="rockchip/rk3568-yy3568.dtb"',
                    "CONFIG_CMD_SCSI=y", "CONFIG_CMD_DHCP=y", "CONFIG_CMD_PXE=y",
                    "CONFIG_CMD_NET=y", "CONFIG_CMD_TFTPBOOT=y",
                    "CONFIG_BOOTMETH_EXTLINUX_PXE=y", "CONFIG_CMD_SF=y"),
        "rock3a": (),
    }
    require(uboot_policy["backend"] == "mainline-fit",
            f"{board}: built U-Boot did not use the mainline backend")
    for setting in common_settings + board_settings[board]:
        require(setting in config, f"{board}: built U-Boot config lacks {setting}")
    uboot = (source / "u-boot.bin").read_bytes()
    if board == "yy3568":
        pcie_driver = (source / "drivers" / "pci" /
                       "pcie_dw_rockchip.c").read_text(encoding="utf-8")
        init_port = pcie_driver[pcie_driver.index("rockchip_pcie_init_port"):
                                pcie_driver.index("rockchip_pcie_parse_dt")]
        require(init_port.index("clk_enable_bulk(&priv->clks)") <
                init_port.index("generic_phy_init(&priv->phy)"),
                "YY3568 U-Boot initializes the PCIe PHY before its clocks")
        require("clk_release_bulk(&priv->clks)" not in pcie_driver,
                "YY3568 U-Boot retained unbalanced PCIe clock release paths")
        require(b"boot_targets=mmc1 nvme mmc0 scsi usb pxe dhcp" in uboot,
                "YY3568 U-Boot does not contain its automatic scan environment")
        require(b"bootcmd=bootflow scan -lb nvme" not in uboot,
                "normal YY3568 U-Boot inherited the NVMe-only boot command")
        require(b"boot_targets=mmc1 mmc0 nvme scsi usb pxe dhcp spi" not in uboot,
                "YY3568 U-Boot retained the broad RK3568 default boot targets")

        variant_root = (ROOT / "build" / "chainload" / board / "variants" /
                        "sd_nvme_only")
        variant_fit = variant_root / "u-boot.itb"
        variant_rockchip = variant_root / "u-boot-rockchip.bin"
        variant_source = variant_root / "source"
        require(variant_fit.is_file() and variant_rockchip.is_file(),
                "YY3568 NVMe-only build outputs are incomplete")
        subprocess.run([str(ROOT / "tools" / "chainfit.out"), board,
                        str(variant_fit), *fit_args], check=True)
        variant_config = (variant_source / ".config").read_text(encoding="utf-8")
        for setting in common_settings + board_settings[board] + (
            "CONFIG_USE_BOOTCOMMAND=y",
            'CONFIG_BOOTCOMMAND="bootflow scan -lb nvme"',
        ):
            require(setting in variant_config,
                    f"YY3568 NVMe-only U-Boot config lacks {setting}")
        variant_uboot = (variant_source / "u-boot.bin").read_bytes()
        require(b"bootcmd=bootflow scan -lb nvme" in variant_uboot,
                "YY3568 NVMe-only U-Boot lacks its targeted boot command")
        require((variant_source / "u-boot.dtb").read_bytes() ==
                (source / "u-boot.dtb").read_bytes(),
                "YY3568 NVMe-only variant changed the board hardware DTB")
        rockchip = variant_rockchip.read_bytes()
        require(rockchip == (variant_source / "u-boot-rockchip.bin").read_bytes(),
                "YY3568 NVMe-only payload differs from U-Boot binman output")
        idbloader = (variant_source / "idbloader.img").read_bytes()
        ddr = (ROOT / str(manifest["boot_media"]["ddr"])).read_bytes()
        # Binman expands the SPL entry and patches binman symbols, so the bytes
        # packaged by mkimage intentionally differ from spl/u-boot-spl.bin.
        packaged_spl = rkns_v2_padded_payload(idbloader, 1)
        check_rkns_v2(idbloader, ddr, packaged_spl)
        require(rockchip.startswith(idbloader),
                "YY3568 NVMe-only binman image does not begin with idbloader.img")
        pad_match = re.search(r"^CONFIG_SPL_PAD_TO=(.+)$", variant_config,
                              re.MULTILINE)
        require(pad_match is not None,
                "YY3568 NVMe-only config lacks CONFIG_SPL_PAD_TO")
        fit_offset = int(pad_match.group(1), 0)
        require(rockchip[fit_offset:fit_offset + variant_fit.stat().st_size] ==
                variant_fit.read_bytes(),
                "YY3568 NVMe-only binman image lacks its FIT at SPL_PAD_TO")
    else:
        require(b"boot_targets=nvme mmc1 usb mmc0" in uboot,
                "ROCK 3A U-Boot does not contain its automatic scan environment")
        require(b"boot_targets=mmc1 mmc0 nvme scsi usb pxe dhcp spi" not in uboot,
                "ROCK 3A U-Boot retained the broad RK3568 default boot targets")
    compiled_dts = subprocess.run(
        ["dtc", "-I", "dtb", "-O", "dts", str(source / "u-boot.dtb")],
        check=True, stdout=subprocess.PIPE,
    ).stdout.decode("utf-8")
    identity = "youyeetoo,yy3568" if board == "yy3568" else "radxa,rock3a"
    for marker in (identity, "pcie@fe260000", "pcie@fe280000",
                   "reset-gpios", "jedec,spi-nor"):
        require(marker in compiled_dts, f"{board}: built U-Boot DTB lacks {marker}")
    if board == "yy3568":
        for marker in ("num-lanes = <0x02>;", "regulator-always-on;",
                       "spi-max-frequency = <0x5f5e100>;"):
            require(marker in compiled_dts,
                    f"YY3568 built U-Boot DTB lacks {marker}")
        require("data-lanes =" not in compiled_dts,
                "YY3568 built U-Boot DTB bifurcates the PCIe 3 x2 PHY")
    spi_name = "spi@fe300000"
    spi_node = f"{spi_name} {{"
    require(spi_node in compiled_dts,
            f"{board}: built U-Boot DTB lacks its SPI-NOR controller")
    spi = compiled_dts[compiled_dts.index(spi_node):].split("};", 1)[0]
    require('status = "okay"' in spi,
            f"{board}: built U-Boot DTB does not enable SPI NOR")
    emmc_name = "mmc@fe310000"
    emmc_node = f"{emmc_name} {{"
    require(emmc_node in compiled_dts,
            f"{board}: built U-Boot DTB lacks its enabled eMMC controller")
    emmc = compiled_dts[compiled_dts.index(emmc_node):].split("};", 1)[0]
    require('status = "okay"' in emmc,
            f"{board}: built U-Boot DTB does not enable eMMC")
    require((ROOT / str(artifacts["source"])).is_file(),
            f"{board}: corresponding U-Boot source archive was not generated")
    check_media_images(board, manifest)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--board")
    args = parser.parse_args()
    manifests = load_manifests()
    check_manifests(manifests)
    check_manifest_rejections(manifests)
    check_bl31()
    check_isolation()
    check_overlays()
    if args.offline:
        for board in manifests:
            stage = ROOT / "build" / "chainload" / board / "stage.bin"
            require(stage.is_file() and stage.stat().st_size < 0x40000,
                    f"{board}: offline chainloader stage was not built or is oversized")
    if args.board:
        check_built(args.board)
    print("chainloader checks passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f"chainload check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
