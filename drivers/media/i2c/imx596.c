// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony IMX596 Camera Sensor Driver
 * Tailored for Xiaomi Pad 6 Pro (liuqin) front camera.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "imx596_regs.h"

#define IMX596_REG_CHIP_ID              CCI_REG16(0x0016)
#define IMX596_CHIP_ID                  0x0596

#define IMX596_REG_MODE_SELECT          CCI_REG8(0x0100)
#define IMX596_MODE_STANDBY             0x00
#define IMX596_MODE_STREAMING           0x01

#define IMX596_REG_ORIENTATION          CCI_REG8(0x0101)
#define IMX596_REG_GROUP_HOLD           CCI_REG8(0x0104)

#define IMX596_REG_EXPOSURE             CCI_REG16(0x0202)
#define IMX596_EXPOSURE_MIN             8
#define IMX596_EXPOSURE_STEP            1
#define IMX596_EXPOSURE_MARGIN          24

#define IMX596_REG_AGAIN                CCI_REG16(0x0204)
#define IMX596_AGAIN_MIN                0
#define IMX596_AGAIN_MAX                960
#define IMX596_AGAIN_STEP               1
#define IMX596_AGAIN_DEFAULT            0

#define IMX596_REG_VTS                  CCI_REG16(0x0340)
#define IMX596_VTS_MAX                  0xffff

#define IMX596_REG_HTS                  CCI_REG16(0x0342)

#define IMX596_REG_TEST_PATTERN         CCI_REG8(0x0601)

#define IMX596_WIDTH                    2592
#define IMX596_HEIGHT                   1952
#define IMX596_HTS                      4644
#define IMX596_VTS                      6302
#define IMX596_LINK_FREQ                678400000ULL
#define IMX596_PIXEL_RATE               542720000ULL
#define IMX596_DATA_LANES               4

struct imx596 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator *dvdd;
	struct regulator *avdd;
	struct regulator *vio;

	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;

	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *again;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;

	bool streaming;
};

static inline struct imx596 *to_imx596(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx596, sd);
}

/* Formats covering horizontal and vertical flip */
static const u32 imx596_mbus_formats[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,     /* 0: normal (v=0, h=0) */
	MEDIA_BUS_FMT_SGRBG10_1X10,     /* 1: H-FLIP (v=0, h=1) */
	MEDIA_BUS_FMT_SGBRG10_1X10,     /* 2: V-FLIP (v=1, h=0) */
	MEDIA_BUS_FMT_SBGGR10_1X10,     /* 3: HV-FLIP (v=1, h=1) */
};

static u32 imx596_get_format_code(struct imx596 *imx596)
{
	int vflip = 1;
	int hflip = 1;

	if (imx596 && imx596->vflip)
		vflip = imx596->vflip->cur.val || imx596->vflip->val;
	if (imx596 && imx596->hflip)
		hflip = imx596->hflip->cur.val || imx596->hflip->val;

	return imx596_mbus_formats[(vflip ? 2 : 0) | (hflip ? 1 : 0)];
}

static const s64 imx596_link_freq_menu[] = {
	IMX596_LINK_FREQ,
};

static const char * const imx596_test_pattern_menu[] = {
	"Disabled",
	"100% Color Bars",
};

