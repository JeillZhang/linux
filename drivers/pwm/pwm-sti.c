// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PWM device driver for ST SoCs
 *
 * Copyright (C) 2013-2016 STMicroelectronics (R&D) Limited
 *
 * Author: Ajit Pal Singh <ajitpal.singh@st.com>
 *         Lee Jones <lee.jones@linaro.org>
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>

#define PWM_OUT_VAL(x)	(0x00 + (4 * (x))) /* Device's Duty Cycle register */
#define PWM_CPT_VAL(x)	(0x10 + (4 * (x))) /* Capture value */
#define PWM_CPT_EDGE(x) (0x30 + (4 * (x))) /* Edge to capture on */

#define STI_PWM_CTRL		0x50	/* Control/Config register */
#define STI_INT_EN		0x54	/* Interrupt Enable/Disable register */
#define STI_INT_STA		0x58	/* Interrupt Status register */
#define PWM_INT_ACK		0x5c
#define PWM_PRESCALE_LOW_MASK	0x0f
#define PWM_PRESCALE_HIGH_MASK	0xf0
#define PWM_CPT_EDGE_MASK	0x03
#define PWM_INT_ACK_MASK	0x1ff

#define STI_MAX_CPT_DEVS	4
#define CPT_DC_MAX		0xff

/* Regfield IDs */
enum {
	/* Bits in PWM_CTRL*/
	PWMCLK_PRESCALE_LOW,
	PWMCLK_PRESCALE_HIGH,
	CPTCLK_PRESCALE,

	PWM_OUT_EN,
	PWM_CPT_EN,

	PWM_CPT_INT_EN,
	PWM_CPT_INT_STAT,

	/* Keep last */
	MAX_REGFIELDS
};

/*
 * Each capture input can be programmed to detect rising-edge, falling-edge,
 * either edge or neither egde.
 */
enum sti_cpt_edge {
	CPT_EDGE_DISABLED,
	CPT_EDGE_RISING,
	CPT_EDGE_FALLING,
	CPT_EDGE_BOTH,
};

struct sti_cpt_ddata {
	u32 snapshot[3];
	unsigned int index;
	struct mutex lock;
	wait_queue_head_t wait;
};

struct sti_pwm_chip {
	struct device *dev;
	struct clk *pwm_clk;
	struct clk *cpt_clk;
	struct regmap *regmap;
	unsigned int pwm_num_devs;
	unsigned int cpt_num_devs;
	unsigned int max_pwm_cnt;
	unsigned int max_prescale;
	struct sti_cpt_ddata *ddata;
	struct regmap_field *prescale_low;
	struct regmap_field *prescale_high;
	struct regmap_field *pwm_out_en;
	struct regmap_field *pwm_cpt_en;
	struct regmap_field *pwm_cpt_int_en;
	struct regmap_field *pwm_cpt_int_stat;
	struct pwm_device *cur;
	unsigned long configured;
	unsigned int en_count;
	void __iomem *mmio;
};

static const struct reg_field sti_pwm_regfields[MAX_REGFIELDS] = {
	[PWMCLK_PRESCALE_LOW] = REG_FIELD(STI_PWM_CTRL, 0, 3),
	[PWMCLK_PRESCALE_HIGH] = REG_FIELD(STI_PWM_CTRL, 11, 14),
	[CPTCLK_PRESCALE] = REG_FIELD(STI_PWM_CTRL, 4, 8),
	[PWM_OUT_EN] = REG_FIELD(STI_PWM_CTRL, 9, 9),
	[PWM_CPT_EN] = REG_FIELD(STI_PWM_CTRL, 10, 10),
	[PWM_CPT_INT_EN] = REG_FIELD(STI_INT_EN, 1, 4),
	[PWM_CPT_INT_STAT] = REG_FIELD(STI_INT_STA, 1, 4),
};

static inline struct sti_pwm_chip *to_sti_pwmchip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

/*
 * Calculate the prescaler value corresponding to the period.
 */
