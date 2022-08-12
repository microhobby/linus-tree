// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sun6i_hwspinlock.c - hardware spinlock driver for sun6i compatible Allwinner SoCs
 * Copyright (C) 2020 Wilken Gottwalt <wilken.gottwalt@posteo.net>
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "hwspinlock_internal.h"

#define DRIVER_NAME		"sun6i_hwspinlock"

#define SPINLOCK_BASE_ID	0 /* there is only one hwspinlock device per SoC */
#define SPINLOCK_SYSSTATUS_REG	0x0000
#define SPINLOCK_LOCK_REGN	0x0100
#define SPINLOCK_NOTTAKEN	0

struct sun6i_hwspinlock_data {
	struct hwspinlock_device *bank;
	struct reset_control *reset;
	struct clk *ahb_clk;
	struct dentry *debugfs;
	int nlocks;
};

#ifdef CONFIG_DEBUG_FS

static int hwlocks_supported_show(struct seq_file *seqf, void *unused)
{
	struct sun6i_hwspinlock_data *priv = seqf->private;

	seq_printf(seqf, "%d\n", priv->nlocks);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hwlocks_supported);

static void sun6i_hwspinlock_debugfs_init(struct sun6i_hwspinlock_data *priv)
{
	priv->debugfs = debugfs_create_dir(DRIVER_NAME, NULL);
	debugfs_create_file("supported", 0444, priv->debugfs, priv, &hwlocks_supported_fops);
}

#else

static void sun6i_hwspinlock_debugfs_init(struct sun6i_hwspinlock_data *priv)
{
}

#endif

static int sun6i_hwspinlock_trylock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	return (readl(lock_addr) == SPINLOCK_NOTTAKEN);
}

static void sun6i_hwspinlock_unlock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	writel(SPINLOCK_NOTTAKEN, lock_addr);
}

static const struct hwspinlock_ops sun6i_hwspinlock_ops = {
	.trylock	= sun6i_hwspinlock_trylock,
	.unlock		= sun6i_hwspinlock_unlock,
};

static void sun6i_hwspinlock_disable(void *data)
{
	struct sun6i_hwspinlock_data *priv = data;

	debugfs_remove_recursive(priv->debugfs);
	clk_disable_unprepare(priv->ahb_clk);
	reset_control_assert(priv->reset);
}

static int sun6i_hwspinlock_probe(struct platform_device *pdev)
{
	struct sun6i_hwspinlock_data *priv;
	struct hwspinlock *hwlock;
	void __iomem *io_base;
	u32 num_banks;
	int err, i;

	io_base = devm_platform_ioremap_resource(pdev, SPINLOCK_BASE_ID);
	if (IS_ERR(io_base))
		return PTR_ERR(io_base);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ahb_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->ahb_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->ahb_clk),
				     "unable to get AHB clock\n");

	priv->reset = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(priv->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->reset),
				     "unable to get reset control\n");

	err = reset_control_deassert(priv->reset);
	if (err) {
		dev_err(&pdev->dev, "deassert reset control failure (%d)\n", err);
		return err;
	}

	err = clk_prepare_enable(priv->ahb_clk);
	if (err) {
		dev_err(&pdev->dev, "unable to prepare AHB clk (%d)\n", err);
		goto clk_fail;
	}

	/*
	 * Bits 28 and 29 represent the number of available locks.
	 *
	 * The datasheets have two conflicting interpretations for these bits:
	 *   |  00 | 01 |  10 |  11 |
	 *   +-----+----+-----+-----+
	 *   | 256 | 32 |  64 | 128 | A80, A83T, H3, A64, A50, D1
	 *   |  32 | 64 | 128 | 256 | H5, H6, R329
	 * where some datasheets use "4" instead of "0" for the first column.
	 *
	 * Experiments shows that the first interpretation is correct, as all
	 * known implementations report the value "1" and have 32 spinlocks.
	 */
	num_banks = readl(io_base + SPINLOCK_SYSSTATUS_REG) >> 28 & 0x3;
	if (!num_banks)
		num_banks = 4;
	priv->nlocks = 1 << (4 + num_banks);

	priv->bank = devm_kzalloc(&pdev->dev, struct_size(priv->bank, lock, priv->nlocks),
				  GFP_KERNEL);
	if (!priv->bank) {
		err = -ENOMEM;
		goto bank_fail;
	}

	for (i = 0; i < priv->nlocks; ++i) {
		hwlock = &priv->bank->lock[i];
		hwlock->priv = io_base + SPINLOCK_LOCK_REGN + sizeof(u32) * i;
	}

	/* failure of debugfs is considered non-fatal */
	sun6i_hwspinlock_debugfs_init(priv);
	if (IS_ERR(priv->debugfs))
		priv->debugfs = NULL;

	err = devm_add_action_or_reset(&pdev->dev, sun6i_hwspinlock_disable, priv);
	if (err) {
		dev_err(&pdev->dev, "failed to add hwspinlock disable action\n");
		goto bank_fail;
	}

	platform_set_drvdata(pdev, priv);

	return devm_hwspin_lock_register(&pdev->dev, priv->bank, &sun6i_hwspinlock_ops,
					 SPINLOCK_BASE_ID, priv->nlocks);

bank_fail:
	clk_disable_unprepare(priv->ahb_clk);
clk_fail:
	reset_control_assert(priv->reset);

	return err;
}

static const struct of_device_id sun6i_hwspinlock_ids[] = {
	{ .compatible = "allwinner,sun6i-a31-hwspinlock", },
	{},
};
MODULE_DEVICE_TABLE(of, sun6i_hwspinlock_ids);

static struct platform_driver sun6i_hwspinlock_driver = {
	.probe	= sun6i_hwspinlock_probe,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= sun6i_hwspinlock_ids,
	},
};
module_platform_driver(sun6i_hwspinlock_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SUN6I hardware spinlock driver");
MODULE_AUTHOR("Wilken Gottwalt <wilken.gottwalt@posteo.net>");
