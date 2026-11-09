// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony IMX596 image sensor driver for the Xiaomi Pad 6 Pro (Liuqin).
 *
 * The register tables are the byte-write containers extracted from the
 * shipped sensor-module blobs; the link frequency and Bayer order are the
 * values measured on the vendor stack.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define IMX596_MCLK_FREQ	(19200000UL)
#define IMX596_VTS_MAX		0xffff
#define IMX596_EXPOSURE_MIN	8
#define IMX596_EXPOSURE_MARGIN	8

struct imx596_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct imx596_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts;
	struct imx596_reg_list reg_list;
};

struct imx596_sensor_desc {
	const char *name;
	unsigned int lanes;
	u32 mbus_code;
	u64 link_freq;
	const s64 *link_freq_menu;
	unsigned int num_link_freqs;
	unsigned int chip_reg;
	u16 chip_id;
	unsigned int vts_reg;
	unsigned int exposure_reg;
	unsigned int again_reg;
	u32 again_min;
	u32 again_max;
	const struct imx596_reg_list *init;
	const struct imx596_reg_list *stream_on;
	const struct imx596_reg_list *stream_off;
	const struct imx596_mode *modes;
	unsigned int num_modes;
};

struct imx596 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator *vdda;
	struct regulator *vddd;
	struct regulator *vddio;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	const struct imx596_sensor_desc *desc;
	const struct imx596_mode *mode;
};

#define to_imx596(_sd) container_of(_sd, struct imx596, sd)

