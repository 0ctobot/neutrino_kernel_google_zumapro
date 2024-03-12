// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based tk4b AMOLED LCD panel driver.
 *
 * Copyright (c) 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <video/mipi_display.h>

#include "trace/dpu_trace.h"
#include "panel/panel-samsung-drv.h"
#include "gs_drm/gs_display_mode.h"

#define TK4B_DDIC_ID_LEN 8
#define TK4B_DIMMING_FRAME 32

#define MIPI_DSI_FREQ_MBPS_DEFAULT 756
#define MIPI_DSI_FREQ_MBPS_ALTERNATIVE 776

#define WIDTH_MM 64
#define HEIGHT_MM 145

#define PROJECT "TK4B"

/**
 * struct tk4b_panel - panel specific runtime info
 *
 * This struct maintains tk4b panel specific runtime info, any fixed details about panel
 * should most likely go into struct exynos_panel_desc.
 */
struct tk4b_panel {
	/** @base: base panel struct */
	struct exynos_panel base;
	/** @idle_exit_dimming_delay_ts: dimming delay on idle exit */
	ktime_t idle_exit_dimming_delay_ts;
};

#define to_spanel(ctx) container_of(ctx, struct tk4b_panel, base)

static const struct exynos_dsi_cmd tk4b_lp_cmds[] = {
	/* Disable the Black insertion in AoD */
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),
	EXYNOS_DSI_CMD_SEQ(0xC0, 0x54),

	/* disable dimming */
	EXYNOS_DSI_CMD_SEQ(0x53, 0x20),
	/* enter AOD */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_ENTER_IDLE_MODE),
	/* Settings AOD Hclk */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x0E),
	EXYNOS_DSI_CMD_SEQ(0xF5, 0x20),
	/* Lock TE2 30Hz */
	EXYNOS_DSI_CMD_SEQ(0x5A, 0x04),
};
static DEFINE_EXYNOS_CMD_SET(tk4b_lp);

static const struct exynos_dsi_cmd tk4b_lp_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x04),
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x00),
};

static const struct exynos_dsi_cmd tk4b_lp_night_cmds[] = {
	/* 2 nit */
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x04),
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x03),
};

static const struct exynos_dsi_cmd tk4b_lp_low_cmds[] = {
	/* 10 nit */
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x04),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1_1),
				MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x07, 0xB2),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1_1),
				MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x33),
};

static const struct exynos_dsi_cmd tk4b_lp_high_cmds[] = {
	/* 50 nit */
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x04),
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFE),
};

static const struct exynos_binned_lp tk4b_binned_lp[] = {
	BINNED_LP_MODE("off", 0, tk4b_lp_off_cmds),
	/* rising = 0, falling = 45 */
	/* night threshold 4 nits */
	BINNED_LP_MODE_TIMING("night", 104, tk4b_lp_night_cmds, 0, 45),
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 932, tk4b_lp_low_cmds, 0, 45),
	BINNED_LP_MODE_TIMING("high", 3574, tk4b_lp_high_cmds, 0, 45),
};

