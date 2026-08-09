#include <string.h>
#include "main.h"
#include "rk356x.h"

#define HDMI(reg) ((volatile uint32_t *)(RK356X_HDMI + ((reg) << 2)))

struct Rk356xVideo rk356x_video;

extern int rk356x_display_clocks(uint32_t pixel_clock_khz);
extern void rk356x_vop2_setup(const struct Rk356xVideoMode *mode,
		uint32_t stride);

static const struct Rk356xVideoMode fixed_mode = {
	.pixel_clock_khz = 148500,
	.hactive = 1920,
	.hfront_porch = 88,
	.hsync_len = 44,
	.hback_porch = 148,
	.vactive = 1080,
	.vfront_porch = 4,
	.vsync_len = 5,
	.vback_porch = 36,
	.refresh_hz = 60,
	.flags = RK356X_VIDEO_HSYNC_HIGH | RK356X_VIDEO_VSYNC_HIGH,
};

static uint8_t hdmi_read(unsigned int offset) {
	return (uint8_t)*HDMI(offset);
}

static void hdmi_write(unsigned int offset, uint8_t value) {
	*HDMI(offset) = value;
}

static void hdmi_modify(unsigned int offset, uint8_t mask, uint8_t value) {
	hdmi_write(offset, (hdmi_read(offset) & ~mask) | (value & mask));
}

static void fc_write16(unsigned int low, uint16_t value) {
	hdmi_write(low, value);
	hdmi_write(low + 1, value >> 8);
}

static void hdmi_video_setup(const struct Rk356xVideoMode *m) {
	uint16_t hblank = m->hfront_porch + m->hsync_len + m->hback_porch;
	uint8_t vblank = m->vfront_porch + m->vsync_len + m->vback_porch;
	uint8_t invidconf = 0x98; /* HDCP keepout, HDMI, high DE, progressive RGB. */
	if (m->flags & RK356X_VIDEO_HSYNC_HIGH) invidconf |= 0x20;
	if (m->flags & RK356X_VIDEO_VSYNC_HIGH) invidconf |= 0x40;
	hdmi_write(0x1000, invidconf);
	fc_write16(0x1001, m->hactive);
	fc_write16(0x1003, hblank);
	fc_write16(0x1005, m->vactive);
	hdmi_write(0x1007, vblank);
	fc_write16(0x1008, m->hfront_porch);
	fc_write16(0x100a, m->hsync_len);
	hdmi_write(0x100c, m->vfront_porch);
	hdmi_write(0x100d, m->vsync_len);

	/* RGB888 video sampler and a bypassed 8-bpc packetizer. */
	hdmi_write(0x0200, 0x01);
	hdmi_write(0x0201, 0x07);
	for (unsigned int r = 0x0202; r <= 0x0207; r++) hdmi_write(r, 0);
	hdmi_write(0x0801, 0);
	hdmi_write(0x0802, 0x07);
	hdmi_write(0x0803, 0);
	hdmi_write(0x0804, 0x47);

	/* RGB AVI infoframe for the fixed CEA-861 1080p60 mode (VIC 16). */
	hdmi_write(0x1019, 0);
	hdmi_write(0x101a, 0x20);
	hdmi_write(0x101b, 0x08);
	hdmi_write(0x101c, 16);
	hdmi_write(0x1011, 12);
	hdmi_write(0x1012, 32);
	hdmi_write(0x1013, 1);
	hdmi_write(0x1014, 0x0b);
	hdmi_write(0x1015, 0x16);
	hdmi_write(0x1016, 0x21);
	hdmi_write(0x4004, 0); /* Feed-through, no CSC. */
	hdmi_write(0x4001, 0x7e);
	hdmi_write(0x4001, 0x7c);
}

struct PhyRate {
	uint32_t max_khz;
	uint16_t cpce;
	uint16_t gmp;
	uint16_t sym;
	uint16_t term;
	uint16_t vlev;
};

static const struct PhyRate phy_rates[] = {
	{ 30666, 0x00b3, 0x0000, 0x8009, 0x0004, 0x0272 },
	{ 74250, 0x0072, 0x0001, 0x8009, 0x0004, 0x0272 },
	{ 165000, 0x0051, 0x0002, 0x802b, 0x0004, 0x0209 },
	{ 184000, 0x0051, 0x0002, 0x8039, 0x0005, 0x028d },
	{ 340000, 0x0040, 0x0003, 0x8039, 0x0005, 0x028d },
};

