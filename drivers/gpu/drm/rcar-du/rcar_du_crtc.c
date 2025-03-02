/*
 * rcar_du_crtc.c  --  R-Car Display Unit CRTCs
 *
 * Copyright (C) 2013-2015 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/sys_soc.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_plane_helper.h>

#include "rcar_du_crtc.h"
#include "rcar_du_drv.h"
#include "rcar_du_encoder.h"
#include "rcar_du_kms.h"
#include "rcar_du_plane.h"
#include "rcar_du_regs.h"
#include "rcar_du_vsp.h"
#include "rcar_lvds.h"
#include "rzg2l_mipi_dsi.h"

static u32 rcar_du_crtc_read(struct rcar_du_crtc *rcrtc, u32 reg)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	return rcar_du_read(rcdu, rcrtc->mmio_offset + reg);
}

static void rcar_du_crtc_write(struct rcar_du_crtc *rcrtc, u32 reg, u32 data)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	rcar_du_write(rcdu, rcrtc->mmio_offset + reg, data);
}

static void rcar_du_crtc_clr(struct rcar_du_crtc *rcrtc, u32 reg, u32 clr)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	rcar_du_write(rcdu, rcrtc->mmio_offset + reg,
		      rcar_du_read(rcdu, rcrtc->mmio_offset + reg) & ~clr);
}

static void rcar_du_crtc_set(struct rcar_du_crtc *rcrtc, u32 reg, u32 set)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	rcar_du_write(rcdu, rcrtc->mmio_offset + reg,
		      rcar_du_read(rcdu, rcrtc->mmio_offset + reg) | set);
}

void rcar_du_crtc_dsysr_clr_set(struct rcar_du_crtc *rcrtc, u32 clr, u32 set)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	rcrtc->dsysr = (rcrtc->dsysr & ~clr) | set;
	rcar_du_write(rcdu, rcrtc->mmio_offset + DSYSR, rcrtc->dsysr);
}

/* -----------------------------------------------------------------------------
 * Hardware Setup
 */

struct dpll_info {
	unsigned int output;
	unsigned int fdpll;
	unsigned int n;
	unsigned int m;
};

static void rcar_du_dpll_divider(struct rcar_du_crtc *rcrtc,
				 struct dpll_info *dpll,
				 unsigned long input,
				 unsigned long target)
{
	unsigned long best_diff = (unsigned long)-1;
	unsigned long diff;
	unsigned int fdpll;
	unsigned int m;
	unsigned int n;

	/*
	 *   fin                                 fvco        fout       fclkout
	 * in --> [1/M] --> |PD| -> [LPF] -> [VCO] -> [1/P] -+-> [1/FDPLL] -> out
	 *              +-> |  |                             |
	 *              |                                    |
	 *              +---------------- [1/N] <------------+
	 *
	 *	fclkout = fvco / P / FDPLL -- (1)
	 *
	 * fin/M = fvco/P/N
	 *
	 *	fvco = fin * P *  N / M -- (2)
	 *
	 * (1) + (2) indicates
	 *
	 *	fclkout = fin * N / M / FDPLL
	 *
	 * NOTES
	 *	N	: (n + 1)
	 *	M	: (m + 1)
	 *	FDPLL	: (fdpll + 1)
	 *	P	: 2
	 *	2kHz < fvco < 4096MHz
	 *
	 * To minimize the jitter,
	 * N : as large as possible
	 * M : as small as possible
	 */
	for (m = 0; m < 4; m++) {
		for (n = 119; n > 38; n--) {
			/*
			 * This code only runs on 64-bit architectures, the
			 * unsigned long type can thus be used for 64-bit
			 * computation. It will still compile without any
			 * warning on 32-bit architectures.
			 *
			 * To optimize calculations, use fout instead of fvco
			 * to verify the VCO frequency constraint.
			 */
			unsigned long fout = input * (n + 1) / (m + 1);

			if (fout < 1000 || fout > 2048 * 1000 * 1000U)
				continue;

			for (fdpll = 1; fdpll < 32; fdpll++) {
				unsigned long output;

				output = fout / (fdpll + 1);
				if (output >= 400 * 1000 * 1000)
					continue;

				diff = abs((long)output - (long)target);
				if (best_diff > diff) {
					best_diff = diff;
					dpll->n = n;
					dpll->m = m;
					dpll->fdpll = fdpll;
					dpll->output = output;
				}

				if (diff == 0)
					goto done;
			}
		}
	}

done:
	dev_dbg(rcrtc->group->dev->dev,
		"output:%u, fdpll:%u, n:%u, m:%u, diff:%lu\n",
		 dpll->output, dpll->fdpll, dpll->n, dpll->m,
		 best_diff);
}

struct du_clk_params {
	struct clk *clk;
	unsigned long rate;
	unsigned long diff;
	u32 escr;
};

static void rcar_du_escr_divider(struct clk *clk, unsigned long target,
				 u32 escr, struct du_clk_params *params)
{
	unsigned long rate;
	unsigned long diff;
	u32 div;

	/*
	 * If the target rate has already been achieved perfectly we can't do
	 * better.
	 */
	if (params->diff == 0)
		return;

	/*
	 * Compute the input clock rate and internal divisor values to obtain
	 * the clock rate closest to the target frequency.
	 */
	rate = clk_round_rate(clk, target);
	div = clamp(DIV_ROUND_CLOSEST(rate, target), 1UL, 64UL) - 1;
	diff = abs(rate / (div + 1) - target);

	/*
	 * Store the parameters if the resulting frequency is better than any
	 * previously calculated value.
	 */
	if (diff < params->diff) {
		params->clk = clk;
		params->rate = rate;
		params->diff = diff;
		params->escr = escr | div;
	}
}

static const struct soc_device_attribute rcar_du_r8a7795_es1[] = {
	{ .soc_id = "r8a7795", .revision = "ES1.*" },
	{ /* sentinel */ }
};

struct cpg_param {
	u32	frequency;
	u32	pl5_refdiv;
	u32	pl5_intin;
	u32	pl5_fracin;
	u32	pl5_postdiv1;
	u32	pl5_postdiv2;
	u32	pl5_divval;
	u32	pl5_spread;
	u32	dsi_div_a;
	u32	dsi_div_b;
};

#define	TABLE_MAX	14
#define reg_write(x, a)	iowrite32(a, x)
#define	reg_read(x)	ioread32(x)
#define CPG_LPCLK_DIV	0