static const struct exynos_dsi_cmd tk4b_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ_DELAY(100, MIPI_DCS_SET_DISPLAY_OFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(tk4b_off);

static const struct exynos_dsi_cmd tk4b_init_cmds[] = {
	/* CMD2, Page0 */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),

	/* b/314125080 Add ELVSS Settings for avoid exit AOD flashing */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0x6F, 0x06),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0xB5, 0x7F, 0x00, 0x2C, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0x6F, 0x11),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0xB5, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0x6F, 0x2D),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0xB5, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
				0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x25, 0x25,
				0x1B, 0x1B, 0x13, 0x13, 0x0C, 0x0C, 0x0C, 0x0C, 0x07),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0x6F, 0x44),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_DVT1), 0xB5, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
				0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x25, 0x25,
				0x1B, 0x1B, 0x13, 0x13, 0x0C, 0x0C, 0x0C, 0x0C, 0x07),

	/* b/295134228 correct EM pulse and EM-off time */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x18),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xB2, 0x38, 0xB0, 0x3F, 0xFF),

	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x1B),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xBA, 0x18),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x1C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x2C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
				0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x3C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x01, 0x01, 0x01, 0x01, 0x03, 0x03, 0x03, 0x03,
				0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x4C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x01, 0x01, 0x01, 0x01, 0x0B, 0x0B, 0x0B, 0x0B,
				0x0B, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x5C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
				0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x6C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
				0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x7C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
				0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x8C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x9C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0xA4),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xBA, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0xA8),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0xB0),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1),
				0xBA, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x08),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xBB, 0x01, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x18),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xBB, 0x01, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x1C),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xBB, 0x01, 0x00),

	/* CMD2, Page1 */
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x05),
	EXYNOS_DSI_CMD_SEQ(0xC5, 0x15, 0x15, 0x15, 0xDD),

	/* FFC Off */
	EXYNOS_DSI_CMD_SEQ(0xC3, 0x00),
	/* FFC setting (MIPI: 756Mbps) */
	EXYNOS_DSI_CMD_SEQ(0xC3, 0x00, 0x06, 0x20, 0x0C, 0xFF, 0x00, 0x06, 0x20,
				 0x0C, 0xFF, 0x00, 0x04, 0x63, 0x0C, 0x05, 0xD9,
				 0x10, 0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10, 0x04,
				 0x63, 0x0C, 0x05, 0xD9, 0x10, 0x04, 0x63, 0x0C,
				 0x05, 0xD9, 0x10, 0x04, 0x63, 0x0C, 0x05, 0xD9,
				 0x10),

	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x0A),
	/* VGSP adjust */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xE3, 0x00, 0x00, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6F, 0x18),
	/* Vin source adjust */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0xD8, 0x38),

	/* CMD2, Page7 */
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x07),
	/* Round algorithm OFF */
	EXYNOS_DSI_CMD_SEQ(0xC0, 0x00),

	/* CMD2, Page4 */
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x04),
	/* Deburn in setting */
	EXYNOS_DSI_CMD_SEQ(0xBB, 0xF3),
	/* CMD2, Page8 */
	EXYNOS_DSI_CMD_SEQ(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x08),
	/* Deburn in setting */
	EXYNOS_DSI_CMD_SEQ(0xC2, 0x33, 0x00, 0x78, 0x03, 0x82, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x06),
	EXYNOS_DSI_CMD_SEQ(0xC2, 0x79, 0xA5, 0xBF, 0xD2, 0xE1, 0xEF, 0xFA),
	EXYNOS_DSI_CMD_SEQ(0xC3, 0x00, 0x00, 0x02, 0x00, 0x02, 0x08, 0x00, 0x08,
				0x16, 0x00, 0x16, 0x47, 0x00, 0x47, 0x8D, 0x10,
				0x8D, 0x87, 0x41, 0x87, 0x2E, 0x84, 0x2E, 0x00,
				0xE8, 0x00, 0x4B, 0xEE, 0x4B, 0x4B, 0xFE, 0x4B,
				0xFF),
	EXYNOS_DSI_CMD_SEQ(0xC7, 0x00),
	EXYNOS_DSI_CMD_SEQ(0xC8, 0x00, 0x00, 0x01, 0x02, 0x04, 0x09, 0x13, 0x26, 0x4C),
	EXYNOS_DSI_CMD_SEQ(0xC9, 0x00, 0x01, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0),
	EXYNOS_DSI_CMD_SEQ(0xCA, 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80),
	EXYNOS_DSI_CMD_SEQ(0xCB, 0x11, 0x00, 0x00, 0x32, 0x00, 0x00, 0x75, 0x00,
				0x00, 0xBB, 0x00, 0x00, 0x9A, 0x00, 0x00, 0xF9,
				0x00, 0xFF),
	EXYNOS_DSI_CMD_SEQ(0xCC, 0x22, 0x00, 0x00, 0x32, 0x00, 0x00, 0x52, 0x00,
				0x00, 0xA7, 0x00, 0x00, 0xBB, 0x00, 0x00, 0xFB,
				0x00, 0xFF),
	EXYNOS_DSI_CMD_SEQ(0xCE, 0x22, 0x00, 0x00, 0x32, 0x00, 0x00, 0x63, 0x00,
				0x00, 0xAA, 0x00, 0x00, 0xAC, 0x00, 0x00, 0xFA,
				0x00, 0xFF),
	EXYNOS_DSI_CMD_SEQ(0xCF, 0x56, 0x00, 0x00, 0x45, 0x00, 0x00, 0x54, 0x00,
				0x00, 0x45, 0x00, 0x00, 0x44, 0x00, 0x00, 0x34,
				0x00, 0x00, 0x33, 0x00, 0x00, 0x33, 0x66, 0x00,
				0x23, 0x00, 0x00, 0x22, 0x00, 0x00, 0x22, 0x00,
				0x00),

	/* CMD3, Page0 */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x80),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x19),
	EXYNOS_DSI_CMD_SEQ(0xF2, 0x00),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x1A),
	EXYNOS_DSI_CMD_SEQ(0xF4, 0x55),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x2D),
	EXYNOS_DSI_CMD_SEQ(0xFC, 0x44),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x11),
	EXYNOS_DSI_CMD_SEQ(0xF8, 0x01, 0x7B),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x2D),
	EXYNOS_DSI_CMD_SEQ(0xF8, 0x01, 0x1D),

	/* CMD3, Page1 */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x05),
	EXYNOS_DSI_CMD_SEQ(0xFE, 0x3C),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x02),
	EXYNOS_DSI_CMD_SEQ(0xF9, 0x04),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x1E),
	EXYNOS_DSI_CMD_SEQ(0xFB, 0x0F),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x0D),
	EXYNOS_DSI_CMD_SEQ(0xFB, 0x84),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x0F),
	EXYNOS_DSI_CMD_SEQ(0xF5, 0x20),
	/* CMD3, Page2 */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x82),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x09),
	EXYNOS_DSI_CMD_SEQ(0xF2, 0x55),
	/* CMD3, Page3 */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x83),
	EXYNOS_DSI_CMD_SEQ(0x6F, 0x12),
	EXYNOS_DSI_CMD_SEQ(0xFE, 0x41),

	/* CMD, Disable */
	EXYNOS_DSI_CMD_SEQ(0xFF, 0xAA, 0x55, 0xA5, 0x00),

	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_TEAR_SCANLINE, 0x00, 0x00),
	/* TE2 width 45H */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_TEAR_ON, 0x00, 0x2D),
	/* BC Dimming OFF */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20),
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),
	/* Normal GMA */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_GAMMA_CURVE, 0x00),

	/* CMD1, DPC Temperature 25 */
	EXYNOS_DSI_CMD_SEQ(0x81, 0x01, 0x19),
	EXYNOS_DSI_CMD_SEQ(0x03, 0x01),
	EXYNOS_DSI_CMD_SEQ(0x90, 0x03, 0x03),
	/* 2DSC & slice high 24 DSC v1.2a */
	EXYNOS_DSI_CMD_SEQ(0x91, 0x89, 0xA8, 0x00, 0x18, 0xD2, 0x00, 0x02, 0x25, 0x02,
				0x35, 0x00, 0x07, 0x04, 0x86, 0x04, 0x3D, 0x10, 0xF0),
	/* Long V = 60Hz */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0x2F, 0x02),
	/* 60Hz */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x2F, 0x30),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_EVT1), 0x6D, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_DELAY(60, MIPI_DCS_EXIT_SLEEP_MODE)
};
static DEFINE_EXYNOS_CMD_SET(tk4b_init);

