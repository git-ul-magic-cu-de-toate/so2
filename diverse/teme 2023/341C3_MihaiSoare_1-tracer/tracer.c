// SPDX-License-Identifier: GPL-2.0+

/*
 * tracer.c - Linux Kernel Kprobe based tracer
 *
 * Author: Mihai Soare (mihai_daniel.soare@stud.acs.upb.ro)
 */
#include "tracer.h"
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/hashtable.h>

#define MAXACTIVE 64

struct kmem_data {
	size_t size;
};

struct kprobe_data {
	unsigned long kmalloc_count;
	unsigned long kfree_count;
	unsigned long kmalloc_mem;
	unsigned long kfree_mem;
	unsigned long sched_count;
	unsigned long up_count;
	unsigned long down_count;
	unsigned long lock_count;
	unsigned long unlock_count;
};

struct kprobe_proc_htable {
	pid_t pid;
	struct kprobe_data data;
	struct hlist_node node;
};

struct kmem_htable {
	unsigned long address;
	size_t size;
	struct hlist_node node;
};

DEFINE_HASHTABLE(kprobe_hash_table, 8);
DEFINE_HASHTABLE(kmem_hash_table, 8);

DEFINE_SPINLOCK(lock);

// [---------------------------------- HTABLES FUNCS ----------------------------------]

static int kprobe_proc_htable_add(pid_t pid)
{
	struct kprobe_proc_htable *proc_entry;

	proc_entry = kmalloc(sizeof(*proc_entry), GFP_ATOMIC);
	if (!proc_entry)
		return -ENOMEM;

	proc_entry->pid = pid;
	proc_entry->data.kmalloc_count = 0;
	proc_entry->data.kfree_count = 0;
	proc_entry->data.kmalloc_mem = 0;
	proc_entry->data.kfree_mem = 0;
	proc_entry->data.sched_count = 0;
	proc_entry->data.up_count = 0;
	proc_entry->data.down_count = 0;
	proc_entry->data.lock_count = 0;
	proc_entry->data.unlock_count = 0;

	spin_lock(&lock);
	hash_add(kprobe_hash_table, &proc_entry->node, proc_entry->pid);
	spin_unlock(&lock);

	return 0;
}

static int kprobe_proc_htable_remove(pid_t pid)
{
	struct kprobe_proc_htable *proc_entry;
	struct hlist_node *curr;
	int i;

	spin_lock(&lock);
	hash_for_each_safe(kprobe_hash_table, i, curr, proc_entry, node) {
		if (proc_entry->pid == pid) {
			hash_del(&proc_entry->node);
			kfree(&proc_entry);
			spin_unlock(&lock);
			return 0;
		}
	}
	spin_unlock(&lock);

	return 0;
}

static struct kprobe_proc_htable *kprobe_proc_htable_get(pid_t pid)
{
	struct kprobe_proc_htable *probe_it;

	hash_for_each_possible(kprobe_hash_table, probe_it, node, pid) {
		if (probe_it->pid == pid)
			return probe_it;
	}

	return NULL;
}

static void kprobe_proc_htable_clear(void)
{
	struct kprobe_proc_htable *proc_entry;
	struct hlist_node *curr;
	int i;

	spin_lock(&lock);
	hash_for_each_safe(kprobe_hash_table, i, curr, proc_entry, node) {
		hash_del(&proc_entry->node);
		kfree(&proc_entry);
	}
	spin_unlock(&lock);
}

static int kmem_htable_add(unsigned long address, size_t size)
{
	struct kmem_htable *kmem_entry;

	kmem_entry = kmalloc(sizeof(*kmem_entry), GFP_ATOMIC);
	if (!kmem_entry)
		return -ENOMEM;

	kmem_entry->address = address;
	kmem_entry->size = size;

	spin_lock(&lock);
	hash_add(kmem_hash_table, &kmem_entry->node, kmem_entry->address);
	spin_unlock(&lock);

	return 0;
}

