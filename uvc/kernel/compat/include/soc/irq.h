/* Ingenic 3.10 interrupt numbers. With a device tree, Linux numbers are
 * assigned at run time: t20_intc_irq() maps an INTC line (vendor soc/irq.h
 * order) to one. */
#ifndef __T20_COMPAT_IRQ_H__
#define __T20_COMPAT_IRQ_H__
int t20_intc_irq(unsigned int hwirq);
#define IRQ_SFC		t20_intc_irq(7)
#define IRQ_PDMA	t20_intc_irq(10)
#define IRQ_AIC0	t20_intc_irq(1)
#define IRQ_DMIC	t20_intc_irq(0)
#define IRQ_ISP		t20_intc_irq(29)
#define IRQ_VPU0	t20_intc_irq(62)
#define IRQ_VPU(ID)	t20_intc_irq(62 + (ID))
#define IRQ_IPU		t20_intc_irq(6)
#define IRQ_CSI		t20_intc_irq(4)
#endif