static const struct cci_reg_sequence imx596_init[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x321c), 0x00 },
	{ CCI_REG8(0x33f0), 0x02 },
	{ CCI_REG8(0x33f1), 0x03 },
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x32c8), 0x01 },
	{ CCI_REG8(0x40a2), 0x01 },
	{ CCI_REG8(0x441f), 0x01 },
	{ CCI_REG8(0x4b20), 0x07 },
	{ CCI_REG8(0x5abe), 0x3a },
	{ CCI_REG8(0x5ac8), 0x3a },
	{ CCI_REG8(0x5ad0), 0x3a },
	{ CCI_REG8(0x5ada), 0x3a },
	{ CCI_REG8(0x5c04), 0x00 },
	{ CCI_REG8(0x5c05), 0x00 },
	{ CCI_REG8(0x5c06), 0x00 },
	{ CCI_REG8(0x5c0b), 0x00 },
	{ CCI_REG8(0x5c0c), 0x00 },
	{ CCI_REG8(0x5c0d), 0x00 },
	{ CCI_REG8(0x5c0e), 0x00 },
	{ CCI_REG8(0x5d55), 0x00 },
	{ CCI_REG8(0x5d56), 0x00 },
	{ CCI_REG8(0x6104), 0x0a },
	{ CCI_REG8(0x6105), 0x0a },
	{ CCI_REG8(0x6107), 0x0a },
	{ CCI_REG8(0x610e), 0x07 },
	{ CCI_REG8(0x610f), 0x07 },
	{ CCI_REG8(0x6110), 0x07 },
	{ CCI_REG8(0x6111), 0x07 },
	{ CCI_REG8(0x6112), 0x07 },
	{ CCI_REG8(0x6113), 0x07 },
	{ CCI_REG8(0x6114), 0x07 },
	{ CCI_REG8(0x6115), 0x07 },
	{ CCI_REG8(0x611c), 0x0b },
	{ CCI_REG8(0x611d), 0x07 },
	{ CCI_REG8(0x611e), 0x09 },
	{ CCI_REG8(0x611f), 0x07 },
	{ CCI_REG8(0x6122), 0x09 },
	{ CCI_REG8(0x612b), 0x07 },
	{ CCI_REG8(0x612d), 0x07 },
	{ CCI_REG8(0x612e), 0x07 },
	{ CCI_REG8(0x612f), 0x07 },
	{ CCI_REG8(0x6131), 0x07 },
	{ CCI_REG8(0x6138), 0x0b },
	{ CCI_REG8(0x6139), 0x07 },
	{ CCI_REG8(0x613a), 0x0b },
	{ CCI_REG8(0x613f), 0x07 },
	{ CCI_REG8(0x6182), 0x01 },
	{ CCI_REG8(0x6183), 0x01 },
	{ CCI_REG8(0x6184), 0x01 },
	{ CCI_REG8(0x6185), 0x01 },
	{ CCI_REG8(0x6186), 0x01 },
	{ CCI_REG8(0x6187), 0x01 },
	{ CCI_REG8(0x6188), 0x01 },
	{ CCI_REG8(0x6189), 0x01 },
	{ CCI_REG8(0x618a), 0x01 },
	{ CCI_REG8(0x618b), 0x01 },
	{ CCI_REG8(0x618c), 0x01 },
	{ CCI_REG8(0x618d), 0x01 },
	{ CCI_REG8(0x618e), 0x01 },
	{ CCI_REG8(0x618f), 0x01 },
	{ CCI_REG8(0x6194), 0x01 },
	{ CCI_REG8(0x6195), 0x01 },
	{ CCI_REG8(0x6197), 0x01 },
	{ CCI_REG8(0x6199), 0x01 },
	{ CCI_REG8(0x619b), 0x01 },
	{ CCI_REG8(0x619d), 0x01 },
	{ CCI_REG8(0x61a2), 0x01 },
	{ CCI_REG8(0x61a3), 0x01 },
	{ CCI_REG8(0x61a5), 0x01 },
	{ CCI_REG8(0x6205), 0x29 },
	{ CCI_REG8(0x6207), 0x29 },
	{ CCI_REG8(0x6209), 0x29 },
	{ CCI_REG8(0x620b), 0x29 },
	{ CCI_REG8(0x620d), 0x29 },
	{ CCI_REG8(0x620f), 0x29 },
	{ CCI_REG8(0x6211), 0x29 },
	{ CCI_REG8(0x6213), 0x29 },
	{ CCI_REG8(0x6215), 0x29 },
	{ CCI_REG8(0x6217), 0x29 },
	{ CCI_REG8(0x6219), 0x29 },
	{ CCI_REG8(0x621b), 0x29 },
	{ CCI_REG8(0x621d), 0x29 },
	{ CCI_REG8(0x621f), 0x29 },
	{ CCI_REG8(0x6221), 0x0a },
	{ CCI_REG8(0x622b), 0x29 },
	{ CCI_REG8(0x622d), 0x29 },
	{ CCI_REG8(0x6231), 0x29 },
	{ CCI_REG8(0x6235), 0x29 },
	{ CCI_REG8(0x6239), 0x29 },
	{ CCI_REG8(0x623d), 0x29 },
	{ CCI_REG8(0x6249), 0x29 },
	{ CCI_REG8(0x624b), 0x29 },
	{ CCI_REG8(0x624f), 0x29 },
	{ CCI_REG8(0x628f), 0x90 },
	{ CCI_REG8(0x6291), 0x90 },
	{ CCI_REG8(0x6293), 0x90 },
	{ CCI_REG8(0x6295), 0x90 },
	{ CCI_REG8(0x6297), 0x90 },
	{ CCI_REG8(0x6299), 0x90 },
	{ CCI_REG8(0x629b), 0x90 },
	{ CCI_REG8(0x629d), 0x90 },
	{ CCI_REG8(0x629f), 0x90 },
	{ CCI_REG8(0x62a1), 0x90 },
	{ CCI_REG8(0x62a3), 0x90 },
	{ CCI_REG8(0x62a5), 0x90 },
	{ CCI_REG8(0x62a7), 0x90 },
	{ CCI_REG8(0x62a9), 0x90 },
	{ CCI_REG8(0x62ab), 0x24 },
	{ CCI_REG8(0x62b5), 0x90 },
	{ CCI_REG8(0x62b7), 0x90 },
	{ CCI_REG8(0x62bb), 0x90 },
	{ CCI_REG8(0x62bf), 0x90 },
	{ CCI_REG8(0x62c3), 0x90 },
	{ CCI_REG8(0x62c7), 0x90 },
	{ CCI_REG8(0x62d3), 0x90 },
	{ CCI_REG8(0x62d5), 0x90 },
	{ CCI_REG8(0x62d9), 0x90 },
	{ CCI_REG8(0x6319), 0x90 },
	{ CCI_REG8(0x631b), 0x90 },
	{ CCI_REG8(0x631f), 0x90 },
	{ CCI_REG8(0x6323), 0x90 },
	{ CCI_REG8(0x636d), 0x28 },
	{ CCI_REG8(0x636e), 0x2a },
	{ CCI_REG8(0x636f), 0x29 },
	{ CCI_REG8(0x6370), 0x29 },
	{ CCI_REG8(0x6371), 0x0e },
	{ CCI_REG8(0x6372), 0x0e },
	{ CCI_REG8(0x6373), 0x28 },
	{ CCI_REG8(0x6374), 0x0f },
	{ CCI_REG8(0x6375), 0x1e },
	{ CCI_REG8(0x6376), 0x0d },
	{ CCI_REG8(0x637d), 0x2a },
	{ CCI_REG8(0x637e), 0x2b },
	{ CCI_REG8(0x637f), 0x2a },
	{ CCI_REG8(0x6380), 0x2a },
	{ CCI_REG8(0x6381), 0x28 },
	{ CCI_REG8(0x6382), 0x29 },
	{ CCI_REG8(0x6383), 0x2a },
	{ CCI_REG8(0x6384), 0x27 },
	{ CCI_REG8(0x6385), 0x1e },
	{ CCI_REG8(0x6386), 0x1e },
	{ CCI_REG8(0x638d), 0x28 },
	{ CCI_REG8(0x638e), 0x32 },
	{ CCI_REG8(0x638f), 0x28 },
	{ CCI_REG8(0x6390), 0x32 },
	{ CCI_REG8(0x6391), 0x29 },
	{ CCI_REG8(0x6392), 0x2a },
	{ CCI_REG8(0x6393), 0x28 },
	{ CCI_REG8(0x6394), 0x29 },
	{ CCI_REG8(0x6395), 0x1d },
	{ CCI_REG8(0x6396), 0x1f },
	{ CCI_REG8(0x639d), 0x28 },
	{ CCI_REG8(0x639e), 0x32 },
	{ CCI_REG8(0x639f), 0x28 },
	{ CCI_REG8(0x63a0), 0x32 },
	{ CCI_REG8(0x63a1), 0x2a },
	{ CCI_REG8(0x63a2), 0x2b },
	{ CCI_REG8(0x63a3), 0x28 },
	{ CCI_REG8(0x63a4), 0x2a },
	{ CCI_REG8(0x63a5), 0x1e },
	{ CCI_REG8(0x63a6), 0x20 },
	{ CCI_REG8(0x63ad), 0x28 },
	{ CCI_REG8(0x63ae), 0x32 },
	{ CCI_REG8(0x63af), 0x28 },
	{ CCI_REG8(0x63b0), 0x1e },
	{ CCI_REG8(0x63b4), 0x28 },
	{ CCI_REG8(0x63b5), 0x32 },
	{ CCI_REG8(0x63b6), 0x28 },
	{ CCI_REG8(0x63b7), 0x1e },
	{ CCI_REG8(0x63c9), 0x01 },
	{ CCI_REG8(0x63da), 0x03 },
	{ CCI_REG8(0x63de), 0x03 },
	{ CCI_REG8(0x63ea), 0x05 },
	{ CCI_REG8(0x63ed), 0x01 },
	{ CCI_REG8(0x63ee), 0x04 },
	{ CCI_REG8(0x63f8), 0x03 },
	{ CCI_REG8(0x63fa), 0x03 },
	{ CCI_REG8(0x63ff), 0x05 },
	{ CCI_REG8(0x6401), 0x04 },
	{ CCI_REG8(0x6403), 0x04 },
	{ CCI_REG8(0x6404), 0x03 },
	{ CCI_REG8(0x6405), 0x04 },
	{ CCI_REG8(0x6406), 0x03 },
	{ CCI_REG8(0x6407), 0x1f },
	{ CCI_REG8(0x6408), 0x0e },
	{ CCI_REG8(0x6409), 0x02 },
	{ CCI_REG8(0x640a), 0x1f },
	{ CCI_REG8(0x640b), 0x0d },
	{ CCI_REG8(0x640c), 0x1f },
	{ CCI_REG8(0x6417), 0x04 },
	{ CCI_REG8(0x6418), 0x03 },
	{ CCI_REG8(0x641a), 0x02 },
	{ CCI_REG8(0x641b), 0x07 },
	{ CCI_REG8(0x641c), 0x0d },
	{ CCI_REG8(0x6427), 0x04 },
	{ CCI_REG8(0x6428), 0x03 },
	{ CCI_REG8(0x642a), 0x03 },
	{ CCI_REG8(0x642b), 0x08 },
	{ CCI_REG8(0x642c), 0x0c },
	{ CCI_REG8(0x6437), 0x01 },
	{ CCI_REG8(0x643a), 0x02 },
	{ CCI_REG8(0x643b), 0x05 },
	{ CCI_REG8(0x643c), 0x07 },
	{ CCI_REG8(0x6446), 0x04 },
	{ CCI_REG8(0x644d), 0x01 },
	{ CCI_REG8(0x6499), 0x01 },
	{ CCI_REG8(0x649a), 0x01 },
	{ CCI_REG8(0x649b), 0x01 },
	{ CCI_REG8(0x649c), 0x01 },
	{ CCI_REG8(0x649d), 0x01 },
	{ CCI_REG8(0x649e), 0x01 },
	{ CCI_REG8(0x649f), 0x01 },
	{ CCI_REG8(0x64a0), 0x01 },
	{ CCI_REG8(0x64a7), 0x01 },
	{ CCI_REG8(0x64a8), 0x01 },
	{ CCI_REG8(0x64a9), 0x01 },
	{ CCI_REG8(0x64aa), 0x01 },
	{ CCI_REG8(0x64ab), 0x01 },
	{ CCI_REG8(0x64ac), 0x01 },
	{ CCI_REG8(0x64ad), 0x01 },
	{ CCI_REG8(0x64ae), 0x01 },
	{ CCI_REG8(0x64b5), 0x01 },
	{ CCI_REG8(0x64b6), 0x01 },
	{ CCI_REG8(0x64b7), 0x01 },
	{ CCI_REG8(0x64b8), 0x01 },
	{ CCI_REG8(0x64b9), 0x01 },
	{ CCI_REG8(0x64ba), 0x01 },
	{ CCI_REG8(0x64bb), 0x01 },
	{ CCI_REG8(0x64bc), 0x01 },
	{ CCI_REG8(0x64c3), 0x01 },
	{ CCI_REG8(0x64c4), 0x01 },
	{ CCI_REG8(0x64c5), 0x01 },
	{ CCI_REG8(0x64c6), 0x01 },
	{ CCI_REG8(0x64c7), 0x01 },
	{ CCI_REG8(0x64c8), 0x01 },
	{ CCI_REG8(0x64c9), 0x01 },
	{ CCI_REG8(0x64ca), 0x01 },
	{ CCI_REG8(0x64d1), 0x01 },
	{ CCI_REG8(0x64d2), 0x01 },
	{ CCI_REG8(0x64d3), 0x01 },
	{ CCI_REG8(0x64d4), 0x01 },
	{ CCI_REG8(0x64d8), 0x01 },
	{ CCI_REG8(0x64d9), 0x01 },
	{ CCI_REG8(0x64da), 0x01 },
	{ CCI_REG8(0x64db), 0x01 },
	{ CCI_REG8(0x651f), 0x1e },
	{ CCI_REG8(0x6520), 0x1e },
	{ CCI_REG8(0x6523), 0x1e },
	{ CCI_REG8(0x6524), 0x1e },
	{ CCI_REG8(0x6526), 0x1e },
	{ CCI_REG8(0x6528), 0x1e },
	{ CCI_REG8(0x6533), 0x1e },
	{ CCI_REG8(0x6534), 0x1e },
	{ CCI_REG8(0x6536), 0x1e },
	{ CCI_REG8(0x6538), 0x1e },
	{ CCI_REG8(0x666f), 0x15 },
	{ CCI_REG8(0x6670), 0x15 },
	{ CCI_REG8(0x6671), 0x15 },
	{ CCI_REG8(0x6672), 0x15 },
	{ CCI_REG8(0x6673), 0x15 },
	{ CCI_REG8(0x6674), 0x15 },
	{ CCI_REG8(0x6675), 0x15 },
	{ CCI_REG8(0x6676), 0x15 },
	{ CCI_REG8(0x6677), 0x15 },
	{ CCI_REG8(0x6678), 0x15 },
	{ CCI_REG8(0x6679), 0x15 },
	{ CCI_REG8(0x667a), 0x15 },
	{ CCI_REG8(0x667b), 0x15 },
	{ CCI_REG8(0x667c), 0x15 },
	{ CCI_REG8(0x6681), 0x15 },
	{ CCI_REG8(0x6682), 0x15 },
	{ CCI_REG8(0x6684), 0x15 },
	{ CCI_REG8(0x6686), 0x15 },
	{ CCI_REG8(0x6688), 0x15 },
	{ CCI_REG8(0x668a), 0x15 },
	{ CCI_REG8(0x668f), 0x15 },
	{ CCI_REG8(0x6690), 0x15 },
	{ CCI_REG8(0x6692), 0x15 },
	{ CCI_REG8(0x66bd), 0x0a },
	{ CCI_REG8(0x66be), 0x0a },
	{ CCI_REG8(0x66ca), 0x0a },
	{ CCI_REG8(0x66cb), 0x0a },
	{ CCI_REG8(0x66d4), 0x0a },
	{ CCI_REG8(0x66d7), 0x0a },
	{ CCI_REG8(0x6a35), 0x36 },
	{ CCI_REG8(0x6a36), 0x0e },
	{ CCI_REG8(0x6a37), 0x36 },
	{ CCI_REG8(0x6a38), 0x36 },
	{ CCI_REG8(0x6a39), 0x36 },
	{ CCI_REG8(0x6a3a), 0x36 },
	{ CCI_REG8(0x6a3b), 0x36 },
	{ CCI_REG8(0x6a3c), 0x0e },
	{ CCI_REG8(0x6a3d), 0x36 },
	{ CCI_REG8(0x6a3e), 0x36 },
	{ CCI_REG8(0x6a3f), 0x36 },
	{ CCI_REG8(0x6a40), 0x36 },
	{ CCI_REG8(0x6a41), 0x36 },
	{ CCI_REG8(0x7910), 0x00 },
	{ CCI_REG8(0x8502), 0x01 },
	{ CCI_REG8(0x8505), 0x00 },
	{ CCI_REG8(0x8605), 0x01 },
	{ CCI_REG8(0x9003), 0x02 },
	{ CCI_REG8(0x9200), 0x86 },
	{ CCI_REG8(0x9201), 0x08 },
	{ CCI_REG8(0x9202), 0x86 },
	{ CCI_REG8(0x9203), 0x09 },
	{ CCI_REG8(0xbc77), 0x4c },
	{ CCI_REG8(0xbc79), 0x7c },
	{ CCI_REG8(0xbc7a), 0x06 },
	{ CCI_REG8(0xbc7b), 0xe8 },
	{ CCI_REG8(0xbc7c), 0x06 },
	{ CCI_REG8(0xbc7d), 0x08 },
	{ CCI_REG8(0xbc7e), 0x0f },
	{ CCI_REG8(0xbc7f), 0x20 },
	{ CCI_REG8(0xbc80), 0x06 },
	{ CCI_REG8(0xbc81), 0xe8 },
	{ CCI_REG8(0xbc82), 0x06 },
	{ CCI_REG8(0xbc83), 0xe8 },
	{ CCI_REG8(0xbc84), 0x06 },
	{ CCI_REG8(0xbc85), 0xe8 },
	{ CCI_REG8(0xbc86), 0x06 },
	{ CCI_REG8(0xbc87), 0xe8 },
	{ CCI_REG8(0xa015), 0x90 },
	{ CCI_REG8(0xa016), 0x90 },
	{ CCI_REG8(0xa017), 0x34 },
	{ CCI_REG8(0xa018), 0xe8 },
	{ CCI_REG8(0xa019), 0x50 },
	{ CCI_REG8(0xa01a), 0x06 },
	{ CCI_REG8(0xa165), 0x10 },
	{ CCI_REG8(0xa16b), 0x10 },
	{ CCI_REG8(0xa171), 0x10 },
	{ CCI_REG8(0xa189), 0xc4 },
	{ CCI_REG8(0xa18f), 0xc4 },
	{ CCI_REG8(0xa195), 0xc4 },
	{ CCI_REG8(0xa19b), 0x0b },
	{ CCI_REG8(0xa1a1), 0x0b },
	{ CCI_REG8(0xa1a7), 0x0b },
	{ CCI_REG8(0xa337), 0x80 },
	{ CCI_REG8(0xa339), 0x80 },
	{ CCI_REG8(0xa33b), 0x80 },
	{ CCI_REG8(0xa51d), 0x10 },
	{ CCI_REG8(0xa520), 0x00 },
	{ CCI_REG8(0xa905), 0x40 },
	{ CCI_REG8(0xa90b), 0x00 },
	{ CCI_REG8(0xaa08), 0xff },
	{ CCI_REG8(0xaa0e), 0xff },
	{ CCI_REG8(0xab11), 0x40 },
	{ CCI_REG8(0xab1d), 0x40 },
	{ CCI_REG8(0xad01), 0x70 },
	{ CCI_REG8(0xad0d), 0x0b },
	{ CCI_REG8(0xad0e), 0x00 },
	{ CCI_REG8(0xad0f), 0x59 },
	{ CCI_REG8(0xad10), 0x00 },
	{ CCI_REG8(0xad11), 0x76 },
	{ CCI_REG8(0xad13), 0x11 },
	{ CCI_REG8(0xad15), 0xaf },
	{ CCI_REG8(0xad17), 0xe7 },
	{ CCI_REG8(0xad19), 0x0f },
	{ CCI_REG8(0xad1a), 0x00 },
	{ CCI_REG8(0xad1b), 0x69 },
	{ CCI_REG8(0xad1c), 0x00 },
	{ CCI_REG8(0xad1d), 0x89 },
};

