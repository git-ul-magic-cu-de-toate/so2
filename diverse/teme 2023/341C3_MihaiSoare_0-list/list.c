// SPDX-License-Identifier: GPL-2.0+

/*
 * list.c - Linux kernel list API
 *
 * Author: Mihai Soare <mihai_daniel.soare@stud.acs.upb.ro>
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>

#define PROCFS_MAX_SIZE		512

#define procfs_dir_name		"list"
#define procfs_file_read	"preview"
#define procfs_file_write	"management"

#define CHOICE_LEN 4
#define ADD_FIRST 0
#define ADD_LAST 1
#define DELETE_FIRST 0
#define DELETE_ALL 1

struct proc_dir_entry *proc_list;
struct proc_dir_entry *proc_list_read;
struct proc_dir_entry *proc_list_write;

struct string_info {
	char *string;
	struct list_head list;
};

static struct list_head head;

DEFINE_RWLOCK(lock);

static int list_proc_show(struct seq_file *m, void *v)
{
	struct list_head *p, *q;
	struct string_info *string_inst;

	read_lock(&lock);
	list_for_each_safe(p, q, &head) {
		string_inst = list_entry(p, struct string_info, list);
		seq_puts(m, string_inst->string);
		seq_puts(m, "\n");
	}
	read_unlock(&lock);

	return 0;
}

static int list_read_open(struct inode *inode, struct  file *file)
{
	return single_open(file, list_proc_show, NULL);
}

static int list_write_open(struct inode *inode, struct  file *file)
{
	return single_open(file, list_proc_show, NULL);
}

static struct string_info *create_new_string_elth(char *argument)
{
	struct string_info *new_string;

	new_string = kmalloc(sizeof(*new_string), GFP_KERNEL);
	if (!new_string)
		return NULL;

	new_string->string = argument;

	return new_string;
}

static int add_string_in_list(char local_buffer[PROCFS_MAX_SIZE], unsigned long local_buffer_size, int type)
{
	char *argument = NULL;
	char *space, *newline;
	struct string_info *string;

	// get to the space delimiter in the string
	space = local_buffer + CHOICE_LEN + 1;
	if (!space)
		return -EINVAL;

	// delete any additional '\n'
	newline = strchr(space, '\n');
	if (newline)
		*newline = '\0';

	argument = kstrdup(space, GFP_KERNEL);
	if (!argument)
		return -ENOMEM;

	string = create_new_string_elth(argument);

	if (!string) {
		kfree(argument);
		return -ENOMEM;
	}

	// protect the list for adding a new string
	write_lock(&lock);

	// add first or last element in list
	if (type == ADD_FIRST)
		list_add(&string->list, &head);
	else if (type == ADD_LAST)
		list_add_tail(&string->list, &head);

	write_unlock(&lock);

	return local_buffer_size;
}

static int delete_string_from_list(char local_buffer[PROCFS_MAX_SIZE], unsigned long local_buffer_size, int type)
{
	char *argument = NULL;
	char *space, *newline;
	struct list_head *p, *q;
	struct string_info *string_inst;

	// get to the space delimiter in the string
	space = local_buffer + CHOICE_LEN + 1;
	if (!space)
		return -EINVAL;

	// delete any additional '\n'
	newline = strchr(space, '\n');
	if (newline)
		*newline = '\0';

	argument = kstrdup(space, GFP_KERNEL);
	if (!argument)
		return -ENOMEM;

	// protect the list for deleting string instances
	write_lock(&lock);

	// iterate through the list and delete first or all occurences of the argument
	list_for_each_safe(p, q, &head) {
		string_inst = list_entry(p, struct string_info, list);
		if (!strcmp(string_inst->string, argument) && type == DELETE_FIRST) {
			list_del(p);
			kfree(string_inst->string);
			kfree(string_inst);
			write_unlock(&lock);
			kfree(argument);
			return local_buffer_size;
		} else if (!strcmp(string_inst->string, argument) && type == DELETE_ALL) {
			list_del(p);
			kfree(string_inst->string);
			kfree(string_inst);
		}
	}

	write_unlock(&lock);

	kfree(argument);
	return local_buffer_size;
}


static ssize_t list_write(struct file *file, const char __user *buffer,
			  size_t count, loff_t *offs)
{
	char local_buffer[PROCFS_MAX_SIZE];
	unsigned long local_buffer_size = 0;

	local_buffer_size = count;
	if (local_buffer_size > PROCFS_MAX_SIZE)
		local_buffer_size = PROCFS_MAX_SIZE;

	memset(local_buffer, 0, PROCFS_MAX_SIZE);
	if (copy_from_user(local_buffer, buffer, local_buffer_size))
		return -EFAULT;

	if (strncmp(local_buffer, "addf", CHOICE_LEN) != 0 &&
		strncmp(local_buffer, "adde", CHOICE_LEN) != 0 &&
		strncmp(local_buffer, "dela", CHOICE_LEN) != 0 &&
		strncmp(local_buffer, "delf", CHOICE_LEN) != 0)
		return -EINVAL;

	if (strncmp(local_buffer, "addf", CHOICE_LEN) == 0)
		return add_string_in_list(local_buffer, local_buffer_size, ADD_FIRST);
	else if (strncmp(local_buffer, "adde", CHOICE_LEN) == 0)
		return add_string_in_list(local_buffer, local_buffer_size, ADD_LAST);
	else if (strncmp(local_buffer, "delf", CHOICE_LEN) == 0)
		return delete_string_from_list(local_buffer, local_buffer_size, DELETE_FIRST);
	else if (strncmp(local_buffer, "dela", CHOICE_LEN) == 0)
		return delete_string_from_list(local_buffer, local_buffer_size, DELETE_ALL);

	return local_buffer_size;
}

static const struct proc_ops r_pops = {
	.proc_open		= list_read_open,
	.proc_read		= seq_read,
	.proc_release	= single_release,
};

static const struct proc_ops w_pops = {
	.proc_open		= list_write_open,
	.proc_write		= list_write,
	.proc_release	= single_release,
};

static int list_init(void)
{
	proc_list = proc_mkdir(procfs_dir_name, NULL);
	if (!proc_list)
		return -ENOMEM;

	proc_list_read = proc_create(procfs_file_read, 0000, proc_list,
					 &r_pops);
	if (!proc_list_read)
		goto proc_list_cleanup;

	proc_list_write = proc_create(procfs_file_write, 0000, proc_list,
					  &w_pops);
	if (!proc_list_write)
		goto proc_list_read_cleanup;

	// init list after creating both procfs files and dir
	INIT_LIST_HEAD(&head);

	return 0;

proc_list_read_cleanup:
	proc_remove(proc_list_read);
proc_list_cleanup:
	proc_remove(proc_list);
	return -ENOMEM;
}

static void list_exit(void)
{
	struct list_head *p, *q;
	struct string_info *string_inst;

	// clean list
	write_lock(&lock);
	list_for_each_safe(p, q, &head) {
		string_inst = list_entry(p, struct string_info, list);
		list_del(p);
		kfree(string_inst->string);
		kfree(string_inst);
	}

	write_unlock(&lock);

	proc_remove(proc_list);
}

module_init(list_init);
module_exit(list_exit);

MODULE_DESCRIPTION("Linux kernel list API");
MODULE_AUTHOR("Mihai Soare <mihai_daniel.soare@stud.acs.upb.ro>");
MODULE_LICENSE("GPL v2");
