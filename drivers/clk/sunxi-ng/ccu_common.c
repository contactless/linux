// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2016 Maxime Ripard
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>

#include "ccu_common.h"
#include "ccu_gate.h"
#include "ccu_reset.h"

struct sunxi_ccu {
	const struct sunxi_ccu_desc	*desc;
	spinlock_t			lock;
	struct ccu_reset		reset;
};

void ccu_helper_wait_for_lock(struct ccu_common *common, u32 lock)
{
	void __iomem *addr;
	u32 reg;

	if (!lock)
		return;

	if (common->features & CCU_FEATURE_LOCK_REG)
		addr = common->base + common->lock_reg;
	else
		addr = common->base + common->reg;

	WARN_ON(readl_relaxed_poll_timeout(addr, reg, reg & lock, 100, 70000));
}
EXPORT_SYMBOL_NS_GPL(ccu_helper_wait_for_lock, "SUNXI_CCU");

bool ccu_is_better_rate(struct ccu_common *common,
			unsigned long target_rate,
			unsigned long current_rate,
			unsigned long best_rate)
{
	unsigned long min_rate, max_rate;

	clk_hw_get_rate_range(&common->hw, &min_rate, &max_rate);

	if (current_rate > max_rate)
		return false;

	if (current_rate < min_rate)
		return false;

	if (common->features & CCU_FEATURE_CLOSEST_RATE)
		return abs(current_rate - target_rate) < abs(best_rate - target_rate);

	return current_rate <= target_rate && current_rate > best_rate;
}
EXPORT_SYMBOL_NS_GPL(ccu_is_better_rate, "SUNXI_CCU");

/*
 * This clock notifier is called when the frequency of a PLL clock is
 * changed. In common PLL designs, changes to the dividers take effect
 * almost immediately, while changes to the multipliers (implemented
 * as dividers in the feedback loop) take a few cycles to work into
 * the feedback loop for the PLL to stabilize.
 *
 * Sometimes when the PLL clock rate is changed, the decrease in the
 * divider is too much for the decrease in the multiplier to catch up.
 * The PLL clock rate will spike, and in some cases, might lock up
 * completely.
 *
 * This notifier callback will gate and then ungate the clock,
 * effectively resetting it, so it proceeds to work. Care must be
 * taken to reparent consumers to other temporary clocks during the
 * rate change, and that this notifier callback must be the first
 * to be registered.
 */
static int ccu_pll_notifier_cb(struct notifier_block *nb,
			       unsigned long event, void *data)
{
	struct ccu_pll_nb *pll = to_ccu_pll_nb(nb);
	int ret = 0;

	if (event != POST_RATE_CHANGE)
		goto out;

	ccu_gate_helper_disable(pll->common, pll->enable);

	ret = ccu_gate_helper_enable(pll->common, pll->enable);
	if (ret)
		goto out;

	ccu_helper_wait_for_lock(pll->common, pll->lock);

out:
	return notifier_from_errno(ret);
}

int ccu_pll_notifier_register(struct ccu_pll_nb *pll_nb)
{
	pll_nb->clk_nb.notifier_call = ccu_pll_notifier_cb;

	return clk_notifier_register(pll_nb->common->hw.clk,
				     &pll_nb->clk_nb);
}
EXPORT_SYMBOL_NS_GPL(ccu_pll_notifier_register, "SUNXI_CCU");

/*
 * System-suspend context save/restore.
 *
 * On some Allwinner platforms a system ("mem") suspend is implemented as a
 * suspend-to-off: firmware puts the DRAM in self-refresh and the PMIC drops
 * VDD-SYS and every peripheral rail, so on resume the whole CCU register file
 * is back at its reset defaults. Firmware only restores the clocks it needs to
 * re-enter the kernel -- the base PLLs and the CPU/bus/DRAM clock tree -- and
 * leaves the peripheral clock, gate and reset state for the kernel to bring
 * back.
 *
 * We do that from a syscore handler on purpose. syscore_resume() runs on the
 * resume path after firmware but before any device is resumed -- even at the
 * _noirq level -- single-CPU with interrupts off. That is the only phase at
 * which we can guarantee every peripheral clock, gate and reset line is back
 * to its pre-suspend value before a driver touches its hardware, and it lets
 * us work purely from a pre-saved register image, without taking any clk
 * framework lock or sleeping.
 *
 * Because the image is a raw snapshot of the running controller, only the
 * clocks that were enabled at suspend time are re-enabled on resume, so the
 * clk framework's view (which clocks it believes are gated) stays consistent
 * with the hardware, and clocks whose rate was set via the framework get their
 * exact dividers back with no need to re-run set_rate.
 */