static const struct cci_reg_sequence imx596_mode_0[] = {
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x1c },
	{ CCI_REG8(0x0343), 0x78 },
	{ CCI_REG8(0x0340), 0x26 },
	{ CCI_REG8(0x0341), 0xf7 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x14 },
	{ CCI_REG8(0x0349), 0x3f },
	{ CCI_REG8(0x034a), 0x0f },
	{ CCI_REG8(0x034b), 0x3f },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x11 },
	{ CCI_REG8(0x0902), 0x0a },
	{ CCI_REG8(0x30d8), 0x00 },
	{ CCI_REG8(0x3200), 0x01 },
	{ CCI_REG8(0x3201), 0x01 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x14 },
	{ CCI_REG8(0x040d), 0x40 },
	{ CCI_REG8(0x040e), 0x0f },
	{ CCI_REG8(0x040f), 0x40 },
	{ CCI_REG8(0x034c), 0x14 },
	{ CCI_REG8(0x034d), 0x40 },
	{ CCI_REG8(0x034e), 0x0f },
	{ CCI_REG8(0x034f), 0x40 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x1c },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xd4 },
	{ CCI_REG8(0x32d3), 0x01 },
	{ CCI_REG8(0x32d5), 0x01 },
	{ CCI_REG8(0x32d6), 0x01 },
	{ CCI_REG8(0x4000), 0x04 },
	{ CCI_REG8(0x4001), 0x04 },
	{ CCI_REG8(0x40a0), 0x01 },
	{ CCI_REG8(0x40a1), 0xf4 },
	{ CCI_REG8(0x40a4), 0x03 },
	{ CCI_REG8(0x40a5), 0x98 },
	{ CCI_REG8(0x40b8), 0x03 },
	{ CCI_REG8(0x40b9), 0x7a },
	{ CCI_REG8(0x41a4), 0x00 },
	{ CCI_REG8(0x0202), 0x26 },
	{ CCI_REG8(0x0203), 0xdf },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3116), 0x01 },
	{ CCI_REG8(0x3117), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x3118), 0x00 },
	{ CCI_REG8(0x3119), 0x00 },
	{ CCI_REG8(0x311a), 0x01 },
	{ CCI_REG8(0x311b), 0x00 },
	{ CCI_REG8(0x3220), 0x01 },
	{ CCI_REG8(0x0b06), 0x01 },
};

