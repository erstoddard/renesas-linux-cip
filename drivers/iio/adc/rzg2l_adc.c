// SPDX-License-Identifier: GPL-2.0
/*
 * RZ/G2L ADC driver
 *  Copyright (c) 2021 Renesas Electronics Europe GmbH
 *
 * Author: Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <linux/regulator/consumer.h>
#include <linux/iio/iio.h>
#include <linux/reset.h>
#include <linux/slab.h>

#define ADM(n)		(n * 0x4)
#define ADM0_ADCE	BIT(0)
#define ADM0_ADBSY	BIT(1)
#define ADM0_PWDWNB	BIT(2)
#define ADM0_SRESB	BIT(15)
#define ADINT		0x20
#define ADSTS		0x24
#define ADSTS_CSEST	BIT(16)
#define ADIVC		0x28
#define ADFIL		0x2c
#define ADCR(n)		(0x30 + (n * 0x4))

#define ADC_MAX_CHANNELS	8
#define ADC_CHN_MASK		0x7
#define ADC_TIMEOUT		msecs_to_jiffies(1000 * 3)

#define ADC_CHANNEL(_index, _id) {				\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = _index,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.datasheet_name = _id,					\
}

enum trigger_mode {
	SW_TRIGGER = 0,
	EXT_TRIGGER,
	MTU3A_TRIGGER,
	GPT_TRIGGER
};

struct rzg2l_data {
	int				num_bits;
	const struct iio_chan_spec	*channels;
	int				num_channels;
	int				trigger_mode;
};

struct rzg2l_adc {
	void __iomem		*base;
	struct clk		*clk;
	struct reset_control	*rstc;
	struct completion	completion;
	const struct rzg2l_data *data;
	u16			last_val[ADC_MAX_CHANNELS];
};

static unsigned int rzg2l_adc_readl(struct rzg2l_adc *adc, u32 reg)
{
	return readl(adc->base + reg);
}

static void rzg2l_adc_writel(struct rzg2l_adc *adc, unsigned int reg, u32 val)
{
	writel(val, adc->base + reg);
}

static void rzg2l_adc_pwr_down(struct rzg2l_adc *adc, bool sleep)
{
	u32 reg;

	/* stop AD conversion */
	reg = rzg2l_adc_readl(adc, ADM(0));
	reg &= ~ADM0_ADCE;
	rzg2l_adc_writel(adc, ADM(0), reg);
	wmb();

	/* make sure AD conversion has stopped */
	if (sleep)
		while (rzg2l_adc_readl(adc, ADM(0)) & ADM0_ADBSY)
			usleep_range(1000, 2000);

	reg = rzg2l_adc_readl(adc, ADM(0));
	reg &= ~ADM0_PWDWNB;
	rzg2l_adc_writel(adc, ADM(0), reg);
	wmb();
	if (sleep)
		usleep_range(10, 20);
}

static int rzg2l_adc_read_raw(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan,
				    int *val, int *val2, long mask)
{
	struct rzg2l_adc *adc = iio_priv(indio_dev);
	u32 reg;
	u8 ch;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&indio_dev->mlock);

		if (rzg2l_adc_readl(adc, ADM(0)) & ADM0_ADBSY) {
			mutex_unlock(&indio_dev->mlock);
			return -EBUSY;
		}

		reinit_completion(&adc->completion);

		ch = chan->channel & ADC_CHN_MASK;
		/* SW trigger */
		reg = rzg2l_adc_readl(adc, ADM(1));
		reg &= ~GENMASK(13, 12);
		reg &= ~BIT(4); /* 1 buffer mode */
		reg |= BIT(2); /* select mode */
		reg &= ~BIT(0); /* SW mode */
		rzg2l_adc_writel(adc, ADM(1), reg);

		/* select channel */
		reg = rzg2l_adc_readl(adc, ADM(2));
		reg &= ~GENMASK(7, 0);
		reg |= BIT(ch);
		rzg2l_adc_writel(adc, ADM(2), reg);

		reg = rzg2l_adc_readl(adc, ADM(3));
		reg &= ~GENMASK(31, 24);
		reg |= 0xe << 16;
		reg |= 0x578; /* FIXME set ADSMP[15:0] */
		rzg2l_adc_writel(adc, ADM(3), reg);

		reg = rzg2l_adc_readl(adc, ADIVC);
		reg &= ~GENMASK(8, 0);
		reg |= 0x4;
		rzg2l_adc_writel(adc, ADIVC, reg);

		reg = rzg2l_adc_readl(adc, ADINT);
		reg &= ~BIT(31);
		reg |= BIT(16);
		reg |= BIT(ch);
		rzg2l_adc_writel(adc, ADINT, reg);

		reg = rzg2l_adc_readl(adc, ADM(0));
		reg |= ADM0_PWDWNB;
		rzg2l_adc_writel(adc, ADM(0), reg);
		wmb();

		usleep_range(1000, 2000);
		reg = rzg2l_adc_readl(adc, ADM(0));
		reg |= ADM0_ADCE;
		rzg2l_adc_writel(adc, ADM(0), reg);

		if (!wait_for_completion_timeout(&adc->completion, ADC_TIMEOUT)) {
			reg &= ~GENMASK(7, 0);
			rzg2l_adc_writel(adc, ADINT, reg);
			rzg2l_adc_pwr_down(adc, true);
			mutex_unlock(&indio_dev->mlock);
			return -ETIMEDOUT;
		}

		*val = adc->last_val[ch];
		mutex_unlock(&indio_dev->mlock);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static irqreturn_t rzg2l_adc_isr(int irq, void *dev_id)
{
	struct rzg2l_adc *adc = (struct rzg2l_adc *)dev_id;
	u32 reg;
	u8 i;

	reg = rzg2l_adc_readl(adc, ADSTS);
	if (reg & ADSTS_CSEST) {
		/* TODO re-select correct channel */
		rzg2l_adc_writel(adc, ADSTS, reg);
		return IRQ_HANDLED;
	}

	if (!(reg & GENMASK(7, 0)))
		return IRQ_HANDLED;

	for (i = 0; i <= 7; i++) {
		if (reg && BIT(i))
			adc->last_val[i] = rzg2l_adc_readl(adc, ADCR(i)) & ~GENMASK(31, 12);
	}

	/* clear interrupts */
	rzg2l_adc_writel(adc, ADSTS, reg);
	rzg2l_adc_pwr_down(adc, false);

	complete(&adc->completion);

	return IRQ_HANDLED;
}