static void phy_i2c_init(void) {
	/* The raw completion/error status uses these programmed polarities. */
	hdmi_write(0x3027, 0x08);
	hdmi_write(0x3028, 0x88);
	hdmi_write(0x0108, 0xff);
}

static int phy_i2c_write(uint8_t address, uint16_t value) {
	hdmi_write(0x0108, 0xff);
	hdmi_write(0x3021, address);
	hdmi_write(0x3022, value >> 8);
	hdmi_write(0x3023, value);
	hdmi_write(0x3026, 0x10);
	for (unsigned int timeout = 0; timeout < 1000; timeout++) {
		uint8_t status = hdmi_read(0x0108) & 3;
		if (status) {
			hdmi_write(0x0108, status);
			if (status & 2)
				return 0;
			debug("HDMI: PHY I2C error address: ", address);
			return -1;
		}
		msleep(1);
	}
	debug("HDMI: PHY I2C timeout address: ", address);
	return -1;
}

static int hdmi_phy_setup(uint32_t clock_khz) {
	const struct PhyRate *rate = 0;
	for (unsigned int i = 0; i < sizeof(phy_rates) / sizeof(phy_rates[0]); i++)
		if (clock_khz <= phy_rates[i].max_khz) { rate = &phy_rates[i]; break; }
	if (!rate)
		return -1;
	phy_i2c_init();

	/* The Gen2 PHY requires the complete setup sequence twice. */
	for (unsigned int pass = 0; pass < 2; pass++) {
		int locked = 0;
		/* Power down first; the second pass must wait for the first lock to drop. */
		hdmi_modify(0x3000, 0x08, 0);
		for (unsigned int timeout = 0; timeout < 5; timeout++) {
			if (!(hdmi_read(0x3004) & 1))
				break;
			msleep(2);
		}
		hdmi_modify(0x3000, 0x10, 0x10);
		hdmi_modify(0x3000, 0x23, 0x22); /* SVSRET, data polarity, PHY I/F. */
		hdmi_write(0x4005, 1);
		hdmi_write(0x4005, 0);
		hdmi_write(0x4007, 1);
		hdmi_modify(0x3001, 0x20, 0x20);
		hdmi_write(0x3020, 0x69);
		hdmi_modify(0x3001, 0x20, 0);
		if (phy_i2c_write(0x06, rate->cpce) ||
			phy_i2c_write(0x15, rate->gmp) ||
			phy_i2c_write(0x10, 0) || phy_i2c_write(0x13, 0) ||
			phy_i2c_write(0x17, 6) || phy_i2c_write(0x19, rate->term) ||
			phy_i2c_write(0x09, rate->sym) ||
			phy_i2c_write(0x0e, rate->vlev) ||
			phy_i2c_write(0x05, 0x8000))
			return -1;
		hdmi_modify(0x3000, 0x08, 0x08);
		hdmi_modify(0x3000, 0x10, 0);
		for (unsigned int timeout = 0; timeout < 5; timeout++) {
			if (hdmi_read(0x3004) & 1) {
				locked = 1;
				break;
			}
			msleep(2);
		}
		if (!locked) {
			debug("HDMI: PHY PLL lock timeout pass: ", pass + 1);
			return -1;
		}
	}
	return 0;
}

int rk356x_display_init(void) {
	memset(&rk356x_video, 0, sizeof(rk356x_video));
	rk356x_video.mode = fixed_mode;
	rk356x_video.stride = (rk356x_video.mode.hactive * 4 + 63) & ~63U;
	if ((uint64_t)rk356x_video.stride * rk356x_video.mode.vactive >
		RK356X_FB_END - RK356X_FB_START)
		return -1;
	if (rk356x_display_clocks(fixed_mode.pixel_clock_khz))
		return -1;
	memset((void *)RK356X_FB_START, 0,
		(uint64_t)rk356x_video.stride * rk356x_video.mode.vactive);
	rk356x_vop2_setup(&rk356x_video.mode, rk356x_video.stride);
	hdmi_video_setup(&rk356x_video.mode);
	if (hdmi_phy_setup(rk356x_video.mode.pixel_clock_khz))
		return -1;
	rk356x_video.active = 1;
	return 0;
}
