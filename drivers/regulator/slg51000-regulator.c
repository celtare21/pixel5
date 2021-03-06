// SPDX-License-Identifier: GPL-2.0+
/*
 * SLG51000 High PSRR, Multi-Output Regulators
 *
 * Copyright (C) 2019 Google, Inc.
 * Author: CY Tseng <cytseng@google.com>
 *
 * Copyright (C) 2019 Dialog Semiconductor
 * Author: Eric Jeong <eric.jeong.opensource@diasemi.com>
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include "slg51000-regulator.h"

#define SLG51000_SCTL_EVT               7
#define SLG51000_MAX_EVT_REGISTER       8
#define SLG51000_LDOHP_LV_MIN           1200000
#define SLG51000_LDOHP_HV_MIN           2400000

#define MAX_RETRY 10
#define MIN_SLEEP_USEC 3000
#define MAX_SLEEP_USEC 6000
#define SLEEP_10000_USEC 10000
#define SLEEP_RANGE_USEC 1000

u8 chip_id[3];

enum slg51000_regulators {
	SLG51000_REGULATOR_LDO1 = 0,
	SLG51000_REGULATOR_LDO2,
	SLG51000_REGULATOR_LDO3,
	SLG51000_REGULATOR_LDO4,
	SLG51000_REGULATOR_LDO5,
	SLG51000_REGULATOR_LDO6,
	SLG51000_REGULATOR_LDO7,
	SLG51000_MAX_REGULATORS,
};

struct slg51000 {
	struct device *dev;
	struct regmap *regmap;
	struct regulator_desc *rdesc[SLG51000_MAX_REGULATORS];
	struct regulator_dev *rdev[SLG51000_MAX_REGULATORS];
	int chip_irq;
	int chip_cs_pin;
};

struct slg51000_evt_sta {
	unsigned int ereg;
	unsigned int sreg;
};

static const struct slg51000_evt_sta es_reg[SLG51000_MAX_EVT_REGISTER] = {
	{SLG51000_LDO1_EVENT, SLG51000_LDO1_STATUS},
	{SLG51000_LDO2_EVENT, SLG51000_LDO2_STATUS},
	{SLG51000_LDO3_EVENT, SLG51000_LDO3_STATUS},
	{SLG51000_LDO4_EVENT, SLG51000_LDO4_STATUS},
	{SLG51000_LDO5_EVENT, SLG51000_LDO5_STATUS},
	{SLG51000_LDO6_EVENT, SLG51000_LDO6_STATUS},
	{SLG51000_LDO7_EVENT, SLG51000_LDO7_STATUS},
	{SLG51000_SYSCTL_EVENT, SLG51000_SYSCTL_STATUS},
};

static const struct regmap_range slg51000_writeable_ranges[] = {
	regmap_reg_range(SLG51000_SYSCTL_MATRIX_CONF_A,
			 SLG51000_SYSCTL_MATRIX_CONF_A),
	regmap_reg_range(SLG51000_LDO_HP_STARTUP_ILIM,
			 SLG51000_LDO_HP_STARTUP_ILIM),
	regmap_reg_range(SLG51000_LDO1_VSEL, SLG51000_LDO1_VSEL),
	regmap_reg_range(SLG51000_LDO1_MINV, SLG51000_LDO1_MAXV),
	regmap_reg_range(SLG51000_LDO1_IRQ_MASK, SLG51000_LDO1_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO2_VSEL, SLG51000_LDO2_VSEL),
	regmap_reg_range(SLG51000_LDO2_MINV, SLG51000_LDO2_MAXV),
	regmap_reg_range(SLG51000_LDO2_IRQ_MASK, SLG51000_LDO2_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO3_VSEL, SLG51000_LDO3_VSEL),
	regmap_reg_range(SLG51000_LDO3_MINV, SLG51000_LDO3_MAXV),
	regmap_reg_range(SLG51000_LDO3_CONF1, SLG51000_LDO3_CONF1),
	regmap_reg_range(SLG51000_LDO3_IRQ_MASK, SLG51000_LDO3_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO4_VSEL, SLG51000_LDO4_VSEL),
	regmap_reg_range(SLG51000_LDO4_MINV, SLG51000_LDO4_MAXV),
	regmap_reg_range(SLG51000_LDO4_IRQ_MASK, SLG51000_LDO4_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO5_VSEL, SLG51000_LDO5_VSEL),
	regmap_reg_range(SLG51000_LDO5_MINV, SLG51000_LDO5_MAXV),
	regmap_reg_range(SLG51000_LDO5_IRQ_MASK, SLG51000_LDO5_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO6_VSEL, SLG51000_LDO6_VSEL),
	regmap_reg_range(SLG51000_LDO6_MINV, SLG51000_LDO6_MAXV),
	regmap_reg_range(SLG51000_LDO6_IRQ_MASK, SLG51000_LDO6_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO7_VSEL, SLG51000_LDO7_VSEL),
	regmap_reg_range(SLG51000_LDO7_MINV, SLG51000_LDO7_MAXV),
	regmap_reg_range(SLG51000_LDO7_IRQ_MASK, SLG51000_LDO7_IRQ_MASK),
	regmap_reg_range(SLG51000_OTP_IRQ_MASK, SLG51000_OTP_IRQ_MASK),
	regmap_reg_range(SLG51000_SW_TEST_MODE_1, SLG51000_SW_TEST_MODE_4),
	regmap_reg_range(SLG51000_MUXARRAY_INPUT_SEL_39,
				SLG51000_MUXARRAY_INPUT_SEL_39),
	regmap_reg_range(SLG51000_LUTARRAY_LUT_VAL_3,
				SLG51000_LUTARRAY_LUT_VAL_3),
};

static const struct regmap_range slg51000_readable_ranges[] = {
	regmap_reg_range(SLG51000_SYSCTL_PATN_ID_B0,
			 SLG51000_SYSCTL_PATN_ID_B2),
	regmap_reg_range(SLG51000_SYSCTL_SYS_CONF_A,
			 SLG51000_SYSCTL_SYS_CONF_A),
	regmap_reg_range(SLG51000_SYSCTL_SYS_CONF_D,
			 SLG51000_SYSCTL_MATRIX_CONF_B),
	regmap_reg_range(SLG51000_SYSCTL_REFGEN_CONF_C,
			 SLG51000_SYSCTL_UVLO_CONF_A),
	regmap_reg_range(SLG51000_SYSCTL_FAULT_LOG1, SLG51000_SYSCTL_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO_HP_STARTUP_ILIM,
			 SLG51000_LDO_HP_STARTUP_ILIM),
	regmap_reg_range(SLG51000_IO_GPIO1_CONF, SLG51000_IO_GPIO_STATUS),
	regmap_reg_range(SLG51000_LUTARRAY_LUT_VAL_0,
			 SLG51000_LUTARRAY_LUT_VAL_11),
	regmap_reg_range(SLG51000_MUXARRAY_INPUT_SEL_0,
			 SLG51000_MUXARRAY_INPUT_SEL_63),
	regmap_reg_range(SLG51000_PWRSEQ_RESOURCE_EN_0,
			 SLG51000_PWRSEQ_INPUT_SENSE_CONF_B),
	regmap_reg_range(SLG51000_LDO1_VSEL, SLG51000_LDO1_VSEL),
	regmap_reg_range(SLG51000_LDO1_MINV, SLG51000_LDO1_MAXV),
	regmap_reg_range(SLG51000_LDO1_TRIM2, SLG51000_LDO1_VSEL_ACTUAL),
	regmap_reg_range(SLG51000_LDO1_EVENT, SLG51000_LDO1_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO2_VSEL, SLG51000_LDO2_VSEL),
	regmap_reg_range(SLG51000_LDO2_MINV, SLG51000_LDO2_MAXV),
	regmap_reg_range(SLG51000_LDO2_TRIM2, SLG51000_LDO2_VSEL_ACTUAL),
	regmap_reg_range(SLG51000_LDO2_EVENT, SLG51000_LDO2_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO3_VSEL, SLG51000_LDO3_VSEL),
	regmap_reg_range(SLG51000_LDO3_MINV, SLG51000_LDO3_MAXV),
	regmap_reg_range(SLG51000_LDO3_TRIM2, SLG51000_LDO3_VSEL_ACTUAL),
	regmap_reg_range(SLG51000_LDO3_EVENT, SLG51000_LDO3_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO4_VSEL, SLG51000_LDO4_VSEL),
	regmap_reg_range(SLG51000_LDO4_MINV, SLG51000_LDO4_MAXV),
	regmap_reg_range(SLG51000_LDO4_TRIM2, SLG51000_LDO4_VSEL_ACTUAL),
	regmap_reg_range(SLG51000_LDO4_EVENT, SLG51000_LDO4_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO5_VSEL, SLG51000_LDO5_VSEL),
	regmap_reg_range(SLG51000_LDO5_MINV, SLG51000_LDO5_MAXV),
	regmap_reg_range(SLG51000_LDO5_TRIM2, SLG51000_LDO5_VSEL_ACTUAL),
	regmap_reg_range(SLG51000_LDO5_EVENT, SLG51000_LDO5_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO6_VSEL, SLG51000_LDO6_VSEL),
	regmap_reg_range(SLG51000_LDO6_MINV, SLG51000_LDO6_MAXV),
	regmap_reg_range(SLG51000_LDO6_TRIM2, SLG51000_LDO6_VSEL_ACTUAL),
	regmap_reg_range(SLG51000_LDO6_EVENT, SLG51000_LDO6_IRQ_MASK),
	regmap_reg_range(SLG51000_LDO7_VSEL, SLG51000_LDO7_VSEL),
	regmap_reg_range(SLG51000_LDO7_MINV, SLG51000_LDO7_MAXV),
	regmap_reg_range(SLG51000_LDO7_TRIM2, SLG51000_LDO7_VSEL_ACTUAL),
	regmap_reg_range(SLG51000_LDO7_EVENT, SLG51000_LDO7_IRQ_MASK),
	regmap_reg_range(SLG51000_OTP_EVENT, SLG51000_OTP_EVENT),
	regmap_reg_range(SLG51000_OTP_IRQ_MASK, SLG51000_OTP_IRQ_MASK),
	regmap_reg_range(SLG51000_LOCK_GLOBAL_LOCK_CTRL1,
			 SLG51000_LOCK_GLOBAL_LOCK_CTRL1),
};

static const struct regmap_range slg51000_volatile_ranges[] = {
	regmap_reg_range(SLG51000_SYSCTL_FAULT_LOG1, SLG51000_SYSCTL_STATUS),
	regmap_reg_range(SLG51000_IO_GPIO_STATUS, SLG51000_IO_GPIO_STATUS),
	regmap_reg_range(SLG51000_LDO1_EVENT, SLG51000_LDO1_STATUS),
	regmap_reg_range(SLG51000_LDO2_EVENT, SLG51000_LDO2_STATUS),
	regmap_reg_range(SLG51000_LDO3_EVENT, SLG51000_LDO3_STATUS),
	regmap_reg_range(SLG51000_LDO4_EVENT, SLG51000_LDO4_STATUS),
	regmap_reg_range(SLG51000_LDO5_EVENT, SLG51000_LDO5_STATUS),
	regmap_reg_range(SLG51000_LDO6_EVENT, SLG51000_LDO6_STATUS),
	regmap_reg_range(SLG51000_LDO7_EVENT, SLG51000_LDO7_STATUS),
	regmap_reg_range(SLG51000_OTP_EVENT, SLG51000_OTP_EVENT),
};

static const struct regmap_access_table slg51000_writeable_table = {
	.yes_ranges	= slg51000_writeable_ranges,
	.n_yes_ranges	= ARRAY_SIZE(slg51000_writeable_ranges),
};

static const struct regmap_access_table slg51000_readable_table = {
	.yes_ranges	= slg51000_readable_ranges,
	.n_yes_ranges	= ARRAY_SIZE(slg51000_readable_ranges),
};

static const struct regmap_access_table slg51000_volatile_table = {
	.yes_ranges	= slg51000_volatile_ranges,
	.n_yes_ranges	= ARRAY_SIZE(slg51000_volatile_ranges),
};

static const struct regmap_config slg51000_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x8000,
	.wr_table = &slg51000_writeable_table,
	.rd_table = &slg51000_readable_table,
	.volatile_table = &slg51000_volatile_table,
};

static int slg51000_regmap_read(struct regmap *map, unsigned int reg,
			      unsigned int *val)
{
	int ret, retry = 0;

	if (map == NULL || val == NULL)
		return -EINVAL;

	do {
		ret = regmap_read(map, reg, val);
		if (ret >= 0)
			break;
		retry++;
		pr_err("%s: i2c read error=%d, retry count: %d\n",
			__func__, ret, retry);
		usleep_range(MIN_SLEEP_USEC, MAX_SLEEP_USEC);
	} while (retry < MAX_RETRY);

	return ret;
}

static int slg51000_regmap_bulk_read(struct regmap *map, unsigned int reg,
				   void *val, size_t val_count)
{
	int ret, retry = 0;

	if (map == NULL || val == NULL)
		return -EINVAL;

	do {
		ret = regmap_bulk_read(map, reg, val, val_count);
		if (ret >= 0)
			break;
		retry++;
		pr_err("%s: i2c read error=%d, retry count: %d\n",
			__func__, ret, retry);
		usleep_range(MIN_SLEEP_USEC, MAX_SLEEP_USEC);
	} while (retry < MAX_RETRY);

	return ret;
}

static int slg51000_regmap_write(struct regmap *map, unsigned int reg,
			       unsigned int val)
{
	int ret, retry = 0;

	if (map == NULL)
		return -EINVAL;

	do {
		ret = regmap_write(map, reg, val);
		if (ret >= 0)
			break;
		retry++;
		pr_err("%s: i2c write error=%d, retry count: %d\n",
			__func__, ret, retry);
		usleep_range(MIN_SLEEP_USEC, MAX_SLEEP_USEC);
	} while (retry < MAX_RETRY);

	return ret;
}

static int slg51000_regmap_bulk_write(struct regmap *map, unsigned int reg,
				    const void *val, size_t val_count)
{
	int ret, retry = 0;

	if (map == NULL || val == NULL)
		return -EINVAL;

	do {
		ret = regmap_bulk_write(map, reg, val, val_count);
		if (ret >= 0)
			break;
		retry++;
		pr_err("%s: i2c write error=%d, retry count: %d\n",
			__func__, ret, retry);
		usleep_range(MIN_SLEEP_USEC, MAX_SLEEP_USEC);
	} while (retry < MAX_RETRY);

	return ret;
}

static int slg51000_get_status(struct regulator_dev *rdev)
{
	struct slg51000 *chip = rdev_get_drvdata(rdev);
	int ret, id = rdev_get_id(rdev);
	unsigned int status;

	ret = regulator_is_enabled_regmap(rdev);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to read enable register(%d)\n",
			ret);
		return ret;
	}

	if (!ret)
		return REGULATOR_STATUS_OFF;

	ret = slg51000_regmap_read(chip->regmap, es_reg[id].sreg, &status);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to read status register(%d)\n",
			ret);
		return ret;
	}

	if (!(status & SLG51000_STA_ILIM_FLAG_MASK) &&
	    (status & SLG51000_STA_VOUT_OK_FLAG_MASK)) {
		if (rdev->desc->n_voltages == 0 &&
		    (id == SLG51000_REGULATOR_LDO5 ||
		     id == SLG51000_REGULATOR_LDO6))
			return REGULATOR_STATUS_BYPASS;
		else
			return REGULATOR_STATUS_ON;
	} else {
		return REGULATOR_STATUS_ERROR;
	}
}

static const struct regulator_ops slg51000_regl_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_status = slg51000_get_status,
};

static const struct regulator_ops slg51000_switch_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = slg51000_get_status,
};

static int slg51000_of_parse_cb(struct device_node *np,
				const struct regulator_desc *desc,
				struct regulator_config *config)
{
	int ena_gpio;

	ena_gpio = of_get_named_gpio(np, "enable-gpios", 0);
	if (ena_gpio)
		config->ena_gpio = ena_gpio;

	return 0;
}

#define SLG51000_REGL_DESC(_id, _name, _s_name, _min, _step)       \
	[SLG51000_REGULATOR_##_id] = {                             \
		.name = #_name,                                    \
		.supply_name = _s_name,                            \
		.id = SLG51000_REGULATOR_##_id,                    \
		.of_match = of_match_ptr(#_name),                  \
		.of_parse_cb = slg51000_of_parse_cb,               \
		.ops = &slg51000_regl_ops,                         \
		.regulators_node = of_match_ptr("regulators"),     \
		.n_voltages = 256,                                 \
		.min_uV = _min,                                    \
		.uV_step = _step,                                  \
		.linear_min_sel = 0,                               \
		.vsel_mask = SLG51000_VSEL_MASK,                   \
		.vsel_reg = SLG51000_##_id##_VSEL,                 \
		.enable_reg = SLG51000_SYSCTL_MATRIX_CONF_A,       \
		.enable_mask = BIT(SLG51000_REGULATOR_##_id),      \
		.type = REGULATOR_VOLTAGE,                         \
		.owner = THIS_MODULE,                              \
	}

static struct regulator_desc regls_desc[SLG51000_MAX_REGULATORS] = {
	SLG51000_REGL_DESC(LDO1, ldo1, NULL,   2200000,  5000),
	SLG51000_REGL_DESC(LDO2, ldo2, NULL,   2200000,  5000),
	SLG51000_REGL_DESC(LDO3, ldo3, "vin3", 1200000, 10000),
	SLG51000_REGL_DESC(LDO4, ldo4, "vin4", 1200000, 10000),
	SLG51000_REGL_DESC(LDO5, ldo5, "vin5",  400000,  5000),
	SLG51000_REGL_DESC(LDO6, ldo6, "vin6",  400000,  5000),
	SLG51000_REGL_DESC(LDO7, ldo7, "vin7", 1200000, 10000),
};

static int slg51000_regulator_init(struct slg51000 *chip)
{
	struct regulator_config config = { };
	struct regulator_desc *rdesc;
	unsigned int reg, val;
	u8 vsel_range[2];
	int id, ret = 0;
	const unsigned int min_regs[SLG51000_MAX_REGULATORS] = {
		SLG51000_LDO1_MINV, SLG51000_LDO2_MINV, SLG51000_LDO3_MINV,
		SLG51000_LDO4_MINV, SLG51000_LDO5_MINV, SLG51000_LDO6_MINV,
		SLG51000_LDO7_MINV,
	};

	for (id = 0; id < SLG51000_MAX_REGULATORS; id++) {
		chip->rdesc[id] = &regls_desc[id];
		rdesc = chip->rdesc[id];
		config.regmap = chip->regmap;
		config.dev = chip->dev;
		config.driver_data = chip;

		ret = slg51000_regmap_bulk_read(chip->regmap, min_regs[id],
						vsel_range, 2);

		switch (id) {
		case SLG51000_REGULATOR_LDO5:
		case SLG51000_REGULATOR_LDO6:
			if (id == SLG51000_REGULATOR_LDO5)
				reg = SLG51000_LDO5_TRIM2;
			else
				reg = SLG51000_LDO6_TRIM2;

			ret = slg51000_regmap_read(chip->regmap, reg, &val);

			if (val & SLG51000_SEL_BYP_MODE_MASK) {
				rdesc->ops = &slg51000_switch_ops;
				rdesc->n_voltages = 0;
				rdesc->min_uV = 0;
				rdesc->uV_step = 0;
				rdesc->linear_min_sel = 0;
				break;
			}
			/* Fall through - to the check below.*/

		default:
			rdesc->linear_min_sel = vsel_range[0];
			rdesc->n_voltages = vsel_range[1] + 1;
			rdesc->min_uV = rdesc->min_uV
					+ (vsel_range[0] * rdesc->uV_step);
			break;
		}

		chip->rdev[id] = devm_regulator_register(chip->dev, rdesc,
							 &config);
		if (IS_ERR(chip->rdev[id])) {
			ret = PTR_ERR(chip->rdev[id]);
			dev_err(chip->dev,
				"Failed to register regulator(%s):%d\n",
				chip->rdesc[id]->name, ret);
			return ret;
		}
	}

	return 0;
}

