/* kxo: A Tic-Tac-Toe Game Engine implemented as Linux kernel module */

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#if defined(CONFIG_X86)
#include <linux/vmalloc.h>
#endif

#include "game.h"
#include "mcts.h"
#include "negamax.h"
#include "xo_common.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("In-kernel Tic-Tac-Toe game engine");

/* Macro DECLARE_TASKLET_OLD exists for compatibiity.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "kxo"

#define NR_KMLDRV 1

static int delay = 100; /* time (in ms) to generate an event */
static struct xo_board last_board;
static struct xo_result last_result;

/* Declare kernel module attribute for sysfs */

struct kxo_attr {
    char display;
    char resume;
    char end;
    rwlock_t lock;
};

static struct kxo_attr attr_obj;

static ssize_t kxo_state_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    read_lock(&attr_obj.lock);
    int ret = snprintf(buf, 6, "%c %c %c\n", attr_obj.display, attr_obj.resume,
                       attr_obj.end);
    read_unlock(&attr_obj.lock);
    return ret;
}

static ssize_t kxo_state_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf,
                               size_t count)
{
    write_lock(&attr_obj.lock);
    sscanf(buf, "%c %c %c", &(attr_obj.display), &(attr_obj.resume),
           &(attr_obj.end));
    write_unlock(&attr_obj.lock);
    return count;
}

static DEVICE_ATTR_RW(kxo_state);

/* Data produced by the simulated device */

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;

/* Character device stuff */
static int major;
static struct class *kxo_class;
static struct cdev kxo_cdev;

/* Data are stored into a kfifo buffer before passing them to the userspace */
static DECLARE_KFIFO_PTR(rx_fifo, unsigned char);

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);

static DEFINE_MUTEX(kxo_lock);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

/* Mutex to serialize kfifo writers within the workqueue handler */
static DEFINE_MUTEX(producer_lock);

/* Mutex to serialize fast_buf consumers: we can use a mutex because consumers
 * run in workqueue handler (kernel thread context).
 */
static DEFINE_MUTEX(consumer_lock);

/* We use an additional "faster" circular buffer to quickly store data from
 * interrupt context, before adding them to the kfifo.
 */
static struct circ_buf fast_buf;

static char table[N_GRIDS];

/* Clear all data from the circular buffer fast_buf */
static void fast_buf_clear(void)
{
    fast_buf.head = fast_buf.tail = 0;
}

static char turn;
static int finish;

static void ai_one_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();
    mutex_lock(&producer_lock);
    int move;
    WRITE_ONCE(move, mcts(table, 'O'));

    smp_mb();

    if (move != -1)
        WRITE_ONCE(table[move], 'O');

    WRITE_ONCE(turn, 'X');
    WRITE_ONCE(finish, 1);
    smp_wmb();
    mutex_unlock(&producer_lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kxo: [CPU#%d] doing %s for %llu usec\n", cpu, __func__,
            (unsigned long long) nsecs >> 10);
    put_cpu();
}

static void ai_two_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();
    mutex_lock(&producer_lock);
    int move;
    WRITE_ONCE(move, negamax_predict(table, 'X').move);

    smp_mb();

    if (move != -1)
        WRITE_ONCE(table[move], 'X');

    WRITE_ONCE(turn, 'O');
    WRITE_ONCE(finish, 1);
    smp_wmb();
    mutex_unlock(&producer_lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kxo: [CPU#%d] end doing %s for %llu usec\n", cpu, __func__,
            (unsigned long long) nsecs >> 10);
    put_cpu();
}

/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *kxo_workqueue;

/* Work item: holds a pointer to the function that is going to be executed
 * asynchronously.
 */
static DECLARE_WORK(ai_one_work, ai_one_work_func);
static DECLARE_WORK(ai_two_work, ai_two_work_func);