NOKPROBE_SYMBOL(kmem_htable_add);

static int kmem_htable_remove(unsigned long address)
{
	struct kmem_htable *kmem_entry;
	struct hlist_node *curr;
	int i;

	spin_lock(&lock);
	hash_for_each_safe(kmem_hash_table, i, curr, kmem_entry, node) {
		if (kmem_entry->address == address) {
			hash_del(&kmem_entry->node);
			kfree(&kmem_entry);
			spin_unlock(&lock);
			return 0;
		}
	}
	spin_unlock(&lock);

	return 0;
}

NOKPROBE_SYMBOL(kmem_htable_remove);

static size_t kmem_htable_get(unsigned long address)
{
	struct kmem_htable *kmem_it;

	spin_lock(&lock);
	hash_for_each_possible(kmem_hash_table, kmem_it, node, address) {
		if (kmem_it->address == address) {
			spin_unlock(&lock);
			return kmem_it->size;
		}
	}
	spin_unlock(&lock);

	return -1;
}

static void kmem_htable_clear(void)
{
	struct kmem_htable *kmem_entry;
	struct hlist_node *curr;
	int i;

	spin_lock(&lock);
	hash_for_each_safe(kmem_hash_table, i, curr, kmem_entry, node) {
		hash_del(&kmem_entry->node);
		kfree(&kmem_entry);
	}
	spin_unlock(&lock);
}

NOKPROBE_SYMBOL(kmem_htable_clear);

// [---------------------------------- KPROBES FUNCS & STRUCTS ----------------------------------]

static int kmalloc_probe_entry_handler(struct kretprobe_instance *p, struct pt_regs *regs)
{
	struct kprobe_proc_htable *proc_entry;
	struct kmem_data *data;

	proc_entry = kprobe_proc_htable_get(current->pid);
	if (!proc_entry)
		return -1;

	data = (struct kmem_data *) p->data;
	data->size = regs->ax;

	return 0;
}

NOKPROBE_SYMBOL(kmalloc_probe_entry_handler);

static int kmalloc_probe_ret_handler(struct kretprobe_instance *p, struct pt_regs *regs)
{
	struct kprobe_proc_htable *proc_entry;
	struct kmem_data *data;
	unsigned long address;

	proc_entry = kprobe_proc_htable_get(current->pid);
	if (!proc_entry)
		return -1;

	data = (struct kmem_data *) p->data;
	address = regs_return_value(regs);

	kmem_htable_add(address, data->size);

	proc_entry->data.kmalloc_count += 1;
	proc_entry->data.kmalloc_mem += (unsigned long) data->size;

	return 0;
}

NOKPROBE_SYMBOL(kmalloc_probe_ret_handler);

static struct kretprobe kmalloc_probe = {
	.kp = {.symbol_name = "__kmalloc"},
	.entry_handler = kmalloc_probe_entry_handler,
	.handler = kmalloc_probe_ret_handler,
	.maxactive = MAXACTIVE
};

static int kfree_probe_handler(struct kretprobe_instance *p, struct pt_regs *regs)
{
	struct kprobe_proc_htable *proc_entry;
	unsigned long address;
	size_t size;

	proc_entry = kprobe_proc_htable_get(current->pid);
	if (!proc_entry)
		return -1;

	address = regs->ax;
	size = kmem_htable_get(address);

	kmem_htable_remove(address);

	proc_entry->data.kfree_count += 1;
	proc_entry->data.kfree_mem += size;

	return 0;
}

NOKPROBE_SYMBOL(kfree_probe_handler);

static struct kretprobe kfree_probe = {
	.kp = {.symbol_name = "kfree"},
	.entry_handler = kfree_probe_handler,
	.maxactive = MAXACTIVE
};

static int lock_probe_handler(struct kretprobe_instance *p, struct pt_regs *regs)
{
	struct kprobe_proc_htable *proc_entry;

	proc_entry = kprobe_proc_htable_get(current->pid);
	if (!proc_entry)
		return -1;

	proc_entry->data.lock_count += 1;

	return 0;
}