static void tk4b_update_te2(struct exynos_panel *ctx)
{
	struct exynos_panel_te2_timing timing;
	u8 width = 0x2D; /* default width 45H */
	u32 rising = 0, falling;
	int ret;

	if (!ctx)
		return;

	ret = exynos_panel_get_current_mode_te2(ctx, &timing);
	if (!ret) {
		falling = timing.falling_edge;
		if (falling >= timing.rising_edge) {
			rising = timing.rising_edge;
			width = falling - rising;
		} else {
			dev_warn(ctx->dev, "invalid timing, use default setting\n");
		}
	} else if (ret == -EAGAIN) {
		dev_dbg(ctx->dev, "Panel is not ready, use default setting\n");
	} else {
		return;
	}

	dev_dbg(ctx->dev, "TE2 updated: rising= 0x%x, width= 0x%x", rising, width);

	EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, rising);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_SET_TEAR_ON, 0x00, width);
}

static void tk4b_update_irc(struct exynos_panel *ctx,
				const enum exynos_hbm_mode hbm_mode,
				const int vrefresh)
{
	const u16 level = exynos_panel_get_brightness(ctx);

	if (!IS_HBM_ON(hbm_mode)) {
		dev_info(ctx->dev, "hbm is off, skip update irc\n");
		return;
	}

	if (IS_HBM_ON_IRC_OFF(hbm_mode)) {
		/* sync from bigSurf : to achieve the max brightness with IRC off which need to set dbv to 0xFFF */
		if (level == ctx->desc->brt_capability->hbm.level.max)
			EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFF);

		/* IRC Off */
		EXYNOS_DCS_BUF_ADD(ctx, 0x5F, 0x01);
		if (vrefresh == 120) {
			EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x00);
			EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x02);
			EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
			EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
			if (ctx->panel_rev < PANEL_REV_EVT1)
				EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x32);
			else
				EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x40);
		} else {
			if (ctx->panel_rev < PANEL_REV_EVT1) {
				EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x30);
				EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
				EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0xB0);
				EXYNOS_DCS_BUF_ADD(ctx, 0xBA, 0x44);
				EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
				EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x32);
			} else {
				EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x02);
				EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
				EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
				EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x40);
			}
		}
	} else {
		const u8 val1 = level >> 8;
		const u8 val2 = level & 0xff;

		/* IRC ON */
		EXYNOS_DCS_BUF_ADD(ctx, 0x5F, 0x00);
		if (vrefresh == 120) {
			EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x00);
			EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
			EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
			EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
			if (ctx->panel_rev < PANEL_REV_EVT1)
				EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x30);
			else
				EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x10);
		} else {
			if (ctx->panel_rev < PANEL_REV_EVT1) {
				EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x30);
				EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
				EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0xB0);
				EXYNOS_DCS_BUF_ADD(ctx, 0xBA, 0x41);
				EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
				EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x30);
			} else {
				EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x02);
				EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
				EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
				EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x10);
			}
		}
		/* sync from bigSurf : restore the dbv value while IRC ON */
		EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, val1, val2);
	}
	/* Empty command is for flush */
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x00);
}

