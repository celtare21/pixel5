// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/interrupt.h>

#include <soc/qcom/cx_ipeak.h>

#define TCSR_CXIP_LM_VOTE_FEATURE_ENABLE_OFFSET		0x10

/* v1 register set */
#define TCSR_CXIP_LM_VOTE_BYPASS_OFFSET			0x4
#define TCSR_CXIP_LM_VOTE_CLEAR_OFFSET			0x8
#define TCSR_CXIP_LM_VOTE_SET_OFFSET			0xC
#define TCSR_CXIP_LM_TRS_OFFSET				0x24

/* v2 register set */
#define TCSR_CXIP_LM_VOTE_CLIENTx_BYPASS_OFFSET		0x4
#define TCSR_CXIP_LM_DANGER_OFFSET			0x24

#define CXIP_CLIENT_OFFSET				0x1000
#define CXIP_CLIENT10_OFFSET				0x3000
#define CXIP_VICTIM_OFFSET				0xB000

#define CXIP_POLL_TIMEOUT_US (50 * 1000)

#define CXIP_VICTIMS    3
#define VICTIM_ENTRIES    3

struct cx_ipeak_client;

struct cx_ipeak_core_ops {
	int (*update)(struct cx_ipeak_client *client, bool vote);
	struct cx_ipeak_client* (*register_client)(int client_id);
};

static struct cx_ipeak_device {
	spinlock_t vote_lock;
	void __iomem *tcsr_vptr;
	struct cx_ipeak_core_ops *core_ops;
	u32 victims_count;
	u32 victims_reg_count;
	u32 danger_intr_num;
	u32 safe_intr_num;
} device_ipeak;

struct cx_ipeak_client {
	int vote_count;
	unsigned int offset;
	int client_id;
	struct cx_ipeak_device *dev;
};

static struct cx_ipeak_victims {
	int client_id;
	int victim_id;
	u32 freq_limit;
	void *data;
	cx_ipeak_victim_fn victim_cb;
} victims_ipeak[CXIP_VICTIMS];

/**
 * cx_ipeak_register() - allocate client structure and fill device private and
 *			offset details.
 * @dev_node: device node of the client
 * @client_name: property name of the client
 *
 * Allocate client memory and fill the structure with device private and bit
 *
 */
struct cx_ipeak_client *cx_ipeak_register(struct device_node *dev_node,
		const char *client_name)
{
	struct of_phandle_args cx_spec;
	struct cx_ipeak_client *client = NULL;
	int ret;

	ret = of_parse_phandle_with_fixed_args(dev_node, client_name,
			1, 0, &cx_spec);
	if (ret)
		return ERR_PTR(-EINVAL);

	if (!of_device_is_available(cx_spec.np))
		return NULL;

	if (device_ipeak.tcsr_vptr == NULL)
		return ERR_PTR(-EPROBE_DEFER);

	if (cx_spec.args[0] > 31)
		return ERR_PTR(-EINVAL);

	if (device_ipeak.core_ops)
		client =  device_ipeak.core_ops->register_client
						(cx_spec.args[0]);

	client->client_id = cx_spec.args[0];

	return client;
}
EXPORT_SYMBOL(cx_ipeak_register);

static struct cx_ipeak_client *cx_ipeak_register_v1(int client_id)
{
	struct cx_ipeak_client *client;
	unsigned int reg_enable, reg_bypass;
	void __iomem *vptr = device_ipeak.tcsr_vptr;

	reg_enable = readl_relaxed(device_ipeak.tcsr_vptr +
			TCSR_CXIP_LM_VOTE_FEATURE_ENABLE_OFFSET);
	reg_bypass = readl_relaxed(vptr +
			TCSR_CXIP_LM_VOTE_BYPASS_OFFSET) &
			BIT(client_id);
	if (!reg_enable || reg_bypass)
		return NULL;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->offset = BIT(client_id);
	client->dev = &device_ipeak;

	return client;
}

static struct cx_ipeak_client *cx_ipeak_register_v2(int client_id)
{
	unsigned int reg_bypass, reg_enable;
	struct cx_ipeak_client *client;
	unsigned int client_offset = 0;
	void __iomem *vptr = device_ipeak.tcsr_vptr;
	int i;

	for (i = 0; i <= client_id; i++)
		client_offset += CXIP_CLIENT_OFFSET;

	if (client_id >= 10)
		client_offset += CXIP_CLIENT10_OFFSET;

	reg_enable = readl_relaxed(device_ipeak.tcsr_vptr +
			TCSR_CXIP_LM_VOTE_FEATURE_ENABLE_OFFSET);
	reg_bypass = readl_relaxed(vptr + client_offset +
			TCSR_CXIP_LM_VOTE_CLIENTx_BYPASS_OFFSET) &
			BIT(0);

	if (!reg_enable || reg_bypass)
		return NULL;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->offset = client_offset;
	client->dev = &device_ipeak;

	return client;
}

/**
 * cx_ipeak_victim_register - victim registration API to handle
 * the cx ipeak hw interrupts (danger/safe) to throttle freq.
 * @client: cx ipeak client
 * @victim_cb: callback function of victim
 * @data: data to be passed to victim while handling irq
 */