struct cpg_param resolution_param_def[TABLE_MAX] = {
	{
		/* VGA 25.175MHz	*/
		/* frequency		*/	25175,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	125,
		/* pl5_fracin		*/	14680064,
		/* pl5_postdiv1		*/	5,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* VGA 25.200MHz	*/
		/* frequency		*/	25200,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	75,
		/* pl5_fracin		*/	10066329,
		/* pl5_postdiv1		*/	3,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 480p/576p 27.000MHz	*/
		/* frequency		*/	27000,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	81,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	3,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 480p 27.027MHz	*/
		/* frequency		*/	27027,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	81,
		/* pl5_fracin		*/	1358954,
		/* pl5_postdiv1		*/	3,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* WVGA 29.605MHz	*/
		/* frequency		*/	29605,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	88,
		/* pl5_fracin		*/	13673431,
		/* pl5_postdiv1		*/	3,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* SVGA 40.00MHz	*/
		/* frequency		*/	40000,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	80,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	2,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* XGA	65.00MHz	*/
		/* frequency		*/	65000,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	130,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	2,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* WXGA 1280x800 71.0MHz	*/
		/* frequency		*/	71000,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	71,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 720p 74.176MHz	*/
		/* frequency		*/	74176,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	74,
		/* pl5_fracin		*/	2952790,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 720p 74.25MHz	*/
		/* frequency		*/	74250,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	75,
		/* pl5_fracin		*/	4194304,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* FWXGA 1360x768 85.5MHz	*/
		/* frequency		*/	85500,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	85,
		/* pl5_fracin		*/	8388608,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* WXGA+ 1440x900 88.75MHz		*/
		/* frequency		*/	88750,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	88,
		/* pl5_fracin		*/	12582912,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* SXGA 108.0MHz	*/
		/* frequency		*/	108000,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	108,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 1080p 148.5MHz	*/
		/* frequency		*/	148500,
		/* pl5_refdiv		*/	2,
		/* pl5_intin		*/	158,
		/* pl5_fracin		*/	5242880,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	1,	// 1/2
		/* dsi_div_b		*/	2,	// 1/3
	},
};

struct cpg_param resolution_param_2lane[TABLE_MAX] = {
	{
		/* VGA 25.175MHz	*/
		/* frequency		*/	25175,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	25,
		/* pl5_fracin		*/	2936012,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* VGA 25.200MHz	*/
		/* frequency		*/	25200,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	25,
		/* pl5_fracin		*/	3355443,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 480p/576p 27.000MHz	*/
		/* frequency		*/	27000,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	27,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 480p 27.027MHz	*/
		/* frequency		*/	27027,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	27,
		/* pl5_fracin		*/	452984,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* WVGA 29.605MHz	*/
		/* frequency		*/	29605,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	29,
		/* pl5_fracin		*/	10150216,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* SVGA 40.00MHz	*/
		/* frequency		*/	40000,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	40,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* XGA	65.00MHz	*/
		/* frequency		*/	65000,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	65,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* WXGA 1280x800 71.0MHz	*/
		/* frequency		*/	71000,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	71,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 720p 74.176MHz	*/
		/* frequency		*/	74176,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	74,
		/* pl5_fracin		*/	2952790,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* 720p 74.25MHz	*/
		/* frequency		*/	74250,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	74,
		/* pl5_fracin		*/	4194304,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* FWXGA 1360x768 85.5MHz	*/
		/* frequency		*/	85500,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	85,
		/* pl5_fracin		*/	8388608,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* WXGA+ 1440x900 88.75MHz		*/
		/* frequency		*/	88750,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	88,
		/* pl5_fracin		*/	12582912,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		/* SXGA 108.0MHz	*/
		/* frequency		*/	108000,
		/* pl5_refdiv		*/	1,
		/* pl5_intin		*/	108,
		/* pl5_fracin		*/	0,
		/* pl5_postdiv1		*/	1,
		/* pl5_postdiv2		*/	1,
		/* pl5_divval		*/	0,
		/* pl5_spread		*/	0x16,
		/* dsi_div_a		*/	3,	// 1/8
		/* dsi_div_b		*/	2,	// 1/3
	},
	{
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0
	},
};

#define	CPG_SIPPL3_CLK5		(0x0134)
#define	CPG_SIPLL5_STBY		(0x0140)
#define	CPG_SIPLL5_CLK1		(0x0144)
#define	CPG_SIPLL5_CLK3		(0x014C)
#define	CPG_SIPLL5_CLK4		(0x0150)
#define	CPG_SIPLL5_CLK5		(0x0154)
#define	CPG_SIPLL5_MON		(0x015C)
#define	CPG_PL2_DDIV		(0x0204)
#define	CPG_CPG_CLKSTATUS	(0x0280)
#define	CPG_PL5_SDIV		(0x0420)
#define	CPG_OTHERFUNC1_REG	(0x0BE8)
#define	CPG_CLKON_LCDC		(0x056c)
#define	CPG_CLKMON_LCDC		(0x06EC)

#define	PLL5_MON_PLL5_LOCK	(1 << 4)
#define	DIVDSILPCLK_STS		(1 << 7)