static void tk4b_change_frequency(struct exynos_panel *ctx,
				    const struct exynos_panel_mode *pmode)
{
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (!ctx || (vrefresh != 60 && vrefresh != 120))
		return;

	if (!IS_HBM_ON(ctx->hbm_mode)) {
		if (vrefresh == 120) {
			EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x00);
			EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
		} else {
			if (ctx->panel_rev < PANEL_REV_EVT1) {
				EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x30);
				EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
				EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0xB0);
				EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xBA, 0x41);
			} else {
				EXYNOS_DCS_BUF_ADD(ctx, 0x2F, 0x02);
				EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
				EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x03);
				EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xC0, 0x10);
			}
		}
	} else {
		tk4b_update_irc(ctx, ctx->hbm_mode, vrefresh);
	}

	dev_dbg(ctx->dev, "%s: change to %uhz\n", __func__, vrefresh);
}

static void tk4b_set_dimming_on(struct exynos_panel *ctx,
				 bool dimming_on)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;

	if (pmode->exynos_mode.is_lp_mode) {
		dev_warn(ctx->dev, "in lp mode, skip to update\n");
		return;
	}

	ctx->dimming_on = dimming_on;
	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					ctx->dimming_on ? 0x28 : 0x20);
	dev_dbg(ctx->dev, "%s dimming_on=%d\n", __func__, dimming_on);
}

