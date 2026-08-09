#include <stdint.h>
#include <string.h>
#include "rk356x.h"

#define ATAGS_START 0x001fe000UL
#define ATAGS_END   0x00200000UL
#define ATAG_DDR_MEM 0x54410052U
#define ATAG_DDR_WORDS 48U
#define RK356X_MMIO_SIZE (RK356X_PHYS_4G - RK356X_MMIO_START)

static struct Rk356xDramLayout detected_layout;
static uint8_t layout_ready;

static uint32_t read_le32(const uint8_t *data) {
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
		((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t read_le64(const uint8_t *data) {
	return read_le32(data) | ((uint64_t)read_le32(data + 4) << 32);
}

static uint32_t js_hash(const void *buffer, uint32_t length) {
	const uint8_t *p = buffer;
	uint32_t hash = 0x47c6a7e6;
	for (uint32_t i = 0; i < length; i++)
		hash ^= (hash << 5) + p[i] + (hash >> 2);
	return hash;
}

int rk356x_parse_atags(const void *data, unsigned long length,
		struct Rk356xDramLayout *layout) {
	const uint8_t *cursor = data;
	const uint8_t *end = cursor + length;
	struct Rk356xDramLayout parsed;

	memset(&parsed, 0, sizeof(parsed));
	while ((unsigned long)(end - cursor) >= 8) {
		uint32_t words = read_le32(cursor);
		uint32_t magic = read_le32(cursor + 4);
		unsigned long bytes;

		if (!words)
			return -1;
		if (words < 2 || words > (unsigned long)(end - cursor) / 4)
			return -1;
		bytes = (unsigned long)words * 4;
		if (magic != ATAG_DDR_MEM) {
			cursor += bytes;
			continue;
		}
		if (words != ATAG_DDR_WORDS)
			return -1;

		uint32_t count = read_le32(cursor + 8);
		uint32_t expected_hash = read_le32(cursor + bytes - 4);
		uint64_t total = 0;
		uint64_t previous_end = 0;
		if (!count || count > RK356X_MAX_DRAM_BANKS || !expected_hash)
			return -1;
		if (js_hash(cursor, (uint32_t)bytes - 4) != expected_hash)
			return -1;

		for (uint32_t i = 0; i < count; i++) {
			uint64_t start = read_le64(cursor + 16 + i * 8);
			uint64_t size = read_le64(cursor + 16 + (count + i) * 8);
			uint64_t bank_end = start + size;
			if (!size || bank_end < start || (start | size) & 0xfff)
				return -1;
			if (i && start < previous_end)
				return -1;
			if (total + size < total)
				return -1;
			parsed.banks[i].start = start;
			parsed.banks[i].size = size;
			previous_end = bank_end;
			total += size;
		}
		parsed.bank_count = count;
		parsed.total_bytes = total;
		parsed.atag_bytes = total;
		parsed.atags_valid = 1;
		parsed.atags_accepted = 1;
		parsed.source = RK356X_DRAM_ATAGS;
		*layout = parsed;
		return 0;
	}
	return -1;
}

static void synthesize_geometry(uint64_t capacity,
		struct Rk356xDramLayout *layout) {
	uint64_t low = capacity < RK356X_MMIO_START ? capacity : RK356X_MMIO_START;
	layout->bank_count = 0;
	if (low) {
		layout->banks[layout->bank_count].start = 0;
		layout->banks[layout->bank_count++].size = low;
	}
	if (capacity > RK356X_PHYS_4G) {
		layout->banks[layout->bank_count].start = RK356X_PHYS_4G;
		layout->banks[layout->bank_count++].size = capacity - RK356X_PHYS_4G;
	}
}

static int bank_within_capacity(const struct Rk356xDramBank *bank,
		uint64_t capacity) {
	uint64_t end = bank->start + bank->size;
	uint64_t high_end;
	if (end < bank->start)
		return 0;
	if (bank->start < RK356X_MMIO_START)
		return end <= RK356X_MMIO_START && end <= capacity;
	if (bank->start < RK356X_PHYS_4G || capacity <= RK356X_MMIO_START)
		return 0;
	/* ATAGS may describe DRAM remapped above the 256 MiB MMIO aperture. */
	high_end = capacity + RK356X_MMIO_SIZE;
	if (high_end < capacity)
		return 0;
	return end <= high_end;
}

static int atags_match_geometry(const struct Rk356xDramLayout *atags,
		uint64_t capacity) {
	if (!atags->bank_count || atags->atag_bytes != capacity ||
		atags->banks[0].start > 0x00200000)
		return 0;
	for (uint32_t i = 0; i < atags->bank_count; i++) {
		if (!bank_within_capacity(&atags->banks[i], capacity))
			return 0;
	}
	return 1;
}

void rk356x_select_dram_layout(const void *atags, unsigned long atags_length,
		uint32_t os_reg2, uint32_t os_reg3,
		struct Rk356xDramLayout *layout) {
	struct Rk356xDramLayout parsed;
	uint64_t geometry = rk356x_decode_pmugrf(os_reg2, os_reg3);
	int have_atags;

	memset(&parsed, 0, sizeof(parsed));
	have_atags = atags &&
		rk356x_parse_atags(atags, atags_length, &parsed) == 0;

	memset(layout, 0, sizeof(*layout));
	layout->geometry_bytes = geometry;
	layout->geometry_valid = geometry != 0;
	if (have_atags) {
		layout->atag_bytes = parsed.atag_bytes;
		layout->atags_valid = 1;
	}

	if (geometry && have_atags && atags_match_geometry(&parsed, geometry)) {
		*layout = parsed;
		layout->total_bytes = geometry;
		layout->geometry_bytes = geometry;
		layout->geometry_valid = 1;
		return;
	}
	if (geometry) {
		layout->total_bytes = geometry;
		layout->source = RK356X_DRAM_PMUGRF;
		synthesize_geometry(geometry, layout);
		return;
	}
	if (have_atags) {
		*layout = parsed;
		return;
	}

	layout->total_bytes = 1ULL << 30;
	layout->source = RK356X_DRAM_FALLBACK;
	synthesize_geometry(layout->total_bytes, layout);
}

const struct Rk356xDramLayout *rk356x_get_dram_layout(void) {
	if (!layout_ready) {
		uint32_t os_reg2 = *(volatile uint32_t *)(uintptr_t)
			(RK356X_PMUGRF + 0x208);
		uint32_t os_reg3 = *(volatile uint32_t *)(uintptr_t)
			(RK356X_PMUGRF + 0x20c);
		rk356x_select_dram_layout((const void *)(uintptr_t)ATAGS_START,
			ATAGS_END - ATAGS_START, os_reg2, os_reg3, &detected_layout);
		layout_ready = 1;
	}
	return &detected_layout;
}

uint64_t rk356x_detect_dram(void) {
	return rk356x_get_dram_layout()->total_bytes;
}