static int slg51000_config_tuning(struct slg51000 *chip)
{
	int ret;
	unsigned int ldo_hp_startup_current;
	const u8 sw_test_mode_on_vals[] = {
		SLG51000_SW_TEST_MODE_1_ON, SLG51000_SW_TEST_MODE_2_ON,
		SLG51000_SW_TEST_MODE_3_ON, SLG51000_SW_TEST_MODE_4_ON,
	};

	const u8 sw_test_mode_off_vals[] = {
		SLG51000_SW_TEST_MODE_1_OFF, SLG51000_SW_TEST_MODE_2_OFF,
		SLG51000_SW_TEST_MODE_3_OFF, SLG51000_SW_TEST_MODE_4_OFF,
	};

	if (chip == NULL) {
		pr_err("[%s] Invalid arguments\n", __func__);
		return -EINVAL;
	}

	// enter software test mode
	ret = slg51000_regmap_bulk_write(chip->regmap, SLG51000_SW_TEST_MODE_1,
			sw_test_mode_on_vals,
			ARRAY_SIZE(sw_test_mode_on_vals));
	if (ret < 0) {
		dev_err(chip->dev, "Failed to enter software test mode\n");
		return ret;
	}

	ret = slg51000_regmap_write(chip->regmap, SLG51000_LDO3_CONF1, 0x28);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to set LDO3_CONF1\n");
		return ret;
	}

	ret = slg51000_regmap_write(chip->regmap, SLG51000_LDO5_VSEL, 0x9f);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to set LDO5_VSEL\n");
		return ret;
	}

	ret = slg51000_regmap_write(chip->regmap,
			SLG51000_MUXARRAY_INPUT_SEL_39, 0x34);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to set MUXARRAY_INPUT_SEL_39\n");
		return ret;
	}

	ret = slg51000_regmap_write(chip->regmap,
			SLG51000_LUTARRAY_LUT_VAL_3, 0xea);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to set LUTARRAY_LUT_VAL_3\n");
		return ret;
	}

	// set startup current limit
	ret = slg51000_regmap_read(chip->regmap,
			SLG51000_LDO_HP_STARTUP_ILIM, &ldo_hp_startup_current);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to get LDO_HP_STARTUP_ILIM\n");
		return ret;
	}

	ldo_hp_startup_current &= SLG51000_LDO_HP_STARTUP_ILIM_ORI_MASK;
	ldo_hp_startup_current |= SLG51000_LDO_HP_STARTUP_ILIM_0_MASK;

	ret = slg51000_regmap_write(chip->regmap,
			SLG51000_LDO_HP_STARTUP_ILIM, ldo_hp_startup_current);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to set LDO_HP_STARTUP_ILIM\n");
		return ret;
	}

	// exit software test mode
	ret = slg51000_regmap_bulk_write(chip->regmap, SLG51000_SW_TEST_MODE_1,
			sw_test_mode_off_vals,
			ARRAY_SIZE(sw_test_mode_off_vals));
	if (ret < 0) {
		dev_err(chip->dev, "Failed to exit software test mode\n");
		return ret;
	}

	return ret;
}

