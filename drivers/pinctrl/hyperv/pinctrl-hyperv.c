// SPDX-License-Identifier: GPL-2.0
/*
 * Hyper-V WSL 2 pinctrl/GPIO driver
 *
 * Copyright (C) 2020 Matheus Castello
 * Author: Matheus Castello <matheus@castello.eng.br>
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/driver.h>
#include "../../gpio/gpiolib.h"

#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>

#define MODULE_NAME "pinctrl-hyperv"
#define HYPERV_NUM_GPIOS 36
#define HYPERV_FSEL_COUNT 8
/* TODO make it configurable */
#define HYPERV_BANK_COUNT 7

static const char * const hyperv_pctl_functions[HYPERV_FSEL_COUNT] = {
	[0] = "gpio_in",
	[1] = "gpio_out",
	[2] = "alt0",
	[3] = "alt1",
	[4] = "alt2",
	[5] = "alt3",
	[6] = "alt4",
	[7] = "alt5",
};

struct hyperv_pinctrl {
	struct device *dev;

	struct gpio_chip *gpio_chip;
	struct pinctrl_desc *pctl_desc;
	struct pinctrl_dev *pctl_dev;
	struct pinctrl_gpio_range gpio_range;
};

#define HYPERV_GPIO_PIN(a) \
		PINCTRL_PIN(a + 1, "gpio" #a + 1), \
		PINCTRL_PIN(a + 2, "gpio" #a + 1), \
		PINCTRL_PIN(a + 3, "gpio" #a + 1), \
		PINCTRL_PIN(a + 4, "gpio" #a + 1), \
		PINCTRL_PIN(a + 5, "gpio" #a + 1), \
		PINCTRL_PIN(a + 6, "gpio" #a + 1), \
		PINCTRL_PIN(a + 7, "gpio" #a + 1), \
		PINCTRL_PIN(a + 8, "gpio" #a + 1), \
		PINCTRL_PIN(a + 9, "gpio" #a + 1), \
		PINCTRL_PIN(a + 10, "gpio" #a + 1), \
		PINCTRL_PIN(a + 11, "gpio" #a + 1), \
		PINCTRL_PIN(a + 12, "gpio" #a + 1)

static struct pinctrl_pin_desc hyperv_gpio_pins[] = {
	HYPERV_GPIO_PIN(0),
	HYPERV_GPIO_PIN(12),
	HYPERV_GPIO_PIN(24)
};

static const char * const hyperv_gpio_groups[] = {
	"gpio0",
	"gpio1",
	"gpio2",
	"gpio3",
	"gpio4",
	"gpio5",
	"gpio6",
	"gpio7",
	"gpio8",
	"gpio9",
	"gpio10",
	"gpio11",
	"gpio12",
	"gpio13",
	"gpio14",
	"gpio15",
	"gpio16",
	"gpio17",
	"gpio18",
	"gpio19",
	"gpio20",
	"gpio21",
	"gpio22",
	"gpio23",
	"gpio24",
	"gpio25",
	"gpio26",
	"gpio27",
	"gpio28",
	"gpio29",
	"gpio30",
	"gpio31",
	"gpio32",
	"gpio33",
	"gpio34",
	"gpio35",
};

static int hyperv_get_functions_count(struct pinctrl_dev *pctldev)
{
	return HYPERV_FSEL_COUNT;
}

static const char *hyperv_get_function_name(struct pinctrl_dev *pctldev,
		unsigned int selector)
{
	return hyperv_pctl_functions[selector];
}

static int hyperv_get_function_groups(struct pinctrl_dev *pctldev,
		unsigned int selector,
		const char * const **groups,
		unsigned * const num_groups)
{
	/* every pin can do every function */
	/*groups = bcm2835_gpio_groups;*/
	*num_groups = HYPERV_NUM_GPIOS;

	return 0;
}

static int hyperv_set_mux(struct pinctrl_dev *pctldev,
		unsigned int func_selector,
		unsigned int group_selector)
{
	return 0;
}

static void hyperv_gpio_disable_free(struct pinctrl_dev *pctldev,
		struct pinctrl_gpio_range *range,
		unsigned int offset)
{ }

static int hyperv_gpio_set_direction(struct pinctrl_dev *pctldev,
		struct pinctrl_gpio_range *range,
		unsigned int offset,
		bool input)
{
	return 0;
}

static int hyperv_get_groups_count(struct pinctrl_dev *pctldev)
{
	return HYPERV_NUM_GPIOS;
}

static const char *hyperv_get_group_name(struct pinctrl_dev *pctldev,
		unsigned int selector)
{
	return hyperv_gpio_groups[selector];
}

static int hyperv_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	int *p = 0;

	pr_info("%s: config my virtual bank %d line %d to input\n",
		__func__, chip->gpiodev->id, offset);

	if (offset == 47)
		panic("WSL CONF this is a panic\n");

	if (offset == 22) {
		pr_info("WSL CONF this is a BUG\n");
		BUG();
	}

	if (offset == 7) {
		pr_info("WSL CONF this is a oops\n");
		pr_info("%d\n", *p);
	}

	WARN_ON(offset == 9);

	return 0;
}

static int hyperv_gpio_direction_output(struct gpio_chip *chip,
		unsigned int offset, int value)
{
	pr_info("%s: config my virtual bank %d line %d to output %d\n",
		__func__, chip->gpiodev->id, offset, value);

	return 0;
}

static void hyperv_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	int *p = 0;

	pr_info("%s: config my virtual bank %d line %d to output %d\n",
		__func__, chip->gpiodev->id, offset, value);

	if (offset == 47)
		panic("WSL CONF this is a panic\n");

	if (offset == 22) {
		pr_info("WSL CONF this is a BUG\n");
		BUG();
	}

	if (offset == 7) {
		pr_info("WSL CONF this is a oops\n");
		pr_info("%d\n", *p);
	}

	WARN_ON(offset == 9);
}

static int hyperv_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int hyperv_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	pr_info("%s: get my virtual bank %d line %d\n",
		__func__, chip->gpiodev->id, offset);

	return 0;
}

static const struct pinctrl_ops hyperv_pctl_ops = {
	.get_groups_count = hyperv_get_groups_count,
	.get_group_name = hyperv_get_group_name,
};

static const struct pinmux_ops hyperv_pinmux_ops = {
	.get_functions_count = hyperv_get_functions_count,
	.get_function_name = hyperv_get_function_name,
	.get_function_groups = hyperv_get_function_groups,
	.set_mux = hyperv_set_mux,
	.gpio_disable_free = hyperv_gpio_disable_free,
	.gpio_set_direction = hyperv_gpio_set_direction,
};

static const struct pinctrl_desc hyperv_pinctrl_desc = {
	.name = MODULE_NAME,
	.pins = hyperv_gpio_pins,
	.npins = HYPERV_NUM_GPIOS,
	.pctlops = &hyperv_pctl_ops,
	.pmxops = &hyperv_pinmux_ops,
	.owner = THIS_MODULE,
};

static int hyperv_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hyperv_pinctrl *pc;
	struct gpio_chip *chip;
	int err, i;

	/* prepare */
	pc = devm_kzalloc(dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	platform_set_drvdata(pdev, pc);
	pc->dev = dev;

	/* add the chardevs */
	for (i = 0; i < HYPERV_BANK_COUNT; i++) {
		chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
		pc->gpio_chip = chip;

		chip->owner = THIS_MODULE,
		chip->request = gpiochip_generic_request,
		chip->free = gpiochip_generic_free,
		chip->direction_input = hyperv_gpio_direction_input,
		chip->direction_output = hyperv_gpio_direction_output,
		chip->get_direction = hyperv_gpio_get_direction,
		chip->get = hyperv_gpio_get,
		chip->set = hyperv_gpio_set,
		chip->can_sleep = false,
		chip->ngpio = HYPERV_NUM_GPIOS;
		chip->label = MODULE_NAME;
		chip->parent = dev;
		chip->base = (pdev->id * HYPERV_NUM_GPIOS);

		err = devm_gpiochip_add_data(dev, pc->gpio_chip, pc);
		if (err) {
			dev_err(dev, "could not add GPIO chip\n");
			return err;
		}
	}

	/* register pinctrl */
	pc->pctl_desc = &hyperv_pinctrl_desc;
	pc->pctl_dev =  devm_pinctrl_register(dev, pc->pctl_desc,
					      pc);
	if (IS_ERR(pc->pctl_dev)) {
		dev_err(dev, "failed to register pinctrl driver\n");
		return PTR_ERR(pc->pctl_dev);
	}

	dev_info(dev, "HELLO WSL 2\n");

	return 0;
}

static const struct acpi_device_id hyperv_pinctrl_acpi_match[] = {
	{ "PNP0003", (kernel_ulong_t)&hyperv_pinctrl_desc },
	{ "VMBUS", (kernel_ulong_t)&hyperv_pinctrl_desc },
	{ "ACPI0003", (kernel_ulong_t)&hyperv_pinctrl_desc },
	{ "PNP0C0A", (kernel_ulong_t)&hyperv_pinctrl_desc },
	{ }
};
MODULE_DEVICE_TABLE(acpi, icl_pinctrl_acpi_match);

static struct platform_driver hyperv_pinctrl_driver = {
	.probe = hyperv_pinctrl_probe,
	.driver = {
		.name = "hyperv-pinctrl",
		.acpi_match_table = hyperv_pinctrl_acpi_match,
	},
};

static int __init hyperv_pinctrl_init(void)
{
	return platform_driver_register(&hyperv_pinctrl_driver);
}
arch_initcall(hyperv_pinctrl_init);

static void __exit hyperv_pinctrl_exit(void)
{
	platform_driver_unregister(&hyperv_pinctrl_driver);
}
module_exit(hyperv_pinctrl_exit);

MODULE_AUTHOR("Matheus Castello <matheus@castello.eng.br>");
MODULE_DESCRIPTION("Virtual pinctrl/GPIO driver");
MODULE_LICENSE("GPL v2");