struct ccu_pm_cache {
	struct list_head	node;
	void __iomem		*base;
	const struct ccu_pm	*pm;
	u32			*regs;
};

static LIST_HEAD(ccu_pm_caches);

/* PLL lock poll: match ccu_helper_wait_for_lock()'s 70 ms bound. */
#define CCU_PM_LOCK_DELAY_US	10
#define CCU_PM_LOCK_TIMEOUT_US	70000
/* Settle time for PLLs that have no usable lock bit (e.g. the T507 GPU PLL). */
#define CCU_PM_PLL_SETTLE_US	100

static bool ccu_pm_reg_is_firmware(const struct ccu_pm *pm, unsigned int off)
{
	unsigned int i;

	for (i = 0; i < pm->num_firmware_regs; i++)
		if (pm->firmware_regs[i] == off)
			return true;

	return false;
}

static bool ccu_pm_reg_is_pll(const struct ccu_pm *pm, unsigned int off)
{
	unsigned int i;

	for (i = 0; i < pm->num_plls; i++)
		if (pm->plls[i].reg == off)
			return true;

	return false;
}

static int ccu_pm_suspend(void)
{
	struct ccu_pm_cache *cache;

	list_for_each_entry(cache, &ccu_pm_caches, node) {
		unsigned int off;

		for (off = 0; off < cache->pm->reg_size; off += sizeof(u32))
			cache->regs[off / sizeof(u32)] = readl(cache->base + off);
	}

	return 0;
}

static void ccu_pm_restore_pll(struct ccu_pm_cache *cache,
			       const struct ccu_pm_pll *pll)
{
	u32 val = cache->regs[pll->reg / sizeof(u32)];
	u32 reg;

	/*
	 * The saved value already carries the enable, lock-enable and
	 * output-enable bits that probe set up.
	 */
	writel(val, cache->base + pll->reg);

	/* A PLL that was gated at suspend time stays gated; nothing to lock. */
	if (!(val & pll->enable))
		return;

	if (pll->lock)
		WARN_ON(readl_poll_timeout_atomic(cache->base + pll->reg, reg,
						  reg & pll->lock,
						  CCU_PM_LOCK_DELAY_US,
						  CCU_PM_LOCK_TIMEOUT_US));
	else
		udelay(CCU_PM_PLL_SETTLE_US);
}

static void ccu_pm_resume(void)
{
	struct ccu_pm_cache *cache;

	list_for_each_entry(cache, &ccu_pm_caches, node) {
		const struct ccu_pm *pm = cache->pm;
		unsigned int off, i;

		/*
		 * PLLs first, so they have re-locked before anything that muxes
		 * off a PLL is restored.
		 */
		for (i = 0; i < pm->num_plls; i++)
			ccu_pm_restore_pll(cache, &pm->plls[i]);

		/*
		 * Everything else in ascending offset order. Module clocks
		 * (muxes/dividers) sit at lower offsets than the bus-gate/reset
		 * register they share a peripheral with, so restoring each word
		 * re-enables the gate and deasserts the reset together, in the
		 * same state they had at suspend. Firmware-owned registers (the
		 * base PLLs and the live CPU/bus/DRAM tree) and the PLLs already
		 * handled above are skipped.
		 */
		for (off = 0; off < pm->reg_size; off += sizeof(u32)) {
			if (ccu_pm_reg_is_firmware(pm, off) ||
			    ccu_pm_reg_is_pll(pm, off))
				continue;

			writel(cache->regs[off / sizeof(u32)], cache->base + off);
		}
	}
}

static struct syscore_ops ccu_pm_syscore_ops = {
	.suspend	= ccu_pm_suspend,
	.resume		= ccu_pm_resume,
};

static int ccu_pm_init(void __iomem *reg, const struct ccu_pm *pm)
{
	struct ccu_pm_cache *cache;

	cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	if (!cache)
		return -ENOMEM;

	cache->regs = kcalloc(pm->reg_size / sizeof(u32), sizeof(u32),
			      GFP_KERNEL);
	if (!cache->regs) {
		kfree(cache);
		return -ENOMEM;
	}

	cache->base = reg;
	cache->pm = pm;

	/*
	 * The clock controller is an essential, never-unbound provider, so the
	 * cache is registered for the lifetime of the system: the syscore ops
	 * are registered once, on the first participating instance, and the
	 * cache is never freed.
	 */
	if (list_empty(&ccu_pm_caches))
		register_syscore_ops(&ccu_pm_syscore_ops);

	list_add_tail(&cache->node, &ccu_pm_caches);

	return 0;
}