static void rcar_du_crtc_set_display_timing(struct rcar_du_crtc *rcrtc)
{
	const struct drm_display_mode *mode = &rcrtc->crtc.state->adjusted_mode;
	struct rcar_du_device *rcdu = rcrtc->group->dev;
	unsigned long mode_clock = mode->clock * 1000;
	u32 dsmr;
	u32 escr;
	u32 val, nowLcdcClkOn;
	struct cpg_param *resolution_param;
	unsigned long clk_pll5;

	if( rcdu->dsi_lanes == 2 ) {
		resolution_param = resolution_param_2lane;
		clk_pll5 = 0x10000; //SEL_PLL5_1 clock
	} else {
		/* support 4-lane MIPI DSI */
		resolution_param = resolution_param_def;
		clk_pll5 = 0x10001; //SEL_PLL5_3 clock
	}

	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L)) {
		u32 ditr0, ditr1, ditr2, ditr3, ditr4, ditr5, pbcr0;
		void __iomem *cpg_base = ioremap_nocache(0x11010000, 0x1000);
		u32 i, index, prevIndex = 0;

		for (i = 0; i < TABLE_MAX; i++)
		{
			if (resolution_param[i].frequency == mode->clock)
			{
				index = i;
				break;
			}
			if (resolution_param[i].frequency > mode->clock)
			{
				if ((resolution_param[i].frequency - mode->clock) > (mode->clock - resolution_param[prevIndex].frequency))
				{
					index = prevIndex;
				}
				else
				{
					index = i;
				}
				break;
			}
			prevIndex = i;
		}

		if (i == TABLE_MAX)
		{
			index = TABLE_MAX - 1;
		}

		if (resolution_param[i].frequency > 74250)
		{
			reg_write(cpg_base + CPG_SIPPL3_CLK5, 0x02);
		}
		val = reg_read(cpg_base + CPG_CLKON_LCDC);
		if (val != 0)
		{
			nowLcdcClkOn = 1;
		}
		else
		{
			nowLcdcClkOn = 0;
		}
		if (nowLcdcClkOn)
		{
			reg_write(cpg_base + CPG_CLKON_LCDC, 0x30000); //LCDC Clock Off
			do
			{
				val = reg_read(cpg_base + CPG_CLKMON_LCDC);
			} while(val != 0);
		}

		reg_write(cpg_base + CPG_SIPLL5_STBY, 0x10000); // RESETB = 0 Reset State
		do
		{
			val = reg_read(cpg_base + CPG_SIPLL5_MON);
		} while ((val & PLL5_MON_PLL5_LOCK) != 0);
		reg_write(cpg_base + CPG_SIPLL5_CLK1, 0x01110000 | (resolution_param[index].pl5_postdiv1<<0) | (resolution_param[index].pl5_postdiv2<<4) | (resolution_param[index].pl5_refdiv<<8)); //POSTDIV1, POSTDIV2, REFDIV
		reg_write(cpg_base + CPG_SIPLL5_CLK3, (resolution_param[index].pl5_divval<<0) | (resolution_param[index].pl5_fracin<<8)); //DIVVAL, FRACIN
		reg_write(cpg_base + CPG_SIPLL5_CLK4, 0x000000ff | (resolution_param[index].pl5_intin<<16)); //INTIN
		reg_write(cpg_base + CPG_SIPLL5_CLK5, (resolution_param[index].pl5_spread<<0)); //SPREAD

		do
		{
			val = reg_read(cpg_base + CPG_CPG_CLKSTATUS);
		} while ((val & DIVDSILPCLK_STS) != 0);
		reg_write(cpg_base + CPG_PL2_DDIV, 0x10000000 | (CPG_LPCLK_DIV<<12)); //DIV_DSI_LPCLK
		do
		{
			val = reg_read(cpg_base + CPG_CPG_CLKSTATUS);
		} while ((val & DIVDSILPCLK_STS) != 0);
	
		reg_write(cpg_base + CPG_PL5_SDIV, 0x01010000 | (resolution_param[index].dsi_div_a<<0) | (resolution_param[index].dsi_div_b << 8)); //DIV_DSI_A, DIV_DSI_B
		reg_write(cpg_base + CPG_OTHERFUNC1_REG, clk_pll5); //SEL_PLL5_4 Route Setting
		reg_write(cpg_base + CPG_SIPLL5_STBY, 0x00050001);
		do
		{
			val = reg_read(cpg_base + CPG_SIPLL5_MON);
		} while ((val & PLL5_MON_PLL5_LOCK) == 0);

		if (nowLcdcClkOn)
		{
			reg_write(cpg_base + CPG_CLKON_LCDC, 0x30003);// LCDC Clock On
			do
			{
				val = reg_read(cpg_base + CPG_CLKMON_LCDC);
			} while(val == 0);
		}
		iounmap(cpg_base);

		ditr0 = (DU_DITR0_DEMD_HIGH
		| ((mode->flags & DRM_MODE_FLAG_PVSYNC) ? DU_DITR0_VSPOL : 0)
		| ((mode->flags & DRM_MODE_FLAG_PHSYNC) ? DU_DITR0_HSPOL : 0));

		ditr1 = DU_DITR1_VSA(mode->vsync_end - mode->vsync_start)
		      | DU_DITR1_VACTIVE(mode->vdisplay);

		ditr2 = DU_DITR2_VBP(mode->vtotal - mode->vsync_end)
		      | DU_DITR2_VFP(mode->vsync_start - mode->vdisplay);

		ditr3 = DU_DITR3_HSA(mode->hsync_end - mode->hsync_start)
		      | DU_DITR3_HACTIVE(mode->hdisplay);

		ditr4 = DU_DITR4_HBP(mode->htotal - mode->hsync_end)
		      | DU_DITR4_HFP(mode->hsync_start - mode->hdisplay);

		ditr5 = DU_DITR5_VSFT(0) | DU_DITR5_HSFT(0);

		pbcr0 = DU_PBCR0_PB_DEP(0x1F);

		rcar_du_write(rcdu, DU_DITR0, ditr0);
		rcar_du_write(rcdu, DU_DITR1, ditr1);
		rcar_du_write(rcdu, DU_DITR2, ditr2);
		rcar_du_write(rcdu, DU_DITR3, ditr3);
		rcar_du_write(rcdu, DU_DITR4, ditr4);
		rcar_du_write(rcdu, DU_DITR5, ditr5);
		rcar_du_write(rcdu, DU_PBCR0, pbcr0);

		return;
	}

	if (rcdu->info->dpll_mask & (1 << rcrtc->index)) {
		unsigned long target = mode_clock;
		struct dpll_info dpll = { 0 };
		unsigned long extclk;
		u32 dpllcr;
		u32 div = 0;

		/*
		 * DU channels that have a display PLL can't use the internal
		 * system clock, and have no internal clock divider.
		 */

		/*
		 * The H3 ES1.x exhibits dot clock duty cycle stability issues.
		 * We can work around them by configuring the DPLL to twice the
		 * desired frequency, coupled with a /2 post-divider. Restrict
		 * the workaround to H3 ES1.x as ES2.0 and all other SoCs have
		 * no post-divider when a display PLL is present (as shown by
		 * the workaround breaking HDMI output on M3-W during testing).
		 */
		if (soc_device_match(rcar_du_r8a7795_es1)) {
			target *= 2;
			div = 1;
		}

		extclk = clk_get_rate(rcrtc->extclock);
		rcar_du_dpll_divider(rcrtc, &dpll, extclk, target);

		dpllcr = DPLLCR_CODE | DPLLCR_CLKE
		       | DPLLCR_FDPLL(dpll.fdpll)
		       | DPLLCR_N(dpll.n) | DPLLCR_M(dpll.m)
		       | DPLLCR_STBY;

		if (rcrtc->index == 1)
			dpllcr |= DPLLCR_PLCS1
			       |  DPLLCR_INCS_DOTCLKIN1;
		else
			dpllcr |= DPLLCR_PLCS0
			       |  DPLLCR_INCS_DOTCLKIN0;

		rcar_du_group_write(rcrtc->group, DPLLCR, dpllcr);

		escr = ESCR_DCLKSEL_DCLKIN | div;
	} else if (rcdu->info->lvds_clk_mask & BIT(rcrtc->index)) {
		/*
		 * Use the LVDS PLL output as the dot clock when outputting to
		 * the LVDS encoder on an SoC that supports this clock routing
		 * option. We use the clock directly in that case, without any
		 * additional divider.
		 */
		escr = ESCR_DCLKSEL_DCLKIN;
	} else {
		struct du_clk_params params = { .diff = (unsigned long)-1 };

		rcar_du_escr_divider(rcrtc->clock, mode_clock,
				     ESCR_DCLKSEL_CLKS, &params);
		if (rcrtc->extclock)
			rcar_du_escr_divider(rcrtc->extclock, mode_clock,
					     ESCR_DCLKSEL_DCLKIN, &params);

		dev_dbg(rcrtc->group->dev->dev,	"mode clock %lu %s rate %lu\n",
			mode_clock, params.clk == rcrtc->clock ? "cpg" : "ext",
			params.rate);

		clk_set_rate(params.clk, params.rate);
		escr = params.escr;
	}

	dev_dbg(rcrtc->group->dev->dev, "%s: ESCR 0x%08x\n", __func__, escr);

	rcar_du_group_write(rcrtc->group, rcrtc->index % 2 ? ESCR2 : ESCR,
			    escr);
	rcar_du_group_write(rcrtc->group, rcrtc->index % 2 ? OTAR2 : OTAR, 0);

	/* Signal polarities */
	dsmr = ((mode->flags & DRM_MODE_FLAG_PVSYNC) ? DSMR_VSL : 0)
	     | ((mode->flags & DRM_MODE_FLAG_PHSYNC) ? DSMR_HSL : 0)
	     | ((mode->flags & DRM_MODE_FLAG_INTERLACE) ? DSMR_ODEV : 0)
	     | DSMR_DIPM_DISP | DSMR_CSPM;
	rcar_du_crtc_write(rcrtc, DSMR, dsmr);

	/* Display timings */
	rcar_du_crtc_write(rcrtc, HDSR, mode->htotal - mode->hsync_start - 19);
	rcar_du_crtc_write(rcrtc, HDER, mode->htotal - mode->hsync_start +
					mode->hdisplay - 19);
	rcar_du_crtc_write(rcrtc, HSWR, mode->hsync_end -
					mode->hsync_start - 1);
	rcar_du_crtc_write(rcrtc, HCR,  mode->htotal - 1);

	rcar_du_crtc_write(rcrtc, VDSR, mode->crtc_vtotal -
					mode->crtc_vsync_end - 2);
	rcar_du_crtc_write(rcrtc, VDER, mode->crtc_vtotal -
					mode->crtc_vsync_end +
					mode->crtc_vdisplay - 2);
	rcar_du_crtc_write(rcrtc, VSPR, mode->crtc_vtotal -
					mode->crtc_vsync_end +
					mode->crtc_vsync_start - 1);
	rcar_du_crtc_write(rcrtc, VCR,  mode->crtc_vtotal - 1);

	rcar_du_crtc_write(rcrtc, DESR,  mode->htotal - mode->hsync_start - 1);
	rcar_du_crtc_write(rcrtc, DEWR,  mode->hdisplay);
}

