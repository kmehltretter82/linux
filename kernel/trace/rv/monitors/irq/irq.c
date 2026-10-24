// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rv.h>

#define MODULE_NAME "irq"

#include "irq.h"

struct rv_monitor rv_irq = {
	.name = "irq",
	.description = "IRQ context monitors.",
	.enable = NULL,
	.disable = NULL,
	.reset = NULL,
	.enabled = 0,
};

static int __init register_irq(void)
{
	return rv_register_monitor(&rv_irq, NULL);
}

static void __exit unregister_irq(void)
{
	rv_unregister_monitor(&rv_irq);
}

module_init(register_irq);
module_exit(unregister_irq);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Karl Mehltretter <kmehltretter@gmail.com>");
MODULE_DESCRIPTION("irq: IRQ context monitors.");
