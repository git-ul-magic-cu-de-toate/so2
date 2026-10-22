// SPDX-License-Identifier: GPL-2.0+

/*
 * uart16550.c - Driver UART16550 using FIFO
 *
 * Author: Mihai Soare <mihai.orange7@gmail.com>
 */
#include "uart16550.h"
#include <asm/io.h>
#include <linux/uaccess.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>

#define MODULE_NAME             "uart16550"

#define DEFAULT_MAJOR           42
#define COM1_MINOR              0
#define COM2_MINOR              1

#define COM1_BASE_PORT          0x3F8
#define COM2_BASE_PORT          0x2F8

#define NR_PORTS                8

#define IRQ_COM1                4
#define IRQ_COM2                3

#define FREED                   1
#define NOT_FREED               0

#define OPENED                  1
#define NOT_OPENED              0

#define BUFF_SIZE               4096

#define RHR                     0
#define THR                     0
#define IER                     1
#define ISR                     2
#define FCR                     2

#define LCR                     3
#define MCR                     4
#define LSR                     5

#define DLL                     0
#define DLM                     1

#define IER_DATA_READY          0
#define IER_THR_EMPTY           1

#define ISR_DATA_READY          4
#define ISR_THR_EMPTY           2

#define FCR_FIFO_EN             0
#define FCR_RXFIFO_R            1
#define FCR_TXFIFO_R            2
#define FCR_TRIGGER_BIT1        6
#define FCR_TRIGGER_BIT2        7

#define LCR_DLAB                7

#define MCR_GLOBAL_INTERRUPT    3

#define LSR_DATA_READY          0
#define LSR_THR_EMPTY           5

struct uart16550_dev {
	int base_port;
	int is_open;

	wait_queue_head_t read_waitq;
	wait_queue_head_t write_waitq;

	DECLARE_KFIFO(write_buffer, unsigned char, BUFF_SIZE);
	DECLARE_KFIFO(read_buffer, unsigned char, BUFF_SIZE);

	struct cdev cdev;
} devs[MAX_NUMBER_DEVICES];

static int major = DEFAULT_MAJOR;
static int option = OPTION_BOTH;
static unsigned int coms_freed[2];

module_param(major, int, 0);
module_param(option, int, 0);

static void uart16550_set_bit(int base, unsigned char port, unsigned char bit)
{
	unsigned char value;

	value = inb(base + port);
	value |= (1 << bit);
	outb(value, base + port);
}

static void uart16550_clear_bit(int base, unsigned char port, unsigned char bit)
{
	unsigned char value;

	value = inb(base + port);
	value &= ~(1 << bit);
	outb(value, base + port);
}

static unsigned char uart16550_get_bit(int base, unsigned char port, unsigned char bit)
{
	unsigned char value;

	value = inb(base + port);
	return (value & (1 << bit)) >> bit;
}

static int uart16550_open(struct inode *inode, struct file *file)
{
	int minor;

	minor = iminor(inode);
	file->private_data = (void *) &devs[minor];

	if (devs[minor].is_open == OPENED)
		return -EEXIST;

	devs[minor].is_open = OPENED;
	return 0;
}

static int uart16550_release(struct inode *inode, struct file *file)
{
	int minor;

	minor = iminor(inode);
	devs[minor].is_open = NOT_OPENED;

	return 0;
}

static ssize_t uart16550_read(struct file *file,  char __user *user_buffer,
			size_t size, loff_t *offset)
{
	struct uart16550_dev *uart_dev;
	int ret, copied;

	uart_dev = (struct uart16550_dev *) file->private_data;

	ret = wait_event_interruptible(uart_dev->read_waitq, !kfifo_is_empty(&uart_dev->read_buffer));
	if (ret < 0)
		return ret;

	ret = kfifo_to_user(&uart_dev->read_buffer, user_buffer, size, &copied);
	if (ret) {
		pr_err("Failed to kfifo_to_user in %s: %d\n", __func__, ret);
		return ret;
	}

	wake_up_interruptible(&uart_dev->read_waitq);

	// enable data ready interrupts, can read again
	uart16550_set_bit(uart_dev->base_port, IER, IER_DATA_READY);

	return copied;
}

static ssize_t uart16550_write(struct file *file, const char __user *user_buffer,
			size_t size, loff_t *offset)
{
	struct uart16550_dev *uart_dev;
	int ret, copied;

	uart_dev = (struct uart16550_dev *) file->private_data;

	ret = wait_event_interruptible(uart_dev->write_waitq, !kfifo_is_full(&uart_dev->write_buffer));
	if (ret < 0)
		return ret;

	ret = kfifo_from_user(&uart_dev->write_buffer, user_buffer, size, &copied);
	if (ret) {
		pr_err("Failed to kfifo_from_user in %s: %d\n", __func__, ret);
		return ret;
	}

	wake_up_interruptible(&uart_dev->write_waitq);

	// enable thr empty interrupts - can write again
	uart16550_set_bit(uart_dev->base_port, IER, IER_THR_EMPTY);

	return copied;
}

