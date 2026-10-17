// SPDX-License-Identifier: GPL-2.0
/*
 * imx219 driver for Rockchip RV1106 (rkcif/rkisp + rkaiq)
 *
 * Skeleton follows the Rockchip sc3336 driver (rkmodule ioctls, runtime PM,
 * frame interval, mbus config). Register tables come from the mainline
 * imx219 driver (Raspberry Pi firmware register sets).
 *
 * Gain control: V4L2_CID_ANALOGUE_GAIN is linear total gain x256
 * (256 = 1x). Analog gain is used up to 10.67x, digital gain above that.
 *
 * V0.0X01.0X00 first version
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <linux/pinctrl/consumer.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x00)

#define IMX219_LANES			2
#define IMX219_BITS_PER_SAMPLE		10
#define IMX219_LINK_FREQ		456000000
/* 456MHz DDR * 2 lanes / 10 bit = 182.4M */
#define IMX219_PIXEL_RATE		(IMX219_LINK_FREQ * 2LL * \
					 IMX219_LANES / IMX219_BITS_PER_SAMPLE)
#define IMX219_XVCLK_FREQ		24000000

#define IMX219_REG_CHIP_ID		0x0000
#define IMX219_CHIP_ID			0x0219

#define IMX219_REG_CTRL_MODE		0x0100
#define IMX219_MODE_SW_STANDBY		0x00
#define IMX219_MODE_STREAMING		0x01

#define IMX219_REG_EXPOSURE		0x015a
#define IMX219_EXPOSURE_MIN		4
#define IMX219_EXPOSURE_STEP		1
/* exposure must stay at least 4 lines below frame length */
#define IMX219_EXPOSURE_MARGIN		4

#define IMX219_REG_VTS			0x0160
#define IMX219_VTS_MAX			0xffff

#define IMX219_REG_ANA_GAIN		0x0157
#define IMX219_ANA_GAIN_CODE_MAX	232	/* 256 / (256 - 232) = 10.67x */
#define IMX219_REG_DIG_GAIN		0x0158
#define IMX219_DIG_GAIN_MAX		0x0fff	/* 8.8 fixed point, ~16x */

/* linear total gain, x256 */
#define IMX219_GAIN_MIN			256
#define IMX219_GAIN_MAX			(256 * 64)
#define IMX219_GAIN_STEP		1
#define IMX219_GAIN_DEFAULT		256
/* analog gain x256 at code 232 = 256 * 256 / 24 */
#define IMX219_ANA_GAIN_MAX_X256	2730

#define IMX219_REG_TEST_PATTERN		0x0600

#define REG_NULL			0xFFFF

#define IMX219_REG_VALUE_08BIT		1
#define IMX219_REG_VALUE_16BIT		2

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"
#define IMX219_NAME			"imx219"

static const char * const imx219_supply_names[] = {
	"avdd",		/* Analog power (2.8V) */
	"dovdd",	/* Digital I/O power (1.8V) */
	"dvdd",		/* Digital core power (1.2V) */
};

#define IMX219_NUM_SUPPLIES ARRAY_SIZE(imx219_supply_names)

struct regval {
	u16 addr;
	u8 val;
};

struct imx219_mode {
	u32 bus_fmt;
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *reg_list;
	u32 hdr_mode;
	u32 vc[PAD_MAX];
};

struct imx219 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[IMX219_NUM_SUPPLIES];

	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;
	struct v4l2_ctrl	*test_pattern;
	struct mutex		mutex;
	struct v4l2_fract	cur_fps;
	bool			streaming;
	bool			power_on;
	const struct imx219_mode *cur_mode;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
	u32			cur_vts;
};

#define to_imx219(sd) container_of(sd, struct imx219, subdev)

/* Common: unlock manufacturer registers, 2 lanes, 24MHz in, RAW10 */
#define IMX219_COMMON_HEAD		\
	{0x0100, 0x00},			\
	{0x30eb, 0x05},			\
	{0x30eb, 0x0c},			\
	{0x300a, 0xff},			\
	{0x300b, 0xff},			\
	{0x30eb, 0x05},			\
	{0x30eb, 0x09},			\
	{0x0114, 0x01},			\
	{0x0128, 0x00},			\
	{0x012a, 0x18},			\
	{0x012b, 0x00},			\
	{0x0162, 0x0d},			\
	{0x0163, 0x78}

