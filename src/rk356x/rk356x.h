#ifndef RK356X_H
#define RK356X_H

#include <stdint.h>

struct FuMemoryMap;
struct FuMemoryMapItem;

#define RK356X_VIDEO_HSYNC_HIGH (1U << 0)
#define RK356X_VIDEO_VSYNC_HIGH (1U << 1)

#define RK356X_PMUGRF       0xfdc20000UL
#define RK356X_GRF          0xfdc60000UL
#define RK356X_SGRF         0xfdd18000UL
#define RK356X_PMUCRU       0xfdd00000UL
#define RK356X_CRU          0xfdd20000UL
#define RK356X_PMU          0xfdd90000UL
#define RK356X_UART2        0xfe660000UL
#define RK356X_VOP2         0xfe040000UL
#define RK356X_HDMI         0xfe0a0000UL
#define RK356X_OHCI0        0xfd840000UL
#define RK356X_OHCI1        0xfd8c0000UL

#define RK356X_PAYLOAD      0x00a00000UL
#define RK356X_STACK_BOTTOM 0x07ff0000UL
#define RK356X_STACK_TOP    0x08000000UL
#define RK356X_DMA_START    0x08000000UL
#define RK356X_DMA_END      0x08400000UL
#define RK356X_DMA_LIMIT    0x08300000UL
#define RK356X_DTB          0x08300000UL
#define RK356X_SHARED       0x083f0000UL
#define RK356X_FB_START     0x10000000UL
#define RK356X_FB_END       0x12000000UL
#define RK356X_MMIO_START   0xf0000000UL
#define RK356X_PHYS_4G      0x100000000ULL

#define RK356X_MAX_DRAM_BANKS 10

enum Rk356xDramSource {
	RK356X_DRAM_FALLBACK,
	RK356X_DRAM_PMUGRF,
	RK356X_DRAM_ATAGS,
};

struct Rk356xDramBank {
	uint64_t start;
	uint64_t size;
};

struct Rk356xDramLayout {
	uint64_t total_bytes;
	uint64_t atag_bytes;
	uint64_t geometry_bytes;
	struct Rk356xDramBank banks[RK356X_MAX_DRAM_BANKS];
	uint32_t bank_count;
	uint8_t source;
	uint8_t atags_valid;
	uint8_t atags_accepted;
	uint8_t geometry_valid;
};

struct Rk356xGpioPin {
	uint8_t bank;
	uint8_t pin;
	uint8_t active_high;
	uint8_t valid;
};

struct Rk356xBoard {
	const char *name;
	const char *vendor;
	const char *product;
	const char *soc;
	const char *connector_notes;
	struct Rk356xGpioPin leds[2];
	struct Rk356xGpioPin usb_vbus[2];
	uint8_t ohci_mask;
};

struct Rk356xVideoMode {
	uint32_t pixel_clock_khz;
	uint16_t hactive;
	uint16_t hfront_porch;
	uint16_t hsync_len;
	uint16_t hback_porch;
	uint16_t vactive;
	uint16_t vfront_porch;
	uint16_t vsync_len;
	uint16_t vback_porch;
	uint16_t refresh_hz;
	uint16_t flags;
};

struct Rk356xVideo {
	struct Rk356xVideoMode mode;
	uint32_t stride;
	uint8_t active;
};

extern const struct Rk356xBoard *rk356x_board;
extern struct Rk356xVideo rk356x_video;

void rk356x_enable_uart(void);
void rk356x_setup_security(void);
int rk356x_enable_vo_domain(void);
void rk356x_gpio_output(struct Rk356xGpioPin pin, int asserted);
uint64_t rk356x_decode_pmugrf(uint32_t os_reg2, uint32_t os_reg3);
int rk356x_parse_atags(const void *data, unsigned long length,
		struct Rk356xDramLayout *layout);
void rk356x_select_dram_layout(const void *atags, unsigned long atags_length,
		uint32_t os_reg2, uint32_t os_reg3,
		struct Rk356xDramLayout *layout);
const struct Rk356xDramLayout *rk356x_get_dram_layout(void);
uint64_t rk356x_detect_dram(void);
void rk356x_build_mem_map(const struct Rk356xDramLayout *layout,
		uint64_t payload_end, struct FuMemoryMap *map);
struct FuMemoryMapItem *rk356x_largest_low_free(struct FuMemoryMap *map);
int rk356x_format_u64(uint64_t value, char *buffer,
		unsigned int buffer_size, unsigned int base);
void rk356x_debug_u64(const char *label, uint64_t value);
void rk356x_set_dtb(const void *data, unsigned int size);
int rk356x_display_init(void);
void rk356x_usb_init(void);
int rk356x_board_entry(const struct Rk356xBoard *board,
		const void *dtb, unsigned int dtb_size);

#endif