static unsigned int plane_zpos(struct rcar_du_plane *plane)
{
	return plane->plane.state->normalized_zpos;
}

static const struct rcar_du_format_info *
plane_format(struct rcar_du_plane *plane)
{
	return to_rcar_plane_state(plane->plane.state)->format;
}

static void rcar_du_crtc_update_planes(struct rcar_du_crtc *rcrtc)
{
	struct rcar_du_plane *planes[RCAR_DU_NUM_HW_PLANES];
	struct rcar_du_device *rcdu = rcrtc->group->dev;
	unsigned int num_planes = 0;
	unsigned int dptsr_planes;
	unsigned int hwplanes = 0;
	unsigned int prio = 0;
	unsigned int i;
	u32 dspr = 0;

	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L))
		return;

	for (i = 0; i < rcrtc->group->num_planes; ++i) {
		struct rcar_du_plane *plane = &rcrtc->group->planes[i];
		unsigned int j;

		if (plane->plane.state->crtc != &rcrtc->crtc ||
		    !plane->plane.state->visible)
			continue;

		/* Insert the plane in the sorted planes array. */
		for (j = num_planes++; j > 0; --j) {
			if (plane_zpos(planes[j-1]) <= plane_zpos(plane))
				break;
			planes[j] = planes[j-1];
		}

		planes[j] = plane;
		prio += plane_format(plane)->planes * 4;
	}

	for (i = 0; i < num_planes; ++i) {
		struct rcar_du_plane *plane = planes[i];
		struct drm_plane_state *state = plane->plane.state;
		unsigned int index = to_rcar_plane_state(state)->hwindex;

		prio -= 4;
		dspr |= (index + 1) << prio;
		hwplanes |= 1 << index;

		if (plane_format(plane)->planes == 2) {
			index = (index + 1) % 8;

			prio -= 4;
			dspr |= (index + 1) << prio;
			hwplanes |= 1 << index;
		}
	}

	/* If VSP+DU integration is enabled the plane assignment is fixed. */
	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_VSP1_SOURCE)) {
		if (rcdu->info->gen < 3) {
			dspr = (rcrtc->index % 2) + 1;
			hwplanes = 1 << (rcrtc->index % 2);
		} else {
			dspr = (rcrtc->index % 2) ? 3 : 1;
			hwplanes = 1 << ((rcrtc->index % 2) ? 2 : 0);
		}
	}

	/*
	 * Update the planes to display timing and dot clock generator
	 * associations.
	 *
	 * Updating the DPTSR register requires restarting the CRTC group,
	 * resulting in visible flicker. To mitigate the issue only update the
	 * association if needed by enabled planes. Planes being disabled will
	 * keep their current association.
	 */
	mutex_lock(&rcrtc->group->lock);

	dptsr_planes = rcrtc->index % 2 ? rcrtc->group->dptsr_planes | hwplanes
		     : rcrtc->group->dptsr_planes & ~hwplanes;

	if (dptsr_planes != rcrtc->group->dptsr_planes) {
		rcar_du_group_write(rcrtc->group, DPTSR,
				    (dptsr_planes << 16) | dptsr_planes);
		rcrtc->group->dptsr_planes = dptsr_planes;

		if (rcrtc->group->used_crtcs)
			rcar_du_group_restart(rcrtc->group);
	}

	/* Restart the group if plane sources have changed. */
	if (rcrtc->group->need_restart)
		rcar_du_group_restart(rcrtc->group);

	mutex_unlock(&rcrtc->group->lock);

	rcar_du_group_write(rcrtc->group, rcrtc->index % 2 ? DS2PR : DS1PR,
			    dspr);
}