static int check_line_info(struct uart16550_line_info line_info)
{
	if (line_info.baud != UART16550_BAUD_1200 && line_info.baud != UART16550_BAUD_2400 &&
		line_info.baud != UART16550_BAUD_9600 && line_info.baud != UART16550_BAUD_19200 &&
		line_info.baud != UART16550_BAUD_38400 && line_info.baud != UART16550_BAUD_56000 &&
		line_info.baud != UART16550_BAUD_4800 && line_info.baud != UART16550_BAUD_115200) {
		pr_err("Failed cause invalid baud rate: %d\n", line_info.baud);
		return -1;
	}

	if (line_info.len != UART16550_LEN_5 && line_info.len != UART16550_LEN_6 &&
		line_info.len != UART16550_LEN_7 && line_info.len != UART16550_LEN_8) {
		pr_err("Failed cause invalid data length: %d\n", line_info.len);
		return -1;
	}

	if (line_info.par != UART16550_PAR_NONE && line_info.par != UART16550_PAR_ODD &&
		line_info.par != UART16550_PAR_EVEN && line_info.par != UART16550_PAR_STICK) {
		pr_err("Failed cause invalid parity: %d\n", line_info.par);
		return -1;
	}

	if (line_info.stop != UART16550_STOP_2 && line_info.stop != UART16550_STOP_1) {
		pr_err("Failed cause invalid stop rate: %d\n", line_info.stop);
		return -1;
	}

	return 0;
}

static void set_uart16550_settings(struct uart16550_dev *uart_dev,
							struct uart16550_line_info line_info)
{
	// set dlab
	uart16550_set_bit(uart_dev->base_port, LCR, LCR_DLAB);

	// set baud
	outb(0, uart_dev->base_port + DLM);
	outb(line_info.baud, uart_dev->base_port + DLL);

	// clear dlab
	uart16550_clear_bit(uart_dev->base_port, LCR, LCR_DLAB);

	// set line par | len | stop
	outb(line_info.len | line_info.par | line_info.stop, uart_dev->base_port + LCR);

	// enable data ready interrupt
	uart16550_set_bit(uart_dev->base_port, IER, IER_DATA_READY);
}

static long uart16550_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct uart16550_line_info line_info;
	struct uart16550_dev *uart_dev;
	int err;

	err = 0;

	if (cmd != UART16550_IOCTL_SET_LINE)
		return -EINVAL;

	err = copy_from_user(&line_info, (struct uart16550_line_info *) arg, sizeof(line_info));
	if (err)
		return -EFAULT;

	err = check_line_info(line_info);
	if (err)
		return err;

	uart_dev = (struct uart16550_dev *) file->private_data;

	// set uart dev settings like baud, stop, len, par
	set_uart16550_settings(uart_dev, line_info);

	return 0;
}

static void uart16550_interrupt_read(struct uart16550_dev *uart_dev, int isr)
{
	unsigned char c;

	if ((isr & ISR_DATA_READY) == 0)
		return;

	// read while lsr_data_ready is set
	while (uart16550_get_bit(uart_dev->base_port, LSR, LSR_DATA_READY)) {
		c = inb(uart_dev->base_port + RHR);

		if (kfifo_put(&uart_dev->read_buffer, c) != 0) {
			uart16550_clear_bit(uart_dev->base_port, IER, IER_DATA_READY);
			break;
		}
	}

	wake_up_interruptible(&uart_dev->read_waitq);
}

static void uart16550_interrupt_write(struct uart16550_dev *uart_dev, int isr)
{
	unsigned char c;

	// check if THR_EMPTY interrupt is set
	if ((isr & ISR_THR_EMPTY) == 0)
		return;

	// write while THR_EMPTY LSR is set
	while (uart16550_get_bit(uart_dev->base_port, LSR, LSR_THR_EMPTY)) {
		if (kfifo_get(&uart_dev->write_buffer, &c) == 0) {
			uart16550_clear_bit(uart_dev->base_port, IER, IER_THR_EMPTY);
			break;
		}

		outb(c, uart_dev->base_port + THR);
	}

	wake_up_interruptible(&uart_dev->write_waitq);
}

irqreturn_t uart16550_interrupt_handler(int irq_no, void *dev_id)
{
	struct uart16550_dev *uart_dev;
	int isr;

	uart_dev = (struct uart16550_dev *) dev_id;
	isr = inb(uart_dev->base_port + ISR);

	uart16550_interrupt_read(uart_dev, isr);

	uart16550_interrupt_write(uart_dev, isr);

	return IRQ_HANDLED;
}

