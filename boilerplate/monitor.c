/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1
#define MONITOR_LOGQ_SIZE 256
#define MONITOR_LOG_MSG_LEN 192

/* Monitored process metadata stored in the kernel list. */
struct monitor_entry {
    pid_t pid;
    char container_id[64];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    bool soft_limit_exceeded;
    struct list_head list;
};
/* Global monitor registry protected by a mutex. */
static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

struct monitor_log_event {
    char msg[MONITOR_LOG_MSG_LEN];
};

struct monitor_log_queue {
    struct monitor_log_event entries[MONITOR_LOGQ_SIZE];
    int head;
    int tail;
    int count;
    bool shutdown;
    spinlock_t lock;
    wait_queue_head_t waitq;
};

static struct monitor_log_queue monitor_logq;
static struct task_struct *monitor_log_thread;

static void enqueue_monitor_log(const char *fmt, ...)
{
    unsigned long flags;
    va_list args;
    int idx;

    spin_lock_irqsave(&monitor_logq.lock, flags);

    if (monitor_logq.shutdown) {
        spin_unlock_irqrestore(&monitor_logq.lock, flags);
        return;
    }

    idx = monitor_logq.head;
    va_start(args, fmt);
    vscnprintf(monitor_logq.entries[idx].msg,
               sizeof(monitor_logq.entries[idx].msg),
               fmt,
               args);
    va_end(args);

    monitor_logq.head = (monitor_logq.head + 1) % MONITOR_LOGQ_SIZE;
    if (monitor_logq.count == MONITOR_LOGQ_SIZE) {
        monitor_logq.tail = (monitor_logq.tail + 1) % MONITOR_LOGQ_SIZE;
    } else {
        monitor_logq.count++;
    }

    spin_unlock_irqrestore(&monitor_logq.lock, flags);
    wake_up_interruptible(&monitor_logq.waitq);
}

static int monitor_log_consumer(void *data)
{
    struct monitor_log_event event;

    (void)data;

    while (true) {
        unsigned long flags;
        int has_entry = 0;
        int should_exit = 0;

        wait_event_interruptible(
            monitor_logq.waitq,
            kthread_should_stop() || monitor_logq.shutdown || monitor_logq.count > 0);

        spin_lock_irqsave(&monitor_logq.lock, flags);
        if (monitor_logq.count > 0) {
            event = monitor_logq.entries[monitor_logq.tail];
            monitor_logq.tail = (monitor_logq.tail + 1) % MONITOR_LOGQ_SIZE;
            monitor_logq.count--;
            has_entry = 1;
        } else if (monitor_logq.shutdown || kthread_should_stop()) {
            should_exit = 1;
        }
        spin_unlock_irqrestore(&monitor_logq.lock, flags);

        if (has_entry)
            printk(KERN_INFO "%s\n", event.msg);

        if (should_exit)
            break;
    }

    return 0;
}