static int sti_pwm_get_prescale(struct sti_pwm_chip *pc, unsigned long period,
				unsigned int *prescale)
{
	unsigned long clk_rate;
	unsigned long value;
	unsigned int ps;

	clk_rate = clk_get_rate(pc->pwm_clk);
	if (!clk_rate) {
		dev_err(pc->dev, "failed to get clock rate\n");
		return -EINVAL;
	}

	/*
	 * prescale = ((period_ns * clk_rate) / (10^9 * (max_pwm_cnt + 1)) - 1
	 */
	value = NSEC_PER_SEC / clk_rate;
	value *= pc->max_pwm_cnt + 1;

	if (period % value)
		return -EINVAL;

	ps  = period / value - 1;
	if (ps > pc->max_prescale)
		return -EINVAL;

	*prescale = ps;

	return 0;
}

/*
 * For STiH4xx PWM IP, the PWM period is fixed to 256 local clock cycles. The
 * only way to change the period (apart from changing the PWM input clock) is
 * to change the PWM clock prescaler.
 *
 * The prescaler is of 8 bits, so 256 prescaler values and hence 256 possible
 * period values are supported (for a particular clock rate). The requested
 * period will be applied only if it matches one of these 256 values.
 */
static int sti_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			  int duty_ns, int period_ns)
{
	struct sti_pwm_chip *pc = to_sti_pwmchip(chip);
	unsigned int ncfg, value, prescale = 0;
	struct pwm_device *cur = pc->cur;
	struct device *dev = pc->dev;
	bool period_same = false;
	int ret;

	ncfg = hweight_long(pc->configured);
	if (ncfg)
		period_same = (period_ns == pwm_get_period(cur));

	/*
	 * Allow configuration changes if one of the following conditions
	 * satisfy.
	 * 1. No devices have been configured.
	 * 2. Only one device has been configured and the new request is for
	 *    the same device.
	 * 3. Only one device has been configured and the new request is for
	 *    a new device and period of the new device is same as the current
	 *    configured period.
	 * 4. More than one devices are configured and period of the new
	 *    requestis the same as the current period.
	 */
	if (!ncfg ||
	    ((ncfg == 1) && (pwm->hwpwm == cur->hwpwm)) ||
	    ((ncfg == 1) && (pwm->hwpwm != cur->hwpwm) && period_same) ||
	    ((ncfg > 1) && period_same)) {
		/* Enable clock before writing to PWM registers. */
		ret = clk_enable(pc->pwm_clk);
		if (ret)
			return ret;

		ret = clk_enable(pc->cpt_clk);
		if (ret)
			return ret;

		if (!period_same) {
			ret = sti_pwm_get_prescale(pc, period_ns, &prescale);
			if (ret)
				goto clk_dis;

			value = prescale & PWM_PRESCALE_LOW_MASK;

			ret = regmap_field_write(pc->prescale_low, value);
			if (ret)
				goto clk_dis;

			value = (prescale & PWM_PRESCALE_HIGH_MASK) >> 4;

			ret = regmap_field_write(pc->prescale_high, value);
			if (ret)
				goto clk_dis;
		}

		/*
		 * When PWMVal == 0, PWM pulse = 1 local clock cycle.
		 * When PWMVal == max_pwm_count,
		 * PWM pulse = (max_pwm_count + 1) local cycles,
		 * that is continuous pulse: signal never goes low.
		 */
		value = pc->max_pwm_cnt * duty_ns / period_ns;

		ret = regmap_write(pc->regmap, PWM_OUT_VAL(pwm->hwpwm), value);
		if (ret)
			goto clk_dis;

		ret = regmap_field_write(pc->pwm_cpt_int_en, 0);

		set_bit(pwm->hwpwm, &pc->configured);
		pc->cur = pwm;

		dev_dbg(dev, "prescale:%u, period:%i, duty:%i, value:%u\n",
			prescale, period_ns, duty_ns, value);
	} else {
		return -EINVAL;
	}

clk_dis:
	clk_disable(pc->pwm_clk);
	clk_disable(pc->cpt_clk);
	return ret;
}