static const struct iio_chan_spec rzg2l_adc_iio_channels[] = {
	ADC_CHANNEL(0, "adc0"),
	ADC_CHANNEL(1, "adc1"),
	ADC_CHANNEL(2, "adc2"),
	ADC_CHANNEL(3, "adc3"),
	ADC_CHANNEL(4, "adc4"),
	ADC_CHANNEL(5, "adc5"),
	ADC_CHANNEL(6, "adc6"),
	ADC_CHANNEL(7, "adc7"),
	ADC_CHANNEL(8, "adc8"),
};

static const struct iio_info rzg2l_adc_iio_info = {
	.read_raw = rzg2l_adc_read_raw,
};

static void rzg2l_reset_controller(struct rzg2l_adc *adc)
{
	u32 val;

	reset_control_assert(adc->rstc);
	usleep_range(1000, 2000);
	reset_control_deassert(adc->rstc);
	usleep_range(1000, 2000);

	val = rzg2l_adc_readl(adc, ADM(0));
	val |= ADM0_SRESB;
	rzg2l_adc_writel(adc, ADM(0), val);
	wmb();
	usleep_range(1000, 2000);

	while (!(rzg2l_adc_readl(adc, ADM(0)) & ADM0_SRESB))
		usleep_range(1000, 2000);
}

static int rzg2l_parse_of(struct platform_device *pdev, struct rzg2l_adc *adc)
{
	struct rzg2l_data *data;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* TODO: read from OF */
	data->num_channels = 8;
	data->trigger_mode = SW_TRIGGER;
	adc->data = data;

	return 0;
}

static int rzg2l_adc_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct rzg2l_adc *adc;
	struct resource *res;
	int ret;
	int irq;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*adc));
	if (!indio_dev) {
		dev_err(&pdev->dev, "failed allocating iio device\n");
		return -ENOMEM;
	}

	adc = iio_priv(indio_dev);
	if (!adc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	adc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(adc->base)) {
		dev_err(&pdev->dev, "missing mem resource");
		return PTR_ERR(adc->base);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq resource\n");
		return irq;
	}

	adc->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(adc->clk)) {
		dev_err(&pdev->dev, "missing controller clock");
		return PTR_ERR(adc->clk);
	}

	adc->rstc = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(adc->rstc)) {
		dev_err(&pdev->dev, "failed to get cpg reset\n");
		return PTR_ERR(adc->rstc);
	}

	ret = clk_prepare_enable(adc->clk);
	if (ret)
		return ret;

	ret = rzg2l_parse_of(pdev, adc);
	if (ret)
		return ret;

	rzg2l_reset_controller(adc);

	init_completion(&adc->completion);

	platform_set_drvdata(pdev, indio_dev);

	ret = devm_request_irq(&pdev->dev, irq, rzg2l_adc_isr,
			       0, dev_name(&pdev->dev), adc);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed requesting irq %d\n", irq);
		return ret;
	}

	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->dev.of_node = pdev->dev.of_node;
	indio_dev->info = &rzg2l_adc_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = rzg2l_adc_iio_channels;
	indio_dev->num_channels = adc->data->num_channels;

	ret = iio_device_register(indio_dev);
	if (ret) {
		clk_disable_unprepare(adc->clk);
		return ret;
	}

	return 0;
}

static int rzg2l_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct rzg2l_adc *adc = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	kfree(indio_dev->name);
	iio_device_free(indio_dev);

	clk_disable_unprepare(adc->clk);

	return 0;
}

static const struct of_device_id rzg2l_adc_match[] = {
	{
		.compatible = "renesas,adc-r9a07g044l",
	},
	{
		.compatible = "renesas,adc-r9a07g054l",
	},
	{},
};
MODULE_DEVICE_TABLE(of, rzg2l_adc_match);

static struct platform_driver rzg2l_adc_driver = {
	.probe		= rzg2l_adc_probe,
	.remove		= rzg2l_adc_remove,
	.driver		= {
		.name	= "rzg2l-adc",
		.of_match_table = rzg2l_adc_match,
	},
};

module_platform_driver(rzg2l_adc_driver);

MODULE_AUTHOR("Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/G2L ADC driver");
MODULE_LICENSE("GPL v2");
