// SPDX-License-Identifier: GPL-2.0
/*
 * What Ingenic's 3.10.14 arch code gives the camera drivers and the
 * mainline kernel does not, for the drivers ported from it (tx-isp, the
 * sensor drivers, the VPU and audio):
 *
 *   /proc/jz                  get_jz_proc_root(), jz_proc_mkdir()
 *   jzgpio_set_func()         pin functions by port and mask, written to
 *                             the GPIO registers as the vendor gpio.c does
 *   t20_intc_irq()            Linux interrupt number of an INTC line
 *   ispmem_base, ispmem_size  the ISP's reserved memory (vendor ispmem=)
 *   the tx-isp platform devices (vendor soc-t20/common/platform.c)
 *
 * Load it before any of them.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>

#include <soc/base.h>
#include <soc/gpio.h>
#include <soc/irq.h>
#include <mach/tx_isp.h>
#include <jz_proc.h>

/* /proc/jz */

static struct proc_dir_entry *jz_proc_root;

struct proc_dir_entry *get_jz_proc_root(void)
{
	return jz_proc_root;
}
EXPORT_SYMBOL(get_jz_proc_root);

struct proc_dir_entry *jz_proc_mkdir(char *s)
{
	return proc_mkdir(s, jz_proc_root);
}
EXPORT_SYMBOL(jz_proc_mkdir);

/* GPIO functions (soc-t20/common/gpio.c gpio_set_func) */

#define PXINTS		0x14
#define PXINTC		0x18
#define PXMSKS		0x24
#define PXMSKC		0x28
#define PXPAT1S		0x34
#define PXPAT1C		0x38
#define PXPAT0S		0x44
#define PXPAT0C		0x48
#define PXPENS		0x64
#define PXPENC		0x68

static void __iomem *gpio_base;
static DEFINE_SPINLOCK(gpio_lock);

int jzgpio_set_func(enum gpio_port port, enum gpio_function func, unsigned long pins)
{
	void __iomem *r;
	unsigned long flags;

	if (port >= GPIO_PORT_E || !gpio_base)
		return -EINVAL;
	r = gpio_base + port * 0x100;

	spin_lock_irqsave(&gpio_lock, flags);
	writel(pins, r + ((func & 0x8) ? PXINTS : PXINTC));
	writel(pins, r + ((func & 0x4) ? PXMSKS : PXMSKC));
	writel(pins, r + ((func & 0x2) ? PXPAT1S : PXPAT1C));
	writel(pins, r + ((func & 0x1) ? PXPAT0S : PXPAT0C));
	writel(pins, r + ((func & 0x10) ? PXPENS : PXPENC));
	spin_unlock_irqrestore(&gpio_lock, flags);
	return 0;
}
EXPORT_SYMBOL(jzgpio_set_func);

int jz_gpio_set_func(int gpio, enum gpio_function func)
{
	return jzgpio_set_func(gpio / 32, func, BIT(gpio % 32));
}
EXPORT_SYMBOL(jz_gpio_set_func);

int jzgpio_ctrl_pull(enum gpio_port port, int enable_pull, unsigned long pins)
{
	if (port >= GPIO_PORT_E || !gpio_base)
		return -EINVAL;
	writel(pins, gpio_base + port * 0x100 + (enable_pull ? PXPENS : PXPENC));
	return 0;
}
EXPORT_SYMBOL(jzgpio_ctrl_pull);

/* Interrupts */

static struct device_node *intc_np;

int t20_intc_irq(unsigned int hwirq)
{
	struct of_phandle_args args = {
		.np = intc_np,
		.args_count = 1,
		.args = { hwirq },
	};

	return intc_np ? irq_create_of_mapping(&args) : -ENODEV;
}
EXPORT_SYMBOL(t20_intc_irq);

/* ISP reserved memory: outside Linux's memory (see the device tree) */

unsigned long ispmem_base = 0x6000000;
EXPORT_SYMBOL(ispmem_base);
module_param(ispmem_base, ulong, 0444);

unsigned long ispmem_size = 0x800000;
EXPORT_SYMBOL(ispmem_size);
module_param(ispmem_size, ulong, 0444);

/* tx-isp platform devices (soc-t20/common/platform.c, CONFIG_VIDEO_TX_ISP) */

static u64 tx_isp_module_dma_mask = ~(u64)0;

static struct resource tx_isp_csi_resource[] = {
	DEFINE_RES_MEM_NAMED(MIPI_CSI_IOBASE, 0x1000 + 1, "mipi_csi"),
};

static struct tx_isp_subdev_clk_info tx_csi_clk_info[] = {
	{ "csi", DUMMY_CLOCK_RATE },
};