NOKPROBE_SYMBOL(lock_probe_handler);

static struct kretprobe lock_probe = {
	.kp = {.symbol_name = "mutex_lock_nested"},
	.entry_handler = lock_probe_handler,
	.maxactive = MAXACTIVE
};

static int unlock_probe_handler(struct kretprobe_instance *p, struct pt_regs *regs)
{
	struct kprobe_proc_htable *proc_entry;

	proc_entry = kprobe_proc_htable_get(current->pid);
	if (!proc_entry)
		return -1;

	proc_entry->data.unlock_count += 1;

	return 0;
}

NOKPROBE_SYMBOL(unlock_probe_handler);

static struct kretprobe unlock_probe = {
	.kp = {.symbol_name = "mutex_unlock"},
	.entry_handler = unlock_probe_handler,
	.maxactive = MAXACTIVE
};

static int sched_probe_handler(struct kretprobe_instance *p, struct pt_regs *regs)
{
	struct kprobe_proc_htable *proc_entry;

	proc_entry = kprobe_proc_htable_get(current->pid);
	if (!proc_entry)
		return -1;

	proc_entry->data.sched_count += 1;

	return 0;
}

NOKPROBE_SYMBOL(sched_probe_handler);

static struct kretprobe sched_probe = {
	.kp = {.symbol_name = "schedule"},
	.entry_handler = sched_probe_handler,
	.maxactive = MAXACTIVE
};

static int up_probe_handler(struct kretprobe_instance *p, struct pt_regs *regs)
{
	struct kprobe_proc_htable *proc_entry;

	proc_entry = kprobe_proc_htable_get(current->pid);
	if (!proc_entry)
		return -1;

	proc_entry->data.up_count += 1;

	return 0;
}

NOKPROBE_SYMBOL(up_probe_handler);

static struct kretprobe up_probe = {
	.kp = {.symbol_name = "up"},
	.entry_handler = up_probe_handler,
	.maxactive = MAXACTIVE
};

static int down_probe_handler(struct kretprobe_instance *p, struct pt_regs *regs)
{
	struct kprobe_proc_htable *proc_entry;

	proc_entry = kprobe_proc_htable_get(current->pid);
	if (!proc_entry)
		return -1;

	proc_entry->data.down_count += 1;

	return 0;
}

NOKPROBE_SYMBOL(down_probe_handler);

static struct kretprobe down_probe = {
	.kp = {.symbol_name = "down_interruptible"},
	.entry_handler = down_probe_handler,
	.maxactive = MAXACTIVE
};

// [---------------------------------- TRACER FUNCS ----------------------------------]

static int print_tracer(struct seq_file *m, void *v)
{
	struct kprobe_proc_htable *proc_entry;
	unsigned int bkt;

	seq_puts(m, "PID\tkmalloc\tkfree\tkmalloc_mem\tkfree_mem\tsched\tup\tdown\tlock\tunlock\n");

	spin_lock(&lock);
	hash_for_each(kprobe_hash_table, bkt, proc_entry, node) {
		seq_printf(m, "%d\t", (pid_t) proc_entry->pid);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.kmalloc_count);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.kfree_count);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.kmalloc_mem);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.kfree_mem);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.sched_count);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.up_count);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.down_count);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.lock_count);
		seq_printf(m, "%ld\t", (unsigned long) proc_entry->data.unlock_count);
	}
	spin_unlock(&lock);

	return 0;
}

static int tracer_read_open(struct inode *inode, struct  file *file)
{
	return single_open(file, print_tracer, NULL);
}

static long tracer_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	pid_t curr_pid;
	int ret;

	curr_pid = (pid_t) arg;
	ret = 0;

	switch (cmd) {
	case TRACER_ADD_PROCESS:
		ret = kprobe_proc_htable_add(curr_pid);
		break;
	case TRACER_REMOVE_PROCESS:
		ret = kprobe_proc_htable_remove(curr_pid);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct file_operations tracer_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = tracer_ioctl,
};

