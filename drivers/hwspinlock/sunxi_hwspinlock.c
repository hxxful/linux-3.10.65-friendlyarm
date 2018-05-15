/*
 * SUNXI hardware spinlock driver
 *
 * Copyright (C) 2015 Allwinnertech - http://www.allwinnertech.com
 *
 * Contact: Feng Xia <xiafeng@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hwspinlock.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/err.h>

#include "hwspinlock_internal.h"

/* hardware spinlock register list */
#define	LOCK_SYS_STATUS_REG             (0x0000)
#define	LOCK_STATUS_REG                 (0x0010)
#define	LOCK_IRQ_EN_REG                 (0x0020)
#define LOCK_IRQ_PEND_REG               (0x0040)
#define LOCK_BASE_OFFSET                (0x0100)
#define LOCK_BASE_ID                    (0)

/* Possible values of SPINLOCK_LOCK_REG */
#define SPINLOCK_NOTTAKEN               (0)     /* free */
#define SPINLOCK_TAKEN                  (1)     /* locked */

static int sunxi_hwspinlock_trylock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	/* attempt to acquire the lock by reading its value */
	return (SPINLOCK_NOTTAKEN == readl(lock_addr));
}

static void sunxi_hwspinlock_unlock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	/* release the lock by writing 0 to it */
	writel(SPINLOCK_NOTTAKEN, lock_addr);
}

/*
 * relax the SUNXI interconnect while spinning on it.
 *
 * The specs recommended that the retry delay time will be
 * just over half of the time that a requester would be
 * expected to hold the lock.
 *
 * The number below is taken from an hardware specs example,
 * obviously it is somewhat arbitrary.
 */
static void sunxi_hwspinlock_relax(struct hwspinlock *lock)
{
	ndelay(50);
}

static const struct hwspinlock_ops sunxi_hwspinlock_ops = {
	.trylock = sunxi_hwspinlock_trylock,
	.unlock = sunxi_hwspinlock_unlock,
	.relax = sunxi_hwspinlock_relax,
};

static int sunxi_hwspinlock_probe(struct platform_device *pdev)
{
	struct hwspinlock_device *bank;
	struct hwspinlock *hwlock;
	void __iomem *iobase;
	int num_locks, i, ret;
	struct resource res;
	//struct clk *hwspinlock_rst, *hwspinlock_bus;
	struct device_node *node = pdev->dev.of_node;

	ret = of_address_to_resource(node, 0, &res);
	if (unlikely(ret || !res.start)) {
		pr_err("get spinlock pbase error\n");
		return -ENODEV;
	}

	iobase = of_iomap(node, 0);
	if (IS_ERR(iobase))
		return PTR_ERR(iobase);

#if 0
	hwspinlock_bus = of_clk_get_by_name(node, "clk_hwspinlock_bus");
	if(!hwspinlock_bus || IS_ERR(hwspinlock_bus)){
		pr_err("try to get clk_spinlock failed!\n");
		return -EINVAL;
	}

	ret = clk_prepare(hwspinlock_bus);
	if (ret)
		return ret;
	ret = clk_enable(hwspinlock_bus);
	if (ret)
		clk_unprepare(hwspinlock_bus);

	hwspinlock_rst = of_clk_get_by_name(node, "clk_hwspinlock_rst");
	if(!hwspinlock_rst || IS_ERR(hwspinlock_rst)){
		pr_err("try to get clk_spinlock failed!\n");
		return -EINVAL;
	}

	ret = clk_prepare(hwspinlock_rst);
	if (ret)
		return ret;
	ret = clk_enable(hwspinlock_rst);
	if (ret)
		clk_unprepare(hwspinlock_rst);
#endif

	ret = of_property_read_u32(node, "num-locks", &num_locks);
	if (ret || (num_locks == 0))
		return -ENODEV;

	bank = devm_kzalloc(&pdev->dev, sizeof(*bank) + num_locks * sizeof(*hwlock), GFP_KERNEL);
	if (!bank) {
		ret = -ENOMEM;
		goto iounmap_base;
	}

	platform_set_drvdata(pdev, bank);

	for (i = 0, hwlock = &bank->lock[0]; i < num_locks; i++, hwlock++)
		hwlock->priv = iobase + LOCK_BASE_OFFSET + sizeof(u32) * i;

	/*
	 * runtime PM will make sure the clock of this module is
	 * enabled iff at least one lock is requested
	 */
	pm_runtime_enable(&pdev->dev);

	ret = hwspin_lock_register(bank, &pdev->dev, &sunxi_hwspinlock_ops,
	                           LOCK_BASE_ID, num_locks);
	if (ret)
		goto reg_fail;

	pr_info("sunxi hwspinlock vbase:0x%p\n", iobase);

	return 0;

reg_fail:
	pm_runtime_disable(&pdev->dev);
	kfree(bank);

iounmap_base:
	iounmap(iobase);

	return ret;
}

static int sunxi_hwspinlock_remove(struct platform_device *pdev)
{
	struct hwspinlock_device *bank = platform_get_drvdata(pdev);
	void __iomem *iobase = bank->lock[0].priv - LOCK_BASE_OFFSET;
	int ret;

	ret = hwspin_lock_unregister(bank);
	if (ret) {
		dev_err(&pdev->dev, "%s failed: %d\n", __func__, ret);
		return ret;
	}

	pm_runtime_disable(&pdev->dev);
	iounmap(iobase);
	kfree(bank);

	return 0;
}

static const struct of_device_id sunxi_hwspinlock_of_match[] = {
	{ .compatible = "allwinner,sunxi-hwspinlock", },
	{ /* end */ },
};
MODULE_DEVICE_TABLE(of, sunxi_hwspinlock_of_match);

static struct platform_driver sunxi_hwspinlock_driver = {
	.probe		= sunxi_hwspinlock_probe,
	.remove		= sunxi_hwspinlock_remove,
	.driver		= {
		.name	= "sunxi-hwspinlock",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(sunxi_hwspinlock_of_match),
	},
};

static int __init sunxi_hwspinlock_init(void)
{
	return platform_driver_register(&sunxi_hwspinlock_driver);
}
/* board init code might need to reserve hwspinlocks for predefined purposes */
postcore_initcall(sunxi_hwspinlock_init);

static void __exit sunxi_hwspinlock_exit(void)
{
	platform_driver_unregister(&sunxi_hwspinlock_driver);
}
module_exit(sunxi_hwspinlock_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Hardware spinlock driver for SUNXI");
MODULE_AUTHOR("Feng Xia <xiafeng@allwinnertech.com>");