static irqreturn_t slg51000_irq_handler(int irq, void *data)
{
	struct slg51000 *chip = data;
	struct regmap *regmap = chip->regmap;
	enum { R0 = 0, R1, R2, REG_MAX };
	u8 evt[SLG51000_MAX_EVT_REGISTER][REG_MAX];
	int ret, i, handled = IRQ_NONE;
	unsigned int evt_otp, mask_otp;

	/* Read event[R0], status[R1] and mask[R2] register */
	for (i = 0; i < SLG51000_MAX_EVT_REGISTER; i++) {
		ret = regmap_bulk_read(regmap, es_reg[i].ereg,
						evt[i], REG_MAX);
		if (ret < 0) {
			dev_err(chip->dev,
				"Failed to read event registers(%d)\n", ret);
			return IRQ_NONE;
		}
	}

	ret = regmap_read(regmap, SLG51000_OTP_EVENT, &evt_otp);
	if (ret < 0) {
		dev_err(chip->dev,
			"Failed to read otp event registers(%d)\n", ret);
		return IRQ_NONE;
	}

	ret = regmap_read(regmap, SLG51000_OTP_IRQ_MASK, &mask_otp);
	if (ret < 0) {
		dev_err(chip->dev,
			"Failed to read otp mask register(%d)\n", ret);
		return IRQ_NONE;
	}

	if ((evt_otp & SLG51000_EVT_CRC_MASK) &&
	    !(mask_otp & SLG51000_IRQ_CRC_MASK)) {
		dev_info(chip->dev,
			 "OTP has been read or OTP crc is not zero\n");
		handled = IRQ_HANDLED;
	}

	for (i = 0; i < SLG51000_MAX_REGULATORS; i++) {
		if (!(evt[i][R2] & SLG51000_IRQ_ILIM_FLAG_MASK) &&
		    (evt[i][R0] & SLG51000_EVT_ILIM_FLAG_MASK)) {
			regulator_notifier_call_chain(chip->rdev[i],
						REGULATOR_EVENT_OVER_CURRENT,
						NULL);

			if (evt[i][R1] & SLG51000_STA_ILIM_FLAG_MASK)
				dev_warn(chip->dev,
					 "Over-current limit(ldo%d)\n", i + 1);
			handled = IRQ_HANDLED;
		}
	}

	if (!(evt[SLG51000_SCTL_EVT][R2] & SLG51000_IRQ_HIGH_TEMP_WARN_MASK) &&
	    (evt[SLG51000_SCTL_EVT][R0] & SLG51000_EVT_HIGH_TEMP_WARN_MASK)) {
		for (i = 0; i < SLG51000_MAX_REGULATORS; i++) {
			if (!(evt[i][R1] & SLG51000_STA_ILIM_FLAG_MASK) &&
			    (evt[i][R1] & SLG51000_STA_VOUT_OK_FLAG_MASK)) {
				regulator_notifier_call_chain(chip->rdev[i],
						REGULATOR_EVENT_OVER_TEMP,
						NULL);
			}
		}
		handled = IRQ_HANDLED;
		if (evt[SLG51000_SCTL_EVT][R1] &
		    SLG51000_STA_HIGH_TEMP_WARN_MASK)
			dev_warn(chip->dev, "High temperature warning!\n");
	}

	return handled;
}