static const struct proc_ops r_pops = {
	.proc_open		= tracer_read_open,
	.proc_read		= seq_read,
	.proc_release	= single_release
};

static struct miscdevice tracer_device = {
	.minor = TRACER_DEV_MINOR,
	.name = TRACER_DEV_NAME,
	.fops = &tracer_fops
};

struct proc_dir_entry *proc_tracer_read;

static int register_tracer_kprobes(void)
{
	int ret;

	ret = 0;

	ret = register_kretprobe(&kmalloc_probe);
	if (ret < 0) {
		pr_err("register kmalloc kretprobe failed\n");
		return ret;
	}

	ret = register_kretprobe(&kfree_probe);
	if (ret < 0) {
		unregister_kretprobe(&kmalloc_probe);
		pr_err("register kfree kretprobe failed\n");
		return ret;
	}

	ret = register_kretprobe(&up_probe);
	if (ret < 0) {
		unregister_kretprobe(&kmalloc_probe);
		unregister_kretprobe(&kfree_probe);
		pr_err("register up kretprobe failed\n");
		return ret;
	}

	ret = register_kretprobe(&down_probe);
	if (ret < 0) {
		unregister_kretprobe(&kmalloc_probe);
		unregister_kretprobe(&kfree_probe);
		unregister_kretprobe(&up_probe);
		pr_err("register down kretprobe failed\n");
		return ret;
	}

	ret = register_kretprobe(&sched_probe);
	if (ret < 0) {
		unregister_kretprobe(&kmalloc_probe);
		unregister_kretprobe(&kfree_probe);
		unregister_kretprobe(&up_probe);
		unregister_kretprobe(&down_probe);
		pr_err("register sched kretprobe failed\n");
		return ret;
	}

	ret = register_kretprobe(&unlock_probe);
	if (ret < 0) {
		unregister_kretprobe(&kmalloc_probe);
		unregister_kretprobe(&kfree_probe);
		unregister_kretprobe(&up_probe);
		unregister_kretprobe(&down_probe);
		unregister_kretprobe(&sched_probe);
		pr_err("register unlock kretprobe failed\n");
		return ret;
	}

	ret = register_kretprobe(&lock_probe);
	if (ret < 0) {
		unregister_kretprobe(&kmalloc_probe);
		unregister_kretprobe(&kfree_probe);
		unregister_kretprobe(&up_probe);
		unregister_kretprobe(&down_probe);
		unregister_kretprobe(&sched_probe);
		unregister_kretprobe(&unlock_probe);
		pr_err("register lock kretprobe failed\n");
		return ret;
	}

	return ret;
}

static int tracer_init(void)
{

	int ret;

	ret = 0;

	proc_tracer_read = proc_create(TRACER_DEV_NAME, 0000, NULL, &r_pops);
	if (!proc_tracer_read)
		return -ENOMEM;

	ret = misc_register(&tracer_device);
	if (ret) {
		pr_err("failed to register tracer device\n");
		return ret;
	}

	register_tracer_kprobes();

	return ret;
}

static void tracer_exit(void)
{
	misc_deregister(&tracer_device);

	unregister_kretprobe(&kmalloc_probe);
	unregister_kretprobe(&kfree_probe);
	unregister_kretprobe(&up_probe);
	unregister_kretprobe(&down_probe);
	unregister_kretprobe(&sched_probe);
	unregister_kretprobe(&lock_probe);
	unregister_kretprobe(&unlock_probe);

	kprobe_proc_htable_clear();
	kmem_htable_clear();

	remove_proc_entry("tracer", NULL);
}

module_init(tracer_init);
module_exit(tracer_exit);

MODULE_DESCRIPTION("Linux Kernel Kprobe based tracer");
MODULE_AUTHOR("Mihai Soare (mihai_daniel.soare@stud.acs.upb.ro)");
MODULE_LICENSE("GPL v2");