const struct file_operations uart16550_fops = {
	.owner = THIS_MODULE,
	.open = uart16550_open,
	.read = uart16550_read,
	.write = uart16550_write,
	.release = uart16550_release,
	.unlocked_ioctl = uart16550_ioctl,
};

static int serial_device_init(int minor, int base_port, int irq_com)
{
	int err;

	err = register_chrdev_region(MKDEV(major, minor), 1, MODULE_NAME);
	if (err != 0) {
		pr_err("Failed COM%d register_chrdev_region: %d\n", minor + 1, err);
		coms_freed[minor] = FREED;
		return err;
	}

	if (request_region(base_port, NR_PORTS, MODULE_NAME) == NULL) {
		err = -ENODEV;
		pr_err("Failed COM%d request_region: %d\n", minor + 1, err);

		unregister_chrdev_region(MKDEV(major, minor), 1);
		coms_freed[minor] = FREED;
		return err;
	}

	err = request_irq(irq_com, uart16550_interrupt_handler, IRQF_SHARED, MODULE_NAME, &devs[minor]);
	if (err != 0) {
		pr_err("Failed COM%d request_irq: %d\n", minor + 1, err);
		release_region(base_port, NR_PORTS);
		unregister_chrdev_region(MKDEV(major, minor), 1);
		coms_freed[minor] = FREED;
		return err;
	}

	cdev_init(&devs[minor].cdev, &uart16550_fops);
	err = cdev_add(&devs[minor].cdev, MKDEV(major, minor), 1);
	devs[minor].base_port = base_port;
	devs[minor].is_open = NOT_OPENED;

	INIT_KFIFO(devs[minor].read_buffer);
	INIT_KFIFO(devs[minor].write_buffer);

	init_waitqueue_head(&devs[minor].read_waitq);
	init_waitqueue_head(&devs[minor].write_waitq);

	// enable fifo, reset tx-rx
	outb(0, base_port + FCR);
	uart16550_set_bit(base_port, FCR, FCR_FIFO_EN);
	uart16550_set_bit(base_port, FCR, FCR_RXFIFO_R);
	uart16550_set_bit(base_port, FCR, FCR_TXFIFO_R);

	// set trigger level 14 (chars to be read from kfifo buffers)
	uart16550_set_bit(base_port, FCR, FCR_TRIGGER_BIT1);
	uart16550_set_bit(base_port, FCR, FCR_TRIGGER_BIT2);

	// enable global interrupt
	outb(0, base_port + MCR);
	uart16550_set_bit(base_port, MCR, MCR_GLOBAL_INTERRUPT);

	// enable data ready interrupt
	outb(0, base_port + IER);
	uart16550_set_bit(base_port, IER, IER_DATA_READY);

	return err;
}

static void serial_device_free(int minor, int base_port, int irq_com)
{
	if (coms_freed[minor] == FREED)
		return;

	cdev_del(&devs[minor].cdev);

	free_irq(irq_com, &devs[minor]);

	release_region(base_port, NR_PORTS);

	unregister_chrdev_region(MKDEV(major, minor), 1);
}

static int uart16550_init(void)
{
	int err;

	err = 0;
	coms_freed[COM1_MINOR] = NOT_FREED;
	coms_freed[COM2_MINOR] = NOT_FREED;

	if (option == OPTION_BOTH) {
		err = serial_device_init(COM1_MINOR, COM1_BASE_PORT, IRQ_COM1);

		if (err == 0)
			err = serial_device_init(COM2_MINOR, COM2_BASE_PORT, IRQ_COM2);

		// com2 failed to init, but com1 initiated correctly
		if (coms_freed[COM2_MINOR] == FREED && coms_freed[COM1_MINOR] == NOT_FREED)
			serial_device_free(COM1_MINOR, COM1_BASE_PORT, IRQ_COM1);

		return err;
	} else if (option == OPTION_COM1) {
		err = serial_device_init(COM1_MINOR, COM1_BASE_PORT, IRQ_COM1);
	} else if (option == OPTION_COM2) {
		err = serial_device_init(COM2_MINOR, COM2_BASE_PORT, IRQ_COM2);
	}

	return err;
}

static void uart16550_exit(void)
{
	if (option == OPTION_BOTH) {
		serial_device_free(COM1_MINOR, COM1_BASE_PORT, IRQ_COM1);
		serial_device_free(COM2_MINOR, COM2_BASE_PORT, IRQ_COM2);
	} else if (option == OPTION_COM1) {
		serial_device_free(COM1_MINOR, COM1_BASE_PORT, IRQ_COM1);
	} else if (option == OPTION_COM2) {
		serial_device_free(COM2_MINOR, COM2_BASE_PORT, IRQ_COM2);
	}
}

module_init(uart16550_init);
module_exit(uart16550_exit);

MODULE_DESCRIPTION("Linux Driver UART");
MODULE_AUTHOR("Mihai Soare <mihai.orange7@gmail.com>");
MODULE_LICENSE("GPL v2");