static void slg51000_clear_fault_log(struct slg51000 *chip)
{
	unsigned int val = 0;
	int ret = 0;

	ret = slg51000_regmap_read(chip->regmap,
					SLG51000_SYSCTL_FAULT_LOG1, &val);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to read Fault log register\n");
		return;
	}

	if (val & SLG51000_FLT_OVER_TEMP_MASK)
		dev_dbg(chip->dev, "Fault log: FLT_OVER_TEMP\n");
	if (val & SLG51000_FLT_POWER_SEQ_CRASH_REQ_MASK)
		dev_dbg(chip->dev, "Fault log: FLT_POWER_SEQ_CRASH_REQ\n");
	if (val & SLG51000_FLT_RST_MASK)
		dev_dbg(chip->dev, "Fault log: FLT_RST\n");
	if (val & SLG51000_FLT_POR_MASK)
		dev_dbg(chip->dev, "Fault log: FLT_POR\n");
}

static int read_chip_id(struct slg51000 *chip)
{
	int ret;

	if (chip == NULL) {
		pr_err("[%s] Invalid arguments\n", __func__);
		return -EINVAL;
	}

	ret = slg51000_regmap_bulk_read(chip->regmap,
					SLG51000_SYSCTL_PATN_ID_B0,
					chip_id, 3);
	if (ret < 0) {
		dev_err(chip->dev,
			"Failed to read chip id registers(%d)\n", ret);
		return ret;
	}

	return ret;
}

