// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/G2L Pin Function Controller and GPIO support
 *
 * Copyright (C) 2020 Renesas Electronics Corp.
 */

#ifndef _RZG2L_PINCTRL_
#define _RZG2L_PINCTRL_

#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/phy.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinmux.h"

#define P(n)	(0x0000 + 0x10 + (n))     /* Port Register */
#define PM(n)	(0x0100 + 0x20 + (n) * 2) /* Port Mode Register */
#define PMC(n)	(0x0200 + 0x10 + (n))     /* Port Mode Control Register */
#define PFC(n)	(0x0400 + 0x40 + (n) * 4) /* Port Function Control Register */
#define PIN(n)	(0x0800 + 0x10 + (n))     /* Port Input Register */
#define ISEL(n)	(0x2C00 + 0x80 + (n) * 8) /* IRQ Enable Control Register */
#define PWPR	(0x3014)                  /* Port Write Protection Register */

#define IOLH(n) (0x1000 + (n) * 8)	/* IOLH Switch Register */
#define SR(n)   (0x1400 + (n) * 8)	/* Slew Rate Switch Register */
#define IEN(n)  (0x1800 + (n) * 8)	/* Input Enable Switch Register */
#define SD_CH(n) (0x3000 + (n) * 4)	/* SD IO Voltage Control Register */
#define QSPI	(0x3008)		/* QSPI IO Voltage Control Register */
#define ETH_CH(n) (0x300C + (n) * 4)	/* Ether Voltage Control Register */
#define ETH_MODE_CTRL	0x3018	/* Ether MII/RGMII Mode Control Register */

#define PWPR_B0WI		BIT(7)	/* Bit Write Disable */
#define PWPR_PFCWE		BIT(6)	/* PFC Register Write Enable */
#define PVDD_1800		1	/* I/O domain voltage <= 1.8V */
#define PVDD_3300		0	/* I/O domain voltage >= 3.3V */
#define ETH_PVDD_2500		BIT(1)	/* Ether I/O voltage 2.5V */
#define ETH_PVDD_1800		BIT(0)	/* Ether I/O voltage 1.8V */
#define ETH_PVDD_3300		0	/* Ether I/O voltage 3.3V */
#define ETH_MII_CH(x)		(1 << (x)) /* Ether channel x MII mode */
#define ETH_RGMII_CH(x)		(0 << (x)) /* Ether channel x RGMII mode */

#define PM_INPUT		0x1	/* Input Mode */
#define PM_OUTPUT		0x2	/* Output Mode (disable Input) */
#define PM_OUTPUT_INPUT		0x3	/* Output Mode (enable Input) */

#define PM_MASK			0x03
#define PFC_MASK		0x07
#define IOLH_MASK		0x03
#define IEN_MASK		0x01
#define SR_MASK			0x01
#define PVDD_MASK		0x01
#define ETH_PVDD_MASK		0x03

#define RZG2L_MAX_PINS_PER_PORT		8
#define RZG2L_PIN_ID_TO_PORT(id)	((id) / RZG2L_MAX_PINS_PER_PORT)
#define RZG2L_PIN_ID_TO_PIN(id)		((id) % RZG2L_MAX_PINS_PER_PORT)

#define PIN_CFG_DRIVE_STRENGTH		BIT(0)
#define PIN_CFG_SLEW_RATE		BIT(1)
#define PIN_CFG_INPUT_ENABLE		BIT(2)
#define PIN_CFG_IO_VOLTAGE_SD0		BIT(3)
#define PIN_CFG_IO_VOLTAGE_SD1		BIT(4)
#define PIN_CFG_IO_VOLTAGE_QSPI		BIT(5)
#define PIN_CFG_IO_VOLTAGE_ETH0		BIT(6)
#define PIN_CFG_IO_VOLTAGE_ETH1		BIT(7)
#define PIN_CFG_IO_VOLTAGE		GENMASK(7, 3)

#define GPIOF_OUTPUT			0
#define GPIOF_INPUT			1
#define GPIOF_BIDIRECTION		2
#define GPIOF_HI_Z			3

