// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the SmartSens SC202CS depth camera on the Xiaomi Pad 6 Pro
 * (liuqin).
 *
 * The register tables are the byte-write containers extracted from the
 * shipped sensor-module blob.  They are intentionally kept as data tables;
 * no vendor camera ABI is required by this driver.
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

#define SC202CS_MCLK_FREQ	(19200000UL)
#define SC202CS_VTS_MAX		0xffff
#define SC202CS_EXPOSURE_MIN	8
#define SC202CS_EXPOSURE_MARGIN	8
#define SC202CS_AGAIN_MIN	1
#define SC202CS_AGAIN_MAX	16

struct sc202cs_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct sc202cs_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts;
	struct sc202cs_reg_list reg_list;
};

struct sc202cs_sensor_desc {
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
	const struct sc202cs_reg_list *init;
	const struct sc202cs_reg_list *stream_on;
	const struct sc202cs_reg_list *stream_off;
	const struct sc202cs_mode *modes;
	unsigned int num_modes;
	bool exposure_20bit;
};

struct sc202cs_sensor {
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
	const struct sc202cs_sensor_desc *desc;
	const struct sc202cs_mode *mode;
};

#define to_sc202cs(_sd) container_of(_sd, struct sc202cs_sensor, sd)

static const struct cci_reg_sequence sc202cs_init[] = {
	{ CCI_REG8(0x00fd), 0x01 },
	{ CCI_REG8(0x0018), 0x01 },
	{ CCI_REG8(0x00fe), 0x02 },
	{ CCI_REG8(0x00fd), 0x00 },
	{ CCI_REG8(0x0036), 0x00 },
	{ CCI_REG8(0x0037), 0x0d },
	{ CCI_REG8(0x007b), 0x80 },
	{ CCI_REG8(0x00fd), 0x01 },
	{ CCI_REG8(0x0094), 0x02 },
	{ CCI_REG8(0x0095), 0xc9 },
};