static struct tx_isp_subdev_platform_data tx_isp_subdev_csi = {
	.grp_id = TX_ISP_CSI_GRP_IDX,
	.clks = tx_csi_clk_info,
	.clk_num = ARRAY_SIZE(tx_csi_clk_info),
};

static struct resource tx_isp_vic_resource[] = {
	DEFINE_RES_MEM_NAMED(ISP_VIC_IOBASE, 0x10000 + 1, "isp_vic"),
};

static struct tx_isp_subdev_platform_data tx_isp_subdev_vic = {
	.grp_id = TX_ISP_VIC_GRP_IDX,
};

static struct resource tx_isp_core_resource[] = {
	DEFINE_RES_MEM_NAMED(ISP_CORE_IOBASE, 0x80000 + 1, "isp_core"),
};

static struct tx_isp_subdev_clk_info isp_core_clk_info[] = {
	{ "cgu_isp", 85000000 },
	{ "isp", DUMMY_CLOCK_RATE },
};

static struct tx_isp_subdev_platform_data tx_isp_subdev_core = {
	.grp_id = TX_ISP_CORE_GRP_IDX,
	.clks = isp_core_clk_info,
	.clk_num = ARRAY_SIZE(isp_core_clk_info),
};

/* The interrupt is filled in at load time */
static struct resource tx_isp_device_resource[] = {
	DEFINE_RES_MEM_NAMED(ISP_IRQ_IOBASE, 0x10000 + 1, "isp_device_irq"),
	DEFINE_RES_IRQ_NAMED(0, "isp_irq_id"),
};

static struct tx_isp_subdev_platform_data tx_isp_subdev_video_in = {
	.grp_id = TX_ISP_VIDEO_IN_GRP_IDX,
};

#define TX_ISP_PDEV(_var, _name, _pdata, _res)			\
static struct platform_device _var = {				\
	.name = _name,						\
	.id = -1,						\
	.dev = {						\
		.dma_mask = &tx_isp_module_dma_mask,		\
		.coherent_dma_mask = 0xffffffff,		\
		.platform_data = _pdata,			\
	},							\
	.num_resources = ARRAY_SIZE(_res),			\
	.resource = _res,					\
}

TX_ISP_PDEV(tx_isp_csi_platform_device, TX_ISP_CSI_NAME, &tx_isp_subdev_csi, tx_isp_csi_resource);
TX_ISP_PDEV(tx_isp_vic_platform_device, TX_ISP_VIC_NAME, &tx_isp_subdev_vic, tx_isp_vic_resource);
TX_ISP_PDEV(tx_isp_core_platform_device, TX_ISP_CORE_NAME, &tx_isp_subdev_core, tx_isp_core_resource);
TX_ISP_PDEV(tx_isp_platform_device, "tx-isp", &tx_isp_subdev_video_in, tx_isp_device_resource);

/* The vendor board registers them in this order (board_base.c) */
static struct platform_device *tx_isp_devices[] = {
	&tx_isp_csi_platform_device,
	&tx_isp_vic_platform_device,
	&tx_isp_core_platform_device,
	&tx_isp_platform_device,
};

static int __init t20_compat_init(void)
{
	int irq, ret;

	intc_np = of_find_compatible_node(NULL, NULL, "ingenic,t20-intc");
	if (!intc_np)
		return -ENODEV;

	gpio_base = ioremap(GPIO_IOBASE, 0x1000);
	if (!gpio_base) {
		ret = -ENOMEM;
		goto err_np;
	}

	jz_proc_root = proc_mkdir("jz", NULL);
	if (!jz_proc_root) {
		ret = -ENOMEM;
		goto err_gpio;
	}

	irq = t20_intc_irq(29);
	if (irq <= 0) {
		ret = irq ? irq : -EINVAL;
		goto err_proc;
	}
	tx_isp_device_resource[1].start = tx_isp_device_resource[1].end = irq;

	ret = platform_add_devices(tx_isp_devices, ARRAY_SIZE(tx_isp_devices));
	if (ret)
		goto err_proc;
	return 0;

err_proc:
	proc_remove(jz_proc_root);
err_gpio:
	iounmap(gpio_base);
err_np:
	of_node_put(intc_np);
	return ret;
}

static void __exit t20_compat_exit(void)
{
	int i;

	for (i = ARRAY_SIZE(tx_isp_devices) - 1; i >= 0; i--)
		platform_device_unregister(tx_isp_devices[i]);
	proc_remove(jz_proc_root);
	iounmap(gpio_base);
	of_node_put(intc_np);
}

module_init(t20_compat_init);
module_exit(t20_compat_exit);
MODULE_DESCRIPTION("Ingenic T20 3.10 platform interfaces for ported camera drivers");
MODULE_LICENSE("GPL");