/* PLL: 24MHz -> 912Mbps/lane, 182.4M pixel rate; RAW10; analog tuning */
#define IMX219_COMMON_TAIL		\
	{0x0170, 0x01},			\
	{0x0171, 0x01},			\
	{0x0301, 0x05},			\
	{0x0303, 0x01},			\
	{0x0304, 0x03},			\
	{0x0305, 0x03},			\
	{0x0306, 0x00},			\
	{0x0307, 0x39},			\
	{0x030b, 0x01},			\
	{0x030c, 0x00},			\
	{0x030d, 0x72},			\
	{0x018c, 0x0a},			\
	{0x018d, 0x0a},			\
	{0x0309, 0x0a},			\
	{0x0172, 0x00},			\
	{0x455e, 0x00},			\
	{0x471e, 0x4b},			\
	{0x4767, 0x0f},			\
	{0x4750, 0x14},			\
	{0x4540, 0x00},			\
	{0x47b4, 0x14},			\
	{0x4713, 0x30},			\
	{0x478b, 0x10},			\
	{0x478f, 0x10},			\
	{0x4793, 0x10},			\
	{0x4797, 0x0e},			\
	{0x479b, 0x0e},			\
	{REG_NULL, 0x00}

/*
 * 3280x2464 full array, 15fps
 * line length 3448, frame length 3526
 */
static const struct regval imx219_3280x2464_15fps_regs[] = {
	IMX219_COMMON_HEAD,
	{0x0160, 0x0d},
	{0x0161, 0xc6},
	{0x0164, 0x00},
	{0x0165, 0x00},
	{0x0166, 0x0c},
	{0x0167, 0xcf},
	{0x0168, 0x00},
	{0x0169, 0x00},
	{0x016a, 0x09},
	{0x016b, 0x9f},
	{0x016c, 0x0c},
	{0x016d, 0xd0},
	{0x016e, 0x09},
	{0x016f, 0xa0},
	{0x0174, 0x00},
	{0x0175, 0x00},
	{0x0624, 0x0c},
	{0x0625, 0xd0},
	{0x0626, 0x09},
	{0x0627, 0xa0},
	IMX219_COMMON_TAIL,
};

/*
 * 1640x1232 2x2 binned full field of view, 30fps
 * line length 3448, frame length 1763
 */
static const struct regval imx219_1640x1232_30fps_regs[] = {
	IMX219_COMMON_HEAD,
	{0x0160, 0x06},
	{0x0161, 0xe3},
	{0x0164, 0x00},
	{0x0165, 0x00},
	{0x0166, 0x0c},
	{0x0167, 0xcf},
	{0x0168, 0x00},
	{0x0169, 0x00},
	{0x016a, 0x09},
	{0x016b, 0x9f},
	{0x016c, 0x06},
	{0x016d, 0x68},
	{0x016e, 0x04},
	{0x016f, 0xd0},
	{0x0174, 0x01},
	{0x0175, 0x01},
	{0x0624, 0x06},
	{0x0625, 0x68},
	{0x0626, 0x04},
	{0x0627, 0xd0},
	IMX219_COMMON_TAIL,
};

/*
 * 1920x1080 center crop, 30fps
 * line length 3448, frame length 1763
 */
static const struct regval imx219_1920x1080_30fps_regs[] = {
	IMX219_COMMON_HEAD,
	{0x0160, 0x06},
	{0x0161, 0xe3},
	{0x0164, 0x02},
	{0x0165, 0xa8},
	{0x0166, 0x0a},
	{0x0167, 0x27},
	{0x0168, 0x02},
	{0x0169, 0xb4},
	{0x016a, 0x06},
	{0x016b, 0xeb},
	{0x016c, 0x07},
	{0x016d, 0x80},
	{0x016e, 0x04},
	{0x016f, 0x38},
	{0x0174, 0x00},
	{0x0175, 0x00},
	{0x0624, 0x07},
	{0x0625, 0x80},
	{0x0626, 0x04},
	{0x0627, 0x38},
	IMX219_COMMON_TAIL,
};

