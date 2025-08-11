/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Synaptics SL261X clock tree IDs
 *
 * Copyright (C) 2025 Synaptics Incorporated
 *
 * Author: Jisheng Zhang <jszhang@kernel.org>
 */

/* common clks */
#define CLK_CPUFASTREF		0
#define CLK_MEMFASTREF		1
#define CLK_CFG			2
#define CLK_SYS			3
#define CLK_PERIFSYS		4
#define CLK_APBCORE		5
#define CLK_APBSER		6
#define CLK_ATB			7
#define CLK_HPC			8
#define CLK_EMMC		9
#define CLK_SD0			10
#define CLK_SD1			11
#define CLK_DECODER		12
#define CLK_GETHRGMII		13
#define CLK_GETHRGMII1		14
#define CLK_GE0_PTP_REF		15
#define CLK_GE1_PTP_REF		16
#define CLK_USB2TEST		17
#define CLK_USB2TEST480MG0	18
#define CLK_USB2TEST480MG1	19
#define CLK_USB2TEST480MG2	20
#define CLK_USB2TEST100MG0	21
#define CLK_USB2TEST100MG1	22
#define CLK_USB2TEST100MG2	23
#define CLK_USB2TEST100MG3	24
#define CLK_PERIFTEST125MG0	25
#define CLK_PERIFTEST200MG0	26
#define CLK_PERIFTEST200MG1	27
#define CLK_GPU			28
#define CLK_NPU			29
#define CLK_AVIOSYS		30
#define CLK_AIOSYS		31
#define CLK_AVIO_LCDC2SCAN	32
#define CLK_AVIO_IPI		33
#define CLK_AVIO_P		34
#define CLK_AVIO_DPHYRXTXESC	35
#define CLK_AVIOFPLL		36
#define CLK_AVIO_RX_SCANBYTE	37
#define CLK_AVIO_RX_SCANTEST	38

/* gate clks */
#define CLK_USB0CORE		0
#define CLK_SDIOSYS		1
#define CLK_EMMCSYS		2
#define CLK_GPUAXI		3
#define CLK_GETHRGMIISYS	4
#define CLK_SDIO1SYS		5
#define CLK_USB1CORE		6
#define CLK_GETHRGMII1SYS	7
#define CLK_USB0PHYREF		8
#define CLK_USB1PHYREF		9
#define CLK_APBUART0		10
#define CLK_APBUART1		11
#define CLK_APBUART2		12
#define CLK_APBUART3		13
#define CLK_APBI2C0		14
#define CLK_APBI2C1		15
#define CLK_APBSPI0		16
#define CLK_APBSPI1		17
#define CLK_APBSPI2		18
#define CLK_APBSPI3		19
#define CLK_APBGPIO		20
#define CLK_APBTIMERS		21
#define CLK_APBSYSCNT		22
#define CLK_APBWDT		23