static ssize_t kxo_write(struct file *file,
                         const char __user *buf,
                         size_t len,
                         loff_t *off)
{
    if (mutex_lock_interruptible(&kxo_lock))
        return -ERESTARTSYS;
    printk(KERN_INFO "kxo_write: len=%zu expect=%zu\n", len,
           sizeof(struct xo_board));
    if (len != sizeof(struct xo_board))
        return -EINVAL;
    if (copy_from_user(&last_board, buf, sizeof(struct xo_board)))
        return -EFAULT;

    last_result.move = mcts(last_board.table, last_board.player);

    mutex_unlock(&kxo_lock);
    return sizeof(struct xo_board);
}

static ssize_t kxo_read(struct file *file,
                        char __user *buf,
                        size_t len,
                        loff_t *off)
{
    if (mutex_lock_interruptible(&kxo_lock))
        return -ERESTARTSYS;
    if (len != sizeof(struct xo_result))
        return -EINVAL;
    if (copy_to_user(buf, &last_result, sizeof(struct xo_result)))
        return -EFAULT;
    mutex_unlock(&kxo_lock);
    return sizeof(struct xo_result);
}

static atomic_t open_cnt;

static int kxo_open(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    pr_info("openm current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static int kxo_release(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt)) {
        del_timer_sync(&timer);
        if (kxo_workqueue) {
            flush_workqueue(kxo_workqueue);
        }
        fast_buf_clear();
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static const struct file_operations kxo_fops = {
    .read = kxo_read,
    .write = kxo_write,
    .llseek = no_llseek,
    .open = kxo_open,
    .release = kxo_release,
    .owner = THIS_MODULE,
};

static int __init kxo_init(void)
{
    dev_t dev_id;
    int ret;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, NR_KMLDRV, DEV_NAME);
    if (ret)
        return ret;
    major = MAJOR(dev_id);

    /* Add the character device to the system */
    cdev_init(&kxo_cdev, &kxo_fops);
    ret = cdev_add(&kxo_cdev, dev_id, NR_KMLDRV);
    if (ret) {
        kobject_put(&kxo_cdev.kobj);
        unregister_chrdev_region(dev_id, NR_KMLDRV);
        return ret;
    }

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    kxo_class = class_create(THIS_MODULE, DEV_NAME);
#else
    kxo_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(kxo_class)) {
        ret = PTR_ERR(kxo_class);
        cdev_del(&kxo_cdev);
        unregister_chrdev_region(dev_id, NR_KMLDRV);
        return ret;
    }

    /* Register the device with sysfs */
    struct device *kxo_dev =
        device_create(kxo_class, NULL, MKDEV(major, 0), NULL, DEV_NAME);

    ret = device_create_file(kxo_dev, &dev_attr_kxo_state);
    if (ret < 0) {
        device_destroy(kxo_class, dev_id);
        class_destroy(kxo_class);
        cdev_del(&kxo_cdev);
        unregister_chrdev_region(dev_id, NR_KMLDRV);
        return ret;
    }

    negamax_init();
    mcts_init();
    memset(table, ' ', N_GRIDS);

    attr_obj.display = '1';
    attr_obj.resume = '1';
    attr_obj.end = '0';
    rwlock_init(&attr_obj.lock);

    atomic_set(&open_cnt, 0);

    pr_info("kxo: registered new kxo device: %d,%d\n", major, 0);
    return 0;
}

static void __exit kxo_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);

    del_timer_sync(&timer);
    if (kxo_workqueue) {
        flush_workqueue(kxo_workqueue);
    }
    destroy_workqueue(kxo_workqueue);
    vfree(fast_buf.buf);
    device_destroy(kxo_class, dev_id);
    class_destroy(kxo_class);
    cdev_del(&kxo_cdev);
    unregister_chrdev_region(dev_id, NR_KMLDRV);

    kfifo_free(&rx_fifo);
    pr_info("kxo: unloaded\n");
}

module_init(kxo_init);
module_exit(kxo_exit);