static int sti_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct sti_pwm_chip *pc = to_sti_pwmchip(chip);
	struct device *dev = pc->dev;
	int ret;

	/*
	 * Since we have a common enable for all PWM devices, do not enable if
	 * already enabled.
	 */

	if (!pc->en_count) {
		ret = clk_enable(pc->pwm_clk);
		if (ret)
			return ret;

		ret = clk_enable(pc->cpt_clk);
		if (ret)
			return ret;

		ret = regmap_field_write(pc->pwm_out_en, 1);
		if (ret) {
			dev_err(dev, "failed to enable PWM device %u: %d\n",
				pwm->hwpwm, ret);
			return ret;
		}
	}

	pc->en_count++;

	return 0;
}

static void sti_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct sti_pwm_chip *pc = to_sti_pwmchip(chip);

	if (--pc->en_count)
		return;

	regmap_field_write(pc->pwm_out_en, 0);

	clk_disable(pc->pwm_clk);
	clk_disable(pc->cpt_clk);
}

static void sti_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct sti_pwm_chip *pc = to_sti_pwmchip(chip);

	clear_bit(pwm->hwpwm, &pc->configured);
}

static int sti_pwm_capture(struct pwm_chip *chip, struct pwm_device *pwm,
			   struct pwm_capture *result, unsigned long timeout)
{
	struct sti_pwm_chip *pc = to_sti_pwmchip(chip);
	struct sti_cpt_ddata *ddata = &pc->ddata[pwm->hwpwm];
	struct device *dev = pc->dev;
	unsigned int effective_ticks;
	unsigned long long high, low;
	int ret;

	if (pwm->hwpwm >= pc->cpt_num_devs) {
		dev_err(dev, "device %u is not valid\n", pwm->hwpwm);
		return -EINVAL;
	}

	mutex_lock(&ddata->lock);
	ddata->index = 0;

	/* Prepare capture measurement */
	regmap_write(pc->regmap, PWM_CPT_EDGE(pwm->hwpwm), CPT_EDGE_RISING);
	regmap_field_write(pc->pwm_cpt_int_en, BIT(pwm->hwpwm));

	/* Enable capture */
	ret = regmap_field_write(pc->pwm_cpt_en, 1);
	if (ret) {
		dev_err(dev, "failed to enable PWM capture %u: %d\n",
			pwm->hwpwm, ret);
		goto out;
	}

	ret = wait_event_interruptible_timeout(ddata->wait, ddata->index > 1,
					       msecs_to_jiffies(timeout));

	regmap_write(pc->regmap, PWM_CPT_EDGE(pwm->hwpwm), CPT_EDGE_DISABLED);

	if (ret == -ERESTARTSYS)
		goto out;

	switch (ddata->index) {
	case 0:
	case 1:
		/*
		 * Getting here could mean:
		 *  - input signal is constant of less than 1 Hz
		 *  - there is no input signal at all
		 *
		 * In such case the frequency is rounded down to 0
		 */
		result->period = 0;
		result->duty_cycle = 0;

		break;

	case 2:
		/* We have everying we need */
		high = ddata->snapshot[1] - ddata->snapshot[0];
		low = ddata->snapshot[2] - ddata->snapshot[1];

		effective_ticks = clk_get_rate(pc->cpt_clk);

		result->period = (high + low) * NSEC_PER_SEC;
		result->period /= effective_ticks;

		result->duty_cycle = high * NSEC_PER_SEC;
		result->duty_cycle /= effective_ticks;

		break;

	default:
		dev_err(dev, "internal error\n");
		break;
	}

out:
	/* Disable capture */
	regmap_field_write(pc->pwm_cpt_en, 0);

	mutex_unlock(&ddata->lock);
	return ret;
}

static int sti_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			 const struct pwm_state *state)
{
	struct sti_pwm_chip *pc = to_sti_pwmchip(chip);
	struct device *dev = pc->dev;
	int err;

	if (pwm->hwpwm >= pc->pwm_num_devs) {
		dev_err(dev, "device %u is not valid for pwm mode\n",
			pwm->hwpwm);
		return -EINVAL;
	}

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		if (pwm->state.enabled)
			sti_pwm_disable(chip, pwm);

		return 0;
	}

	err = sti_pwm_config(chip, pwm, state->duty_cycle, state->period);
	if (err)
		return err;

	if (!pwm->state.enabled)
		err = sti_pwm_enable(chip, pwm);

	return err;
}