int cx_ipeak_victim_register(struct cx_ipeak_client *client,
		cx_ipeak_victim_fn victim_cb, void *data)
{
	int i = 0;

	if (!victim_cb)
		return -EINVAL;

	while (i < device_ipeak.victims_count) {
		if (client->client_id == victims_ipeak[i].client_id) {
			victims_ipeak[i].victim_cb = victim_cb;
			victims_ipeak[i].data = data;
			device_ipeak.victims_reg_count++;
			break;
		}
		i++;
	}
	return 0;
}
EXPORT_SYMBOL(cx_ipeak_victim_register);

/**
 * cx_ipeak_victim_unregister - unregister victim client from
 * cx_ipeak driver.
 * @client: cx ipeak client
 */

void cx_ipeak_victim_unregister(struct cx_ipeak_client *client)
{
	int i = 0;

	while (i < device_ipeak.victims_count) {
		if (client->client_id == victims_ipeak[i].client_id) {
			victims_ipeak[i].victim_cb = NULL;
			victims_ipeak[i].data = NULL;
			device_ipeak.victims_reg_count--;
			break;
		}
		i++;
	}
}
EXPORT_SYMBOL(cx_ipeak_victim_unregister);

/**
 * cx_ipeak_update() - Set/Clear client vote for Cx iPeak limit
 * manager to throttle cDSP.
 * @client: client handle.
 * @vote: True to set the vote and False for reset.
 *
 * Receives vote from each client and decides whether to throttle cDSP or not.
 * This function is NOP for the targets which does not support TCSR Cx iPeak.
 */
int cx_ipeak_update(struct cx_ipeak_client *client, bool vote)
{
	/* Check for client and device availability and proceed */
	if (!client)
		return 0;

	if (!client->dev || !client->dev->core_ops || !client->dev->tcsr_vptr)
		return -EINVAL;

	return client->dev->core_ops->update(client, vote);
}
EXPORT_SYMBOL(cx_ipeak_update);

static int cx_ipeak_update_v1(struct cx_ipeak_client *client, bool vote)
{
	unsigned int reg_val;
	int ret = 0;

	spin_lock(&client->dev->vote_lock);

	if (vote) {
		if (client->vote_count == 0) {
			writel_relaxed(client->offset,
				       client->dev->tcsr_vptr +
				       TCSR_CXIP_LM_VOTE_SET_OFFSET);
			/*
			 * Do a dummy read to give enough time for TRS register
			 * to become 1 when the last client votes.
			 */
			readl_relaxed(client->dev->tcsr_vptr +
				      TCSR_CXIP_LM_TRS_OFFSET);

			ret = readl_poll_timeout(client->dev->tcsr_vptr +
						 TCSR_CXIP_LM_TRS_OFFSET,
						 reg_val, !reg_val, 0,
						 CXIP_POLL_TIMEOUT_US);
			if (ret) {
				writel_relaxed(client->offset,
					       client->dev->tcsr_vptr +
					       TCSR_CXIP_LM_VOTE_CLEAR_OFFSET);
				goto done;
			}
		}
		client->vote_count++;
	} else {
		if (client->vote_count > 0) {
			client->vote_count--;
			if (client->vote_count == 0) {
				writel_relaxed(client->offset,
					       client->dev->tcsr_vptr +
					       TCSR_CXIP_LM_VOTE_CLEAR_OFFSET);
			}
		} else
			ret = -EINVAL;
	}

done:
	spin_unlock(&client->dev->vote_lock);
	return ret;
}

static int cx_ipeak_update_v2(struct cx_ipeak_client *client, bool vote)
{
	unsigned int reg_val;
	int ret = 0;

	spin_lock(&client->dev->vote_lock);

	if (vote) {
		if (client->vote_count == 0) {
			writel_relaxed(BIT(0),
				       client->dev->tcsr_vptr +
				       client->offset);

			ret = readl_poll_timeout(client->dev->tcsr_vptr +
						 TCSR_CXIP_LM_DANGER_OFFSET,
						 reg_val, !reg_val, 0,
						 CXIP_POLL_TIMEOUT_US);
			if (ret) {
				writel_relaxed(0,
					       client->dev->tcsr_vptr +
					       client->offset);
				goto done;
			}
		}
		client->vote_count++;
	} else {
		if (client->vote_count > 0) {
			client->vote_count--;
			if (client->vote_count == 0) {
				writel_relaxed(0,
					       client->dev->tcsr_vptr +
					       client->offset);
			}
		} else {
			ret = -EINVAL;
		}
	}

done:
	spin_unlock(&client->dev->vote_lock);
	return ret;
}