/* -----------------------------------------------------------------------------
 * Page Flip
 */

void rcar_du_crtc_finish_page_flip(struct rcar_du_crtc *rcrtc)
{
	struct drm_pending_vblank_event *event;
	struct drm_device *dev = rcrtc->crtc.dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	event = rcrtc->event;
	rcrtc->event = NULL;
	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (event == NULL)
		return;

	spin_lock_irqsave(&dev->event_lock, flags);
	drm_crtc_send_vblank_event(&rcrtc->crtc, event);
	wake_up(&rcrtc->flip_wait);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	drm_crtc_vblank_put(&rcrtc->crtc);
}

static bool rcar_du_crtc_page_flip_pending(struct rcar_du_crtc *rcrtc)
{
	struct drm_device *dev = rcrtc->crtc.dev;
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&dev->event_lock, flags);
	pending = rcrtc->event != NULL;
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return pending;
}

static void rcar_du_crtc_wait_page_flip(struct rcar_du_crtc *rcrtc)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	if (wait_event_timeout(rcrtc->flip_wait,
			       !rcar_du_crtc_page_flip_pending(rcrtc),
			       msecs_to_jiffies(50)))
		return;

	dev_warn(rcdu->dev, "page flip timeout\n");

	rcar_du_crtc_finish_page_flip(rcrtc);
}

/* -----------------------------------------------------------------------------
 * Start/Stop and Suspend/Resume
 */

static void rcar_du_crtc_setup(struct rcar_du_crtc *rcrtc)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	if (!rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L)) {
		/* Set display off and background to black */
		rcar_du_crtc_write(rcrtc, DOOR, DOOR_RGB(0, 0, 0));
		rcar_du_crtc_write(rcrtc, BPOR, BPOR_RGB(0, 0, 0));

		/* Configure display timings and output routing */
		rcar_du_crtc_set_display_timing(rcrtc);
		rcar_du_group_set_routing(rcrtc->group);

		/* Start with all planes disabled. */
		rcar_du_group_write(rcrtc->group,
				    rcrtc->index % 2 ? DS2PR : DS1PR, 0);
	} else {
		/* Configure display timings and output routing */
		rcar_du_crtc_set_display_timing(rcrtc);
	}

	/* Enable the VSP compositor. */
	if (rcar_du_has(rcrtc->group->dev, RCAR_DU_FEATURE_VSP1_SOURCE))
		rcar_du_vsp_enable(rcrtc);

	/* Turn vertical blanking interrupt reporting on. */
	drm_crtc_vblank_on(&rcrtc->crtc);
}

static int rcar_du_crtc_get(struct rcar_du_crtc *rcrtc)
{
	int ret;

	/*
	 * Guard against double-get, as the function is called from both the
	 * .atomic_enable() and .atomic_begin() handlers.
	 */
	if (rcrtc->initialized)
		return 0;

	if (rcrtc->rstc)
		reset_control_deassert(rcrtc->rstc);

	ret = clk_prepare_enable(rcrtc->clock);
	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(rcrtc->extclock);
	if (ret < 0)
		goto error_clock;

	ret = rcar_du_group_get(rcrtc->group);
	if (ret < 0)
		goto error_group;

	rcar_du_crtc_setup(rcrtc);
	rcrtc->initialized = true;

	return 0;

error_group:
	clk_disable_unprepare(rcrtc->extclock);
error_clock:
	clk_disable_unprepare(rcrtc->clock);

	if (rcrtc->rstc)
		reset_control_assert(rcrtc->rstc);

	return ret;
}

static void rcar_du_crtc_put(struct rcar_du_crtc *rcrtc)
{
	rcar_du_group_put(rcrtc->group);

	clk_disable_unprepare(rcrtc->extclock);
	clk_disable_unprepare(rcrtc->clock);

	if (rcrtc->rstc)
		reset_control_assert(rcrtc->rstc);

	rcrtc->initialized = false;
}

static void rcar_du_crtc_start(struct rcar_du_crtc *rcrtc)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;
	bool interlaced;

	if (!rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L)) {
		/*
		 * Select master sync mode. This enables display operation in master
		 * sync mode (with the HSYNC and VSYNC signals configured as outputs and
		 * actively driven).
		 */
		interlaced = rcrtc->crtc.mode.flags & DRM_MODE_FLAG_INTERLACE;
		rcar_du_crtc_dsysr_clr_set(rcrtc, DSYSR_TVM_MASK | DSYSR_SCM_MASK,
					   (interlaced ? DSYSR_SCM_INT_VIDEO : 0) |
					   DSYSR_TVM_MASTER);
	}

	rcar_du_group_start_stop(rcrtc->group, true);
}

static void rcar_du_crtc_disable_planes(struct rcar_du_crtc *rcrtc)
{
	struct rcar_du_device *rcdu = rcrtc->group->dev;
	struct drm_crtc *crtc = &rcrtc->crtc;
	u32 status;

	/* Make sure vblank interrupts are enabled. */
	drm_crtc_vblank_get(crtc);

	/*
	 * Disable planes and calculate how many vertical blanking interrupts we
	 * have to wait for. If a vertical blanking interrupt has been triggered
	 * but not processed yet, we don't know whether it occurred before or
	 * after the planes got disabled. We thus have to wait for two vblank
	 * interrupts in that case.
	 */
	spin_lock_irq(&rcrtc->vblank_lock);
	rcar_du_group_write(rcrtc->group, rcrtc->index % 2 ? DS2PR : DS1PR, 0);
	status = rcar_du_crtc_read(rcrtc, DSSR);
	rcrtc->vblank_count = status & DSSR_VBK ? 2 : 1;
	spin_unlock_irq(&rcrtc->vblank_lock);

	if (!wait_event_timeout(rcrtc->vblank_wait, rcrtc->vblank_count == 0,
				msecs_to_jiffies(100)))
		dev_warn(rcdu->dev, "vertical blanking timeout\n");

	drm_crtc_vblank_put(crtc);
}