static const struct cci_reg_sequence sc202cs_mode[] = {
	{ CCI_REG8(0x0103), 0x01 },
	{ CCI_REG8(0x0100), 0x00 },
	{ CCI_REG8(0x36e9), 0x80 },
	{ CCI_REG8(0x36ea), 0x0f },
	{ CCI_REG8(0x36eb), 0x0c },
	{ CCI_REG8(0x36ec), 0x01 },
	{ CCI_REG8(0x36ed), 0x28 },
	{ CCI_REG8(0x36e9), 0x20 },
	{ CCI_REG8(0x301f), 0x04 },
	{ CCI_REG8(0x320c), 0x07 },
	{ CCI_REG8(0x320d), 0x78 },
	{ CCI_REG8(0x320e), 0x04 },
	{ CCI_REG8(0x320f), 0xe7 },
	{ CCI_REG8(0x3301), 0xff },
	{ CCI_REG8(0x3304), 0x68 },
	{ CCI_REG8(0x3306), 0x40 },
	{ CCI_REG8(0x3308), 0x08 },
	{ CCI_REG8(0x3309), 0xa8 },
	{ CCI_REG8(0x330b), 0xb0 },
	{ CCI_REG8(0x330c), 0x18 },
	{ CCI_REG8(0x330d), 0xff },
	{ CCI_REG8(0x330e), 0x20 },
	{ CCI_REG8(0x331e), 0x59 },
	{ CCI_REG8(0x331f), 0x99 },
	{ CCI_REG8(0x3333), 0x10 },
	{ CCI_REG8(0x335e), 0x06 },
	{ CCI_REG8(0x335f), 0x08 },
	{ CCI_REG8(0x3364), 0x1f },
	{ CCI_REG8(0x337c), 0x02 },
	{ CCI_REG8(0x337d), 0x0a },
	{ CCI_REG8(0x338f), 0xa0 },
	{ CCI_REG8(0x3390), 0x01 },
	{ CCI_REG8(0x3391), 0x03 },
	{ CCI_REG8(0x3392), 0x1f },
	{ CCI_REG8(0x3393), 0xff },
	{ CCI_REG8(0x3394), 0xff },
	{ CCI_REG8(0x3395), 0xff },
	{ CCI_REG8(0x33a2), 0x04 },
	{ CCI_REG8(0x33ad), 0x0c },
	{ CCI_REG8(0x33b1), 0x20 },
	{ CCI_REG8(0x33b3), 0x38 },
	{ CCI_REG8(0x33f9), 0x40 },
	{ CCI_REG8(0x33fb), 0x48 },
	{ CCI_REG8(0x33fc), 0x0f },
	{ CCI_REG8(0x33fd), 0x1f },
	{ CCI_REG8(0x349f), 0x03 },
	{ CCI_REG8(0x34a6), 0x03 },
	{ CCI_REG8(0x34a7), 0x1f },
	{ CCI_REG8(0x34a8), 0x38 },
	{ CCI_REG8(0x34a9), 0x30 },
	{ CCI_REG8(0x34ab), 0xb0 },
	{ CCI_REG8(0x34ad), 0xb0 },
	{ CCI_REG8(0x34f8), 0x1f },
	{ CCI_REG8(0x34f9), 0x20 },
	{ CCI_REG8(0x3630), 0xa0 },
	{ CCI_REG8(0x3631), 0x92 },
	{ CCI_REG8(0x3632), 0x64 },
	{ CCI_REG8(0x3633), 0x43 },
	{ CCI_REG8(0x3637), 0x49 },
	{ CCI_REG8(0x363a), 0x85 },
	{ CCI_REG8(0x363c), 0x0f },
	{ CCI_REG8(0x3650), 0x31 },
	{ CCI_REG8(0x3670), 0x0d },
	{ CCI_REG8(0x3674), 0xc0 },
	{ CCI_REG8(0x3675), 0xa0 },
	{ CCI_REG8(0x3676), 0xa0 },
	{ CCI_REG8(0x3677), 0x92 },
	{ CCI_REG8(0x3678), 0x96 },
	{ CCI_REG8(0x3679), 0x9a },
	{ CCI_REG8(0x367c), 0x03 },
	{ CCI_REG8(0x367d), 0x0f },
	{ CCI_REG8(0x367e), 0x01 },
	{ CCI_REG8(0x367f), 0x0f },
	{ CCI_REG8(0x3698), 0x83 },
	{ CCI_REG8(0x3699), 0x86 },
	{ CCI_REG8(0x369a), 0x8c },
	{ CCI_REG8(0x369b), 0x94 },
	{ CCI_REG8(0x36a2), 0x01 },
	{ CCI_REG8(0x36a3), 0x03 },
	{ CCI_REG8(0x36a4), 0x07 },
	{ CCI_REG8(0x36ae), 0x0f },
	{ CCI_REG8(0x36af), 0x1f },
	{ CCI_REG8(0x36bd), 0x22 },
	{ CCI_REG8(0x36be), 0x22 },
	{ CCI_REG8(0x36bf), 0x22 },
	{ CCI_REG8(0x36d0), 0x01 },
	{ CCI_REG8(0x370f), 0x02 },
	{ CCI_REG8(0x3721), 0x6c },
	{ CCI_REG8(0x3722), 0x8d },
	{ CCI_REG8(0x3725), 0xc5 },
	{ CCI_REG8(0x3727), 0x14 },
	{ CCI_REG8(0x3728), 0x04 },
	{ CCI_REG8(0x37b7), 0x04 },
	{ CCI_REG8(0x37b8), 0x04 },
	{ CCI_REG8(0x37b9), 0x06 },
	{ CCI_REG8(0x37bd), 0x07 },
	{ CCI_REG8(0x37be), 0x0f },
	{ CCI_REG8(0x3901), 0x02 },
	{ CCI_REG8(0x3903), 0x40 },
	{ CCI_REG8(0x3905), 0x8d },
	{ CCI_REG8(0x3907), 0x00 },
	{ CCI_REG8(0x3908), 0x41 },
	{ CCI_REG8(0x391f), 0x41 },
	{ CCI_REG8(0x3933), 0x80 },
	{ CCI_REG8(0x3934), 0x02 },
	{ CCI_REG8(0x3937), 0x6f },
	{ CCI_REG8(0x393a), 0x01 },
	{ CCI_REG8(0x393d), 0x01 },
	{ CCI_REG8(0x393e), 0xc0 },
	{ CCI_REG8(0x39dd), 0x41 },
	{ CCI_REG8(0x3e00), 0x00 },
	{ CCI_REG8(0x3e01), 0x4d },
	{ CCI_REG8(0x3e02), 0x40 },
	{ CCI_REG8(0x3e09), 0x00 },
	{ CCI_REG8(0x4509), 0x28 },
	{ CCI_REG8(0x450d), 0x61 },
	/*
	 * 0x66 flips both axes; this module needs one: 0x06 (0x60 if the axis
	 * check shows the flip moved - the register is not reachable live).
	 */
	{ CCI_REG8(0x3221), 0x06 },
};

