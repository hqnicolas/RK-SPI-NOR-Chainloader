#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

board=${1:-}
medium=${2:-all}
if [[ -z "$board" || ! "$board" =~ ^[a-z0-9][a-z0-9_-]*$ ]]; then
	echo "usage: $0 BOARD [sd|emmc|spi-nor|sd-nvme|all]" >&2
	exit 2
fi

repo=$(cd "$(dirname "$0")/.." && pwd)
manifest="$repo/config/chainload/$board.json"
[[ -f "$manifest" ]] || { echo "chainload-media: board '$board' has no manifest" >&2; exit 2; }
manifest_tool="$repo/tools/chainload-manifest.py"
python3 "$manifest_tool" validate "$board"

json() {
	python3 "$manifest_tool" get "$board" "$1"
}

[[ $(json board) == "$board" ]] || { echo "chainload-media: manifest identity mismatch" >&2; exit 2; }
if [[ "$medium" != all ]]; then
	python3 "$manifest_tool" media-artifact "$board" "$medium" >/dev/null
fi
soc=$(json soc)
ddr="$repo/$(json boot_media.ddr)"
combined="$repo/$(json artifacts.binary)"
idblock="$repo/$(json artifacts.idblock)"
spi="$repo/$(json artifacts.spi_nor)"
sd="$repo/$(json artifacts.image)"
build_root=${CHAINLOAD_BUILD_DIR:-"$repo/build/chainload/$board"}
default_mkimage="$build_root/source/tools/mkimage"
mkimage=${MKIMAGE:-$default_mkimage}

[[ -f "$ddr" ]] || { echo "chainload-media: missing DDR image: $ddr" >&2; exit 2; }
[[ -x "$mkimage" ]] || {
	echo "chainload-media: pinned mkimage is unavailable: $mkimage" >&2
	echo "build pinned U-Boot first or set MKIMAGE=/path/to/tools/mkimage" >&2
	exit 2
}

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/rk-chainload-media.XXXXXX")
cleanup() { rm -rf -- "$tmp_dir"; }
trap cleanup EXIT

require_payload() {
	local payload=$1
	[[ -f "$payload" ]] || {
		echo "chainload-media: missing chainloader image: $payload" >&2
		exit 2
	}
}

build_idblock() {
	local payload=$1
	local output=$2
	local temporary=$3
	require_payload "$payload"
	"$mkimage" -n "$soc" -T rksd -d "$ddr:$payload" "$temporary"
	mv "$temporary" "$output"
	echo "chainload-media: built $(basename "$output")"
}

build_sd_image() {
	local payload=$1
	local output=$2
	local prefix=$3
	local raw="$tmp_dir/$prefix-idblock.img"
	local image="$tmp_dir/$prefix.img"
	require_payload "$payload"
	"$mkimage" -n "$soc" -T rksd -d "$ddr:$payload" "$raw"
	# Rockchip BootROM looks for the RKNS v2 ID block at LBA 64 on SD.
	truncate -s $((64 * 512)) "$image"
	cat "$raw" >> "$image"
	mv "$image" "$output"
	echo "chainload-media: built $(basename "$output")"
}

build_binman_sd_image() {
	local payload=$1
	local output=$2
	local image="$tmp_dir/binman-sd.img"
	require_payload "$payload"
	# Upstream Rockchip and Armbian write the complete binman image at 32 KiB.
	truncate -s $((64 * 512)) "$image"
	cat "$payload" >> "$image"
	mv "$image" "$output"
	echo "chainload-media: built $(basename "$output")"
}

build_spi_image() {
	local payload=$1
	local output=$2
	local temporary=$3
	require_payload "$payload"
	# RK3568 SPI NOR uses the flat RKNS/rksd ID block at flash LBA 0x40.
	# The guarded installer supplies that write offset from the manifest.
	"$mkimage" -n "$soc" -T rksd -d "$ddr:$payload" "$temporary"
	mv "$temporary" "$output"
	echo "chainload-media: built $(basename "$output")"
}

build_variant() {
	local variant=$1
	local variant_medium
	local variant_artifact
	local variant_payload
	variant_medium=$(json "variants.$variant.media")
	variant_artifact="$repo/$(json "variants.$variant.artifact")"
	variant_payload="$build_root/variants/$variant/$(json "variants.$variant.payload")"
	case "$variant_medium" in
		sd-nvme)
			build_binman_sd_image "$variant_payload" "$variant_artifact"
			;;
		*)
			echo "chainload-media: unsupported variant media: $variant_medium" >&2
			exit 2
			;;
	esac
}

case "$medium" in
	sd)
		build_sd_image "$combined" "$sd" normal-sd
		;;
	emmc)
		build_idblock "$combined" "$idblock" "$tmp_dir/idblock.img"
		;;
	spi-nor)
		build_spi_image "$combined" "$spi" "$tmp_dir/spi.img"
		;;
	sd-nvme)
		build_variant sd_nvme_only
		;;
	all)
		build_sd_image "$combined" "$sd" normal-sd
		build_idblock "$combined" "$idblock" "$tmp_dir/idblock.img"
		build_spi_image "$combined" "$spi" "$tmp_dir/spi.img"
		for variant in $(python3 "$manifest_tool" variants "$board"); do
			build_variant "$variant"
		done
		;;
	*)
		echo "chainload-media: board '$board' does not support media '$medium'" >&2
		exit 2
		;;
esac