static int sunxi_ccu_probe(struct sunxi_ccu *ccu, struct device *dev,
			   struct device_node *node, void __iomem *reg,
			   const struct sunxi_ccu_desc *desc)
{
	struct ccu_reset *reset;
	int i, ret;

	ccu->desc = desc;

	spin_lock_init(&ccu->lock);

	for (i = 0; i < desc->num_ccu_clks; i++) {
		struct ccu_common *cclk = desc->ccu_clks[i];

		if (!cclk)
			continue;

		cclk->base = reg;
		cclk->lock = &ccu->lock;
	}

	for (i = 0; i < desc->hw_clks->num ; i++) {
		struct clk_hw *hw = desc->hw_clks->hws[i];
		const char *name;

		if (!hw)
			continue;

		name = hw->init->name;
		if (dev)
			ret = clk_hw_register(dev, hw);
		else
			ret = of_clk_hw_register(node, hw);
		if (ret) {
			pr_err("Couldn't register clock %d - %s\n", i, name);
			goto err_clk_unreg;
		}
	}

	for (i = 0; i < desc->num_ccu_clks; i++) {
		struct ccu_common *cclk = desc->ccu_clks[i];

		if (!cclk)
			continue;

		if (cclk->max_rate)
			clk_hw_set_rate_range(&cclk->hw, cclk->min_rate,
					      cclk->max_rate);
		else
			WARN(cclk->min_rate,
			     "No max_rate, ignoring min_rate of clock %d - %s\n",
			     i, clk_hw_get_name(&cclk->hw));
	}

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get,
				     desc->hw_clks);
	if (ret)
		goto err_clk_unreg;

	reset = &ccu->reset;
	reset->rcdev.of_node = node;
	reset->rcdev.ops = &ccu_reset_ops;
	reset->rcdev.owner = dev ? dev->driver->owner : THIS_MODULE;
	reset->rcdev.nr_resets = desc->num_resets;
	reset->base = reg;
	reset->lock = &ccu->lock;
	reset->reset_map = desc->resets;

	ret = reset_controller_register(&reset->rcdev);
	if (ret)
		goto err_del_provider;

	if (desc->pm) {
		ret = ccu_pm_init(reg, desc->pm);
		if (ret)
			goto err_unreg_reset;
	}

	return 0;

err_unreg_reset:
	reset_controller_unregister(&reset->rcdev);
err_del_provider:
	of_clk_del_provider(node);
err_clk_unreg:
	while (--i >= 0) {
		struct clk_hw *hw = desc->hw_clks->hws[i];

		if (!hw)
			continue;
		clk_hw_unregister(hw);
	}
	return ret;
}

static void devm_sunxi_ccu_release(struct device *dev, void *res)
{
	struct sunxi_ccu *ccu = res;
	const struct sunxi_ccu_desc *desc = ccu->desc;
	int i;

	reset_controller_unregister(&ccu->reset.rcdev);
	of_clk_del_provider(dev->of_node);

	for (i = 0; i < desc->hw_clks->num; i++) {
		struct clk_hw *hw = desc->hw_clks->hws[i];

		if (!hw)
			continue;
		clk_hw_unregister(hw);
	}
}

int devm_sunxi_ccu_probe(struct device *dev, void __iomem *reg,
			 const struct sunxi_ccu_desc *desc)
{
	struct sunxi_ccu *ccu;
	int ret;

	ccu = devres_alloc(devm_sunxi_ccu_release, sizeof(*ccu), GFP_KERNEL);
	if (!ccu)
		return -ENOMEM;

	ret = sunxi_ccu_probe(ccu, dev, dev->of_node, reg, desc);
	if (ret) {
		devres_free(ccu);
		return ret;
	}

	devres_add(dev, ccu);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(devm_sunxi_ccu_probe, "SUNXI_CCU");

void of_sunxi_ccu_probe(struct device_node *node, void __iomem *reg,
			const struct sunxi_ccu_desc *desc)
{
	struct sunxi_ccu *ccu;
	int ret;

	ccu = kzalloc(sizeof(*ccu), GFP_KERNEL);
	if (!ccu)
		return;

	ret = sunxi_ccu_probe(ccu, NULL, node, reg, desc);
	if (ret) {
		pr_err("%pOF: probing clocks failed: %d\n", node, ret);
		kfree(ccu);
	}
}

MODULE_DESCRIPTION("Common clock support for Allwinner SoCs");
MODULE_LICENSE("GPL");