static const struct cci_reg_sequence imx596_mode_1[] = {
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x12 },
	{ CCI_REG8(0x0343), 0x24 },
	{ CCI_REG8(0x0340), 0x18 },
	{ CCI_REG8(0x0341), 0x9e },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x14 },
	{ CCI_REG8(0x0349), 0x3f },
	{ CCI_REG8(0x034a), 0x0f },
	{ CCI_REG8(0x034b), 0x3f },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x30d8), 0x00 },
	{ CCI_REG8(0x3200), 0x41 },
	{ CCI_REG8(0x3201), 0x41 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0a },
	{ CCI_REG8(0x040d), 0x20 },
	{ CCI_REG8(0x040e), 0x07 },
	{ CCI_REG8(0x040f), 0xa0 },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x20 },
	{ CCI_REG8(0x034e), 0x07 },
	{ CCI_REG8(0x034f), 0xa0 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x57 },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xd4 },
	{ CCI_REG8(0x32d3), 0x01 },
	{ CCI_REG8(0x32d5), 0x00 },
	{ CCI_REG8(0x32d6), 0x00 },
	{ CCI_REG8(0x4000), 0x06 },
	{ CCI_REG8(0x4001), 0x04 },
	{ CCI_REG8(0x40a0), 0x03 },
	{ CCI_REG8(0x40a1), 0xc6 },
	{ CCI_REG8(0x40a4), 0x03 },
	{ CCI_REG8(0x40a5), 0xc6 },
	{ CCI_REG8(0x40b8), 0x04 },
	{ CCI_REG8(0x40b9), 0x29 },
	{ CCI_REG8(0x41a4), 0x00 },
	{ CCI_REG8(0x0202), 0x18 },
	{ CCI_REG8(0x0203), 0x86 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3116), 0x01 },
	{ CCI_REG8(0x3117), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x3118), 0x00 },
	{ CCI_REG8(0x3119), 0x00 },
	{ CCI_REG8(0x311a), 0x01 },
	{ CCI_REG8(0x311b), 0x00 },
	{ CCI_REG8(0x3220), 0x01 },
	{ CCI_REG8(0x0b06), 0x01 },
};

