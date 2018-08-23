/*
 * Clap Sensor driver
 * This is for that simple sensors devices where we have a mic and the Output
 * from mic is input to opamp to made the logic trigger.
 *
 * Copyright (C) 2018 www.castello.eng.br
 * 2018-07-06: Matheus Castello <matheus@castello.eng.br>
 *             initial version
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/gpio.h>			/* legacy gpio */
#include <linux/gpio/consumer.h>	/* gpiolib */
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/of.h>			/* get gpio from device tree */
#include <linux/of_device.h>
#include <linux/platform_device.h>

struct clap_sensor {
	struct input_dev *idev;
	struct device *dev;
	int debounce;
	bool debounced;
#ifdef CONFIG_CLAP_GPIO_LEGACY
	int gpio;
#endif
#ifdef CONFIG_CLAP_GPIO_LEGACY_OF
	const struct of_device_id *match;
	struct device_node *node;
#endif
#ifdef CONFIG_CLAP_GPIODESC
	struct gpio_desc *gpio;
#endif
};

static irqreturn_t clap_sensor_irq(int irq, void *_clap)
{
	struct clap_sensor *clap = _clap;
	int val;

	/* check debounce */
	if (clap->debounced) {
		/* change led state */
#ifdef CONFIG_CLAP_GPIO_LEGACY_OF
		val = gpio_get_value(clap->gpio);
		val = val ? 0 : 1;
		gpio_set_value(clap->gpio, val);
#endif

#ifdef CONFIG_CLAP_GPIODESC
		val = gpiod_get_value(clap->gpio);
		val = val ? 0 : 1;
		gpiod_set_value(clap->gpio, val);
#endif
		dev_info(clap->dev, "CLAPED\n");
		/* take time to debounce */
		clap->debounced = false;
	}

	return IRQ_WAKE_THREAD;
}

static irqreturn_t clap_sensor_thread_irq(int irq, void *_clap)
{
	struct clap_sensor *clap = _clap;

	if  (!clap->debounced) {
		kobject_uevent(&clap->dev->kobj, KOBJ_CHANGE);
		msleep(clap->debounce);
		clap->debounced = true;
	}

	return IRQ_HANDLED;
}

static int clap_sensor_probe(struct platform_device *pdev)
{
	struct clap_sensor *clap;
	int irq = platform_get_irq(pdev, 0);
	int err;

	/* memory allocation */
	clap = devm_kmalloc(&pdev->dev, sizeof(*clap), GFP_KERNEL);
	if (!clap) {
		dev_err(&pdev->dev, "CLAP malloc error %d\n", -ENOMEM);
		return -ENOMEM;
	}

	clap->idev = devm_input_allocate_device(&pdev->dev);
	if (!clap->idev) {
		dev_err(&pdev->dev, "CLAP clap->idev malloc error %d\n",
				-ENOMEM);
		return -ENOMEM;
	}

	/* initializations */
	clap->dev = &pdev->dev;
	clap->idev->name = "clap-sensor";
	clap->idev->phys = "clap-sensor/input0";
	clap->idev->dev.parent = clap->dev;
	input_set_capability(clap->idev, EV_SND, SND_CLICK);

	/* gpio for led trigger initializations */
#ifdef CONFIG_CLAP_GPIO_LEGACY
	clap->gpio = 23;

#ifdef CONFIG_CLAP_GPIO_LEGACY_OF
	err = of_property_read_u32(clap->dev->of_node,
		"clap-trigger-led", &clap->gpio);
	if (err) {
		dev_err(&pdev->dev, "Error trying request gpio %d\n", err);
		return err;
	} else
		dev_info(&pdev->dev, "We get the GPIO %d\n", clap->gpio);
#endif
	err = gpio_request_one(clap->gpio, GPIOF_DIR_OUT, "clap-trigger");
	if (err) {
		dev_err(&pdev->dev, "Error trying request gpio %d\n", err);
		return err;
	}
#endif

#ifdef CONFIG_CLAP_GPIODESC
	clap->gpio = gpiod_get(clap->dev, "trigger", GPIOD_OUT_LOW); /* GPIOD_ASIS */
	if (IS_ERR(clap->gpio)) {
		err = -ENOENT;
		dev_err(&pdev->dev, "Error trying request gpio %d\n", err);
		return err;
	}
#endif

	/* get debounce time from device tree */
	err = of_property_read_u32(clap->dev->of_node,
		"debounce", &clap->debounce);
	if (err) {
		dev_err(&pdev->dev, "Error trying request debounce %d\n", err);
		return err;
	} else
		dev_info(&pdev->dev, "We get the debounce %d\n", clap->debounce);
	clap->debounced = true;

	dev_info(clap->dev, "initializing CLAP\n");

	/* interrupt */
	if (irq < 0) {
		dev_err(&pdev->dev, "platform IRQ request failed: %d\n", irq);
		return irq;
	}

	err = devm_request_threaded_irq(&pdev->dev, irq, clap_sensor_irq,
		clap_sensor_thread_irq, IRQF_ONESHOT, "clap-sensor", clap);
	if (err < 0) {
		dev_err(&pdev->dev, "IRQ request failed: %d\n", err);
		return err;
	}

	/* put dev in input subsystem */
	err = input_register_device(clap->idev);
	if (err) {
		dev_err(&pdev->dev, "Input register failed: %d\n", err);
		return err;
	}

	device_init_wakeup(&pdev->dev, true);

	dev_info(clap->dev, "CLAP Probed\n");

	return 0;
}

static int clap_sensor_remove (struct platform_device *pdev)
{
	struct clap_sensor *clap = platform_get_drvdata(pdev);
	int irq = platform_get_irq(pdev, 0);

#ifdef CONFIG_CLAP_GPIODESC
	gpiod_put(clap->gpio);
#endif
	/* clear device */
	input_unregister_device(clap->idev);
	free_irq(irq, clap);
	kfree(clap);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id clap_sensor_dt_match_table[] = {
	{ .compatible = "texugo,clap-sensor" },
	{ },
};
MODULE_DEVICE_TABLE(of, clap_sensor_dt_match_table);
#endif

static struct platform_driver clap_sensor_driver = {
	.probe = clap_sensor_probe,
	.remove = clap_sensor_remove,
	.driver = {
		.name = "clap-sensor",
		.of_match_table = of_match_ptr(clap_sensor_dt_match_table),
	},
};
module_platform_driver(clap_sensor_driver);

MODULE_AUTHOR("Matheus Castello <matheus@castello.eng.br>");
MODULE_DESCRIPTION("Driver for generic Clap Sensor from an OpAmp output");
MODULE_LICENSE("GPL v2");
