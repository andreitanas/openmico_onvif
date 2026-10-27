/* Ingenic 3.10 GPIO function API (soc-t20/include/soc/gpio.h), implemented
 * by t20-compat.ko on top of the T20 GPIO registers. */
#ifndef __JZSOC_JZ_GPIO_H__
#define __JZSOC_JZ_GPIO_H__

enum gpio_function {
	GPIO_FUNC_0	= 0x00,
	GPIO_FUNC_1	= 0x01,
	GPIO_FUNC_2	= 0x02,
	GPIO_FUNC_3	= 0x03,
	GPIO_OUTPUT0	= 0x04,
	GPIO_OUTPUT1	= 0x05,
	GPIO_INPUT	= 0x06,
	GPIO_INT_LO	= 0x08,
	GPIO_INT_HI	= 0x09,
	GPIO_INT_FE	= 0x0a,
	GPIO_INT_RE	= 0x0b,
	GPIO_INPUT_PULL	= 0x16,
};

enum gpio_port {
	GPIO_PORT_A, GPIO_PORT_B,
	GPIO_PORT_C, GPIO_PORT_D,
	GPIO_PORT_E, GPIO_PORT_F,
	GPIO_NR_PORTS,
};

#define GPIO_PA(n)	(0 * 32 + (n))
#define GPIO_PB(n)	(1 * 32 + (n))
#define GPIO_PC(n)	(2 * 32 + (n))
#define GPIO_PD(n)	(3 * 32 + (n))

int jzgpio_set_func(enum gpio_port port, enum gpio_function func, unsigned long pins);
int jz_gpio_set_func(int gpio, enum gpio_function func);
int jzgpio_ctrl_pull(enum gpio_port port, int enable_pull, unsigned long pins);

#endif