static int imx596_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx596 *imx596 = to_imx596(sd);
	int ret;

	/* Assert reset initially */
	if (imx596->reset_gpio)
		gpiod_set_value_cansleep(imx596->reset_gpio, 1);

	/* 1. DVDD (1.1V, GPIO 74) */
	if (imx596->dvdd) {
		ret = regulator_enable(imx596->dvdd);
		if (ret) {
			dev_err(dev, "failed to enable dvdd: %d\n", ret);
			return ret;
		}
		usleep_range(1000, 2000);
	}

	/* 2. VIO (1.8V, pm8350c_l1) */
	if (imx596->vio) {
		ret = regulator_enable(imx596->vio);
		if (ret) {
			dev_err(dev, "failed to enable vio: %d\n", ret);
			goto err_disable_dvdd;
		}
		usleep_range(1000, 2000);
	}

	/* 3. AVDD (2.9V, GPIO 119, ramp 100ms) */
	if (imx596->avdd) {
		ret = regulator_enable(imx596->avdd);
		if (ret) {
			dev_err(dev, "failed to enable avdd: %d\n", ret);
			goto err_disable_vio;
		}
		usleep_range(2000, 3000);
	}

	/* 4. MCLK (19.2 MHz) */
	ret = clk_prepare_enable(imx596->mclk);
	if (ret) {
		dev_err(dev, "failed to enable mclk: %d\n", ret);
		goto err_disable_avdd;
	}
	usleep_range(1000, 2000);

	/* 5. Release reset (deassert active-low GPIO) */
	if (imx596->reset_gpio)
		gpiod_set_value_cansleep(imx596->reset_gpio, 0);

	/* Sensor stabilization delay */
	msleep(15);

	dev_info(dev, "power-on complete (dvdd=%d, vio=%d, avdd=%d, mclk=%lu)\n",
		 imx596->dvdd != NULL, imx596->vio != NULL, imx596->avdd != NULL,
		 clk_get_rate(imx596->mclk));
	return 0;

err_disable_avdd:
	if (imx596->avdd)
		regulator_disable(imx596->avdd);
err_disable_vio:
	if (imx596->vio)
		regulator_disable(imx596->vio);
err_disable_dvdd:
	if (imx596->dvdd)
		regulator_disable(imx596->dvdd);

	return ret;
}

static int imx596_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx596 *imx596 = to_imx596(sd);

	/* 1. Assert reset: force pin low */
	if (imx596->reset_gpio) {
		gpiod_set_value_cansleep(imx596->reset_gpio, 1);
		gpiod_set_raw_value_cansleep(imx596->reset_gpio, 0);
	}

	/* 2. Disable MCLK */
	clk_disable_unprepare(imx596->mclk);
	usleep_range(1000, 2000);

	/* 3. Disable AVDD */
	if (imx596->avdd)
		regulator_disable(imx596->avdd);

	/* 4. Disable VIO */
	if (imx596->vio)
		regulator_disable(imx596->vio);
	usleep_range(1000, 2000);

	/* 5. Disable DVDD */
	if (imx596->dvdd)
		regulator_disable(imx596->dvdd);

	dev_info(dev, "safe power-down complete\n");
	return 0;
}

