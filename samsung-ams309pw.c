// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung AMS309PW (Capella) AMOLED DSI panel driver.
 * Copyright (c) 2026 Joanna Hartley <joanna@jarocks.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define AMS309PW_ACL_LEN	29
#define AMS309PW_GAMMA_LEN	26

struct ams309pw {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data *supplies;
	u8 revision;
};

static const struct regulator_bulk_data ams309pw_supplies[] = {
	{ .supply = "iovcc" },
	{ .supply = "elvdd" },
	{ .supply = "elvss" },
};

static const u8 ams309pw_gamma_v1[AMS309PW_GAMMA_LEN] = {
	0xFA, 0x01, 0x44, 0x14, 0x45, 0xB7, 0xC9, 0xB4,
	0xB5, 0xC5, 0xB4, 0xC6, 0xD0, 0xC4, 0x9A, 0xA6,
	0x96, 0xB5, 0xBD, 0xB4, 0x00, 0xB0, 0x00, 0xA0,
	0x00, 0xCC,
};

static const u8 ams309pw_gamma_v2[AMS309PW_GAMMA_LEN] = {
	0xFA, 0x01, 0x49, 0x3C, 0x59, 0xC0, 0xC3, 0xAB,
	0xC0, 0xC2, 0xB0, 0xCC, 0xCE, 0xC1, 0xA0, 0xA4,
	0x96, 0xBD, 0xBE, 0xB5, 0x00, 0xAB, 0x00, 0xA6,
	0x00, 0xBA,
};

static const u8 ams309pw_gamma_v3[AMS309PW_GAMMA_LEN] = {
	0xFA, 0x01, 0x5A, 0x44, 0x66, 0xAF, 0xBF, 0x98,
	0xAF, 0xBB, 0xA3, 0xC1, 0xC9, 0xBA, 0x97, 0xA2,
	0x8D, 0xB2, 0xB9, 0xAD, 0x00, 0xAC, 0x00, 0xA7,
	0x00, 0xBE,
};

static const u8 ams309pw_acl_v1[AMS309PW_ACL_LEN] = {
	0xC1, 0x47, 0x53, 0x13, 0x53, 0x00, 0x00, 0x02,
	0x57, 0x00, 0x00, 0x03, 0xFF, 0x00, 0x00, 0x02,
	0x06, 0x07, 0x0B, 0x0E, 0x10, 0x14, 0x17, 0x1A,
	0x1E, 0x21, 0x24, 0x28, 0x2B,
};

static const u8 ams309pw_acl_v23[AMS309PW_ACL_LEN] = {
	0xC1, 0x47, 0x53, 0x13, 0x53, 0x00, 0x00, 0x02,
	0x57, 0x00, 0x00, 0x03, 0xFF, 0x00, 0x02, 0x06,
	0x07, 0x0A, 0x0E, 0x10, 0x13, 0x16, 0x1A, 0x1D,
	0x20, 0x23, 0x27, 0x2A, 0x2D,
};

static inline struct ams309pw *to_ams309pw(struct drm_panel *panel)
{
	return container_of(panel, struct ams309pw, panel);
}

static void ams309pw_reset(struct ams309pw *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(15000, 16000);
}

static void ams309pw_detect_revision(struct ams309pw *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	u8 id[2] = { 0 };
	int ret;

	ret = mipi_dsi_dcs_read(ctx->dsi, 0xD1, id, sizeof(id));
	if (ret < 0) {
		dev_warn(dev, "panel ID read failed (%d), defaulting to rev 3\n", ret);
		ctx->revision = 3;
		return;
	}

	if (id[0] > 0x18)
		ctx->revision = 3;
	else if (id[0] > 0x15)
		ctx->revision = 2;
	else
		ctx->revision = 1;

	dev_dbg(dev, "panel ID %02x %02x -> rev %u\n",
		id[0], id[1], ctx->revision);
}

static const u8 *ams309pw_gamma_table(const struct ams309pw *ctx)
{
	switch (ctx->revision) {
	case 1:
		return ams309pw_gamma_v1;
	case 2:
		return ams309pw_gamma_v2;
	default:
		return ams309pw_gamma_v3;
	}
}

static const u8 *ams309pw_acl_table(const struct ams309pw *ctx)
{
	return ctx->revision == 1 ? ams309pw_acl_v1 : ams309pw_acl_v23;
}

static u16 ams309pw_brightness_to_dac(u16 brightness)
{
	if (brightness == 0)
		return 0;
	if (brightness < 110)
		return DIV_ROUND_CLOSEST(brightness * 1000, 110);
	return 1000;
}

static void ams309pw_set_brightness(struct mipi_dsi_multi_context *dsi_ctx, u16 dac)
{
	u8 bl = (180 - DIV_ROUND_CLOSEST(dac * 178, 1000)) & 0xFE;

	mipi_dsi_dcs_write_var_seq_multi(dsi_ctx, 0xF8,
		0x36, 0x1B, 0x00, 0x00, 0x00, 0x82, 0x00, 0x1F,
		0x74, 0x07, 0x12, 0x4F, 0x3A, 0x00, 0x00, 0x00,
		0x20, bl,   0x04, 0x3A, 0x00, 0x00, 0x00, 0x02,
		0x04, 0x04, 0x11, 0x19, 0xC0, 0xC1, 0x01, 0x81,
		0xC1, 0x00, 0xC8, 0xC1, 0xD3, 0x01);
}

