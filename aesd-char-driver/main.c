/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h> // kmalloc/kfree
#include <linux/uaccess.h> // copy_to_user/copy_from_user
#include <linux/string.h> // memchr/memcpy
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("seto2103");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;

    PDEBUG("open");
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t bytes_to_copy;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    if (!entry) {
        /* f_pos is beyond the data currently available: nothing to return */
        retval = 0;
        goto out;
    }

    bytes_to_copy = entry->size - entry_offset;
    if (bytes_to_copy > count)
        bytes_to_copy = count;

    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *kbuf;
    char *combined;
    size_t combined_size;
    char *nl;
    ssize_t retval = -ENOMEM;

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if (count == 0)
        return 0;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(kbuf);
        return -ERESTARTSYS;
    }

    /* Append the new bytes onto any command left incomplete by a prior write */
    combined_size = dev->pending_size + count;
    combined = kmalloc(combined_size, GFP_KERNEL);
    if (!combined) {
        mutex_unlock(&dev->lock);
        kfree(kbuf);
        return -ENOMEM;
    }
    if (dev->pending_size)
        memcpy(combined, dev->pending_buf, dev->pending_size);
    memcpy(combined + dev->pending_size, kbuf, count);
    kfree(kbuf);
    kfree(dev->pending_buf);
    dev->pending_buf = NULL;
    dev->pending_size = 0;

    /* Consume every \n-terminated command present in combined, adding each
     * to the circular buffer. Any bytes left after the last terminator
     * become the new pending (incomplete) command. */
    {
        char *pos = combined;
        size_t remaining = combined_size;

        while ((nl = memchr(pos, '\n', remaining)) != NULL) {
            size_t cmd_len = (nl - pos) + 1;
            char *entry_buf = kmalloc(cmd_len, GFP_KERNEL);

            if (entry_buf) {
                struct aesd_buffer_entry new_entry;

                memcpy(entry_buf, pos, cmd_len);

                if (dev->buffer.full)
                    kfree(dev->buffer.entry[dev->buffer.out_offs].buffptr);

                new_entry.buffptr = entry_buf;
                new_entry.size = cmd_len;
                aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);
            }

            pos += cmd_len;
            remaining -= cmd_len;
        }

        if (remaining > 0) {
            dev->pending_buf = kmalloc(remaining, GFP_KERNEL);
            if (dev->pending_buf) {
                memcpy(dev->pending_buf, pos, remaining);
                dev->pending_size = remaining;
            }
        }
        kfree(combined);
    }

    mutex_unlock(&dev->lock);
    retval = count;
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    uint8_t index;
    loff_t total_size = 0;
    loff_t newpos;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        total_size += entry->size;
    }

    newpos = fixed_size_llseek(filp, off, whence, total_size);

    mutex_unlock(&dev->lock);
    return newpos;
}

/* Translates a (write_cmd, write_cmd_offset) pair, relative to the commands
 * currently stored in the circular buffer, into an absolute byte offset.
 * Returns -EINVAL if either value is out of range. Caller must hold dev->lock. */
static long aesd_ioctl_seekto_locked(struct aesd_dev *dev, struct aesd_seekto *seekto, loff_t *newpos)
{
    uint8_t num_entries;
    uint8_t target_index;
    uint8_t i;
    loff_t pos = 0;

    if (dev->buffer.full)
        num_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    else
        num_entries = (dev->buffer.in_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - dev->buffer.out_offs)
                        % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    if (seekto->write_cmd >= num_entries)
        return -EINVAL;

    target_index = (dev->buffer.out_offs + seekto->write_cmd) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    if (seekto->write_cmd_offset >= dev->buffer.entry[target_index].size)
        return -EINVAL;

    for (i = 0; i < seekto->write_cmd; i++) {
        uint8_t idx = (dev->buffer.out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        pos += dev->buffer.entry[idx].size;
    }
    pos += seekto->write_cmd_offset;

    *newpos = pos;
    return 0;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    loff_t newpos;
    long retval;

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC || _IOC_NR(cmd) > AESDCHAR_IOC_MAXNR)
        return -ENOTTY;

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO:
        if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)))
            return -EFAULT;

        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;

        retval = aesd_ioctl_seekto_locked(dev, &seekto, &newpos);
        if (retval == 0)
            filp->f_pos = newpos;

        mutex_unlock(&dev->lock);
        return retval;

    default:
        return -ENOTTY;
    }
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        kfree(entry->buffptr);
    }
    kfree(aesd_device.pending_buf);
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