static int imx596_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx596 *imx596 = container_of(ctrl->handler, struct imx596,
					     ctrl_handler);
	s64 exposure_max;
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		exposure_max = IMX596_HEIGHT + ctrl->val - IMX596_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(imx596->exposure,
					 imx596->exposure->minimum,
					 exposure_max,
					 imx596->exposure->step,
					 imx596->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(imx596->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx596->regmap, IMX596_REG_EXPOSURE, ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(imx596->regmap, IMX596_REG_AGAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(imx596->regmap, IMX596_REG_VTS,
				IMX596_HEIGHT + ctrl->val, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(imx596->regmap, IMX596_REG_TEST_PATTERN,
				ctrl->val ? 0x02 : 0x00, NULL);
		break;
	case V4L2_CID_VFLIP:
	case V4L2_CID_HFLIP:
		ret = cci_write(imx596->regmap, IMX596_REG_ORIENTATION,
				(imx596->vflip->val ? 2 : 0) |
				(imx596->hflip->val ? 1 : 0), NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx596->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx596_ctrl_ops = {
	.s_ctrl = imx596_set_ctrl,
};

static int imx596_init_controls(struct imx596 *imx596)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx596->ctrl_handler;
	s64 hblank, vblank, exposure_max;
	struct v4l2_fwnode_device_properties props;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 10);

	imx596->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx596_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(imx596_link_freq_menu) - 1,
						   0, imx596_link_freq_menu);
	if (imx596->link_freq)
		imx596->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx596->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx596_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       0, IMX596_PIXEL_RATE, 1,
					       IMX596_PIXEL_RATE);
	if (imx596->pixel_rate)
		imx596->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hblank = IMX596_HTS - IMX596_WIDTH;
	imx596->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx596_ctrl_ops,
					   V4L2_CID_HBLANK, hblank,
					   hblank, 1, hblank);
	if (imx596->hblank)
		imx596->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = IMX596_VTS - IMX596_HEIGHT;
	imx596->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx596_ctrl_ops,
					   V4L2_CID_VBLANK,
					   IMX596_EXPOSURE_MARGIN,
					   IMX596_VTS_MAX - IMX596_HEIGHT, 1,
					   vblank);

	imx596->again = v4l2_ctrl_new_std(ctrl_hdlr, &imx596_ctrl_ops,
					  V4L2_CID_ANALOGUE_GAIN,
					  IMX596_AGAIN_MIN, IMX596_AGAIN_MAX,
					  IMX596_AGAIN_STEP, IMX596_AGAIN_DEFAULT);

	exposure_max = IMX596_VTS - IMX596_EXPOSURE_MARGIN;
	imx596->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx596_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX596_EXPOSURE_MIN,
					     exposure_max,
					     IMX596_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx596_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx596_test_pattern_menu) - 1,
				     0, 0, imx596_test_pattern_menu);

	imx596->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx596_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 1);
	if (imx596->hflip)
		imx596->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx596->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx596_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 1);
	if (imx596->vflip)
		imx596->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	ret = v4l2_fwnode_device_parse(imx596->dev, &props);
	if (!ret)
		v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx596_ctrl_ops, &props);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		v4l2_ctrl_handler_free(ctrl_hdlr);
		return ret;
	}

	imx596->sd.ctrl_handler = ctrl_hdlr;
	return 0;
}

static int imx596_start_streaming(struct imx596 *imx596)
{
	int ret = 0;

	ret = pm_runtime_resume_and_get(imx596->dev);
	if (ret < 0)
		return ret;

	/* 1. Global initialization table (364 entries) */
	cci_multi_reg_write(imx596->regmap, imx596_global_regs,
			    ARRAY_SIZE(imx596_global_regs), &ret);
	if (ret) {
		dev_err(imx596->dev, "failed to write global regs: %d\n", ret);
		goto err_rpm_put;
	}

	/* 2. Mode 2592x1952 table (75 entries) */
	cci_multi_reg_write(imx596->regmap, imx596_2592x1952_regs,
			    ARRAY_SIZE(imx596_2592x1952_regs), &ret);
	if (ret) {
		dev_err(imx596->dev, "failed to write mode regs: %d\n", ret);
		goto err_rpm_put;
	}

	/* 3. Setup initial controls */
	ret = __v4l2_ctrl_handler_setup(imx596->sd.ctrl_handler);
	if (ret) {
		dev_err(imx596->dev, "failed to setup controls: %d\n", ret);
		goto err_rpm_put;
	}

	/* 4. Start streaming */
	ret = cci_write(imx596->regmap, IMX596_REG_MODE_SELECT,
			IMX596_MODE_STREAMING, NULL);
	if (ret) {
		dev_err(imx596->dev, "failed to start streaming: %d\n", ret);
		goto err_rpm_put;
	}

	imx596->streaming = true;
	return 0;

err_rpm_put:
	pm_runtime_put_autosuspend(imx596->dev);
	return ret;
}

static int imx596_stop_streaming(struct imx596 *imx596)
{
	int ret;

	ret = cci_write(imx596->regmap, IMX596_REG_MODE_SELECT,
			IMX596_MODE_STANDBY, NULL);
	if (ret)
		dev_err(imx596->dev, "failed to stop streaming: %d\n", ret);

	imx596->streaming = false;
	pm_runtime_mark_last_busy(imx596->dev);
	pm_runtime_put_autosuspend(imx596->dev);

	return ret;
}

static int imx596_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx596 *imx596 = to_imx596(sd);

	if (imx596->streaming)
		return 0;

	return imx596_start_streaming(imx596);
}