static const struct pwm_ops sti_pwm_ops = {
	.capture = sti_pwm_capture,
	.apply = sti_pwm_apply,
	.free = sti_pwm_free,
};

static irqreturn_t sti_pwm_interrupt(int irq, void *data)
{
	struct sti_pwm_chip *pc = data;
	struct device *dev = pc->dev;
	struct sti_cpt_ddata *ddata;
	int devicenum;
	unsigned int cpt_int_stat;
	unsigned int reg;
	int ret = IRQ_NONE;

	ret = regmap_field_read(pc->pwm_cpt_int_stat, &cpt_int_stat);
	if (ret)
		return ret;

	while (cpt_int_stat) {
		devicenum = ffs(cpt_int_stat) - 1;

		ddata = &pc->ddata[devicenum];

		/*
		 * Capture input:
		 *    _______                   _______
		 *   |       |                 |       |
		 * __|       |_________________|       |________
		 *   ^0      ^1                ^2
		 *
		 * Capture start by the first available rising edge. When a
		 * capture event occurs, capture value (CPT_VALx) is stored,
		 * index incremented, capture edge changed.
		 *
		 * After the capture, if the index > 1, we have collected the
		 * necessary data so we signal the thread waiting for it and
		 * disable the capture by setting capture edge to none
		 */

		regmap_read(pc->regmap,
			    PWM_CPT_VAL(devicenum),
			    &ddata->snapshot[ddata->index]);

		switch (ddata->index) {
		case 0:
		case 1:
			regmap_read(pc->regmap, PWM_CPT_EDGE(devicenum), &reg);
			reg ^= PWM_CPT_EDGE_MASK;
			regmap_write(pc->regmap, PWM_CPT_EDGE(devicenum), reg);

			ddata->index++;
			break;

		case 2:
			regmap_write(pc->regmap,
				     PWM_CPT_EDGE(devicenum),
				     CPT_EDGE_DISABLED);
			wake_up(&ddata->wait);
			break;

		default:
			dev_err(dev, "Internal error\n");
		}

		cpt_int_stat &= ~BIT_MASK(devicenum);

		ret = IRQ_HANDLED;
	}

	/* Just ACK everything */
	regmap_write(pc->regmap, PWM_INT_ACK, PWM_INT_ACK_MASK);

	return ret;
}

static int sti_pwm_probe_regmap(struct sti_pwm_chip *pc)
{
	struct device *dev = pc->dev;

	pc->prescale_low = devm_regmap_field_alloc(dev, pc->regmap,
					sti_pwm_regfields[PWMCLK_PRESCALE_LOW]);
	if (IS_ERR(pc->prescale_low))
		return PTR_ERR(pc->prescale_low);

	pc->prescale_high = devm_regmap_field_alloc(dev, pc->regmap,
					sti_pwm_regfields[PWMCLK_PRESCALE_HIGH]);
	if (IS_ERR(pc->prescale_high))
		return PTR_ERR(pc->prescale_high);

	pc->pwm_out_en = devm_regmap_field_alloc(dev, pc->regmap,
						 sti_pwm_regfields[PWM_OUT_EN]);
	if (IS_ERR(pc->pwm_out_en))
		return PTR_ERR(pc->pwm_out_en);

	pc->pwm_cpt_en = devm_regmap_field_alloc(dev, pc->regmap,
						 sti_pwm_regfields[PWM_CPT_EN]);
	if (IS_ERR(pc->pwm_cpt_en))
		return PTR_ERR(pc->pwm_cpt_en);

	pc->pwm_cpt_int_en = devm_regmap_field_alloc(dev, pc->regmap,
						sti_pwm_regfields[PWM_CPT_INT_EN]);
	if (IS_ERR(pc->pwm_cpt_int_en))
		return PTR_ERR(pc->pwm_cpt_int_en);

	pc->pwm_cpt_int_stat = devm_regmap_field_alloc(dev, pc->regmap,
						sti_pwm_regfields[PWM_CPT_INT_STAT]);
	if (PTR_ERR_OR_ZERO(pc->pwm_cpt_int_stat))
		return PTR_ERR(pc->pwm_cpt_int_stat);

	return 0;
}