static ssize_t chip_id_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	if (buf == NULL)
		return -EINVAL;

	return snprintf(buf, PAGE_SIZE, "0x%02x%02x%02x\n",
			chip_id[2], chip_id[1], chip_id[0]);
}
static DEVICE_ATTR_RO(chip_id);

static struct attribute *attrs[] = {
	&dev_attr_chip_id.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static int slg51000_i2c_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct slg51000 *chip;
	int error, cs_gpio, ret;

	chip = devm_kzalloc(dev, sizeof(struct slg51000), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	cs_gpio = of_get_named_gpio(dev->of_node, "dlg,cs-gpios", 0);
	if (cs_gpio > 0) {
		if (!gpio_is_valid(cs_gpio)) {
			dev_err(dev, "Invalid chip select pin\n");
			return -EPERM;
		}

		ret = devm_gpio_request_one(dev, cs_gpio, GPIOF_OUT_INIT_HIGH,
					    "slg51000_cs_pin");
		if (ret) {
			dev_err(dev, "GPIO(%d) request failed(%d)\n",
				cs_gpio, ret);
			return ret;
		}

		chip->chip_cs_pin = cs_gpio;

		/* According to datasheet, turn-on time from CS HIGH to Ready
		state is ~10ms */
		usleep_range(SLEEP_10000_USEC,
			     SLEEP_10000_USEC + SLEEP_RANGE_USEC);
	}

	cs_gpio = of_get_named_gpio(dev->of_node, "dlg,enable-gpios", 0);
	if (cs_gpio > 0) {
		if (!gpio_is_valid(cs_gpio)) {
			dev_err(dev, "Invalid chip select pin\n");
			return -EPERM;
		}

		ret = devm_gpio_request_one(dev, cs_gpio, GPIOF_OUT_INIT_LOW,
					    "slg51000_en_1");
		if (ret) {
			dev_err(dev, "GPIO(%d) request failed(%d)\n",
				cs_gpio, ret);
		}
	}

	cs_gpio = of_get_named_gpio(dev->of_node, "dlg,enable-gpios", 1);
	if (cs_gpio > 0) {
		if (!gpio_is_valid(cs_gpio)) {
			dev_err(dev, "Invalid chip select pin\n");
			return -EPERM;
		}

		ret = devm_gpio_request_one(dev, cs_gpio, GPIOF_OUT_INIT_LOW,
					    "slg51000_en_2");
		if (ret) {
			dev_err(dev, "GPIO(%d) request failed(%d)\n",
				cs_gpio, ret);
		}
	}

	i2c_set_clientdata(client, chip);
	chip->chip_irq = client->irq;
	chip->dev = dev;
	chip->regmap = devm_regmap_init_i2c(client, &slg51000_regmap_config);
	if (IS_ERR(chip->regmap)) {
		error = PTR_ERR(chip->regmap);
		dev_err(dev, "Failed to allocate register map: %d\n",
			error);
		return error;
	}

	ret = slg51000_regulator_init(chip);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to init regulator(%d)\n", ret);
		return ret;
	}

