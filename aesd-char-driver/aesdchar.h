/*
 * aesdchar.h
 *
 *  Created on: Oct 23, 2019
 *      Author: Dan Walkes
 */

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#include <linux/cdev.h>
#include <linux/mutex.h>
#include "aesd-circular-buffer.h"

#define AESD_DEBUG 1  //Remove comment on this line to enable debug

#undef PDEBUG             /* undef it, just in case */
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_DEBUG "aesdchar: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

struct aesd_dev
{
    /**
     * Circular buffer holding pointers to the most recent
     * AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED completed write commands.
     */
    struct aesd_circular_buffer buffer;
    /**
     * Serializes all access to buffer, pending_buf and pending_size so a
     * full write (or read) completes before another is accepted.
     */
    struct mutex lock;
    /**
     * Accumulates bytes for a write command that hasn't yet been terminated
     * by a '\n'. NULL/0 when there is no partial command in progress.
     */
    char *pending_buf;
    size_t pending_size;
    struct cdev cdev;     /* Char device structure      */
};


#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
