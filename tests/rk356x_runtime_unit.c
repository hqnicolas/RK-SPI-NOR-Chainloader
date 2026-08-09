#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "firmware.h"
#include "rk356x.h"

#define ATAG_DDR_MEM 0x54410052U
#define ATAG_BYTES 192U

struct TestMemoryMap {
	uint32_t length;
	uint32_t pad;
	struct FuMemoryMapItem items[32];
};

static char log_output[160];
static unsigned int log_length;

void uart_chr(int value) {
	if (log_length + 1 < sizeof(log_output)) {
		log_output[log_length++] = (char)value;
		log_output[log_length] = '\0';
	}
}

static void put_le32(uint8_t *data, uint32_t value) {
	data[0] = (uint8_t)value;
	data[1] = (uint8_t)(value >> 8);
	data[2] = (uint8_t)(value >> 16);
	data[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *data, uint64_t value) {
	put_le32(data, (uint32_t)value);
	put_le32(data + 4, (uint32_t)(value >> 32));
}

static uint32_t js_hash(const uint8_t *data, uint32_t length) {
	uint32_t hash = 0x47c6a7e6;
	for (uint32_t i = 0; i < length; i++)
		hash ^= (hash << 5) + data[i] + (hash >> 2);
	return hash;
}

static void make_atag(uint8_t data[ATAG_BYTES], const uint64_t *starts,
		const uint64_t *sizes, uint32_t count) {
	memset(data, 0, ATAG_BYTES);
	put_le32(data, ATAG_BYTES / 4);
	put_le32(data + 4, ATAG_DDR_MEM);
	put_le32(data + 8, count);
	put_le32(data + 12, 1);
	for (uint32_t i = 0; i < count; i++) {
		put_le64(data + 16 + i * 8, starts[i]);
		put_le64(data + 16 + (count + i) * 8, sizes[i]);
	}
	put_le32(data + ATAG_BYTES - 4, js_hash(data, ATAG_BYTES - 4));
}

static void pmugrf_fixture(uint32_t type, uint32_t row,
		uint32_t *os_reg2, uint32_t *os_reg3) {
	uint32_t raw = row - 13;
	*os_reg2 = (type << 13) | (1U << 9) | ((raw & 3) << 6) |
		(1U << 2);
	*os_reg3 = (2U << 28) | (((raw >> 2) & 1) << 5);
}

static void expect_format(uint64_t value, int base, const char *expected) {
	char buffer[66];
	memset(buffer, 0xa5, sizeof(buffer));
	assert(rk356x_format_u64(value, buffer, sizeof(buffer), base) >= 0);
	assert(strcmp(buffer, expected) == 0);
}

static void test_formatting(void) {
	expect_format(0, 16, "0");
	expect_format(1ULL << 31, 2,
		"10000000000000000000000000000000");
	expect_format(1ULL << 32, 2,
		"100000000000000000000000000000000");
	expect_format(1ULL << 32, 16, "100000000");
	expect_format(1ULL << 33, 16, "200000000");
	expect_format(UINT64_MAX, 16, "ffffffffffffffff");
	expect_format(UINT64_MAX, 10, "18446744073709551615");
	expect_format(UINT64_MAX, 2,
		"1111111111111111111111111111111111111111111111111111111111111111");
	{
		char buffer[4] = "bad";
		assert(rk356x_format_u64(123, buffer, sizeof(buffer), 1) < 0);
		assert(buffer[0] == '\0');
		assert(rk356x_format_u64(UINT64_MAX, buffer, sizeof(buffer), 16) < 0);
		assert(buffer[0] == '\0');
	}
	memset(log_output, 0, sizeof(log_output));
	log_length = 0;
	rk356x_debug_u64("DDR bytes: ", UINT64_MAX);
	assert(strstr(log_output, "DDR bytes: ffffffffffffffff") != 0);
	assert(strstr(log_output,
		"1111111111111111111111111111111111111111111111111111111111111111") != 0);
}

static void test_pmugrf(void) {
	static const uint64_t sizes[] = {
		1ULL << 30, 2ULL << 30, 4ULL << 30, 8ULL << 30,
	};
	uint32_t os_reg2;
	uint32_t os_reg3;

	for (uint32_t i = 0; i < 4; i++) {
		pmugrf_fixture(0, 15 + i, &os_reg2, &os_reg3);
		assert(rk356x_decode_pmugrf(os_reg2, os_reg3) == sizes[i]);
		pmugrf_fixture(7, 16 + i, &os_reg2, &os_reg3);
		assert(rk356x_decode_pmugrf(os_reg2, os_reg3) == sizes[i]);
	}
	assert(rk356x_decode_pmugrf(3U << 2, 0) == 0);
}

static void test_atags_and_selection(void) {
	uint8_t atag[ATAG_BYTES];
	struct Rk356xDramLayout layout;
	uint32_t os_reg2;
	uint32_t os_reg3;
	const uint64_t starts[] = { 0, RK356X_PHYS_4G };
	const uint64_t sizes[] = { RK356X_MMIO_START, 0x10000000 };
	const uint64_t bad_starts[] = { RK356X_PHYS_4G };
	const uint64_t bad_sizes[] = { 4ULL << 30 };

	make_atag(atag, starts, sizes, 2);
	assert(rk356x_parse_atags(atag, sizeof(atag), &layout) == 0);
	assert(rk356x_parse_atags(atag, 100, &layout) != 0);
	assert(layout.total_bytes == 4ULL << 30);
	assert(layout.bank_count == 2);
	assert(layout.banks[1].start == RK356X_PHYS_4G);

	pmugrf_fixture(7, 18, &os_reg2, &os_reg3);
	rk356x_select_dram_layout(atag, sizeof(atag), os_reg2, os_reg3, &layout);
	assert(layout.source == RK356X_DRAM_ATAGS);
	assert(layout.total_bytes == 4ULL << 30);
	assert(layout.atags_accepted && layout.geometry_valid);

	/* A 4 GiB bank starting at 4 GiB has an 8 GiB endpoint, not 8 GiB RAM. */
	make_atag(atag, bad_starts, bad_sizes, 1);
	rk356x_select_dram_layout(atag, sizeof(atag), os_reg2, os_reg3, &layout);
	assert(layout.source == RK356X_DRAM_PMUGRF);
	assert(layout.total_bytes == 4ULL << 30);
	assert(layout.atags_valid && !layout.atags_accepted);
	{
		const uint64_t short_start[] = { 0 };
		const uint64_t short_size[] = { 2ULL << 30 };
		make_atag(atag, short_start, short_size, 1);
		rk356x_select_dram_layout(atag, sizeof(atag), os_reg2, os_reg3,
			&layout);
		assert(layout.source == RK356X_DRAM_PMUGRF);
		assert(layout.total_bytes == 4ULL << 30);
		assert(!layout.atags_accepted);
	}

	/* With no geometry, a validated ATAG is still usable. */
	make_atag(atag, starts, sizes, 2);
	rk356x_select_dram_layout(atag, sizeof(atag), 0, 0, &layout);
	assert(layout.source == RK356X_DRAM_ATAGS);
	assert(layout.total_bytes == 4ULL << 30);

	/* Hash, count, integer-overflow, ordering and overlap validation. */
	atag[ATAG_BYTES - 1] ^= 1;
	assert(rk356x_parse_atags(atag, sizeof(atag), &layout) != 0);
	make_atag(atag, starts, sizes, 0);
	assert(rk356x_parse_atags(atag, sizeof(atag), &layout) != 0);
	make_atag(atag, starts, sizes, 2);
	put_le32(atag, 47);
	assert(rk356x_parse_atags(atag, sizeof(atag), &layout) != 0);
	make_atag(atag, starts, sizes, 2);
	put_le32(atag + 8, 11);
	put_le32(atag + ATAG_BYTES - 4, js_hash(atag, ATAG_BYTES - 4));
	assert(rk356x_parse_atags(atag, sizeof(atag), &layout) != 0);
	{
		const uint64_t overflow_start[] = { UINT64_MAX - 0xfff };
		const uint64_t overflow_size[] = { 0x2000 };
		make_atag(atag, overflow_start, overflow_size, 1);
		assert(rk356x_parse_atags(atag, sizeof(atag), &layout) != 0);
	}
	{
		const uint64_t overlap_start[] = { 0, 0x20000000 };
		const uint64_t overlap_size[] = { 0x40000000, 0x40000000 };
		make_atag(atag, overlap_start, overlap_size, 2);
		assert(rk356x_parse_atags(atag, sizeof(atag), &layout) != 0);
	}
	{
		const uint64_t reversed_start[] = { 0x40000000, 0 };
		const uint64_t reversed_size[] = { 0x10000000, 0x10000000 };
		make_atag(atag, reversed_start, reversed_size, 2);
		assert(rk356x_parse_atags(atag, sizeof(atag), &layout) != 0);
	}

	memset(atag, 0, sizeof(atag));
	rk356x_select_dram_layout(atag, sizeof(atag), 0, 0, &layout);
	assert(layout.source == RK356X_DRAM_FALLBACK);
	assert(layout.total_bytes == 1ULL << 30);
}

static int has_range(const struct FuMemoryMap *map, uint64_t start,
		uint64_t end, uint32_t flags) {
	for (uint32_t i = 0; i < map->length; i++) {
		if (map->items[i].start_addr == start && map->items[i].end_addr == end &&
			map->items[i].flags == flags)
			return 1;
	}
	return 0;
}

static void test_memory_maps(void) {
	struct Rk356xDramLayout layout;
	struct TestMemoryMap storage;
	struct FuMemoryMap *map = (struct FuMemoryMap *)&storage;
	struct FuMemoryMapItem *chunk;

	memset(&layout, 0, sizeof(layout));
	layout.total_bytes = 2ULL << 30;
	layout.bank_count = 1;
	layout.banks[0].size = layout.total_bytes;
	rk356x_build_mem_map(&layout, 0x00b00000, map);
	assert(has_range(map, 0, 0x00200000, FU_MEM_ATTR_RESERVED));
	assert(has_range(map, RK356X_PAYLOAD, 0x00b00000, FU_MEM_ATTR_PAYLOAD));
	assert(has_range(map, RK356X_MMIO_START, RK356X_PHYS_4G,
		FU_MEM_ATTR_MMIO));
	chunk = rk356x_largest_low_free(map);
	assert(chunk && chunk->end_addr <= RK356X_PHYS_4G);

	memset(&layout, 0, sizeof(layout));
	layout.total_bytes = 8ULL << 30;
	layout.bank_count = 2;
	layout.banks[0].size = RK356X_MMIO_START;
	layout.banks[1].start = RK356X_PHYS_4G;
	layout.banks[1].size = 4ULL << 30;
	rk356x_build_mem_map(&layout, 0x00b00000, map);
	assert(has_range(map, RK356X_PHYS_4G, 8ULL << 30, FU_MEM_ATTR_UNUSED));
	chunk = rk356x_largest_low_free(map);
	assert(chunk && chunk->start_addr < RK356X_PHYS_4G);
	assert(chunk->end_addr <= RK356X_PHYS_4G);
}

int main(void) {
	test_formatting();
	test_pmugrf();
	test_atags_and_selection();
	test_memory_maps();
	puts("RK356x runtime unit tests passed");
	return 0;
}