static int ams309pw_init_sequence(struct ams309pw *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xF0, 0x5A, 0x5A);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xF1, 0x5A, 0x5A);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xFC, 0x5A, 0x5A);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	ams309pw_set_brightness(&dsi_ctx,
		ams309pw_brightness_to_dac(backlight_get_brightness(ctx->panel.backlight)));

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xFF, 0x10, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xF2, 0x5A, 0x03, 0x0D);

	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, ams309pw_gamma_table(ctx), AMS309PW_GAMMA_LEN);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xF7, 0x03, 0x00);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xF6, 0x00, 0x02, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xB6,
		0x0C, 0x02, 0x03, 0x32, 0xC0, 0x44, 0x44, 0xC0, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xF4,
		0xCF, 0x0A, 0x15, 0x10, 0x19, 0x33, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xD9,
		0x14, 0x40, 0x0C, 0xCB, 0xCE, 0x6E, 0xC4,
		0x07, 0xC0, 0x41, 0x9F, 0x00, 0xA0, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC0, 0x01);

	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, ams309pw_acl_table(ctx), AMS309PW_ACL_LEN);

	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int ams309pw_prepare(struct drm_panel *panel)
{
	struct ams309pw *ctx = to_ams309pw(panel);
	int ret;

	ret = regulator_enable(ctx->supplies[0].consumer);	/* iovcc */
	if (ret)
		return ret;
	usleep_range(10000, 11000);

	ret = regulator_enable(ctx->supplies[1].consumer);	/* elvdd */
	if (ret)
		goto err_iovcc;

	ret = regulator_enable(ctx->supplies[2].consumer);	/* elvss */
	if (ret)
		goto err_elvdd;
	usleep_range(10000, 11000);

	ams309pw_reset(ctx);
	ams309pw_detect_revision(ctx);

	ret = ams309pw_init_sequence(ctx);
	if (ret < 0)
		goto err_reset;

	return 0;

err_reset:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->supplies[2].consumer);
err_elvdd:
	regulator_disable(ctx->supplies[1].consumer);
err_iovcc:
	regulator_disable(ctx->supplies[0].consumer);
	return ret;
}

static int ams309pw_unprepare(struct drm_panel *panel)
{
	struct ams309pw *ctx = to_ams309pw(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	regulator_disable(ctx->supplies[2].consumer);
	regulator_disable(ctx->supplies[1].consumer);
	usleep_range(10000, 11000);
	regulator_disable(ctx->supplies[0].consumer);

	return dsi_ctx.accum_err;
}

static const struct drm_display_mode ams309pw_mode = {
	.clock = 38508,
	.hdisplay = 720,
	.hsync_start = 720 + 148,
	.hsync_end = 720 + 148 + 2,
	.htotal = 720 + 148 + 2 + 2,
	.vdisplay = 720,
	.vsync_start = 720 + 13,
	.vsync_end = 720 + 13 + 1,
	.vtotal = 720 + 13 + 1 + 2,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.width_mm = 61,
	.height_mm = 61,
};

static int ams309pw_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &ams309pw_mode);
}

static const struct drm_panel_funcs ams309pw_panel_funcs = {
	.prepare = ams309pw_prepare,
	.unprepare = ams309pw_unprepare,
	.get_modes = ams309pw_get_modes,
};

static int ams309pw_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	ams309pw_set_brightness(&dsi_ctx,
		ams309pw_brightness_to_dac(backlight_get_brightness(bl)));

	return dsi_ctx.accum_err;
}

static const struct backlight_ops ams309pw_bl_ops = {
	.update_status = ams309pw_bl_update_status,
};

static struct backlight_device *
ams309pw_create_backlight(struct mipi_dsi_device *dsi)
{
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 128,
		.max_brightness = 255,
	};

	return devm_backlight_device_register(&dsi->dev, dev_name(&dsi->dev),
					      &dsi->dev, dsi,
					      &ams309pw_bl_ops, &props);
}

static int ams309pw_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ams309pw *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ams309pw, panel,
				   &ams309pw_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(ams309pw_supplies),
					    ams309pw_supplies, &ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = ams309pw_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "failed to register backlight\n");

	drm_panel_add(&ctx->panel);

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach DSI host\n");
	}

	return 0;
}

static void ams309pw_remove(struct mipi_dsi_device *dsi)
{
	struct ams309pw *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ams309pw_of_match[] = {
	{ .compatible = "samsung,ams309pw" },
	{}
};
MODULE_DEVICE_TABLE(of, ams309pw_of_match);

static struct mipi_dsi_driver ams309pw_driver = {
	.probe = ams309pw_probe,
	.remove = ams309pw_remove,
	.driver = {
		.name = "panel-samsung-ams309pw",
		.of_match_table = ams309pw_of_match,
	},
};
module_mipi_dsi_driver(ams309pw_driver);

MODULE_AUTHOR("Joanna Hartley <joanna@jarocks.com>");
MODULE_DESCRIPTION("Samsung AMS309PW AMOLED dsi panel driver");
MODULE_LICENSE("GPL");