static int imx596_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx596 *imx596 = to_imx596(sd);

	if (!imx596->streaming)
		return 0;

	return imx596_stop_streaming(imx596);
}

static int imx596_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx596 *imx596 = to_imx596(sd);

	if (enable) {
		if (imx596->streaming)
			return 0;
		return imx596_start_streaming(imx596);
	} else {
		if (!imx596->streaming)
			return 0;
		return imx596_stop_streaming(imx596);
	}
}

static int imx596_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx596 *imx596 = to_imx596(sd);
	struct v4l2_mbus_framefmt *framefmt;

	fmt->format.width = IMX596_WIDTH;
	fmt->format.height = IMX596_HEIGHT;
	fmt->format.code = imx596_get_format_code(imx596);
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	framefmt = v4l2_subdev_state_get_format(state, fmt->pad);
	*framefmt = fmt->format;

	return 0;
}

static int imx596_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx596 *imx596 = to_imx596(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = IMX596_WIDTH,
			.height = IMX596_HEIGHT,
			.code = imx596_get_format_code(imx596),
			.field = V4L2_FIELD_NONE,
			.colorspace = V4L2_COLORSPACE_SRGB,
			.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
			.quantization = V4L2_QUANTIZATION_FULL_RANGE,
			.xfer_func = V4L2_XFER_FUNC_NONE,
		},
	};

	return imx596_set_pad_format(sd, state, &fmt);
}

static int imx596_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX596_WIDTH;
		sel->r.height = IMX596_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx596_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx596 *imx596 = to_imx596(sd);

	if (code->index > 0)
		return -EINVAL;

	code->code = imx596_get_format_code(imx596);
	return 0;
}

static int imx596_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx596 *imx596 = to_imx596(sd);

	if (fse->index > 0)
		return -EINVAL;

	if (fse->code != imx596_get_format_code(imx596))
		return -EINVAL;

	fse->min_width = IMX596_WIDTH;
	fse->max_width = IMX596_WIDTH;
	fse->min_height = IMX596_HEIGHT;
	fse->max_height = IMX596_HEIGHT;
	return 0;
}

static const struct v4l2_subdev_video_ops imx596_video_ops = {
	.s_stream = imx596_s_stream,
};