static void rcar_du_crtc_stop(struct rcar_du_crtc *rcrtc)
{
	struct drm_crtc *crtc = &rcrtc->crtc;

	/*
	 * Disable all planes and wait for the change to take effect. This is
	 * required as the plane enable registers are updated on vblank, and no
	 * vblank will occur once the CRTC is stopped. Disabling planes when
	 * starting the CRTC thus wouldn't be enough as it would start scanning
	 * out immediately from old frame buffers until the next vblank.
	 *
	 * This increases the CRTC stop delay, especially when multiple CRTCs
	 * are stopped in one operation as we now wait for one vblank per CRTC.
	 * Whether this can be improved needs to be researched.
	 */
	rcar_du_crtc_disable_planes(rcrtc);

	/*
	 * Disable vertical blanking interrupt reporting. We first need to wait
	 * for page flip completion before stopping the CRTC as userspace
	 * expects page flips to eventually complete.
	 */
	rcar_du_crtc_wait_page_flip(rcrtc);
	drm_crtc_vblank_off(crtc);

	/* Disable the VSP compositor. */
	if (rcar_du_has(rcrtc->group->dev, RCAR_DU_FEATURE_VSP1_SOURCE))
		rcar_du_vsp_disable(rcrtc);

	/*
	 * Select switch sync mode. This stops display operation and configures
	 * the HSYNC and VSYNC signals as inputs.
	 *
	 * TODO: Find another way to stop the display for DUs that don't support
	 * TVM sync.
	 */
	if (rcar_du_has(rcrtc->group->dev, RCAR_DU_FEATURE_TVM_SYNC))
		rcar_du_crtc_dsysr_clr_set(rcrtc, DSYSR_TVM_MASK,
					   DSYSR_TVM_SWITCH);

	rcar_du_group_start_stop(rcrtc->group, false);
}

/* -----------------------------------------------------------------------------
 * CRTC Functions
 */

static int rcar_du_crtc_atomic_check(struct drm_crtc *crtc,
				     struct drm_crtc_state *state)
{
	struct rcar_du_crtc_state *rstate = to_rcar_crtc_state(state);
	struct drm_encoder *encoder;

	/* Store the routes from the CRTC output to the DU outputs. */
	rstate->outputs = 0;

	drm_for_each_encoder_mask(encoder, crtc->dev, state->encoder_mask) {
		struct rcar_du_encoder *renc = to_rcar_encoder(encoder);

		rstate->outputs |= BIT(renc->output);
	}

	return 0;
}

static void rcar_du_crtc_atomic_enable(struct drm_crtc *crtc,
				       struct drm_crtc_state *old_state)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
	struct rcar_du_crtc_state *rstate = to_rcar_crtc_state(crtc->state);
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	rcar_du_crtc_get(rcrtc);

	/*
	 * On D3/E3 the dot clock is provided by the LVDS encoder attached to
	 * the DU channel. We need to enable its clock output explicitly if
	 * the LVDS output is disabled.
	 */
	if (rcdu->info->lvds_clk_mask & BIT(rcrtc->index) &&
	    rstate->outputs == BIT(RCAR_DU_OUTPUT_DPAD0)) {
		struct rcar_du_encoder *encoder =
			rcdu->encoders[RCAR_DU_OUTPUT_LVDS0 + rcrtc->index];
		const struct drm_display_mode *mode =
			&crtc->state->adjusted_mode;

		if (rcar_lvds_dual_link(
		    rcdu->encoders[RCAR_DU_OUTPUT_LVDS0]->base.bridge))
			encoder = rcdu->encoders[RCAR_DU_OUTPUT_LVDS0];

		rcar_lvds_clk_enable(encoder->base.bridge,
				     mode->clock * 1000);
	}


	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L) &&
	    rstate->outputs == BIT(RCAR_DU_OUTPUT_MIPI_DSI0)) {
		struct rcar_du_encoder *encoder =
			rcdu->encoders[RCAR_DU_OUTPUT_MIPI_DSI0 + rcrtc->index];

		rzg2l_mipi_dsi_clk_enable(encoder->base.bridge);
	}

	rcar_du_crtc_start(rcrtc);
}

static void rcar_du_crtc_atomic_disable(struct drm_crtc *crtc,
					struct drm_crtc_state *old_state)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
	struct rcar_du_crtc_state *rstate = to_rcar_crtc_state(old_state);
	struct rcar_du_device *rcdu = rcrtc->group->dev;

	rcar_du_crtc_stop(rcrtc);
	rcar_du_crtc_put(rcrtc);

	if (rcdu->info->lvds_clk_mask & BIT(rcrtc->index) &&
	    rstate->outputs == BIT(RCAR_DU_OUTPUT_DPAD0)) {
		struct rcar_du_encoder *encoder =
			rcdu->encoders[RCAR_DU_OUTPUT_LVDS0 + rcrtc->index];

		if (rcar_lvds_dual_link(
		    rcdu->encoders[RCAR_DU_OUTPUT_LVDS0]->base.bridge))
			encoder = rcdu->encoders[RCAR_DU_OUTPUT_LVDS0];
		/*
		 * Disable the LVDS clock output, see
		 * rcar_du_crtc_atomic_enable().
		 */
		rcar_lvds_clk_disable(encoder->base.bridge);
	}

	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L) &&
	    rstate->outputs == BIT(RCAR_DU_OUTPUT_MIPI_DSI0)) {
		struct rcar_du_encoder *encoder =
			rcdu->encoders[RCAR_DU_OUTPUT_MIPI_DSI0 + rcrtc->index];

		rzg2l_mipi_dsi_clk_disable(encoder->base.bridge);
	}

	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void rcar_du_crtc_atomic_begin(struct drm_crtc *crtc,
				      struct drm_crtc_state *old_crtc_state)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	WARN_ON(!crtc->state->enable);

	/*
	 * If a mode set is in progress we can be called with the CRTC disabled.
	 * We thus need to first get and setup the CRTC in order to configure
	 * planes. We must *not* put the CRTC in .atomic_flush(), as it must be
	 * kept awake until the .atomic_enable() call that will follow. The get
	 * operation in .atomic_enable() will in that case be a no-op, and the
	 * CRTC will be put later in .atomic_disable().
	 *
	 * If a mode set is not in progress the CRTC is enabled, and the
	 * following get call will be a no-op. There is thus no need to belance
	 * it in .atomic_flush() either.
	 */
	rcar_du_crtc_get(rcrtc);

	if (rcar_du_has(rcrtc->group->dev, RCAR_DU_FEATURE_VSP1_SOURCE))
		rcar_du_vsp_atomic_begin(rcrtc);
}

