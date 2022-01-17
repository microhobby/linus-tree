// SPDX-License-Identifier: GPL-2.0-only
/*
 * Multiplex several IPIs over a single HW IPI.
 *
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#define pr_fmt(fmt) "riscv: " fmt
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <asm/sbi.h>

#ifdef CONFIG_SMP

static void sbi_send_cpumask_ipi(unsigned int parent_virq,
				 const struct cpumask *target)
{
	sbi_send_ipi(target);
}

static void sbi_ipi_clear(unsigned int parent_virq)
{
	csr_clear(CSR_IP, IE_SIE);
}

static struct ipi_mux_ops sbi_ipi_ops = {
	.ipi_mux_clear = sbi_ipi_clear,
	.ipi_mux_send = sbi_send_cpumask_ipi,
};

void __init sbi_ipi_init(void)
{
	int virq, parent_virq;
	struct irq_domain *domain;

	if (riscv_ipi_have_virq_range())
		return;

	domain = irq_find_matching_fwnode(riscv_get_intc_hwnode(),
					  DOMAIN_BUS_ANY);
	if (!domain) {
		pr_err("unable to find INTC IRQ domain\n");
		return;
	}

	parent_virq = irq_create_mapping(domain, RV_IRQ_SOFT);
	if (!parent_virq) {
		pr_err("unable to create INTC IRQ mapping\n");
		return;
	}

	virq = ipi_mux_create(parent_virq, BITS_PER_LONG, &sbi_ipi_ops);
	if (virq <= 0) {
		pr_err("unable to create muxed IPIs\n");
		irq_dispose_mapping(parent_virq);
		return;
	}

	riscv_ipi_set_virq_range(virq, BITS_PER_LONG, false, false);
	pr_info("providing IPIs using SBI IPI extension\n");
}

#endif