/* --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    enqueue_monitor_log("[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu",
                        container_id,
                        pid,
                        rss_bytes,
                        limit_bytes);
}

/* --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    enqueue_monitor_log("[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu",
                        container_id,
                        pid,
                        rss_bytes,
                        limit_bytes);
}

/* --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *entry, *tmp;
    int entry_count = 0;

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        long rss = get_rss_bytes(entry->pid);
        entry_count++;

        /* Process exited */
        if (rss < 0) {
            enqueue_monitor_log("[container_monitor] Process exited container=%s pid=%d",
                                entry->container_id,
                                entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Hard limit check - must come before soft limit (use >= to trigger at exactly the limit) */
        if (rss >= entry->hard_limit_bytes) {
            printk(KERN_ERR "[container_monitor] HARD LIMIT TRIGGERED: container=%s pid=%d rss=%ld bytes >= hard_limit=%ld bytes - SENDING SIGKILL\n",
                   entry->container_id,
                   entry->pid,
                   rss,
                   entry->hard_limit_bytes);
            kill_process(entry->container_id,
                         entry->pid,
                         entry->hard_limit_bytes,
                         rss);

            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit check - only log once per threshold crossing (use >= to trigger at exactly the limit) */
        if (rss >= entry->soft_limit_bytes) {
            if (!entry->soft_limit_exceeded) {
                /* Direct print for immediate visibility */
                printk(KERN_WARNING "[container_monitor] SOFT LIMIT TRIGGERED: container=%s pid=%d rss=%ld bytes > threshold=%ld bytes\n",
                       entry->container_id,
                       entry->pid,
                       rss,
                       entry->soft_limit_bytes);
                log_soft_limit_event(entry->container_id,
                                     entry->pid,
                                     entry->soft_limit_bytes,
                                     rss);
                entry->soft_limit_exceeded = true;
            }
        } else {
            /* Reset flag if memory drops back below soft-limit */
            if (entry->soft_limit_exceeded) {
                enqueue_monitor_log("[container_monitor] Memory OK container=%s pid=%d rss=%ld limit=%lu",
                                    entry->container_id,
                                    entry->pid,
                                    rss,
                                    entry->soft_limit_bytes);
                entry->soft_limit_exceeded = false;
            }
        }
    }

    if (entry_count == 0) {
        /* Log periodic status when monitoring (helps validate timer is running with data) */
        static int heartbeat_count = 0;
        if (heartbeat_count++ % 10 == 0) {
            enqueue_monitor_log("[container_monitor] Timer heartbeat - no containers monitored");
        }
    }

    mutex_unlock(&monitor_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd == MONITOR_REGISTER) {
        struct monitor_entry *entry;

        if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
            return -EFAULT;

        /* Log with clear formatting for visibility in dmesg */
        printk(KERN_INFO "[container_monitor] REGISTER REQUEST: container=%s pid=%d soft=%lu bytes hard=%lu bytes\n",
               req.container_id,
               req.pid,
               req.soft_limit_bytes,
               req.hard_limit_bytes);
        enqueue_monitor_log("[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu",
                            req.container_id,
                            req.pid,
                            req.soft_limit_bytes,
                            req.hard_limit_bytes);

        /* Validate soft-limit <= hard-limit */
        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_ERR "[container_monitor] Registration FAILED: soft-limit (%lu) > hard-limit (%lu) for container=%s\n",
                   req.soft_limit_bytes,
                   req.hard_limit_bytes,
                   req.container_id);
            enqueue_monitor_log("[container_monitor] Registration FAILED: soft-limit > hard-limit container=%s",
                                req.container_id);
            return -EINVAL;
        }

        /* Allocate and initialize entry */
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
            printk(KERN_ERR "[container_monitor] Registration FAILED: kmalloc error for container=%s\n",
                   req.container_id);
            enqueue_monitor_log("[container_monitor] Registration FAILED: kmalloc error container=%s",
                                req.container_id);
            return -ENOMEM;
        }

        entry->pid = req.pid;
        strncpy(entry->container_id, req.container_id, sizeof(entry->container_id));
        entry->container_id[sizeof(entry->container_id) - 1] = '\0';
        entry->soft_limit_bytes = req.soft_limit_bytes;
        entry->hard_limit_bytes = req.hard_limit_bytes;
        entry->soft_limit_exceeded = false;

        /* Add to monitoring list */
        mutex_lock(&monitor_lock);
        list_add(&entry->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        printk(KERN_INFO "[container_monitor] Registration SUCCESS: container=%s pid=%d soft=%lu hard=%lu (now monitoring)\n",
               req.container_id,
               req.pid,
               req.soft_limit_bytes,
               req.hard_limit_bytes);
        enqueue_monitor_log("[container_monitor] Registration SUCCESS container=%s pid=%d",
                            req.container_id,
                            req.pid);

        return 0;
    }

    if (cmd == MONITOR_UNREGISTER) {
        struct monitor_entry *entry, *tmp;
        int found = 0;

        if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
            return -EFAULT;

        enqueue_monitor_log("[container_monitor] Unregister request container=%s pid=%d",
                            req.container_id,
                            req.pid);

        mutex_lock(&monitor_lock);

        list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
            if (entry->pid == req.pid &&
                strncmp(entry->container_id,
                        req.container_id,
                        sizeof(entry->container_id)) == 0) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                break;
            }
        }

        mutex_unlock(&monitor_lock);

        if (found)
            enqueue_monitor_log("[container_monitor] Unregister SUCCESS container=%s pid=%d",
                                req.container_id,
                                req.pid);

        return found ? 0 : -ENOENT;
    }

    if (cmd == MONITOR_LIST) {
        struct monitor_snapshot snapshot;
        struct monitor_entry *entry;

        memset(&snapshot, 0, sizeof(snapshot));

        mutex_lock(&monitor_lock);
        list_for_each_entry(entry, &monitor_list, list) {
            struct monitor_entry_info *out;

            if (snapshot.count >= MONITOR_MAX_SNAPSHOT)
                break;

            out = &snapshot.entries[snapshot.count];
            out->pid = entry->pid;
            out->soft_limit_bytes = entry->soft_limit_bytes;
            out->hard_limit_bytes = entry->hard_limit_bytes;
            out->soft_limit_exceeded = entry->soft_limit_exceeded ? 1 : 0;
            strncpy(out->container_id, entry->container_id, MONITOR_NAME_LEN - 1);
            out->container_id[MONITOR_NAME_LEN - 1] = '\0';
            snapshot.count++;
        }
        mutex_unlock(&monitor_lock);

        if (copy_to_user((struct monitor_snapshot __user *)arg, &snapshot, sizeof(snapshot)))
            return -EFAULT;

        return 0;
    }

    return -EINVAL;
}

