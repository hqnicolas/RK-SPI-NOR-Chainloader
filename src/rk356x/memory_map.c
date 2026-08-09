#include <stdint.h>
#include "firmware.h"
#include "rk356x.h"

#define RK356X_MAX_MAP_ITEMS 32
#define RK356X_MAX_BOUNDARIES (2 * (RK356X_MAX_DRAM_BANKS + 6))

struct ReservedRange {
	uint64_t start;
	uint64_t end;
	uint32_t flags;
};

static void map_add(struct FuMemoryMap *map, uint64_t start, uint64_t end,
		uint32_t flags) {
	if (end <= start)
		return;
	if (map->length && map->items[map->length - 1].end_addr == start &&
		map->items[map->length - 1].flags == flags) {
		map->items[map->length - 1].end_addr = end;
		return;
	}
	if (map->length >= RK356X_MAX_MAP_ITEMS)
		return;
	map->items[map->length].start_addr = start;
	map->items[map->length].end_addr = end;
	map->items[map->length].flags = flags;
	map->items[map->length].pad2 = 0;
	map->length++;
}

static int add_boundary(uint64_t *boundaries, uint32_t *count, uint64_t value) {
	for (uint32_t i = 0; i < *count; i++) {
		if (boundaries[i] == value)
			return 0;
	}
	if (*count >= RK356X_MAX_BOUNDARIES)
		return -1;
	uint32_t position = *count;
	while (position && boundaries[position - 1] > value) {
		boundaries[position] = boundaries[position - 1];
		position--;
	}
	boundaries[position] = value;
	(*count)++;
	return 0;
}

static int interval_in_bank(const struct Rk356xDramLayout *layout,
		uint64_t start, uint64_t end) {
	for (uint32_t i = 0; i < layout->bank_count; i++) {
		uint64_t bank_start = layout->banks[i].start;
		uint64_t bank_end = bank_start + layout->banks[i].size;
		if (bank_end >= bank_start && start >= bank_start && end <= bank_end)
			return 1;
	}
	return 0;
}

void rk356x_build_mem_map(const struct Rk356xDramLayout *layout,
		uint64_t payload_end, struct FuMemoryMap *map) {
	const struct ReservedRange reserved[] = {
		{ 0, 0x00200000, FU_MEM_ATTR_RESERVED },
		{ RK356X_PAYLOAD, payload_end, FU_MEM_ATTR_PAYLOAD },
		{ RK356X_STACK_BOTTOM, RK356X_STACK_TOP, FU_MEM_ATTR_RESERVED },
		{ RK356X_DMA_START, RK356X_DMA_END, FU_MEM_ATTR_RESERVED },
		{ RK356X_FB_START, RK356X_FB_END, FU_MEM_ATTR_FRAMEBUFFER },
	};
	uint64_t boundaries[RK356X_MAX_BOUNDARIES];
	uint32_t boundary_count = 0;

	map->length = 0;
	map->pad = 0;
	for (uint32_t i = 0; i < layout->bank_count; i++) {
		if (add_boundary(boundaries, &boundary_count, layout->banks[i].start) ||
			add_boundary(boundaries, &boundary_count,
				layout->banks[i].start + layout->banks[i].size))
			return;
	}
	for (uint32_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
		if (add_boundary(boundaries, &boundary_count, reserved[i].start) ||
			add_boundary(boundaries, &boundary_count, reserved[i].end))
			return;
	}
	if (add_boundary(boundaries, &boundary_count, RK356X_MMIO_START) ||
		add_boundary(boundaries, &boundary_count, RK356X_PHYS_4G))
		return;

	for (uint32_t i = 0; i + 1 < boundary_count; i++) {
		uint64_t start = boundaries[i];
		uint64_t end = boundaries[i + 1];
		uint32_t flags = 0;

		if (start >= RK356X_MMIO_START && end <= RK356X_PHYS_4G) {
			flags = FU_MEM_ATTR_MMIO;
		} else if (interval_in_bank(layout, start, end)) {
			flags = FU_MEM_ATTR_UNUSED;
			for (uint32_t j = 0;
				j < sizeof(reserved) / sizeof(reserved[0]); j++) {
				if (start >= reserved[j].start && end <= reserved[j].end) {
					flags = reserved[j].flags;
					break;
				}
			}
		}
		if (flags)
			map_add(map, start, end, flags);
	}
}

struct FuMemoryMapItem *rk356x_largest_low_free(struct FuMemoryMap *map) {
	struct FuMemoryMapItem *best = 0;

	for (uint32_t i = 0; i < map->length; i++) {
		struct FuMemoryMapItem *item = &map->items[i];
		if (!(item->flags & FU_MEM_ATTR_UNUSED) ||
			item->start_addr >= RK356X_PHYS_4G ||
			item->end_addr > RK356X_PHYS_4G)
			continue;
		if (!best || item->end_addr - item->start_addr >
			best->end_addr - best->start_addr)
			best = item;
	}
	return best;
}