static void rcar_du_crtc_atomic_flush(struct drm_crtc *crtc,
				      struct drm_crtc_state *old_crtc_state)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
	struct drm_device *dev = rcrtc->crtc.dev;
	unsigned long flags;

	rcar_du_crtc_update_planes(rcrtc);

	if (crtc->state->event) {
		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&dev->event_lock, flags);
		rcrtc->event = crtc->state->event;
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}

	if (rcar_du_has(rcrtc->group->dev, RCAR_DU_FEATURE_VSP1_SOURCE))
		rcar_du_vsp_atomic_flush(rcrtc);
}

enum drm_mode_status rcar_du_crtc_mode_valid(struct drm_crtc *crtc,
				   const struct drm_display_mode *mode)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
	struct rcar_du_device *rcdu = rcrtc->group->dev;
	bool interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE;
	unsigned int vbp;

	if (interlaced && !rcar_du_has(rcdu, RCAR_DU_FEATURE_INTERLACED))
		return MODE_NO_INTERLACE;

	/*
	 * The hardware requires a minimum combined horizontal sync and back
	 * porch of 20 pixels and a minimum vertical back porch of 3 lines.
	 */
	if (mode->htotal - mode->hsync_start < 20)
		return MODE_HBLANK_NARROW;

	vbp = (mode->vtotal - mode->vsync_end) / (interlaced ? 2 : 1);
	if (vbp < 3)
		return MODE_VBLANK_NARROW;

	return MODE_OK;
}

static const struct drm_crtc_helper_funcs crtc_helper_funcs = {
	.atomic_check = rcar_du_crtc_atomic_check,
	.atomic_begin = rcar_du_crtc_atomic_begin,
	.atomic_flush = rcar_du_crtc_atomic_flush,
	.atomic_enable = rcar_du_crtc_atomic_enable,
	.atomic_disable = rcar_du_crtc_atomic_disable,
	.mode_valid = rcar_du_crtc_mode_valid,
};

static struct drm_crtc_state *
rcar_du_crtc_atomic_duplicate_state(struct drm_crtc *crtc)
{
	struct rcar_du_crtc_state *state;
	struct rcar_du_crtc_state *copy;

	if (WARN_ON(!crtc->state))
		return NULL;

	state = to_rcar_crtc_state(crtc->state);
	copy = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (copy == NULL)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &copy->state);

	return &copy->state;
}

static void rcar_du_crtc_atomic_destroy_state(struct drm_crtc *crtc,
					      struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(to_rcar_crtc_state(state));
}

static void rcar_du_crtc_reset(struct drm_crtc *crtc)
{
	struct rcar_du_crtc_state *state;

	if (crtc->state) {
		rcar_du_crtc_atomic_destroy_state(crtc, crtc->state);
		crtc->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return;

	state->crc.source = VSP1_DU_CRC_NONE;
	state->crc.index = 0;

	crtc->state = &state->state;
	crtc->state->crtc = crtc;
}

static int rcar_du_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	rcar_du_crtc_write(rcrtc, DSRCR, DSRCR_VBCL);
	rcar_du_crtc_set(rcrtc, DIER, DIER_VBE);
	rcrtc->vblank_enable = true;

	return 0;
}

static void rcar_du_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	rcar_du_crtc_clr(rcrtc, DIER, DIER_VBE);
	rcrtc->vblank_enable = false;
}

static int rcar_du_crtc_set_crc_source(struct drm_crtc *crtc,
				       const char *source_name,
				       size_t *values_cnt)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_state *state;
	enum vsp1_du_crc_source source;
	unsigned int index = 0;
	unsigned int i;
	int ret;

	/*
	 * Parse the source name. Supported values are "plane%u" to compute the
	 * CRC on an input plane (%u is the plane ID), and "auto" to compute the
	 * CRC on the composer (VSP) output.
	 */
	if (!source_name) {
		source = VSP1_DU_CRC_NONE;
	} else if (!strcmp(source_name, "auto")) {
		source = VSP1_DU_CRC_OUTPUT;
	} else if (strstarts(source_name, "plane")) {
		source = VSP1_DU_CRC_PLANE;

		ret = kstrtouint(source_name + strlen("plane"), 10, &index);
		if (ret < 0)
			return ret;

		for (i = 0; i < rcrtc->vsp->num_planes; ++i) {
			if (index == rcrtc->vsp->planes[i].plane.base.id) {
				index = i;
				break;
			}
		}

		if (i >= rcrtc->vsp->num_planes)
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	*values_cnt = 1;

	/* Perform an atomic commit to set the CRC source. */
	drm_modeset_acquire_init(&ctx, 0);

	state = drm_atomic_state_alloc(crtc->dev);
	if (!state) {
		ret = -ENOMEM;
		goto unlock;
	}

	state->acquire_ctx = &ctx;

retry:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (!IS_ERR(crtc_state)) {
		struct rcar_du_crtc_state *rcrtc_state;

		rcrtc_state = to_rcar_crtc_state(crtc_state);
		rcrtc_state->crc.source = source;
		rcrtc_state->crc.index = index;

		ret = drm_atomic_commit(state);
	} else {
		ret = PTR_ERR(crtc_state);
	}

	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}

	drm_atomic_state_put(state);

unlock:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}

static const struct drm_crtc_funcs crtc_funcs_gen2 = {
	.reset = rcar_du_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = rcar_du_crtc_atomic_duplicate_state,
	.atomic_destroy_state = rcar_du_crtc_atomic_destroy_state,
	.enable_vblank = rcar_du_crtc_enable_vblank,
	.disable_vblank = rcar_du_crtc_disable_vblank,
};