static void tk4b_set_nolp_mode(struct exynos_panel *ctx,
				  const struct exynos_panel_mode *pmode)
{
	struct tk4b_panel *spanel = to_spanel(ctx);
	int vrefresh = drm_mode_vrefresh(&pmode->mode);
	if (!is_panel_active(ctx))
		return;

	/* exit AOD */
	EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	EXYNOS_DCS_BUF_ADD(ctx, 0xC0, 0x54);
	EXYNOS_DCS_BUF_ADD(ctx, MIPI_DCS_EXIT_IDLE_MODE);
	EXYNOS_DCS_BUF_ADD(ctx, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x0E);
	EXYNOS_DCS_BUF_ADD(ctx, 0xF5, 0x2B);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x5A, 0x04);

	tk4b_change_frequency(ctx, pmode);
	spanel->idle_exit_dimming_delay_ts = ktime_add_us(
		ktime_get(), 100 + EXYNOS_VREFRESH_TO_PERIOD_USEC(vrefresh) * 2);

	dev_info(ctx->dev, "exit LP mode\n");
}

static void tk4b_dimming_frame_setting(struct exynos_panel *ctx, u8 dimming_frame)
{
	/* Fixed time 1 frame */
	if (!dimming_frame)
		dimming_frame = 0x01;

	EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB2, 0x19);
	EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x05);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xB2, dimming_frame, dimming_frame);
}

static int tk4b_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	struct tk4b_panel *spanel = to_spanel(ctx);

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_dbg(ctx->dev, "%s\n", __func__);

	exynos_panel_reset(ctx);
	exynos_panel_send_cmd_set(ctx, &tk4b_init_cmd_set);
	tk4b_change_frequency(ctx, pmode);
	tk4b_dimming_frame_setting(ctx, TK4B_DIMMING_FRAME);
	spanel->idle_exit_dimming_delay_ts = 0;

	if (pmode->exynos_mode.is_lp_mode) {
		exynos_panel_set_lp_mode(ctx, pmode);
	}

	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_SET_DISPLAY_ON);

	ctx->dsi_hs_clk_mbps = MIPI_DSI_FREQ_MBPS_DEFAULT;

	return 0;
}

static int tk4b_atomic_check(struct exynos_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->exynos_connector.base;
	struct drm_connector_state *new_conn_state =
					drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
	    !new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;

	if (!drm_atomic_crtc_effectively_active(old_crtc_state) ||
	    (ctx->current_mode->exynos_mode.is_lp_mode && drm_mode_vrefresh(&new_crtc_state->mode) == 60)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;

		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;
		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			ctx->exynos_connector.needs_commit = true;
			dev_dbg(ctx->dev, "raise mode (%s) clock to 120hz on %s\n",
				mode->name,
				!drm_atomic_crtc_effectively_active(old_crtc_state) ?
				"resume" : "lp exit");
		}
	} else if (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock) {
		/* clock hacked in last commit due to resume or lp exit, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		ctx->exynos_connector.needs_commit = false;
		dev_dbg(ctx->dev, "restore mode (%s) clock after resume or lp exit\n",
			new_crtc_state->mode.name);
	}

	return 0;
}

static void tk4b_pre_update_ffc(struct exynos_panel *ctx)
{
	dev_dbg(ctx->dev, "%s\n", __func__);

	DPU_ATRACE_BEGIN(__func__);

	/* FFC off */
	EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xC3, 0x00);

	DPU_ATRACE_END(__func__);
}