static irqreturn_t cx_ipeak_irq_handler(int irq, void *data)
{
	int i;
	irqreturn_t ret = IRQ_NONE;

	for (i = 0; i < device_ipeak.victims_reg_count; i++) {
		cx_ipeak_victim_fn victim_cb = victims_ipeak[i].victim_cb;

		if (irq == device_ipeak.danger_intr_num) {
		/*
		 * To set frequency limit at victim client
		 * side in danger interrupt case
		 */
			victim_cb(victims_ipeak[i].data,
				victims_ipeak[i].freq_limit);
			writel_relaxed(1, (device_ipeak.tcsr_vptr +
						CXIP_VICTIM_OFFSET +
						((victims_ipeak[i].victim_id)*
							 CXIP_CLIENT_OFFSET)));
			ret = IRQ_HANDLED;
		} else if (irq == device_ipeak.safe_intr_num) {
		/*
		 * To remove frequency limit at victim client
		 * side in safe interrupt case
		 */
			victim_cb(victims_ipeak[i].data, 0);
			writel_relaxed(0, (device_ipeak.tcsr_vptr +
						CXIP_VICTIM_OFFSET +
						((victims_ipeak[i].victim_id)*
						 CXIP_CLIENT_OFFSET)));
			ret = IRQ_HANDLED;
		}
	}

	return ret;
}

int cx_ipeak_request_irq(struct platform_device *pdev, const  char *name,
		irq_handler_t handler, void *data)
{
	int ret, num = platform_get_irq_byname(pdev, name);

	if (num < 0)
		return num;

	ret = devm_request_irq(&pdev->dev, num, handler, IRQF_TRIGGER_RISING,
				name, data);

	if (ret)
		dev_err(&pdev->dev, "Unable to get interrupt %s: %d\n",
				name, ret);

	return ret ? ret : num;
}

/**
 * cx_ipeak_unregister() - unregister client
 * @client: client address to free
 *
 * Free the client memory
 */
void cx_ipeak_unregister(struct cx_ipeak_client *client)
{
	kfree(client);
}
EXPORT_SYMBOL(cx_ipeak_unregister);

struct cx_ipeak_core_ops core_ops_v1 = {
	.update = cx_ipeak_update_v1,
	.register_client = cx_ipeak_register_v1,
};

struct cx_ipeak_core_ops core_ops_v2 = {
	.update = cx_ipeak_update_v2,
	.register_client = cx_ipeak_register_v2,
};

static int cx_ipeak_probe(struct platform_device *pdev)
{
	struct resource *res;
	int status = -EINVAL;
	int i, ret, count;
	u32 victim_en;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	device_ipeak.tcsr_vptr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(device_ipeak.tcsr_vptr))
		return PTR_ERR(device_ipeak.tcsr_vptr);

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,cx-ipeak-v1"))
		device_ipeak.core_ops = &core_ops_v1;
	else if (of_device_is_compatible(pdev->dev.of_node,
					 "qcom,cx-ipeak-v2"))
		device_ipeak.core_ops = &core_ops_v2;
	else
		device_ipeak.core_ops = NULL;

	victim_en = of_property_read_bool(pdev->dev.of_node,
			"victims_table");

	if (victim_en) {
		count = of_property_count_u32_elems(pdev->dev.of_node,
						"victims_table");

		if (((count%VICTIM_ENTRIES) != 0) ||
				((count/VICTIM_ENTRIES) > CXIP_VICTIMS))
			return -EINVAL;

		for (i = 0; i < (count/VICTIM_ENTRIES); i++) {
			ret = of_property_read_u32_index(pdev->dev.of_node,
					"victims_table", i*VICTIM_ENTRIES,
					&victims_ipeak[i].client_id);

			if (ret)
				return ret;

			ret = of_property_read_u32_index(pdev->dev.of_node,
					"victims_table", (i*VICTIM_ENTRIES) + 1,
					&victims_ipeak[i].victim_id);

			if (ret)
				return ret;

			ret = of_property_read_u32_index(pdev->dev.of_node,
					"victims_table", (i*VICTIM_ENTRIES) + 2,
					&victims_ipeak[i].freq_limit);

			if (ret)
				return ret;

			device_ipeak.victims_count++;
		}

		status = cx_ipeak_request_irq(pdev, "cx_ipeak_danger",
				cx_ipeak_irq_handler, NULL);

		if (status < 0)
			return status;

		device_ipeak.danger_intr_num = status;

		status = cx_ipeak_request_irq(pdev, "cx_ipeak_safe",
				cx_ipeak_irq_handler, NULL);

		if (status < 0)
			return status;

		device_ipeak.safe_intr_num = status;
	}

	spin_lock_init(&device_ipeak.vote_lock);
	return 0;
}

static const struct of_device_id cx_ipeak_match_table[] = {
	{ .compatible = "qcom,cx-ipeak-v1"},
	{ .compatible = "qcom,cx-ipeak-v2"},
	{}
};

static struct platform_driver cx_ipeak_platform_driver = {
	.probe = cx_ipeak_probe,
	.driver = {
		.name  = "cx_ipeak",
		.of_match_table = cx_ipeak_match_table,
		.suppress_bind_attrs = true,
	}
};

static int __init cx_ipeak_init(void)
{
	return platform_driver_register(&cx_ipeak_platform_driver);
}

arch_initcall(cx_ipeak_init);