static const struct drm_crtc_funcs crtc_funcs_gen3 = {
	.reset = rcar_du_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = rcar_du_crtc_atomic_duplicate_state,
	.atomic_destroy_state = rcar_du_crtc_atomic_destroy_state,
	.enable_vblank = rcar_du_crtc_enable_vblank,
	.disable_vblank = rcar_du_crtc_disable_vblank,
	.set_crc_source = rcar_du_crtc_set_crc_source,
};

/* -----------------------------------------------------------------------------
 * Interrupt Handling
 */

static irqreturn_t rcar_du_crtc_irq(int irq, void *arg)
{
	struct rcar_du_crtc *rcrtc = arg;
	struct rcar_du_device *rcdu = rcrtc->group->dev;
	irqreturn_t ret = IRQ_NONE;
	u32 status;

	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L)) {
		status = rcar_du_crtc_read(rcrtc, DU_MCR0);
		rcar_du_crtc_write(rcrtc, DU_MCR0, DU_MCR0_PB_CLR & status);
		ret = IRQ_HANDLED;
	} else {
		spin_lock(&rcrtc->vblank_lock);

		status = rcar_du_crtc_read(rcrtc, DSSR);
		rcar_du_crtc_write(rcrtc, DSRCR, status & DSRCR_MASK);

		if (status & DSSR_VBK) {
			/*
			 * Wake up the vblank wait if the counter reaches 0. This must
			 * be protected by the vblank_lock to avoid races in
			 * rcar_du_crtc_disable_planes().
			 */
			if (rcrtc->vblank_count) {
				if (--rcrtc->vblank_count == 0)
					wake_up(&rcrtc->vblank_wait);
			}
		}

		spin_unlock(&rcrtc->vblank_lock);

		if (status & DSSR_VBK) {
			if (rcdu->info->gen < 3) {
				drm_crtc_handle_vblank(&rcrtc->crtc);
				rcar_du_crtc_finish_page_flip(rcrtc);
			}

		ret = IRQ_HANDLED;
		}
	}

	return ret;
}

/* -----------------------------------------------------------------------------
 * Initialization
 */

int rcar_du_crtc_create(struct rcar_du_group *rgrp, unsigned int swindex,
			unsigned int hwindex)
{
	static const unsigned int mmio_offsets[] = {
		DU0_REG_OFFSET, DU1_REG_OFFSET, DU2_REG_OFFSET, DU3_REG_OFFSET
	};

	struct rcar_du_device *rcdu = rgrp->dev;
	struct platform_device *pdev = to_platform_device(rcdu->dev);
	struct rcar_du_crtc *rcrtc = &rcdu->crtcs[swindex];
	struct drm_crtc *crtc = &rcrtc->crtc;
	struct drm_plane *primary;
	unsigned int irqflags;
	struct clk *clk;
	char clk_name[9];
	char *name;
	int irq;
	int ret;

	/* Get the CRTC clock and the optional external clock. */
	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_CRTC_IRQ_CLOCK)) {
		sprintf(clk_name, "du.%u", hwindex);
		name = clk_name;
	} else {
		name = NULL;
	}

	rcrtc->clock = devm_clk_get(rcdu->dev, name);
	if (IS_ERR(rcrtc->clock)) {
		dev_err(rcdu->dev, "no clock for DU channel %u\n", hwindex);
		return PTR_ERR(rcrtc->clock);
	}

	sprintf(clk_name, "dclkin.%u", hwindex);
	clk = devm_clk_get(rcdu->dev, clk_name);
	if (!IS_ERR(clk)) {
		rcrtc->extclock = clk;
	} else if (PTR_ERR(clk) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (rcdu->info->dpll_mask & BIT(hwindex)) {
		/*
		 * DU channels that have a display PLL can't use the internal
		 * system clock and thus require an external clock.
		 */
		ret = PTR_ERR(clk);
		dev_err(rcdu->dev, "can't get dclkin.%u: %d\n", hwindex, ret);
		return ret;
	}

	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L)) {
		rcrtc->rstc = devm_reset_control_get(rcdu->dev, NULL);
		if (IS_ERR(rcrtc->rstc)) {
			dev_err(rcdu->dev, "can't get cpg reset\n");
			return PTR_ERR(rcrtc->rstc);
		}
	}

	init_waitqueue_head(&rcrtc->flip_wait);
	init_waitqueue_head(&rcrtc->vblank_wait);
	spin_lock_init(&rcrtc->vblank_lock);

	rcrtc->group = rgrp;
	rcrtc->mmio_offset = mmio_offsets[hwindex];
	rcrtc->index = hwindex;
	rcrtc->dsysr = (rcrtc->index % 2 ? 0 : DSYSR_DRES) | DSYSR_TVM_TVSYNC;

	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_VSP1_SOURCE))
		primary = &rcrtc->vsp->planes[rcrtc->vsp_pipe].plane;
	else
		primary = &rgrp->planes[swindex % 2].plane;

	ret = drm_crtc_init_with_planes(rcdu->ddev, crtc, primary, NULL,
					rcdu->info->gen <= 2 ?
					&crtc_funcs_gen2 : &crtc_funcs_gen3,
					NULL);
	if (ret < 0)
		return ret;

	drm_crtc_helper_add(crtc, &crtc_helper_funcs);

	/* Start with vertical blanking interrupt reporting disabled. */
	drm_crtc_vblank_off(crtc);

	if (!rcar_du_has(rcdu, RCAR_DU_FEATURE_RZG2L)) {
		/* Register the interrupt handler. */
		if (rcar_du_has(rcdu, RCAR_DU_FEATURE_CRTC_IRQ_CLOCK)) {
			/* The IRQ's are associated with the CRTC (sw)index. */
			irq = platform_get_irq(pdev, swindex);
			irqflags = 0;
		} else {
			irq = platform_get_irq(pdev, 0);
			irqflags = IRQF_SHARED;
		}

		if (irq < 0) {
			dev_err(rcdu->dev, "no IRQ for CRTC %u\n", swindex);
			return irq;
		}

		ret = devm_request_irq(rcdu->dev, irq, rcar_du_crtc_irq,
				       irqflags, dev_name(rcdu->dev), rcrtc);
		if (ret < 0) {
			dev_err(rcdu->dev,
				"failed to register IRQ for CRTC %u\n",
								swindex);
			return ret;
		}
	}
	return 0;
}