/* --------------------------------------------------------------- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --------------------------------------------------------------- */
static int __init monitor_init(void)
{
    printk(KERN_INFO "[container_monitor] Initializing kernel module...\n");
    
    monitor_logq.head = 0;
    monitor_logq.tail = 0;
    monitor_logq.count = 0;
    monitor_logq.shutdown = false;
    spin_lock_init(&monitor_logq.lock);
    init_waitqueue_head(&monitor_logq.waitq);

    monitor_log_thread = kthread_run(monitor_log_consumer, NULL, "monitor_log_consumer");
    if (IS_ERR(monitor_log_thread)) {
        printk(KERN_ERR "[container_monitor] Failed to start log consumer thread\n");
        return PTR_ERR(monitor_log_thread);
    }

    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) {
        printk(KERN_ERR "[container_monitor] Failed to allocate character device region\n");
        goto err_stop_logger;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        printk(KERN_ERR "[container_monitor] Failed to create device class\n");
        unregister_chrdev_region(dev_num, 1);
        goto err_stop_logger;
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        printk(KERN_ERR "[container_monitor] Failed to create device /dev/%s\n", DEVICE_NAME);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        goto err_stop_logger;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        printk(KERN_ERR "[container_monitor] Failed to add character device\n");
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        goto err_stop_logger;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded successfully. Device: /dev/%s (major:%d minor:0)\n", 
           DEVICE_NAME, MAJOR(dev_num));
    enqueue_monitor_log("[container_monitor] Module loaded. Device: /dev/%s", DEVICE_NAME);
    return 0;

err_stop_logger:
    monitor_logq.shutdown = true;
    wake_up_interruptible(&monitor_logq.waitq);
    if (!IS_ERR_OR_NULL(monitor_log_thread))
        kthread_stop(monitor_log_thread);
    return -1;
}

/* --------------------------------------------------------------- */
static void __exit monitor_exit(void)
{
    struct monitor_entry *entry, *tmp;
    unsigned long flags;

    timer_shutdown_sync(&monitor_timer);

    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_lock);

    spin_lock_irqsave(&monitor_logq.lock, flags);
    monitor_logq.shutdown = true;
    spin_unlock_irqrestore(&monitor_logq.lock, flags);
    wake_up_interruptible(&monitor_logq.waitq);
    if (!IS_ERR_OR_NULL(monitor_log_thread))
        kthread_stop(monitor_log_thread);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