static const struct cci_reg_sequence sc202cs_stream_on[] = {
	{ CCI_REG8(0x0100), 0x01 },
};

static const struct cci_reg_sequence sc202cs_stream_off[] = {
	{ CCI_REG8(0x0100), 0x00 },
};

static const struct sc202cs_mode sc202cs_modes[] = {
	{ .width = 1600, .height = 1200, .hts = 1912, .vts = 1255,
	  .reg_list = { sc202cs_mode, ARRAY_SIZE(sc202cs_mode) } },
};

/*
 * 360 MHz is the rate the SC202CS actually runs at: measured on the vendor
 * stack (CAM_START_PHYDEV for csiphy1 reports Datarate 720000000 =
 * 2 x 360 MHz).  The old 180 MHz value halved it twice, so the CSIPHY was
 * mistuned 2x and no data ever arrived.
 */
static const s64 sc202cs_link_freq_menu[] = { 360000000LL };

static const struct sc202cs_reg_list sc202cs_init_list = {
	.regs = sc202cs_init, .num_regs = ARRAY_SIZE(sc202cs_init),
};
static const struct sc202cs_reg_list sc202cs_stream_on_list = {
	.regs = sc202cs_stream_on, .num_regs = ARRAY_SIZE(sc202cs_stream_on),
};
static const struct sc202cs_reg_list sc202cs_stream_off_list = {
	.regs = sc202cs_stream_off, .num_regs = ARRAY_SIZE(sc202cs_stream_off),
};

static const struct sc202cs_sensor_desc sc202cs_desc = {
	.name = "sc202cs",
	/* One lane: the PLL rate (720 Mbps) equals the internal pixel rate. */
	.lanes = 1,
	.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
	.link_freq = 360000000ULL,
	.link_freq_menu = sc202cs_link_freq_menu,
	.num_link_freqs = ARRAY_SIZE(sc202cs_link_freq_menu),
	.chip_reg = CCI_REG16(0x3107),
	.chip_id = 0xeb52,
	.vts_reg = CCI_REG16(0x320e),
	.exposure_reg = CCI_REG16(0x3e00),
	.again_reg = CCI_REG8(0x3e09),
	.again_min = SC202CS_AGAIN_MIN,
	.again_max = SC202CS_AGAIN_MAX,
	.exposure_20bit = true,
	.init = &sc202cs_init_list,
	.stream_on = &sc202cs_stream_on_list,
	.stream_off = &sc202cs_stream_off_list,
	.modes = sc202cs_modes,
	.num_modes = ARRAY_SIZE(sc202cs_modes),
};