	ret = slg51000_config_tuning(chip);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to config tuning(%d)\n", ret);
		return ret;
	}

	slg51000_clear_fault_log(chip);

	if (chip->chip_irq) {
		ret = devm_request_threaded_irq(dev, chip->chip_irq, NULL,
						slg51000_irq_handler,
						(IRQF_TRIGGER_HIGH |
						IRQF_ONESHOT),
						"slg51000-irq", chip);
		if (ret != 0) {
			dev_err(dev, "Failed to request IRQ: %d\n",
				chip->chip_irq);
			return ret;
		}
	} else {
		dev_warn(dev, "No IRQ configured\n");
	}

	ret = read_chip_id(chip);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to read chip id(%d)\n", ret);
		return ret;
	}

	ret = sysfs_create_group(&dev->kobj, &attr_group);
	if (ret) {
		dev_err(dev, "Failed to create attribute group: %d\n", ret);
		return ret;
	}

	return ret;
}

static int slg51000_i2c_remove(struct i2c_client *client)
{
	struct slg51000 *chip = i2c_get_clientdata(client);
	struct gpio_desc *desc;
	int ret = 0;

	if (chip->chip_cs_pin > 0) {
		desc = gpio_to_desc(chip->chip_cs_pin);
		ret = gpiod_direction_output_raw(desc, GPIOF_INIT_LOW);
	}

	return ret;
}

static const struct i2c_device_id slg51000_i2c_id[] = {
	{"slg51000", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, slg51000_i2c_id);

static struct i2c_driver slg51000_regulator_driver = {
	.driver = {
		.name = "slg51000-regulator",
	},
	.probe = slg51000_i2c_probe,
	.remove = slg51000_i2c_remove,
	.id_table = slg51000_i2c_id,
};

module_i2c_driver(slg51000_regulator_driver);

MODULE_AUTHOR("CY Tseng <cytseng@google.com>");
MODULE_AUTHOR("Eric Jeong <eric.jeong.opensource@diasemi.com>");
MODULE_DESCRIPTION("SLG51000 regulator driver");
MODULE_LICENSE("GPL");