static const struct imx219_mode supported_modes[] = {
	{
		.width = 3280,
		.height = 2464,
		.max_fps = {
			.numerator = 10000,
			.denominator = 150000,
		},
		.exp_def = 0x0640,
		.hts_def = 3448,
		.vts_def = 0x0dc6,
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.reg_list = imx219_3280x2464_15fps_regs,
		.hdr_mode = NO_HDR,
		.vc[PAD0] = V4L2_MBUS_CSI2_CHANNEL_0,
	},
	{
		.width = 1640,
		.height = 1232,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x0640,
		.hts_def = 3448,
		.vts_def = 0x06e3,
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.reg_list = imx219_1640x1232_30fps_regs,
		.hdr_mode = NO_HDR,
		.vc[PAD0] = V4L2_MBUS_CSI2_CHANNEL_0,
	},
	{
		.width = 1920,
		.height = 1080,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x0640,
		.hts_def = 3448,
		.vts_def = 0x06e3,
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
		.reg_list = imx219_1920x1080_30fps_regs,
		.hdr_mode = NO_HDR,
		.vc[PAD0] = V4L2_MBUS_CSI2_CHANNEL_0,
	},
};

static const s64 link_freq_menu_items[] = {
	IMX219_LINK_FREQ,
};

static const char * const imx219_test_pattern_menu[] = {
	"Disabled",
	"Solid Color",
	"Color Bars",
	"Grey Color Bars",
	"PN9",
};

/* Write registers up to 4 at a time */
static int imx219_write_reg(struct i2c_client *client, u16 reg,
			    u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;
	return 0;
}

static int imx219_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
		ret = imx219_write_reg(client, regs[i].addr,
				       IMX219_REG_VALUE_08BIT, regs[i].val);

	return ret;
}

/* Read registers up to 4 at a time */
static int imx219_read_reg(struct i2c_client *client, u16 reg, unsigned int len,
			   u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);

	return 0;
}

/*
 * gain: linear x256. Analog gain = 256 / (256 - code), so
 * code = 256 - 256 * 256 / gain. Above 10.67x the rest goes to digital
 * gain (8.8 fixed point).
 */
static int imx219_set_gain_reg(struct imx219 *imx219, u32 gain)
{
	u32 again_code, dgain;
	int ret;

	if (gain < IMX219_GAIN_MIN)
		gain = IMX219_GAIN_MIN;
	else if (gain > IMX219_GAIN_MAX)
		gain = IMX219_GAIN_MAX;

	if (gain <= IMX219_ANA_GAIN_MAX_X256) {
		again_code = 256 - DIV_ROUND_CLOSEST(256 * 256, gain);
		dgain = 0x100;
	} else {
		again_code = IMX219_ANA_GAIN_CODE_MAX;
		/* gain / (256 / 24) in 8.8 fixed point */
		dgain = DIV_ROUND_CLOSEST(gain * (256 - IMX219_ANA_GAIN_CODE_MAX), 256);
		if (dgain > IMX219_DIG_GAIN_MAX)
			dgain = IMX219_DIG_GAIN_MAX;
	}

	dev_dbg(&imx219->client->dev, "gain %u: again code 0x%x, dgain 0x%x\n",
		gain, again_code, dgain);

	ret = imx219_write_reg(imx219->client, IMX219_REG_ANA_GAIN,
			       IMX219_REG_VALUE_08BIT, again_code);
	ret |= imx219_write_reg(imx219->client, IMX219_REG_DIG_GAIN,
				IMX219_REG_VALUE_16BIT, dgain);
	return ret;
}

