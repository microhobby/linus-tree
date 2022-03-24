// SPDX-License-Identifier: GPL-2.0-only
/*
 * Multiplex several virtual IPIs over a single HW IPI.
 *
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#define pr_fmt(fmt) "ipi-mux: " fmt
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/smp.h>

static unsigned int ipi_mux_nr;
static unsigned int ipi_mux_parent_virq;
static struct irq_domain *ipi_mux_domain;
static const struct  ipi_mux_ops *ipi_mux_ops;
static DEFINE_PER_CPU(unsigned long, ipi_mux_bits);

static void ipi_mux_send_mask(struct irq_data *d, const struct cpumask *mask)
{
	int cpu;

	/* Barrier before doing atomic bit update to IPI bits */
	smp_mb__before_atomic();

	for_each_cpu(cpu, mask)
		set_bit(d->hwirq, per_cpu_ptr(&ipi_mux_bits, cpu));

	/* Barrier after doing atomic bit update to IPI bits */
	smp_mb__after_atomic();

	/* Trigger the parent IPI */
	ipi_mux_ops->ipi_mux_send(ipi_mux_parent_virq, mask);
}

static const struct irq_chip ipi_mux_chip = {
	.name		= "IPI Mux",
	.ipi_send_mask	= ipi_mux_send_mask,
};

static int ipi_mux_domain_alloc(struct irq_domain *d, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	struct irq_fwspec *fwspec = arg;
	irq_hw_number_t hwirq;
	unsigned int type;
	int i, ret;

	ret = irq_domain_translate_onecell(d, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		irq_set_percpu_devid(virq + i);
		irq_domain_set_info(d, virq + i, hwirq + i,
				    &ipi_mux_chip, d->host_data,
				    handle_percpu_devid_irq, NULL, NULL);
	}

	return 0;
}

static const struct irq_domain_ops ipi_mux_domain_ops = {
	.alloc		= ipi_mux_domain_alloc,
	.free		= irq_domain_free_irqs_top,
};

/**
 * ipi_mux_process - Process multiplexed virtual IPIs
 */
void ipi_mux_process(void)
{
	unsigned long irqs, *bits = this_cpu_ptr(&ipi_mux_bits);
	irq_hw_number_t hwirq;
	int err;

	/* Clear the parent IPI */
	if (ipi_mux_ops->ipi_mux_clear)
		ipi_mux_ops->ipi_mux_clear(ipi_mux_parent_virq);

	/*
	 * Barrier for IPI bits paired with smp_mb__before_atomic()
	 * and smp_mb__after_atomic() used in ipi_mux_send_mask().
	 */
	smp_mb();

	irqs = xchg(bits, 0);
	if (!irqs)
		return;

	for_each_set_bit(hwirq, &irqs, ipi_mux_nr) {
		err = generic_handle_domain_irq(ipi_mux_domain,
						hwirq);
		if (unlikely(err))
			pr_warn_ratelimited(
				"can't find mapping for hwirq %lu\n",
				hwirq);
	}
}

static void ipi_mux_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);

	chained_irq_enter(chip, desc);
	ipi_mux_process();
	chained_irq_exit(chip, desc);
}

static int ipi_mux_dying_cpu(unsigned int cpu)
{
	disable_percpu_irq(ipi_mux_parent_virq);
	return 0;
}

static int ipi_mux_starting_cpu(unsigned int cpu)
{
	enable_percpu_irq(ipi_mux_parent_virq,
			  irq_get_trigger_type(ipi_mux_parent_virq));
	return 0;
}

/**
 * ipi_mux_create - Create virtual IPIs multiplexed on top of a single
 * parent IPI.
 * @parent_virq:	virq of the parent per-CPU IRQ
 * @nr_ipi:		number of virtual IPIs to create. This should
 * 			be <= BITS_PER_LONG
 * @ops:		multiplexing operations for the parent IPI
 *
 * If the parent IPI > 0 then ipi_mux_process() will be automatically
 * called via chained handler.
 *
 * If the parent IPI <= 0 then it is responsibility of irqchip drivers
 * to explicitly call ipi_mux_process() for processing muxed IPIs.
 *
 * Returns first virq of the newly created virtual IPIs upon success
 * or <=0 upon failure
 */
int ipi_mux_create(unsigned int parent_virq, unsigned int nr_ipi,
		   const struct ipi_mux_ops *ops)
{
	struct fwnode_handle *fwnode;
	struct irq_domain *domain;
	struct irq_fwspec ipi;
	int virq;

	if (ipi_mux_domain || BITS_PER_LONG < nr_ipi ||
	    !ops || !ops->ipi_mux_send)
		return -EINVAL;

	if (parent_virq &&
	    !irqd_is_per_cpu(irq_desc_get_irq_data(irq_to_desc(parent_virq))))
		return -EINVAL;

	fwnode = irq_domain_alloc_named_fwnode("IPI-Mux");
	if (!fwnode) {
		pr_err("unable to create IPI Mux fwnode\n");
		return -ENOMEM;
	}

	domain = irq_domain_create_simple(fwnode, nr_ipi, 0,
					  &ipi_mux_domain_ops, NULL);
	if (!domain) {
		pr_err("unable to add IPI Mux domain\n");
		irq_domain_free_fwnode(fwnode);
		return -ENOMEM;
	}

	ipi.fwnode = domain->fwnode;
	ipi.param_count = 1;
	ipi.param[0] = 0;
	virq = __irq_domain_alloc_irqs(domain, -1, nr_ipi,
				       NUMA_NO_NODE, &ipi, false, NULL);
	if (virq <= 0) {
		pr_err("unable to alloc IRQs from IPI Mux domain\n");
		irq_domain_remove(domain);
		irq_domain_free_fwnode(fwnode);
		return virq;
	}

	ipi_mux_domain = domain;
	ipi_mux_nr = nr_ipi;
	ipi_mux_parent_virq = parent_virq;
	ipi_mux_ops = ops;

	if (parent_virq > 0) {
		irq_set_chained_handler(parent_virq, ipi_mux_handler);
		cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				  "irqchip/ipi-mux:starting",
				  ipi_mux_starting_cpu, ipi_mux_dying_cpu);
	}

	return virq;
}