static void tk4b_update_ffc(struct exynos_panel *ctx, unsigned int hs_clk_mbps)
{
	dev_dbg(ctx->dev, "%s: hs_clk_mbps: current=%d, target=%d\n",
		__func__, ctx->dsi_hs_clk_mbps, hs_clk_mbps);

	DPU_ATRACE_BEGIN(__func__);

	if (hs_clk_mbps != MIPI_DSI_FREQ_MBPS_DEFAULT &&
	    hs_clk_mbps != MIPI_DSI_FREQ_MBPS_ALTERNATIVE) {
		dev_warn(ctx->dev, "invalid hs_clk_mbps=%d for FFC\n", hs_clk_mbps);
	} else if (ctx->dsi_hs_clk_mbps != hs_clk_mbps) {
		dev_info(ctx->dev, "%s: updating for hs_clk_mbps=%d\n", __func__, hs_clk_mbps);
		ctx->dsi_hs_clk_mbps = hs_clk_mbps;

		/* Update FFC */
		EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
		if (hs_clk_mbps == MIPI_DSI_FREQ_MBPS_DEFAULT)
			EXYNOS_DCS_BUF_ADD(ctx, 0xC3, 0x00, 0x06, 0x20, 0x0C, 0xFF,
						0x00, 0x06, 0x20, 0x0C, 0xFF, 0x00,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10);
		else /* MIPI_DSI_FREQ_MBPS_ALTERNATIVE */
			EXYNOS_DCS_BUF_ADD(ctx, 0xC3, 0x00, 0x06, 0x20, 0x0C, 0xFF,
						0x00, 0x06, 0x20, 0x0C, 0xFF, 0x00,
						0x04, 0x46, 0x0C, 0x06, 0x0D, 0x11,
						0x04, 0x46, 0x0C, 0x06, 0x0D, 0x11,
						0x04, 0x46, 0x0C, 0x06, 0x0D, 0x11,
						0x04, 0x46, 0x0C, 0x06, 0x0D, 0x11,
						0x04, 0x46, 0x0C, 0x06, 0x0D, 0x11);
	}

	/* FFC on */
	EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xC3, 0xDD);

	DPU_ATRACE_END(__func__);
}

static int tk4b_set_brightness(struct exynos_panel *ctx, u16 br)
{
	struct tk4b_panel *spanel = to_spanel(ctx);
	u16 brightness;

	if (ctx->current_mode->exynos_mode.is_lp_mode) {
		const struct exynos_panel_funcs *funcs;

		funcs = ctx->desc->exynos_panel_func;
		if (funcs && funcs->set_binned_lp)
			funcs->set_binned_lp(ctx, br);
		return 0;
	}

	if (!br) {
		// turn off panel and set brightness directly.
		return exynos_dcs_set_brightness(ctx, 0);
	}

	if (IS_HBM_ON_IRC_OFF(ctx->hbm_mode) && br == ctx->desc->brt_capability->hbm.level.max)
		br = 0xfff;

	brightness = (br & 0xff) << 8 | br >> 8;

	if (spanel->idle_exit_dimming_delay_ts &&
		(ktime_sub(spanel->idle_exit_dimming_delay_ts, ktime_get()) <= 0)) {
		EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
					ctx->dimming_on ? 0x28 : 0x20);
		spanel->idle_exit_dimming_delay_ts = 0;
	}

	return exynos_dcs_set_brightness(ctx, brightness);
}

static void tk4b_set_hbm_mode(struct exynos_panel *ctx,
				 enum exynos_hbm_mode hbm_mode)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (ctx->hbm_mode == hbm_mode)
		return;

	EXYNOS_DCS_BUF_ADD(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	EXYNOS_DCS_BUF_ADD(ctx, 0x6F, 0x11);
	if (hbm_mode == HBM_OFF)
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xB2, 0x01, 0x01, 0x43);
	else
		EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0xB2, 0x00, 0x00, 0x41);

	tk4b_update_irc(ctx, hbm_mode, vrefresh);

	ctx->hbm_mode = hbm_mode;
	dev_info(ctx->dev, "hbm_on=%d hbm_ircoff=%d\n", IS_HBM_ON(ctx->hbm_mode),
		 IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void tk4b_mode_set(struct exynos_panel *ctx,
			     const struct exynos_panel_mode *pmode)
{
	tk4b_change_frequency(ctx, pmode);
}

static bool tk4b_is_mode_seamless(const struct exynos_panel *ctx,
				     const struct exynos_panel_mode *pmode)
{
	const struct drm_display_mode *c = &ctx->current_mode->mode;
	const struct drm_display_mode *n = &pmode->mode;

	/* seamless mode set can happen if active region resolution is same */
	return (c->vdisplay == n->vdisplay) && (c->hdisplay == n->hdisplay) &&
	       (c->flags == n->flags);
}

static void tk4b_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	const u8 build_code = (id & 0xFF00) >> 8;
	const u8 main = (build_code & 0xE0) >> 3;
	const u8 sub = (build_code & 0x0C) >> 2;

	exynos_panel_get_panel_rev(ctx, main | sub);
}

