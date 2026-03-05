// SPDX-License-Identifier: GPL-2.0+
/*
 * panic_button - Simple GPIO change detector
 *
 * Monitors gpio0 line 6 (porta, pin 6) and prints a message via printk
 * whenever the GPIO input value changes (rising or falling edge).
 *
 * Device tree binding:
 *   compatible = "tdx,panic-button";
 *   gpios = <&portb 6 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>

#define DEBOUNCE_MSEC 50

struct gpio_monitor_priv {
	struct gpio_desc *gpiod;
	int irq;
	int last_val;
	struct delayed_work dwork;
};

static void gpio_monitor_work(struct work_struct *work)
{
	struct gpio_monitor_priv *priv =
		container_of(to_delayed_work(work), struct gpio_monitor_priv, dwork);
	int val = gpiod_get_value_cansleep(priv->gpiod);

	if (val != priv->last_val) {
		printk(KERN_INFO "panic_button: gpio0 line 6 changed: %d -> %d\n",
		       priv->last_val, val);

		if (priv->last_val == 1 && val == 0) {
			panic("PANIC BUTTON PRESSED:: HELLO EW 2026!!\n");
		}

		priv->last_val = val;
	}
}

static irqreturn_t gpio_monitor_isr(int irq, void *dev_id)
{
	struct gpio_monitor_priv *priv = dev_id;

	mod_delayed_work(system_wq, &priv->dwork,
			 msecs_to_jiffies(DEBOUNCE_MSEC));
	return IRQ_HANDLED;
}

static void cancel_dwork(void *data)
{
	cancel_delayed_work_sync(data);
}

static int panic_button_probe(struct platform_device *pdev)
{
	struct gpio_monitor_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	INIT_DELAYED_WORK(&priv->dwork, gpio_monitor_work);

	/* GPIOD_IN applies the DT flags (pull-up + active-low polarity)
	 * during gpiod_get, so the pull-up is armed before the first read. */
	priv->gpiod = devm_gpiod_get(&pdev->dev, NULL, GPIOD_IN);
	if (IS_ERR(priv->gpiod)) {
		dev_err(&pdev->dev, "failed to get GPIO descriptor: %ld\n",
			PTR_ERR(priv->gpiod));
		return PTR_ERR(priv->gpiod);
	}

	priv->last_val = gpiod_get_value_cansleep(priv->gpiod);

	priv->irq = gpiod_to_irq(priv->gpiod);
	if (priv->irq < 0) {
		dev_err(&pdev->dev, "failed to map GPIO to IRQ: %d\n", priv->irq);
		return priv->irq;
	}

	/*
	 * Register the work cancellation before requesting the IRQ so that
	 * devm unwind (LIFO) frees the IRQ first, then drains the work.
	 */
	ret = devm_add_action_or_reset(&pdev->dev, cancel_dwork, &priv->dwork);
	if (ret)
		return ret;

	ret = devm_request_irq(&pdev->dev, priv->irq, gpio_monitor_isr,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       "panic_button", priv);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ %d: %d\n",
			priv->irq, ret);
		return ret;
	}

	platform_set_drvdata(pdev, priv);

	dev_info(&pdev->dev,
		 "monitoring gpio0 line 6 on IRQ %d (initial value: %d)\n",
		 priv->irq, priv->last_val);

	return 0;
}

static const struct of_device_id panic_button_of_match[] = {
	{ .compatible = "tdx,panic-button" },
	{ }
};

MODULE_DEVICE_TABLE(of, panic_button_of_match);

static struct platform_driver panic_button_driver = {
	.probe	= panic_button_probe,
	.driver = {
		.name		= "panic_button",
		.of_match_table	= panic_button_of_match,
	},
};
module_platform_driver(panic_button_driver);

MODULE_AUTHOR("MicroHobby");
MODULE_DESCRIPTION("Panic Button GPIO Monitor");
MODULE_LICENSE("GPL v2");
