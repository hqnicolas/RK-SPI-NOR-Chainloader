#include <stdint.h>
#include "rk356x.h"

static uint32_t
field32(uint32_t value, unsigned int shift, uint32_t mask)
{
	return (value >> shift) & mask;
}

static unsigned int
decode_rows(uint32_t raw)
{
	return raw == 7U ? 12U : 13U + (unsigned int)raw;
}

static int
add_u64(uint64_t *total, uint64_t value)
{
	if (value > ~(uint64_t)0 - *total)
		return 0;

	*total += value;
	return 1;
}

uint64_t
rk356x_decode_pmugrf(uint32_t os_reg2, uint32_t os_reg3)
{
	const uint32_t version = field32(os_reg3, 28U, 0xfU);
	const unsigned int channel_count =
		1U + (unsigned int)field32(os_reg2, 12U, 0x1U);
	uint32_t dram_type = field32(os_reg2, 13U, 0x7U);
	uint64_t total_bytes = 0;
	unsigned int channel;

	if (version >= 3U)
		dram_type |= field32(os_reg3, 12U, 0x3U) << 3U;

	if (dram_type > 10U)
		return 0;

	for (channel = 0; channel < channel_count; ++channel) {
		const uint32_t lane = field32(os_reg2, channel * 16U,
						     0xffffU);
		const unsigned int ranks =
			1U + (unsigned int)field32(lane, 11U, 0x1U);
		const unsigned int cs0_columns =
			9U + (unsigned int)field32(lane, 9U, 0x3U);
		const uint32_t width_code = field32(lane, 2U, 0x3U);
		const unsigned int bank_exponent =
			dram_type == 9U
				? 3U + (unsigned int)field32(lane, 8U, 0x1U)
				: 3U - (unsigned int)field32(lane, 8U, 0x1U);
		unsigned int bank_group_exponent = 0;
		unsigned int cs0_rows;
		unsigned int cs1_rows;
		unsigned int cs1_columns;
		unsigned int exponent;
		uint64_t channel_bytes;

		if (width_code == 3U)
			return 0;

		if (version < 2U) {
			cs0_rows = 13U +
				(unsigned int)field32(lane, 6U, 0x3U);
			cs1_rows = 13U +
				(unsigned int)field32(lane, 4U, 0x3U);
			cs1_columns = cs0_columns;
		} else {
			const uint32_t cs0_raw =
				(field32(os_reg3, 5U + 2U * channel, 0x1U)
				 << 2U) |
				field32(lane, 6U, 0x3U);
			const uint32_t cs1_raw =
				(field32(os_reg3, 4U + 2U * channel, 0x1U)
				 << 2U) |
				field32(lane, 4U, 0x3U);

			cs0_rows = decode_rows(cs0_raw);
			cs1_rows = decode_rows(cs1_raw);
			cs1_columns = 9U +
				(unsigned int)field32(os_reg3, 2U * channel,
						      0x3U);
		}

		if (dram_type == 0U && version != 3U)
			bank_group_exponent =
				field32(lane, 0U, 0x3U) == 2U ? 2U : 1U;

		exponent = cs0_rows + cs0_columns + bank_exponent +
			   bank_group_exponent + (2U - width_code);
		if (exponent < 20U || exponent > 35U)
			return 0;

		channel_bytes = (uint64_t)1U << exponent;

		if (ranks == 2U) {
			int row_delta = (int)cs0_rows - (int)cs1_rows;
			int column_delta =
				(int)cs0_columns - (int)cs1_columns;
			int delta = row_delta + column_delta;
			uint64_t second_rank_bytes;

			if (delta < 0 || delta > 8)
				return 0;

			second_rank_bytes =
				channel_bytes / ((uint64_t)1U << (unsigned int)delta);
			if (!add_u64(&channel_bytes, second_rank_bytes))
				return 0;
		}

		if (field32(os_reg2, 30U + channel, 0x1U) != 0U)
			channel_bytes = (channel_bytes / 4U) * 3U;

		if (!add_u64(&total_bytes, channel_bytes))
			return 0;
	}

	if (total_bytes < ((uint64_t)1U << 30U) ||
	    total_bytes > ((uint64_t)1U << 35U))
		return 0;

	return total_bytes;
}