static const struct cci_reg_sequence imx596_mode_2[] = {
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x12 },
	{ CCI_REG8(0x0343), 0x24 },
	{ CCI_REG8(0x0340), 0x18 },
	{ CCI_REG8(0x0341), 0x9e },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x01 },
	{ CCI_REG8(0x0347), 0xe0 },
	{ CCI_REG8(0x0348), 0x14 },
	{ CCI_REG8(0x0349), 0x3f },
	{ CCI_REG8(0x034a), 0x0d },
	{ CCI_REG8(0x034b), 0x5f },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x30d8), 0x00 },
	{ CCI_REG8(0x3200), 0x41 },
	{ CCI_REG8(0x3201), 0x41 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0a },
	{ CCI_REG8(0x040d), 0x20 },
	{ CCI_REG8(0x040e), 0x05 },
	{ CCI_REG8(0x040f), 0xc0 },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x20 },
	{ CCI_REG8(0x034e), 0x05 },
	{ CCI_REG8(0x034f), 0xc0 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x57 },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xd4 },
	{ CCI_REG8(0x32d3), 0x01 },
	{ CCI_REG8(0x32d5), 0x00 },
	{ CCI_REG8(0x32d6), 0x00 },
	{ CCI_REG8(0x4000), 0x06 },
	{ CCI_REG8(0x4001), 0x04 },
	{ CCI_REG8(0x40a0), 0x03 },
	{ CCI_REG8(0x40a1), 0x70 },
	{ CCI_REG8(0x40a4), 0x00 },
	{ CCI_REG8(0x40a5), 0x14 },
	{ CCI_REG8(0x40b8), 0x04 },
	{ CCI_REG8(0x40b9), 0x7e },
	{ CCI_REG8(0x41a4), 0x00 },
	{ CCI_REG8(0x0202), 0x18 },
	{ CCI_REG8(0x0203), 0x86 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3116), 0x01 },
	{ CCI_REG8(0x3117), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x3118), 0x00 },
	{ CCI_REG8(0x3119), 0x00 },
	{ CCI_REG8(0x311a), 0x01 },
	{ CCI_REG8(0x311b), 0x00 },
	{ CCI_REG8(0x3220), 0x01 },
	{ CCI_REG8(0x0b06), 0x01 },
};

static const struct cci_reg_sequence imx596_mode_3[] = {
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x12 },
	{ CCI_REG8(0x0343), 0x24 },
	{ CCI_REG8(0x0340), 0x0c },
	{ CCI_REG8(0x0341), 0x4f },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x01 },
	{ CCI_REG8(0x0347), 0xe0 },
	{ CCI_REG8(0x0348), 0x14 },
	{ CCI_REG8(0x0349), 0x3f },
	{ CCI_REG8(0x034a), 0x0d },
	{ CCI_REG8(0x034b), 0x5f },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x30d8), 0x00 },
	{ CCI_REG8(0x3200), 0x41 },
	{ CCI_REG8(0x3201), 0x41 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0a },
	{ CCI_REG8(0x040d), 0x20 },
	{ CCI_REG8(0x040e), 0x05 },
	{ CCI_REG8(0x040f), 0xc0 },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x20 },
	{ CCI_REG8(0x034e), 0x05 },
	{ CCI_REG8(0x034f), 0xc0 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x57 },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xd4 },
	{ CCI_REG8(0x32d3), 0x01 },
	{ CCI_REG8(0x32d5), 0x00 },
	{ CCI_REG8(0x32d6), 0x00 },
	{ CCI_REG8(0x4000), 0x06 },
	{ CCI_REG8(0x4001), 0x04 },
	{ CCI_REG8(0x40a0), 0x03 },
	{ CCI_REG8(0x40a1), 0x70 },
	{ CCI_REG8(0x40a4), 0x00 },
	{ CCI_REG8(0x40a5), 0x14 },
	{ CCI_REG8(0x40b8), 0x04 },
	{ CCI_REG8(0x40b9), 0x7e },
	{ CCI_REG8(0x41a4), 0x00 },
	{ CCI_REG8(0x0202), 0x0c },
	{ CCI_REG8(0x0203), 0x37 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3116), 0x01 },
	{ CCI_REG8(0x3117), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x3118), 0x00 },
	{ CCI_REG8(0x3119), 0x00 },
	{ CCI_REG8(0x311a), 0x01 },
	{ CCI_REG8(0x311b), 0x00 },
	{ CCI_REG8(0x3220), 0x01 },
	{ CCI_REG8(0x0b06), 0x01 },
};

static const struct cci_reg_sequence imx596_stream_off[] = {
	{ CCI_REG8(0x0100), 0x00 },
};

static const struct imx596_mode imx596_modes[] = {
	{ .width = 5184, .height = 3904, .hts = 7288, .vts = 9975,
	  .reg_list = { imx596_mode_0, ARRAY_SIZE(imx596_mode_0) } },
	{ .width = 2592, .height = 1952, .hts = 4644, .vts = 6302,
	  .reg_list = { imx596_mode_1, ARRAY_SIZE(imx596_mode_1) } },
	{ .width = 2592, .height = 1472, .hts = 4644, .vts = 6302,
	  .reg_list = { imx596_mode_2, ARRAY_SIZE(imx596_mode_2) } },
	{ .width = 2592, .height = 1472, .hts = 4644, .vts = 3151,
	  .reg_list = { imx596_mode_3, ARRAY_SIZE(imx596_mode_3) } },
};

/*
 * The front preview mode runs at 678.4 MHz: measured on the vendor stack
 * (CAM_START_PHYDEV datarate 1356800000 = 2 x 678.4 MHz).  The old 1.10 GHz
 * default left the CSIPHY mistuned by ~60%, so the sensor streamed but no
 * data ever reached the receiver.  The other rates stay selectable for
 * bring-up.
 */