static const struct v4l2_subdev_pad_ops imx596_pad_ops = {
	.set_fmt = imx596_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = imx596_get_selection,
	.enum_mbus_code = imx596_enum_mbus_code,
	.enum_frame_size = imx596_enum_frame_size,
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

static const struct media_entity_operations imx596_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx596_identify_sensor(struct imx596 *imx596)
{
	u64 chip_id = 0;
	int ret;

	ret = cci_read(imx596->regmap, IMX596_REG_CHIP_ID, &chip_id, NULL);
	if (ret) {
		dev_err(imx596->dev, "failed to read chip ID: %d\n", ret);
		return ret;
	}

	if (chip_id != IMX596_CHIP_ID) {
		dev_err(imx596->dev, "unexpected chip ID: 0x%04llx (expected 0x%04x)\n",
			chip_id, IMX596_CHIP_ID);
		return -ENODEV;
	}

	dev_info(imx596->dev, "found IMX596 sensor (chip ID 0x%04x)\n",
		 (u16)chip_id);
	return 0;
}

static int imx596_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct imx596 *imx596;
	int ret;

	imx596 = devm_kzalloc(dev, sizeof(*imx596), GFP_KERNEL);
	if (!imx596)
		return -ENOMEM;

	imx596->dev = dev;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return dev_err_probe(dev, -EINVAL, "no endpoint found\n");

	ret = v4l2_fwnode_endpoint_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	if (ep.bus.mipi_csi2.num_data_lanes != IMX596_DATA_LANES)
		return dev_err_probe(dev, -EINVAL, "invalid data lanes %u\n",
				     ep.bus.mipi_csi2.num_data_lanes);

	imx596->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx596->regmap))
		return dev_err_probe(dev, PTR_ERR(imx596->regmap),
				     "failed to init CCI regmap\n");

	/* Regulators: dvdd (1.1V), avdd (2.9V), vio (1.8V) */
	imx596->dvdd = devm_regulator_get(dev, "vddd");
	if (IS_ERR(imx596->dvdd))
		return dev_err_probe(dev, PTR_ERR(imx596->dvdd),
				     "failed to get vddd supply\n");

	imx596->avdd = devm_regulator_get(dev, "vdda");
	if (IS_ERR(imx596->avdd))
		return dev_err_probe(dev, PTR_ERR(imx596->avdd),
				     "failed to get vdda supply\n");

	imx596->vio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(imx596->vio))
		return dev_err_probe(dev, PTR_ERR(imx596->vio),
				     "failed to get vddio supply\n");

	imx596->mclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx596->mclk))
		return dev_err_probe(dev, PTR_ERR(imx596->mclk),
				     "failed to get xvclk\n");

	/* Reset GPIO */
	imx596->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(imx596->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(imx596->reset_gpio),
				     "failed to get reset gpio\n");

	v4l2_i2c_subdev_init(&imx596->sd, client, &imx596_subdev_ops);

	/* Probe sensor hardware */
	ret = imx596_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "power-on failed\n");

	ret = imx596_identify_sensor(imx596);
	if (ret)
		goto err_power_off;

	ret = imx596_init_controls(imx596);
	if (ret)
		goto err_power_off;

	imx596->sd.state_lock = imx596->ctrl_handler.lock;
	imx596->sd.internal_ops = &imx596_internal_ops;
	imx596->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx596->sd.entity.ops = &imx596_subdev_entity_ops;
	imx596->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx596->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx596->sd.entity, 1, &imx596->pad);
	if (ret) {
		dev_err_probe(dev, ret, "media_entity_pads_init failed\n");
		goto err_free_ctrls;
	}

	ret = v4l2_subdev_init_finalize(&imx596->sd);
	if (ret) {
		dev_err_probe(dev, ret, "v4l2_subdev_init_finalize failed\n");
		goto err_media_cleanup;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&imx596->sd);
	if (ret) {
		dev_err_probe(dev, ret, "v4l2_async_register_subdev_sensor failed\n");
		goto err_rpm_disable;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	dev_info(dev, "Sony IMX596 initialized successfully\n");
	return 0;

err_rpm_disable:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&imx596->sd);
err_media_cleanup:
	media_entity_cleanup(&imx596->sd.entity);
err_free_ctrls:
	v4l2_ctrl_handler_free(&imx596->ctrl_handler);
err_power_off:
	imx596_power_off(dev);
	return ret;
}

static void imx596_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx596 *imx596 = to_imx596(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&imx596->ctrl_handler);
	pm_runtime_disable(imx596->dev);

	if (!pm_runtime_status_suspended(imx596->dev)) {
		imx596_power_off(imx596->dev);
		pm_runtime_set_suspended(imx596->dev);
	}
}

static const struct dev_pm_ops imx596_pm_ops = {
	SET_RUNTIME_PM_OPS(imx596_power_off, imx596_power_on, NULL)
};

static const struct of_device_id imx596_of_match[] = {
	{ .compatible = "sony,imx596" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx596_of_match);

static const struct i2c_device_id imx596_id[] = {
	{ "imx596", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, imx596_id);

static struct i2c_driver imx596_i2c_driver = {
	.driver = {
		.name = "imx596",
		.pm = &imx596_pm_ops,
		.of_match_table = imx596_of_match,
	},
	.probe = imx596_probe,
	.remove = imx596_remove,
	.id_table = imx596_id,
};

static int __init imx596_init(void)
{
	return i2c_add_driver(&imx596_i2c_driver);
}

static void __exit imx596_exit(void)
{
	i2c_del_driver(&imx596_i2c_driver);
}

module_init(imx596_init);
module_exit(imx596_exit);

MODULE_AUTHOR("Liuqin Bringup Team");
MODULE_DESCRIPTION("Sony IMX596 Camera Sensor Driver");
MODULE_LICENSE("GPL");
