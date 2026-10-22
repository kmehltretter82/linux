// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rv.h>

#define MODULE_NAME "irqentry"

#include "irqentry.h"

struct rv_monitor rv_irqentry = {
	.name = "irqentry",
	.description = "IRQ-entry context monitors.",
	.enable = NULL,
	.disable = NULL,
	.reset = NULL,
	.enabled = 0,
};

static int __init register_irqentry(void)
{
	return rv_register_monitor(&rv_irqentry, NULL);
}

static void __exit unregister_irqentry(void)
{
	rv_unregister_monitor(&rv_irqentry);
}

module_init(register_irqentry);
module_exit(unregister_irqentry);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Karl Mehltretter <kmehltretter@gmail.com>");
MODULE_DESCRIPTION("irqentry: IRQ-entry context monitors.");