static const s64 imx596_link_freq_menu[] = { 678400000LL, 1100000000LL, 549000000LL, 2196000000LL };

static const struct imx596_reg_list imx596_init_list = {
	.regs = imx596_init, .num_regs = ARRAY_SIZE(imx596_init),
};
static const struct imx596_reg_list imx596_stream_off_list = {
	.regs = imx596_stream_off, .num_regs = ARRAY_SIZE(imx596_stream_off),
};
static const struct cci_reg_sequence imx596_stream_on[] = {
	/*
	 * The vendor container streams with the 16-bit write 0x0100 = 0x0103
	 * ({0x01, 0x03}).  The 8-bit form wrote 0x03 into the mode-select
	 * register instead, so the sensor was never told to stream: STREAMON
	 * succeeded and no data ever arrived.
	 */
	{ CCI_REG16(0x0100), 0x0103 },
};
static const struct imx596_reg_list imx596_stream_on_list = {
	.regs = imx596_stream_on, .num_regs = ARRAY_SIZE(imx596_stream_on),
};

static const struct imx596_sensor_desc imx596_desc = {
	.name = "imx596",
	.lanes = 4,
	/*
	 * Measured on the module: a red target comes out blue, i.e. the
	 * readout is BGGR while this first assumed RGGB (the vendor
	 * container carries no order).  The vendor HAL's table says BGGR
	 * too.
	 */
	.mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10,
	/* 678.4 MHz, measured on the vendor stack (CAM_START_PHYDEV). */
	.link_freq = 678400000ULL,
	.link_freq_menu = imx596_link_freq_menu,
	.num_link_freqs = ARRAY_SIZE(imx596_link_freq_menu),
	.chip_reg = CCI_REG16(0x0016),
	.chip_id = 0x0596,
	.vts_reg = CCI_REG16(0x0340),
	.exposure_reg = CCI_REG16(0x0202),
	.again_reg = CCI_REG16(0x0204),
	.again_min = 0,
	.again_max = 1008,
	.init = &imx596_init_list,
	.stream_on = &imx596_stream_on_list,
	.stream_off = &imx596_stream_off_list,
	.modes = imx596_modes,
	.num_modes = ARRAY_SIZE(imx596_modes),
};

static int imx596_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx596 *sensor = container_of(ctrl->handler,
							struct imx596, ctrl_handler);
	const struct imx596_mode *mode = sensor->mode;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		u32 max = mode->height + ctrl->val - IMX596_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(sensor->exposure, IMX596_EXPOSURE_MIN,
					 max, 1, min_t(u32, mode->vts - IMX596_EXPOSURE_MARGIN,
						       max));
	}

	if (!pm_runtime_get_if_active(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(sensor->regmap, sensor->desc->exposure_reg,
				ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(sensor->regmap, sensor->desc->again_reg,
				ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(sensor->regmap, sensor->desc->vts_reg,
				mode->height + ctrl->val, NULL);
		break;
	default:
		break;
	}

	pm_runtime_put(sensor->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx596_ctrl_ops = {
	.s_ctrl = imx596_set_ctrl,
};

static int imx596_init_controls(struct imx596 *sensor)
{
	const struct imx596_mode *mode = sensor->mode;
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	u32 vblank = mode->vts - mode->height;
	u64 pixel_rate = div_u64(sensor->desc->link_freq * 2 * sensor->desc->lanes, 10);
	int ret;

	v4l2_ctrl_handler_init(hdl, 8);
	v4l2_ctrl_new_int_menu(hdl, &imx596_ctrl_ops, V4L2_CID_LINK_FREQ,
			       sensor->desc->num_link_freqs - 1, 0,
			       sensor->desc->link_freq_menu)->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, &imx596_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  1, pixel_rate, 1, pixel_rate)->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, &imx596_ctrl_ops, V4L2_CID_HBLANK,
			  mode->hts - mode->width, mode->hts - mode->width, 1,
			  mode->hts - mode->width)->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	sensor->vblank = v4l2_ctrl_new_std(hdl, &imx596_ctrl_ops,
					  V4L2_CID_VBLANK, vblank,
					  IMX596_VTS_MAX - mode->height, 1, vblank);
	sensor->exposure = v4l2_ctrl_new_std(hdl, &imx596_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX596_EXPOSURE_MIN,
					     mode->vts - IMX596_EXPOSURE_MARGIN, 1,
					     mode->vts - IMX596_EXPOSURE_MARGIN);
	v4l2_ctrl_new_std(hdl, &imx596_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  sensor->desc->again_min, sensor->desc->again_max, 1,
			  sensor->desc->again_min);

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &imx596_ctrl_ops, &props);
	if (ret)
		goto error_free_hdlr;

	ret = hdl->error;
	if (ret)
		goto error_free_hdlr;
	sensor->sd.ctrl_handler = hdl;
	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static void imx596_update_format(struct imx596 *sensor,
				 const struct imx596_mode *mode,
				 struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = sensor->desc->mbus_code;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static struct v4l2_fract imx596_mode_interval(const struct imx596 *sensor,
					 const struct imx596_mode *mode)
{
	u64 pixel_rate = div_u64(sensor->desc->link_freq * 2 * sensor->desc->lanes,
					 10);

	return (struct v4l2_fract) {
		.numerator = (u32)((u64)mode->hts * mode->vts),
		.denominator = (u32)pixel_rate,
	};
}

static u64 imx596_interval_distance(struct v4l2_fract a,
				    struct v4l2_fract b)
{
	s64 distance = (s64)a.numerator * b.denominator -
			       (s64)b.numerator * a.denominator;

	return distance < 0 ? -distance : distance;
}

static void imx596_set_mode(struct imx596 *sensor,
				    const struct imx596_mode *mode)
{
	u32 vblank = mode->vts - mode->height;

	/* Update the mode before changing VBLANK so the control callback uses it. */
	sensor->mode = mode;
	__v4l2_ctrl_modify_range(sensor->vblank, vblank,
				 IMX596_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(sensor->vblank, vblank);
	__v4l2_ctrl_modify_range(sensor->exposure, IMX596_EXPOSURE_MIN,
				 mode->vts - IMX596_EXPOSURE_MARGIN, 1,
				 mode->vts - IMX596_EXPOSURE_MARGIN);
}

static int imx596_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx596 *sensor = to_imx596(sd);
	const struct imx596_mode *mode;

	mode = v4l2_find_nearest_size(sensor->desc->modes,
				      sensor->desc->num_modes, width, height,
				      fmt->format.width, fmt->format.height);
	imx596_update_format(sensor, mode, &fmt->format);
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE && sensor->mode != mode)
		imx596_set_mode(sensor, mode);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	*v4l2_subdev_state_get_interval(state, 0) =
		imx596_mode_interval(sensor, mode);
	return 0;
}

static int imx596_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx596 *sensor = to_imx596(sd);

	if (code->index)
		return -EINVAL;
	code->code = sensor->desc->mbus_code;
	return 0;
}

static int imx596_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx596 *sensor = to_imx596(sd);
	const struct imx596_mode *mode;

	if (fse->index >= sensor->desc->num_modes ||
	    fse->code != sensor->desc->mbus_code)
		return -EINVAL;
	mode = &sensor->desc->modes[fse->index];
	fse->min_width = fse->max_width = mode->width;
	fse->min_height = fse->max_height = mode->height;
	return 0;
}