static int sc202cs_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sc202cs_sensor *sensor = container_of(ctrl->handler,
							struct sc202cs_sensor, ctrl_handler);
	const struct sc202cs_mode *mode = sensor->mode;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		u32 max = mode->height + ctrl->val - SC202CS_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(sensor->exposure, SC202CS_EXPOSURE_MIN,
					 max, 1, min_t(u32, mode->vts - SC202CS_EXPOSURE_MARGIN,
						       max));
	}

	if (!pm_runtime_get_if_active(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		if (sensor->desc->exposure_20bit) {
			struct cci_reg_sequence exposure[] = {
				{ CCI_REG8(0x3e00), (ctrl->val >> 12) & 0x0f },
				{ CCI_REG8(0x3e01), (ctrl->val >> 4) & 0xff },
				{ CCI_REG8(0x3e02), (ctrl->val & 0x0f) << 4 },
			};

			ret = cci_multi_reg_write(sensor->regmap, exposure,
						  ARRAY_SIZE(exposure), NULL);
		} else {
			ret = cci_write(sensor->regmap, sensor->desc->exposure_reg,
					ctrl->val, NULL);
		}
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

static const struct v4l2_ctrl_ops sc202cs_ctrl_ops = {
	.s_ctrl = sc202cs_set_ctrl,
};

static int sc202cs_init_controls(struct sc202cs_sensor *sensor)
{
	const struct sc202cs_mode *mode = sensor->mode;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	u32 vblank = mode->vts - mode->height;
	u64 pixel_rate = div_u64(sensor->desc->link_freq * 2 * sensor->desc->lanes, 10);
	int ret;

	v4l2_ctrl_handler_init(hdl, 8);
	v4l2_ctrl_new_int_menu(hdl, &sc202cs_ctrl_ops, V4L2_CID_LINK_FREQ,
			       sensor->desc->num_link_freqs - 1, 0,
			       sensor->desc->link_freq_menu)->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, &sc202cs_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  1, pixel_rate, 1, pixel_rate)->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, &sc202cs_ctrl_ops, V4L2_CID_HBLANK,
			  mode->hts - mode->width, mode->hts - mode->width, 1,
			  mode->hts - mode->width)->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	sensor->vblank = v4l2_ctrl_new_std(hdl, &sc202cs_ctrl_ops,
					  V4L2_CID_VBLANK, vblank,
					  SC202CS_VTS_MAX - mode->height, 1, vblank);
	sensor->exposure = v4l2_ctrl_new_std(hdl, &sc202cs_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     SC202CS_EXPOSURE_MIN,
					     mode->vts - SC202CS_EXPOSURE_MARGIN, 1,
					     mode->vts - SC202CS_EXPOSURE_MARGIN);
	v4l2_ctrl_new_std(hdl, &sc202cs_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  sensor->desc->again_min, sensor->desc->again_max, 1,
			  sensor->desc->again_min);

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &sc202cs_ctrl_ops, &props);
	if (ret)
		goto error_free_hdlr;

	sensor->sd.ctrl_handler = hdl;
	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static void sc202cs_update_format(struct sc202cs_sensor *sensor,
				 const struct sc202cs_mode *mode,
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

static struct v4l2_fract sc202cs_mode_interval(const struct sc202cs_sensor *sensor,
					 const struct sc202cs_mode *mode)
{
	u64 pixel_rate = div_u64(sensor->desc->link_freq * 2 * sensor->desc->lanes,
					 10);

	return (struct v4l2_fract) {
		.numerator = (u32)((u64)mode->hts * mode->vts),
		.denominator = (u32)pixel_rate,
	};
}

static u64 sc202cs_interval_distance(struct v4l2_fract a,
				    struct v4l2_fract b)
{
	s64 distance = (s64)a.numerator * b.denominator -
			       (s64)b.numerator * a.denominator;

	return distance < 0 ? -distance : distance;
}

static void sc202cs_set_mode(struct sc202cs_sensor *sensor,
				    const struct sc202cs_mode *mode)
{
	u32 vblank = mode->vts - mode->height;

	/* Update the mode before changing VBLANK so the control callback uses it. */
	sensor->mode = mode;
	__v4l2_ctrl_modify_range(sensor->vblank, vblank,
				 SC202CS_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(sensor->vblank, vblank);
	__v4l2_ctrl_modify_range(sensor->exposure, SC202CS_EXPOSURE_MIN,
				 mode->vts - SC202CS_EXPOSURE_MARGIN, 1,
				 mode->vts - SC202CS_EXPOSURE_MARGIN);
}

static int sc202cs_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct sc202cs_sensor *sensor = to_sc202cs(sd);
	const struct sc202cs_mode *mode;

	mode = v4l2_find_nearest_size(sensor->desc->modes,
				      sensor->desc->num_modes, width, height,
				      fmt->format.width, fmt->format.height);
	sc202cs_update_format(sensor, mode, &fmt->format);
	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE && sensor->mode != mode)
		sc202cs_set_mode(sensor, mode);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	*v4l2_subdev_state_get_interval(state, 0) =
		sc202cs_mode_interval(sensor, mode);
	return 0;
}