static const struct regmap_config sti_pwm_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int sti_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	u32 num_devs;
	unsigned int pwm_num_devs = 0;
	unsigned int cpt_num_devs = 0;
	struct pwm_chip *chip;
	struct sti_pwm_chip *pc;
	unsigned int i;
	int irq, ret;

	ret = of_property_read_u32(np, "st,pwm-num-chan", &num_devs);
	if (!ret)
		pwm_num_devs = num_devs;

	ret = of_property_read_u32(np, "st,capture-num-chan", &num_devs);
	if (!ret)
		cpt_num_devs = num_devs;

	if (!pwm_num_devs && !cpt_num_devs)
		return dev_err_probe(dev, -EINVAL, "No channels configured\n");

	chip = devm_pwmchip_alloc(dev, max(pwm_num_devs, cpt_num_devs), sizeof(*pc));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	pc = to_sti_pwmchip(chip);

	pc->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pc->mmio))
		return PTR_ERR(pc->mmio);

	pc->regmap = devm_regmap_init_mmio(dev, pc->mmio,
					   &sti_pwm_regmap_config);
	if (IS_ERR(pc->regmap))
		return dev_err_probe(dev, PTR_ERR(pc->regmap),
				     "Failed to initialize regmap\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(&pdev->dev, irq, sti_pwm_interrupt, 0,
			       pdev->name, pc);
	if (ret < 0)
		dev_err_probe(&pdev->dev, ret, "Failed to request IRQ\n");

	/*
	 * Setup PWM data with default values: some values could be replaced
	 * with specific ones provided from Device Tree.
	 */
	pc->max_prescale = 0xff;
	pc->max_pwm_cnt = 255;
	pc->pwm_num_devs = pwm_num_devs;
	pc->cpt_num_devs = cpt_num_devs;

	pc->dev = dev;
	pc->en_count = 0;

	ret = sti_pwm_probe_regmap(pc);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize regmap fields\n");

	if (pwm_num_devs) {
		pc->pwm_clk = devm_clk_get_prepared(dev, "pwm");
		if (IS_ERR(pc->pwm_clk))
			return dev_err_probe(dev, PTR_ERR(pc->pwm_clk),
					     "failed to get PWM clock\n");
	}

	if (cpt_num_devs) {
		pc->cpt_clk = devm_clk_get_prepared(dev, "capture");
		if (IS_ERR(pc->cpt_clk))
			return dev_err_probe(dev, PTR_ERR(pc->cpt_clk),
					     "failed to get PWM capture clock\n");

		pc->ddata = devm_kcalloc(dev, cpt_num_devs,
					 sizeof(*pc->ddata), GFP_KERNEL);
		if (!pc->ddata)
			return -ENOMEM;

		for (i = 0; i < cpt_num_devs; i++) {
			struct sti_cpt_ddata *ddata = &pc->ddata[i];

			init_waitqueue_head(&ddata->wait);
			mutex_init(&ddata->lock);
		}
	}

	chip->ops = &sti_pwm_ops;

	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register pwm chip\n");

	return 0;
}

static const struct of_device_id sti_pwm_of_match[] = {
	{ .compatible = "st,sti-pwm", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sti_pwm_of_match);

static struct platform_driver sti_pwm_driver = {
	.driver = {
		.name = "sti-pwm",
		.of_match_table = sti_pwm_of_match,
	},
	.probe = sti_pwm_probe,
};
module_platform_driver(sti_pwm_driver);

MODULE_AUTHOR("Ajit Pal Singh <ajitpal.singh@st.com>");
MODULE_DESCRIPTION("STMicroelectronics ST PWM driver");
MODULE_LICENSE("GPL");