static int imx596_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	struct imx596 *sensor = to_imx596(sd);
	unsigned int index = 0;
	unsigned int i;

	if (fie->pad || fie->code != sensor->desc->mbus_code)
		return -EINVAL;

	for (i = 0; i < sensor->desc->num_modes; i++) {
		const struct imx596_mode *mode = &sensor->desc->modes[i];

		if (mode->width != fie->width || mode->height != fie->height)
			continue;
		if (index++ == fie->index) {
			fie->interval = imx596_mode_interval(sensor, mode);
			return 0;
		}
	}

	return -EINVAL;
}

static int imx596_get_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_interval *fi)
{
	if (fi->pad)
		return -EINVAL;

	return v4l2_subdev_get_frame_interval(sd, state, fi);
}

static int imx596_set_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_interval *fi)
{
	struct imx596 *sensor = to_imx596(sd);
	const struct v4l2_mbus_framefmt *fmt;
	const struct imx596_mode *best = NULL;
	u64 best_distance = U64_MAX;
	unsigned int i;

	if (fi->pad || !fi->interval.numerator || !fi->interval.denominator)
		return -EINVAL;
	fmt = v4l2_subdev_state_get_format(state, 0);
	for (i = 0; i < sensor->desc->num_modes; i++) {
		const struct imx596_mode *mode = &sensor->desc->modes[i];
		struct v4l2_fract interval;
		u64 distance;

		if (mode->width != fmt->width || mode->height != fmt->height)
			continue;
		interval = imx596_mode_interval(sensor, mode);
		distance = imx596_interval_distance(interval, fi->interval);
		if (distance < best_distance) {
			best = mode;
			best_distance = distance;
		}
	}
	if (!best)
		return -EINVAL;

	fi->interval = imx596_mode_interval(sensor, best);
	*v4l2_subdev_state_get_interval(state, 0) = fi->interval;
	if (fi->which == V4L2_SUBDEV_FORMAT_ACTIVE && sensor->mode != best)
		imx596_set_mode(sensor, best);
	return 0;
}

static int imx596_init_state(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state)
{
	struct imx596 *sensor = to_imx596(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = { .width = sensor->mode->width, .height = sensor->mode->height },
	};
	return imx596_set_pad_format(sd, state, &fmt);
}

static int imx596_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx596 *sensor = to_imx596(sd);
	int ret;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret < 0)
		return ret;
	cci_multi_reg_write(sensor->regmap, sensor->desc->init->regs,
				sensor->desc->init->num_regs, &ret);
	cci_multi_reg_write(sensor->regmap, sensor->mode->reg_list.regs,
				sensor->mode->reg_list.num_regs, &ret);
	if (!ret)
		ret = __v4l2_ctrl_handler_setup(sensor->sd.ctrl_handler);
	if (!ret)
		cci_multi_reg_write(sensor->regmap, sensor->desc->stream_on->regs,
				 sensor->desc->stream_on->num_regs, &ret);
	if (ret) {
		dev_err(sensor->dev, "failed to start %s: %d\n", sensor->desc->name, ret);
		pm_runtime_put_autosuspend(sensor->dev);
	}
	return ret;
}

static int imx596_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx596 *sensor = to_imx596(sd);
	int ret;

	ret = cci_multi_reg_write(sensor->regmap, sensor->desc->stream_off->regs,
				  sensor->desc->stream_off->num_regs, NULL);
	pm_runtime_put_autosuspend(sensor->dev);
	return ret;
}

static const struct v4l2_subdev_video_ops imx596_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};
static const struct v4l2_subdev_pad_ops imx596_pad_ops = {
	.set_fmt = imx596_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.enum_mbus_code = imx596_enum_mbus_code,
	.enum_frame_size = imx596_enum_frame_size,
	.enum_frame_interval = imx596_enum_frame_interval,
	.get_frame_interval = imx596_get_frame_interval,
	.set_frame_interval = imx596_set_frame_interval,
	.enable_streams = imx596_enable_streams,
	.disable_streams = imx596_disable_streams,
};
static const struct v4l2_subdev_ops imx596_subdev_ops = {
	.video = &imx596_video_ops,
	.pad = &imx596_pad_ops,
};
static const struct v4l2_subdev_internal_ops imx596_internal_ops = {
	.init_state = imx596_init_state,
};
static const struct media_entity_operations imx596_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx596_check_hwcfg(struct imx596 *sensor)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	unsigned long bitmap;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(sensor->dev), NULL);
	if (!ep)
		return -EINVAL;
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;
	if (bus_cfg.bus.mipi_csi2.num_data_lanes != sensor->desc->lanes) {
		dev_err(sensor->dev, "expected %u CSI-2 lanes, got %u\n",
			sensor->desc->lanes, bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto out;
	}
	ret = v4l2_link_freq_to_bitmap(sensor->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       sensor->desc->link_freq_menu,
				       sensor->desc->num_link_freqs, &bitmap);
out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int imx596_power_on(struct device *dev)
{
	struct imx596 *sensor =
		container_of(dev_get_drvdata(dev), struct imx596, sd);
	int ret;

	if (sensor->vddd) {
		ret = regulator_enable(sensor->vddd);
		if (ret)
			return ret;
	}
	if (sensor->vdda) {
		ret = regulator_enable(sensor->vdda);
		if (ret)
			goto disable_vddd;
	}
	if (sensor->vddio) {
		ret = regulator_enable(sensor->vddio);
		if (ret)
			goto disable_vdda;
	}
	ret = clk_prepare_enable(sensor->mclk);
	if (ret)
		goto disable_vddio;
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 11 * USEC_PER_MSEC);
	return 0;
disable_vddio:
	if (sensor->vddio)
		regulator_disable(sensor->vddio);
disable_vdda:
	if (sensor->vdda)
		regulator_disable(sensor->vdda);
disable_vddd:
	if (sensor->vddd)
		regulator_disable(sensor->vddd);
	return ret;
}