static int sc202cs_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct sc202cs_sensor *sensor = to_sc202cs(sd);

	if (code->index)
		return -EINVAL;
	code->code = sensor->desc->mbus_code;
	return 0;
}

static int sc202cs_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct sc202cs_sensor *sensor = to_sc202cs(sd);
	const struct sc202cs_mode *mode;

	if (fse->index >= sensor->desc->num_modes ||
	    fse->code != sensor->desc->mbus_code)
		return -EINVAL;
	mode = &sensor->desc->modes[fse->index];
	fse->min_width = fse->max_width = mode->width;
	fse->min_height = fse->max_height = mode->height;
	return 0;
}

static int sc202cs_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	struct sc202cs_sensor *sensor = to_sc202cs(sd);
	unsigned int index = 0;
	unsigned int i;

	if (fie->pad || fie->code != sensor->desc->mbus_code)
		return -EINVAL;

	for (i = 0; i < sensor->desc->num_modes; i++) {
		const struct sc202cs_mode *mode = &sensor->desc->modes[i];

		if (mode->width != fie->width || mode->height != fie->height)
			continue;
		if (index++ == fie->index) {
			fie->interval = sc202cs_mode_interval(sensor, mode);
			return 0;
		}
	}

	return -EINVAL;
}

static int sc202cs_get_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_interval *fi)
{
	if (fi->pad)
		return -EINVAL;

	return v4l2_subdev_get_frame_interval(sd, state, fi);
}

static int sc202cs_set_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_interval *fi)
{
	struct sc202cs_sensor *sensor = to_sc202cs(sd);
	const struct v4l2_mbus_framefmt *fmt;
	const struct sc202cs_mode *best = NULL;
	u64 best_distance = U64_MAX;
	unsigned int i;

	if (fi->pad || !fi->interval.numerator || !fi->interval.denominator)
		return -EINVAL;
	fmt = v4l2_subdev_state_get_format(state, 0);
	for (i = 0; i < sensor->desc->num_modes; i++) {
		const struct sc202cs_mode *mode = &sensor->desc->modes[i];
		struct v4l2_fract interval;
		u64 distance;

		if (mode->width != fmt->width || mode->height != fmt->height)
			continue;
		interval = sc202cs_mode_interval(sensor, mode);
		distance = sc202cs_interval_distance(interval, fi->interval);
		if (distance < best_distance) {
			best = mode;
			best_distance = distance;
		}
	}
	if (!best)
		return -EINVAL;

	fi->interval = sc202cs_mode_interval(sensor, best);
	*v4l2_subdev_state_get_interval(state, 0) = fi->interval;
	if (fi->which == V4L2_SUBDEV_FORMAT_ACTIVE && sensor->mode != best)
		sc202cs_set_mode(sensor, best);
	return 0;
}

static int sc202cs_init_state(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state)
{
	struct sc202cs_sensor *sensor = to_sc202cs(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = { .width = sensor->mode->width, .height = sensor->mode->height },
	};
	return sc202cs_set_pad_format(sd, state, &fmt);
}

static int sc202cs_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct sc202cs_sensor *sensor = to_sc202cs(sd);
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

static int sc202cs_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct sc202cs_sensor *sensor = to_sc202cs(sd);
	int ret;

	ret = cci_multi_reg_write(sensor->regmap, sensor->desc->stream_off->regs,
				  sensor->desc->stream_off->num_regs, NULL);
	pm_runtime_put_autosuspend(sensor->dev);
	return ret;
}