static int imx219_get_reso_dist(const struct imx219_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct imx219_mode *
imx219_find_best_fit(struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		dist = imx219_get_reso_dist(&supported_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

static int imx219_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct imx219 *imx219 = to_imx219(sd);
	const struct imx219_mode *mode;
	s64 h_blank, vblank_def;

	mutex_lock(&imx219->mutex);

	mode = imx219_find_best_fit(fmt);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
#else
		mutex_unlock(&imx219->mutex);
		return -ENOTTY;
#endif
	} else {
		imx219->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(imx219->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(imx219->vblank, vblank_def,
					 IMX219_VTS_MAX - mode->height,
					 1, vblank_def);
		imx219->cur_fps = mode->max_fps;
	}

	mutex_unlock(&imx219->mutex);

	return 0;
}

static int imx219_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct imx219 *imx219 = to_imx219(sd);
	const struct imx219_mode *mode = imx219->cur_mode;

	mutex_lock(&imx219->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#else
		mutex_unlock(&imx219->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
		/* format info: width/height/data type/virctual channel */
		fmt->reserved[0] = mode->vc[PAD0];
	}
	mutex_unlock(&imx219->mutex);

	return 0;
}

static int imx219_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx219 *imx219 = to_imx219(sd);

	if (code->index != 0)
		return -EINVAL;
	code->code = imx219->cur_mode->bus_fmt;

	return 0;
}

static int imx219_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != supported_modes[0].bus_fmt)
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int imx219_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx219 *imx219 = to_imx219(sd);
	const struct imx219_mode *mode = imx219->cur_mode;

	if (imx219->streaming)
		fi->interval = imx219->cur_fps;
	else
		fi->interval = mode->max_fps;
	return 0;
}

static int imx219_g_mbus_config(struct v4l2_subdev *sd,
				unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->flags = 1 << (IMX219_LANES - 1) |
			V4L2_MBUS_CSI2_CHANNEL_0 |
			V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;

	return 0;
}

static void imx219_get_module_inf(struct imx219 *imx219,
				  struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strscpy(inf->base.sensor, IMX219_NAME, sizeof(inf->base.sensor));
	strscpy(inf->base.module, imx219->module_name,
		sizeof(inf->base.module));
	strscpy(inf->base.lens, imx219->len_name, sizeof(inf->base.lens));
}

static long imx219_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct imx219 *imx219 = to_imx219(sd);
	struct rkmodule_hdr_cfg *hdr;
	long ret = 0;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		imx219_get_module_inf(imx219, (struct rkmodule_inf *)arg);
		break;
	case RKMODULE_GET_HDR_CFG:
		hdr = (struct rkmodule_hdr_cfg *)arg;
		hdr->esp.mode = HDR_NORMAL_VC;
		hdr->hdr_mode = imx219->cur_mode->hdr_mode;
		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = (struct rkmodule_hdr_cfg *)arg;
		if (hdr->hdr_mode != NO_HDR)
			ret = -EINVAL;
		break;
	case RKMODULE_SET_QUICK_STREAM:
		stream = *((u32 *)arg);
		ret = imx219_write_reg(imx219->client, IMX219_REG_CTRL_MODE,
				       IMX219_REG_VALUE_08BIT,
				       stream ? IMX219_MODE_STREAMING :
						IMX219_MODE_SW_STANDBY);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long imx219_compat_ioctl32(struct v4l2_subdev *sd,
				  unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	struct rkmodule_hdr_cfg *hdr;
	long ret;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf)
			return -ENOMEM;

		ret = imx219_ioctl(sd, cmd, inf);
		if (!ret && copy_to_user(up, inf, sizeof(*inf)))
			ret = -EFAULT;
		kfree(inf);
		break;
	case RKMODULE_GET_HDR_CFG:
		hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		if (!hdr)
			return -ENOMEM;

		ret = imx219_ioctl(sd, cmd, hdr);
		if (!ret && copy_to_user(up, hdr, sizeof(*hdr)))
			ret = -EFAULT;
		kfree(hdr);
		break;
	case RKMODULE_SET_HDR_CFG:
		hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
		if (!hdr)
			return -ENOMEM;

		if (copy_from_user(hdr, up, sizeof(*hdr)))
			ret = -EFAULT;
		else
			ret = imx219_ioctl(sd, cmd, hdr);
		kfree(hdr);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		if (copy_from_user(&stream, up, sizeof(u32)))
			ret = -EFAULT;
		else
			ret = imx219_ioctl(sd, cmd, &stream);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int __imx219_start_stream(struct imx219 *imx219)
{
	int ret;

	ret = imx219_write_array(imx219->client, imx219->cur_mode->reg_list);
	if (ret)
		return ret;
	/* In case these controls are set before streaming */
	ret = __v4l2_ctrl_handler_setup(&imx219->ctrl_handler);
	if (ret)
		return ret;

	return imx219_write_reg(imx219->client, IMX219_REG_CTRL_MODE,
				IMX219_REG_VALUE_08BIT, IMX219_MODE_STREAMING);
}