static int tk4b_read_id(struct exynos_panel *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	char buf[TK4B_DDIC_ID_LEN] = {0};
	int ret;

	EXYNOS_DCS_WRITE_SEQ(ctx, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	ret = mipi_dsi_dcs_read(dsi, 0xF2, buf, TK4B_DDIC_ID_LEN);
	if (ret != TK4B_DDIC_ID_LEN) {
		dev_warn(ctx->dev, "Unable to read DDIC id (%d)\n", ret);
		goto done;
	} else {
		ret = 0;
	}

	exynos_bin2hex(buf, TK4B_DDIC_ID_LEN,
		ctx->panel_id, sizeof(ctx->panel_id));
done:
	EXYNOS_DCS_WRITE_SEQ(ctx, 0xFF, 0xAA, 0x55, 0xA5, 0x00);
	return ret;
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 200,
	.te_var = 1,
};

/* Truncate 8-bit signed value to 6-bit signed value */
#define TO_6BIT_SIGNED(v) ((v) & 0x3F)

static const struct drm_dsc_config tk4b_dsc_cfg = {
	.first_line_bpg_offset = 13,
	.rc_range_params = {
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{4, 10, TO_6BIT_SIGNED(-10)},
		{5, 10, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{8, 12, TO_6BIT_SIGNED(-12)},
		{12, 13, TO_6BIT_SIGNED(-12)},
	},
	/* Used DSC v1.2 */
	.dsc_version_major = 1,
	.dsc_version_minor = 2,
};

#define TK4B_DSC_CONFIG \
	.dsc = { \
		.enabled = true, \
		.dsc_count = 2, \
		.slice_count = 2, \
		.slice_height = 24, \
		.cfg = &tk4b_dsc_cfg, \
	}

static const struct exynos_panel_mode tk4b_modes[] = {
	{
		.mode = {
			.name = "1080x2424@60:60",
			DRM_MODE_TIMING(60, 1080, 32, 12, 16, 2424, 12, 4, 15),
			/* aligned to bootloader setting */
			.type = DRM_MODE_TYPE_PREFERRED,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = 8370,
			.bpc = 8,
			TK4B_DSC_CONFIG,
			.underrun_param = &underrun_param,
		},
		.te2_timing = {
			.rising_edge = 0,
			.falling_edge = 45,
		},
	},
	{
		.mode = {
			.name = "1080x2424@120:120",
			DRM_MODE_TIMING(120, 1080, 32, 12, 16, 2424, 12, 4, 15),
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = 276,
			.bpc = 8,
			TK4B_DSC_CONFIG,
			.underrun_param = &underrun_param,
		},
		.te2_timing = {
			.rising_edge = 0,
			.falling_edge = 45,
		},
	},
};

static const struct exynos_panel_mode tk4b_lp_mode = {
	.mode = {
		.name = "1080x2424@30:30",
		DRM_MODE_TIMING(30, 1080, 32, 12, 16, 2424, 12, 4, 15),
		.type = DRM_MODE_TYPE_DRIVER,
		.width_mm = WIDTH_MM,
		.height_mm = HEIGHT_MM,
	},
	.exynos_mode = {
		.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
		.vblank_usec = 120,
		.bpc = 8,
		TK4B_DSC_CONFIG,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	}
};

static void tk4b_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	struct dentry *panel_root, *csroot;

	if (!ctx)
		return;

	panel_root = debugfs_lookup("panel", root);
	if (!panel_root)
		return;

	csroot = debugfs_lookup("cmdsets", panel_root);
	if (!csroot) {
		goto panel_out;
	}

	exynos_panel_debugfs_create_cmdset(ctx, csroot, &tk4b_init_cmd_set, "init");
	dput(csroot);
panel_out:
	dput(panel_root);
}

static void tk4b_panel_init(struct exynos_panel *ctx)
{
	tk4b_dimming_frame_setting(ctx, TK4B_DIMMING_FRAME);
}

static int tk4b_panel_probe(struct mipi_dsi_device *dsi)
{
	struct tk4b_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	return exynos_panel_common_init(dsi, &spanel->base);
}

static const struct drm_panel_funcs tk4b_drm_funcs = {
	.disable = exynos_panel_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = tk4b_enable,
	.get_modes = exynos_panel_get_modes,
	.debugfs_init = tk4b_debugfs_init,
};

static int tk4b_panel_config(struct exynos_panel *ctx);

static const struct exynos_panel_funcs tk4b_exynos_funcs = {
	.set_brightness = tk4b_set_brightness,
	.set_lp_mode = exynos_panel_set_lp_mode,
	.set_nolp_mode = tk4b_set_nolp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_hbm_mode = tk4b_set_hbm_mode,
	.set_dimming_on = tk4b_set_dimming_on,
	.is_mode_seamless = tk4b_is_mode_seamless,
	.mode_set = tk4b_mode_set,
	.panel_init = tk4b_panel_init,
	.panel_config = tk4b_panel_config,
	.get_panel_rev = tk4b_get_panel_rev,
	.get_te2_edges = exynos_panel_get_te2_edges,
	.configure_te2_edges = exynos_panel_configure_te2_edges,
	.update_te2 = tk4b_update_te2,
	.read_id = tk4b_read_id,
	.atomic_check = tk4b_atomic_check,
	.pre_update_ffc = tk4b_pre_update_ffc,
	.update_ffc = tk4b_update_ffc,
};

static const struct exynos_brightness_configuration tk4b_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_LATEST,
		.dft_brightness = 1816,
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1200,
				},
				.level = {
					.min = 1,
					.max = 3628,
				},
				.percentage = {
					.min = 0,
					.max = 67,
				},
			},
			.hbm = {
				.nits = {
					.min = 1200,
					.max = 1800,
				},
				.level = {
					.min = 3629,
					.max = 3939,
				},
				.percentage = {
					.min = 67,
					.max = 100,
				},
			},
		},
	},
};