static const struct v4l2_subdev_video_ops sc202cs_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};
static const struct v4l2_subdev_pad_ops sc202cs_pad_ops = {
	.set_fmt = sc202cs_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.enum_mbus_code = sc202cs_enum_mbus_code,
	.enum_frame_size = sc202cs_enum_frame_size,
	.enum_frame_interval = sc202cs_enum_frame_interval,
	.get_frame_interval = sc202cs_get_frame_interval,
	.set_frame_interval = sc202cs_set_frame_interval,
	.enable_streams = sc202cs_enable_streams,
	.disable_streams = sc202cs_disable_streams,
};
static const struct v4l2_subdev_ops sc202cs_subdev_ops = {
	.video = &sc202cs_video_ops,
	.pad = &sc202cs_pad_ops,
};
static const struct v4l2_subdev_internal_ops sc202cs_internal_ops = {
	.init_state = sc202cs_init_state,
};
static const struct media_entity_operations sc202cs_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int sc202cs_check_hwcfg(struct sc202cs_sensor *sensor)
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

static int sc202cs_power_on(struct device *dev)
{
	struct sc202cs_sensor *sensor =
		container_of(dev_get_drvdata(dev), struct sc202cs_sensor, sd);
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

static int sc202cs_power_off(struct device *dev)
{
	struct sc202cs_sensor *sensor =
		container_of(dev_get_drvdata(dev), struct sc202cs_sensor, sd);

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

static int sc202cs_identify(struct sc202cs_sensor *sensor)
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

static int sc202cs_probe(struct i2c_client *client)
{
	const struct sc202cs_sensor_desc *desc = device_get_match_data(&client->dev);
	struct sc202cs_sensor *sensor;
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
	v4l2_i2c_subdev_init(&sensor->sd, client, &sc202cs_subdev_ops);
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->regmap),
				     "failed to init CCI\n");
	sensor->mclk = devm_clk_get(sensor->dev, NULL);
	if (IS_ERR(sensor->mclk))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->mclk),
				     "failed to get MCLK\n");
	freq = clk_get_rate(sensor->mclk);
	if (freq != SC202CS_MCLK_FREQ)
		return dev_err_probe(sensor->dev, -EINVAL,
				     "MCLK %lu Hz is not 19.2 MHz\n", freq);
	ret = sc202cs_check_hwcfg(sensor);
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
	ret = sc202cs_power_on(sensor->dev);
	if (ret)
		return ret;
	ret = sc202cs_identify(sensor);
	if (ret)
		goto power_off;
	ret = sc202cs_init_controls(sensor);
	if (ret)
		goto power_off;
	sensor->sd.state_lock = sensor->ctrl_handler.lock;
	sensor->sd.internal_ops = &sc202cs_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.ops = &sc202cs_entity_ops;
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
	sc202cs_power_off(sensor->dev);
	return ret;
}

static void sc202cs_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sc202cs_sensor *sensor = to_sc202cs(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev)) {
		sc202cs_power_off(sensor->dev);
		pm_runtime_set_suspended(sensor->dev);
	}
}

static const struct dev_pm_ops sc202cs_pm_ops = {
	SET_RUNTIME_PM_OPS(sc202cs_power_off, sc202cs_power_on, NULL)
};

static const struct of_device_id sc202cs_of_match[] = {
	{ .compatible = "smartsens,sc202cs", .data = &sc202cs_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, sc202cs_of_match);

static struct i2c_driver sc202cs_i2c_driver = {
	.driver = {
		.name = "sc202cs",
		.of_match_table = sc202cs_of_match,
		.pm = pm_ptr(&sc202cs_pm_ops),
	},
	.probe = sc202cs_probe,
	.remove = sc202cs_remove,
};
module_i2c_driver(sc202cs_i2c_driver);

MODULE_AUTHOR("Liuqin mainline bring-up");
MODULE_DESCRIPTION("SmartSens SC202CS sensor driver");
MODULE_LICENSE("GPL");