static int __imx219_stop_stream(struct imx219 *imx219)
{
	return imx219_write_reg(imx219->client, IMX219_REG_CTRL_MODE,
				IMX219_REG_VALUE_08BIT, IMX219_MODE_SW_STANDBY);
}

static int imx219_s_stream(struct v4l2_subdev *sd, int on)
{
	struct imx219 *imx219 = to_imx219(sd);
	struct i2c_client *client = imx219->client;
	int ret = 0;

	mutex_lock(&imx219->mutex);
	on = !!on;
	if (on == imx219->streaming)
		goto unlock_and_return;
	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}
		ret = __imx219_start_stream(imx219);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		__imx219_stop_stream(imx219);
		pm_runtime_put(&client->dev);
	}

	imx219->streaming = on;
unlock_and_return:
	mutex_unlock(&imx219->mutex);
	return ret;
}

static int imx219_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx219 *imx219 = to_imx219(sd);
	struct i2c_client *client = imx219->client;
	int ret = 0;

	mutex_lock(&imx219->mutex);

	/* If the power state is not modified - no work to do. */
	if (imx219->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}
		imx219->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		imx219->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&imx219->mutex);

	return ret;
}

static int __imx219_power_on(struct imx219 *imx219)
{
	int ret;
	struct device *dev = &imx219->client->dev;

	if (!IS_ERR_OR_NULL(imx219->pins_default)) {
		ret = pinctrl_select_state(imx219->pinctrl,
					   imx219->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}
	ret = clk_set_rate(imx219->xvclk, IMX219_XVCLK_FREQ);
	if (ret < 0)
		dev_warn(dev, "Failed to set xvclk rate (24MHz)\n");
	if (clk_get_rate(imx219->xvclk) != IMX219_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched, modes are based on 24MHz\n");
	ret = clk_prepare_enable(imx219->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}

	/* XCLR low while supplies come up */
	if (!IS_ERR(imx219->reset_gpio))
		gpiod_set_value_cansleep(imx219->reset_gpio, 0);

	ret = regulator_bulk_enable(IMX219_NUM_SUPPLIES, imx219->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	if (!IS_ERR(imx219->pwdn_gpio))
		gpiod_set_value_cansleep(imx219->pwdn_gpio, 1);

	usleep_range(500, 1000);

	if (!IS_ERR(imx219->reset_gpio))
		gpiod_set_value_cansleep(imx219->reset_gpio, 1);

	/* XCLR high -> first I2C access: t4 + t5 = 6.2ms (datasheet) */
	usleep_range(6200, 7200);

	return 0;

disable_clk:
	clk_disable_unprepare(imx219->xvclk);

	return ret;
}

static void __imx219_power_off(struct imx219 *imx219)
{
	int ret;
	struct device *dev = &imx219->client->dev;

	if (!IS_ERR(imx219->reset_gpio))
		gpiod_set_value_cansleep(imx219->reset_gpio, 0);
	if (!IS_ERR(imx219->pwdn_gpio))
		gpiod_set_value_cansleep(imx219->pwdn_gpio, 0);
	clk_disable_unprepare(imx219->xvclk);
	if (!IS_ERR_OR_NULL(imx219->pins_sleep)) {
		ret = pinctrl_select_state(imx219->pinctrl,
					   imx219->pins_sleep);
		if (ret < 0)
			dev_dbg(dev, "could not set pins\n");
	}
	regulator_bulk_disable(IMX219_NUM_SUPPLIES, imx219->supplies);
}

static int __maybe_unused imx219_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx219 *imx219 = to_imx219(sd);

	return __imx219_power_on(imx219);
}

static int __maybe_unused imx219_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx219 *imx219 = to_imx219(sd);

	__imx219_power_off(imx219);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx219_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx219 *imx219 = to_imx219(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
	const struct imx219_mode *def_mode = &supported_modes[0];

	mutex_lock(&imx219->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = def_mode->bus_fmt;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&imx219->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static int imx219_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_pad_config *cfg,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	fie->code = supported_modes[fie->index].bus_fmt;
	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	fie->reserved[0] = supported_modes[fie->index].hdr_mode;
	return 0;
}

static const struct dev_pm_ops imx219_pm_ops = {
	SET_RUNTIME_PM_OPS(imx219_runtime_suspend,
			   imx219_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops imx219_internal_ops = {
	.open = imx219_open,
};
#endif

static const struct v4l2_subdev_core_ops imx219_core_ops = {
	.s_power = imx219_s_power,
	.ioctl = imx219_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = imx219_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops imx219_video_ops = {
	.s_stream = imx219_s_stream,
	.g_frame_interval = imx219_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx219_pad_ops = {
	.enum_mbus_code = imx219_enum_mbus_code,
	.enum_frame_size = imx219_enum_frame_sizes,
	.enum_frame_interval = imx219_enum_frame_interval,
	.get_fmt = imx219_get_fmt,
	.set_fmt = imx219_set_fmt,
	.get_mbus_config = imx219_g_mbus_config,
};

static const struct v4l2_subdev_ops imx219_subdev_ops = {
	.core	= &imx219_core_ops,
	.video	= &imx219_video_ops,
	.pad	= &imx219_pad_ops,
};

static void imx219_modify_fps_info(struct imx219 *imx219)
{
	const struct imx219_mode *mode = imx219->cur_mode;

	imx219->cur_fps.denominator = mode->max_fps.denominator * mode->vts_def /
				      imx219->cur_vts;
}

static int imx219_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx219 *imx219 = container_of(ctrl->handler,
					     struct imx219, ctrl_handler);
	struct i2c_client *client = imx219->client;
	s64 max;
	int ret = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = imx219->cur_mode->height + ctrl->val - IMX219_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(imx219->exposure,
					 imx219->exposure->minimum, max,
					 imx219->exposure->step,
					 imx219->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		dev_dbg(&client->dev, "set exposure 0x%x\n", ctrl->val);
		ret = imx219_write_reg(client, IMX219_REG_EXPOSURE,
				       IMX219_REG_VALUE_16BIT, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		dev_dbg(&client->dev, "set gain 0x%x\n", ctrl->val);
		ret = imx219_set_gain_reg(imx219, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		dev_dbg(&client->dev, "set vblank 0x%x\n", ctrl->val);
		imx219->cur_vts = ctrl->val + imx219->cur_mode->height;
		ret = imx219_write_reg(client, IMX219_REG_VTS,
				       IMX219_REG_VALUE_16BIT, imx219->cur_vts);
		imx219_modify_fps_info(imx219);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = imx219_write_reg(client, IMX219_REG_TEST_PATTERN,
				       IMX219_REG_VALUE_16BIT, ctrl->val);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx219_ctrl_ops = {
	.s_ctrl = imx219_set_ctrl,
};

static int imx219_initialize_controls(struct imx219 *imx219)
{
	const struct imx219_mode *mode;
	struct v4l2_ctrl_handler *handler;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;

	handler = &imx219->ctrl_handler;
	mode = imx219->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &imx219->mutex;

	imx219->link_freq = v4l2_ctrl_new_int_menu(handler, NULL,
			V4L2_CID_LINK_FREQ,
			ARRAY_SIZE(link_freq_menu_items) - 1, 0, link_freq_menu_items);
	if (imx219->link_freq)
		imx219->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx219->pixel_rate = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			0, IMX219_PIXEL_RATE, 1, IMX219_PIXEL_RATE);

	h_blank = mode->hts_def - mode->width;
	imx219->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
					   h_blank, h_blank, 1, h_blank);
	if (imx219->hblank)
		imx219->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	vblank_def = mode->vts_def - mode->height;
	imx219->vblank = v4l2_ctrl_new_std(handler, &imx219_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_def,
					   IMX219_VTS_MAX - mode->height,
					   1, vblank_def);
	imx219->cur_vts = mode->vts_def;
	exposure_max = mode->vts_def - IMX219_EXPOSURE_MARGIN;
	imx219->exposure = v4l2_ctrl_new_std(handler, &imx219_ctrl_ops,
					     V4L2_CID_EXPOSURE, IMX219_EXPOSURE_MIN,
					     exposure_max, IMX219_EXPOSURE_STEP,
					     mode->exp_def);
	imx219->anal_gain = v4l2_ctrl_new_std(handler, &imx219_ctrl_ops,
					      V4L2_CID_ANALOGUE_GAIN, IMX219_GAIN_MIN,
					      IMX219_GAIN_MAX, IMX219_GAIN_STEP,
					      IMX219_GAIN_DEFAULT);
	imx219->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
							    &imx219_ctrl_ops,
					V4L2_CID_TEST_PATTERN,
					ARRAY_SIZE(imx219_test_pattern_menu) - 1,
					0, 0, imx219_test_pattern_menu);
	if (handler->error) {
		ret = handler->error;
		dev_err(&imx219->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	imx219->subdev.ctrl_handler = handler;
	imx219->cur_fps = mode->max_fps;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int imx219_check_sensor_id(struct imx219 *imx219,
				  struct i2c_client *client)
{
	struct device *dev = &imx219->client->dev;
	u32 id = 0;
	int ret;

	ret = imx219_read_reg(client, IMX219_REG_CHIP_ID,
			      IMX219_REG_VALUE_16BIT, &id);
	if (id != IMX219_CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%04x), ret(%d)\n", id, ret);
		return -ENODEV;
	}

	dev_info(dev, "Detected IMX%04x sensor\n", IMX219_CHIP_ID);

	return 0;
}

static int imx219_configure_regulators(struct imx219 *imx219)
{
	unsigned int i;

	for (i = 0; i < IMX219_NUM_SUPPLIES; i++)
		imx219->supplies[i].supply = imx219_supply_names[i];

	return devm_regulator_bulk_get(&imx219->client->dev,
				       IMX219_NUM_SUPPLIES,
				       imx219->supplies);
}

static int imx219_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct imx219 *imx219;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		 DRIVER_VERSION >> 16,
		 (DRIVER_VERSION & 0xff00) >> 8,
		 DRIVER_VERSION & 0x00ff);

	imx219 = devm_kzalloc(dev, sizeof(*imx219), GFP_KERNEL);
	if (!imx219)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &imx219->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &imx219->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &imx219->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &imx219->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	imx219->client = client;
	imx219->cur_mode = &supported_modes[0];

	imx219->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx219->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	imx219->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(imx219->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	imx219->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(imx219->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	imx219->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(imx219->pinctrl)) {
		imx219->pins_default =
			pinctrl_lookup_state(imx219->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(imx219->pins_default))
			dev_err(dev, "could not get default pinstate\n");

		imx219->pins_sleep =
			pinctrl_lookup_state(imx219->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(imx219->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	} else {
		dev_err(dev, "no pinctrl\n");
	}

	ret = imx219_configure_regulators(imx219);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	mutex_init(&imx219->mutex);

	sd = &imx219->subdev;
	v4l2_i2c_subdev_init(sd, client, &imx219_subdev_ops);
	ret = imx219_initialize_controls(imx219);
	if (ret)
		goto err_destroy_mutex;

	ret = __imx219_power_on(imx219);
	if (ret)
		goto err_free_handler;

	ret = imx219_check_sensor_id(imx219, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &imx219_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	imx219->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &imx219->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(imx219->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 imx219->module_index, facing,
		 IMX219_NAME, dev_name(sd->dev));
	ret = v4l2_async_register_subdev_sensor_common(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__imx219_power_off(imx219);
err_free_handler:
	v4l2_ctrl_handler_free(&imx219->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&imx219->mutex);

	return ret;
}

static int imx219_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx219 *imx219 = to_imx219(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&imx219->ctrl_handler);
	mutex_destroy(&imx219->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__imx219_power_off(imx219);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id imx219_of_match[] = {
	{ .compatible = "sony,imx219" },
	{},
};
MODULE_DEVICE_TABLE(of, imx219_of_match);
#endif

static const struct i2c_device_id imx219_match_id[] = {
	{ "sony,imx219", 0 },
	{ },
};

static struct i2c_driver imx219_i2c_driver = {
	.driver = {
		.name = IMX219_NAME,
		.pm = &imx219_pm_ops,
		.of_match_table = of_match_ptr(imx219_of_match),
	},
	.probe		= &imx219_probe,
	.remove		= &imx219_remove,
	.id_table	= imx219_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&imx219_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&imx219_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("Sony imx219 sensor driver for Rockchip");
MODULE_LICENSE("GPL v2");