/* Hardware Registers support GPIO interrupt in IA55 Module */
#define TSCR	0x0		/* TINT Interrupt Status Control Register */
#define TITSR0	0x4		/* TINT detection method selection register 0 */
#define TITSR1	0x8		/* TINT detection method selection register 1 */
#define TSSR(n)	(0x10 + (n) * 4) /* TINT source selection register */

#define RISING_EDGE	0
#define FALLING_EDGE	1
#define HIGH_LEVEL	2
#define LOW_LEVEL	3
#define IRQ_MASK	0x3

#define TINT_MAX	32

struct rzg2l_pin_info {
	u32 port;		/* Store port position of a pin */
	u32 bit;		/* Store bit position of a pin */
	u32 gpio_irq_id;	/* Store interrupt id for a pin */
};

struct rzg2l_pin_soc {
	const struct pinctrl_pin_desc	*pins;
	unsigned int			npins;
	const struct group_desc		*groups;
	unsigned int			ngroups;
	const struct function_desc	*funcs;
	unsigned int			nfuncs;

	unsigned int			nports;

	unsigned int			nirqs;
	const struct rzg2l_pin_info	*pin_info;
	unsigned int			ngpioints;
};

struct pin_data {
	u32 port;
	u64 bit;
	u32 configs;
};

struct rzg2l_pinctrl {
	struct pinctrl_dev		*pctrl_dev;
	struct pinctrl_desc		pctrl_desc;

	void __iomem			*base;
	void __iomem			*base_tint;
	struct device			*dev;
	struct clk			*clk;

	struct gpio_chip		gpio_chip;
	struct irq_chip			irq_chip;

	const struct rzg2l_pin_soc	*psoc;

	spinlock_t			lock;

	unsigned int			nports;

	unsigned int			irq_start;

	/* This array will store GPIO IDs for TINT[0-32] with value:
	 * - [15-0] bits: store GPIO IDs (ID = port * 8 + bit).
	 * - [16] bit: store active status (1 for enabled, 0 for disabled).
	 */
	u32				tint[TINT_MAX];
};

#define RZ_G2L_PINCTRL_PIN_GPIO(port, configs)			\
	{							\
		(RZG2L_MAX_PINS_PER_PORT) * (port),		\
		__stringify(P##port##_0),			\
		(void *) configs,				\
	},							\
	{							\
		(RZG2L_MAX_PINS_PER_PORT) * (port) + 1,		\
		__stringify(P##port##_1),			\
		(void *) configs,				\
	},							\
	{							\
		(RZG2L_MAX_PINS_PER_PORT) * (port) + 2,		\
		__stringify(P##port##_2),			\
		(void *) configs,				\
	},							\
	{							\
		(RZG2L_MAX_PINS_PER_PORT) * (port) + 3,		\
		__stringify(P##port##_3),			\
		(void *) configs,				\
	},							\
	{							\
		(RZG2L_MAX_PINS_PER_PORT) * (port) + 4,		\
		__stringify(P##port##_4),			\
		(void *) configs,				\
	},							\
	{							\
		(RZG2L_MAX_PINS_PER_PORT) * (port) + 5,		\
		__stringify(P##port##_5),			\
		(void *) configs,				\
	},							\
	{							\
		(RZG2L_MAX_PINS_PER_PORT) * (port) + 6,		\
		__stringify(P##port##_6),			\
		(void *) configs,				\
	},							\
	{							\
		(RZG2L_MAX_PINS_PER_PORT) * (port) + 7,		\
		__stringify(P##port##_7),			\
		(void *) configs,				\
	}

#define RZ_G2L_PINCTRL_PIN_NO_GPIO(npin_gpios, id, name)	\
	{							\
		npin_gpios + id,				\
		__stringify(name),				\
		name##_data,					\
	}

#define RZ_G2L_PIN(port, bit)		(port * RZG2L_MAX_PINS_PER_PORT + bit)

#define RZ_G2L_PINCTRL_PIN_GROUP(name, mode)			\
	{							\
		__stringify(name),				\
		name##_pins,					\
		ARRAY_SIZE(name##_pins),			\
		(void *) mode,					\
	}

extern const struct rzg2l_pin_soc r9a07g044l_pinctrl_data;

int rzg2l_pinctrl_eth_mode_set(struct device *dev,
			       phy_interface_t interface,
			       unsigned int eth_channel);
#endif