static int imx596_power_off(struct device *dev)
{
	struct imx596 *sensor =
		container_of(dev_get_drvdata(dev), struct imx596, sd);

	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	clk_disable_unprepare(sensor->mclk);
	if (sensor->vddio)
		regulator_disable(sensor->vddio);
	if (sensor->vdda)
		regulator_disable(sensor->vdda);
	if (sensor->vddd)
		regulator_disable(sensor->vddd);
	return 0;
}

static int imx596_identify(struct imx596 *sensor)
{
	u64 id;
	int ret, i;

	/*
	 * The module's i2c comes up marginal on this board: the same probe that
	 * succeeds on one boot times out (-110/-EIO) on another, and the failed
	 * device then keeps the whole async notifier incomplete (no subdev nodes
	 * at all).  Retry the chip-id read and power-cycle the sensor's reset
	 * line between attempts: the part sometimes comes up with its i2c state
	 * machine dead and only a reset pulse revives it - a plain retry within
	 * the same boot never recovers.
	 */
	for (i = 0; i < 12; i++) {
		ret = cci_read(sensor->regmap, sensor->desc->chip_reg, &id, NULL);
		if (!ret)
			break;
		/* Reset pulse after the 1st, 5th and 9th failed attempt. */
		if (i == 0 || i == 4 || i == 8) {
			gpiod_set_value_cansleep(sensor->reset_gpio, 1);
			usleep_range(5 * USEC_PER_MSEC, 6 * USEC_PER_MSEC);
			gpiod_set_value_cansleep(sensor->reset_gpio, 0);
		}
		usleep_range(20 * USEC_PER_MSEC, 25 * USEC_PER_MSEC);
	}
	if (ret)
		return ret;
	if (id != sensor->desc->chip_id) {
		dev_err(sensor->dev, "%s chip id 0x%04llx != 0x%04x\n",
			sensor->desc->name, id, sensor->desc->chip_id);
		return -ENODEV;
	}
	return 0;
}

static int imx596_probe(struct i2c_client *client)
{
	const struct imx596_sensor_desc *desc = device_get_match_data(&client->dev);
	struct imx596 *sensor;
	unsigned long freq;
	int ret;

	if (!desc)
		return -ENODEV;
	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->dev = &client->dev;
	sensor->desc = desc;
	sensor->mode = &desc->modes[0];
	v4l2_i2c_subdev_init(&sensor->sd, client, &imx596_subdev_ops);
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->regmap),
				     "failed to init CCI\n");
	sensor->mclk = devm_clk_get(sensor->dev, NULL);
	if (IS_ERR(sensor->mclk))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->mclk),
				     "failed to get MCLK\n");
	freq = clk_get_rate(sensor->mclk);
	if (freq != IMX596_MCLK_FREQ)
		return dev_err_probe(sensor->dev, -EINVAL,
				     "MCLK %lu Hz is not 19.2 MHz\n", freq);
	ret = imx596_check_hwcfg(sensor);
	if (ret)
		return dev_err_probe(sensor->dev, ret, "invalid CSI-2 endpoint\n");
	sensor->reset_gpio = devm_gpiod_get_optional(sensor->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO\n");
	sensor->vdda = devm_regulator_get_optional(sensor->dev, "vdda");
	if (IS_ERR(sensor->vdda)) {
		ret = PTR_ERR(sensor->vdda);
		if (ret != -ENODEV)
			return dev_err_probe(sensor->dev, ret, "failed to get vdda\n");
		sensor->vdda = NULL;
	}
	sensor->vddd = devm_regulator_get_optional(sensor->dev, "vddd");
	if (IS_ERR(sensor->vddd)) {
		ret = PTR_ERR(sensor->vddd);
		if (ret != -ENODEV)
			return dev_err_probe(sensor->dev, ret, "failed to get vddd\n");
		sensor->vddd = NULL;
	}
	sensor->vddio = devm_regulator_get_optional(sensor->dev, "vddio");
	if (IS_ERR(sensor->vddio)) {
		ret = PTR_ERR(sensor->vddio);
		if (ret != -ENODEV)
			return dev_err_probe(sensor->dev, ret, "failed to get vddio\n");
		sensor->vddio = NULL;
	}
	ret = imx596_power_on(sensor->dev);
	if (ret)
		return ret;
	ret = imx596_identify(sensor);
	if (ret)
		goto power_off;
	ret = imx596_init_controls(sensor);
	if (ret)
		goto power_off;
	sensor->sd.state_lock = sensor->ctrl_handler.lock;
	sensor->sd.internal_ops = &imx596_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.ops = &imx596_entity_ops;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto ctrl_free;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto entity_cleanup;
	pm_runtime_set_active(sensor->dev);
	pm_runtime_enable(sensor->dev);
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto subdev_cleanup;
	pm_runtime_set_autosuspend_delay(sensor->dev, 1000);
	pm_runtime_use_autosuspend(sensor->dev);
	pm_runtime_idle(sensor->dev);
	return 0;
subdev_cleanup:
	v4l2_subdev_cleanup(&sensor->sd);
	pm_runtime_disable(sensor->dev);
entity_cleanup:
	media_entity_cleanup(&sensor->sd.entity);
ctrl_free:
	v4l2_ctrl_handler_free(sensor->sd.ctrl_handler);
power_off:
	imx596_power_off(sensor->dev);
	return ret;
}

static void imx596_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx596 *sensor = to_imx596(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev)) {
		imx596_power_off(sensor->dev);
		pm_runtime_set_suspended(sensor->dev);
	}
}

static const struct dev_pm_ops imx596_pm_ops = {
	SET_RUNTIME_PM_OPS(imx596_power_off, imx596_power_on, NULL)
};

static const struct of_device_id imx596_of_match[] = {
	{ .compatible = "sony,imx596", .data = &imx596_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, imx596_of_match);

static struct i2c_driver imx596_i2c_driver = {
	.driver = {
		.name = "imx596",
		.of_match_table = imx596_of_match,
		.pm = pm_ptr(&imx596_pm_ops),
	},
	.probe = imx596_probe,
	.remove = imx596_remove,
};
module_i2c_driver(imx596_i2c_driver);

MODULE_AUTHOR("Liuqin mainline bring-up");
MODULE_DESCRIPTION("Sony IMX596 sensor");
MODULE_LICENSE("GPL");