struct exynos_panel_desc google_tk4b = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.modes = tk4b_modes,
	.num_modes = ARRAY_SIZE(tk4b_modes),
	.off_cmd_set = &tk4b_off_cmd_set,
	.lp_mode = &tk4b_lp_mode,
	.lp_cmd_set = &tk4b_lp_cmd_set,
	.binned_lp = tk4b_binned_lp,
	.num_binned_lp = ARRAY_SIZE(tk4b_binned_lp),
	.panel_func = &tk4b_drm_funcs,
	.exynos_panel_func = &tk4b_exynos_funcs,
	.default_dsi_hs_clk_mbps = MIPI_DSI_FREQ_MBPS_DEFAULT,
	.reset_timing_ms = {1, 1, 20},
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDD, 10},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VDDD, 10},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDI, 0},
	},
	.refresh_on_lp = true,
};

static int tk4b_panel_config(struct exynos_panel *ctx)
{
	int ret;

	exynos_panel_model_init(ctx, PROJECT, 0);

	ret = exynos_panel_init_brightness(&google_tk4b,
						tk4b_btr_configs,
						ARRAY_SIZE(tk4b_btr_configs),
						ctx->panel_rev);

	return ret;
}

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "google,tk4b", .data = &google_tk4b },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_panel_of_match);

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = tk4b_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-google-tk4b",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Gil Liu <gilliu@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google tk4b panel driver");
MODULE_LICENSE("GPL");
