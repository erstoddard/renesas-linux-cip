/*
 * Driver for the Renesas RZ/V2L DRP-AI unit
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/cacheflush.h>
#include <asm/current.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/dma-mapping.h> /* ISP */
#include <linux/drpai.h>    /* Header file for DRP-AI Driver */
#include "drpai-core.h"     /* Header file for DRP-AI Core */
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/reset.h>

#define DRPAI_DRIVER_VERSION         "1.10"
#define DRPAI_DEV_NUM               (1)
#define DRPAI_DRIVER_NAME           "drpai"     /* Device name */
#define DRPAI_64BYTE_ALIGN          (0x3F)      /* Check 64-byte alignment */
#define DRPAI_STATUS_IDLE_RW        (10)
#define DRPAI_STATUS_ASSIGN         (11)
#define DRPAI_STATUS_DUMP_REG       (12)
#define DRPAI_STATUS_READ_MEM       (13)
#define DRPAI_STATUS_READ_REG       (14)
#define DRPAI_STATUS_WRITE          (15)
#define DRPAI_STATUS_ASSIGN_PARAM   (16)
#define DRPAI_STATUS_WRITE_PARAM    (17)

#define MAX_SEM_TIMEOUT (msecs_to_jiffies(1000))
#define HEAD_SENTINEL (UINT_MAX)

/* ISP */
#define DRPAI_SGL_DRP_DESC_SIZE     (80)
#define DRPAI_DESC_CMD_SIZE         (16)
#define DRPAI_CMA_SIZE              ((DRPAI_SGL_DRP_DESC_SIZE * 1) + DRPAI_DESC_CMD_SIZE + 64)
/* ISP */

#define DRP_PARAM_MAX_LINE_LENGTH (512)
#define DRP_PARAM_raddr           (0)
#define DRP_PARAM_waddr           (4)
#define DRP_PARAM_IMG_IWIDTH      (8)
#define DRP_PARAM_IMG_IHEIGHT     (10)
#define DRP_PARAM_IMG_OWIDTH      (16)
#define DRP_PARAM_IMG_OHEIGHT     (18)
#define DRP_PARAM_CROP_POS_X      (48)
#define DRP_PARAM_CROP_POS_Y      (50)
#define DRP_LIB_NAME_CROP         (",drp_lib:crop,")
#define DRP_PARAM_ATTR_OFFSET_ADD ("OFFSET_ADD:")
#define DRP_PARAM_ATTR_PROP_INPUT (",prop:input,")
#define DRP_PARAM_ATTR_PROP_OUTPUT (",prop:output,")

/* A function called from the kernel */
static int drpai_probe(struct platform_device *pdev);
static int drpai_remove(struct platform_device *pdev);
static int drpai_open(struct inode *inode, struct file *file);
static int drpai_close(struct inode *inode, struct file *file);
static int drpai_flush(struct file *file, fl_owner_t id);
static ssize_t  drpai_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static ssize_t  drpai_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static long drpai_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static unsigned int drpai_poll( struct file* filp, poll_table* wait );
static irqreturn_t irq_drp_errint(int irq, void *dev);
static irqreturn_t irq_mac_nmlint(int irq, void *dev);
static irqreturn_t irq_mac_errint(int irq, void *dev);

/* ISP */
static irqreturn_t irq_drp_nmlint(int irq, void *dev);

/* Function called from the kernel */
int drpai_open_k(void);
int drpai_close_k(void);
int drpai_start_k(drpai_data_t *arg, void (*isp_finish)(int result));
/* ISP */


/* Internal function */
static int drpai_regist_driver(void);
static int drpai_regist_device(struct platform_device *pdev);
static void drpai_unregist_driver(void);
static void drpai_unregist_device(void);
static void drpai_init_device(uint32_t ch);
static int8_t drpai_reset_device(uint32_t ch);
static long drpai_ioctl_assign(struct file *filp, unsigned int cmd, unsigned long arg);
static long drpai_ioctl_start(struct file *filp, unsigned int cmd, unsigned long arg);
static long drpai_ioctl_get_status(struct file *filp, unsigned int cmd, unsigned long arg);
static long drpai_ioctl_reset(struct file *filp, unsigned int cmd, unsigned long arg);
static long drpai_ioctl_reg_dump(struct file *filp, unsigned int cmd, unsigned long arg);
static long drpai_ioctl_assign_param(struct file *filp, unsigned int cmd, unsigned long arg);
static long drpai_ioctl_prepost_crop(struct file *filp, unsigned int cmd, unsigned long arg);
static long drpai_ioctl_prepost_inaddr(struct file *filp, unsigned int cmd, unsigned long arg);
static int8_t get_param_attr(char *line, char *attr, unsigned long *rvalue);
static int8_t drp_param_change16(uint32_t base, uint32_t offset, uint16_t value);
static int8_t drp_param_change32(uint32_t base, uint32_t offset, uint32_t value);
static int8_t drpai_flush_dcache_input_area(drpai_data_t *input);
/* ISP */
static int drpai_drp_config_init(void);
static void drpai_drp_config_uninit(void);
static int drpai_drp_cpg_init(void);
/* ISP */

/* Linux device driver initialization */
static const unsigned int MINOR_BASE = 0;
static const unsigned int MINOR_NUM  = DRPAI_DEV_NUM;       /* Minor number */
static unsigned int drpai_major;                    /* Major number (decided dinamically) */
static struct cdev drpai_cdev;                      /* Character device object */
static struct class *drpai_class = NULL;            /* class object */
struct device *drpai_device_array[DRPAI_DEV_NUM];

static DECLARE_WAIT_QUEUE_HEAD(drpai_waitq);

struct drpai_priv *drpai_priv;

struct drpai_rw_status {
    uint32_t rw_status;
    uint32_t read_count;
    uint32_t write_count;
    uint32_t drp_reg_offset_count;
    uint32_t aimac_reg_offset_count;
    drpai_data_t drpai_data;
    struct list_head list;
    drpai_assign_param_t assign_param;
    char *param_info;
    atomic_t inout_flag;
};

static DEFINE_SEMAPHORE(rw_sem);
static struct drpai_rw_status *drpai_rw_sentinel;

/* Virtual base address of register */
void __iomem *g_drp_base_addr[DRP_CH_NUM];
void __iomem *g_aimac_base_address[AIMAC_CH_NUM];
static resource_size_t drp_size;
static resource_size_t aimac_size;
static resource_size_t drpai_region_base_addr;
static resource_size_t drpai_region_size;
static resource_size_t drpai_linux_mem_start;
static resource_size_t drpai_linux_mem_size;

/* ISP */
static char* p_dmabuf_vaddr;
static dma_addr_t p_dmabuf_phyaddr;
static unsigned char drp_single_desc_bin[] =
{
  0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x80, 0x00, 0x01, 0x00, 0x91, 0x81, 0x50, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x07, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x80, 0x00, 0x01, 0x00, 0x91, 0x81, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

drpai_odif_intcnto_t odif_intcnto;
/* ISP */

/* handler table */
static struct file_operations s_mydevice_fops = {
    .open           = drpai_open,
    .release        = drpai_close,
    .write          = drpai_write,
    .read           = drpai_read,
    .unlocked_ioctl = drpai_ioctl,
    .compat_ioctl   = drpai_ioctl, /* for 32-bit App */
    .poll           = drpai_poll,
    .flush          = drpai_flush,
};
static const struct of_device_id drpai_match[] = {

    { .compatible = "renesas,rzv2l-drpai",},
    { /* sentinel */ }
};
static struct platform_driver drpai_platform_driver = {
    .driver = {
        .name   = "drpai-rz",
        .of_match_table = drpai_match,
    },
    .probe      = drpai_probe,
    .remove     = drpai_remove,
};

static int drpai_probe(struct platform_device *pdev)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    drpai_regist_driver();
    drpai_regist_device(pdev);

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return 0;
}

static int drpai_remove(struct platform_device *pdev)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    drpai_unregist_driver();
    drpai_unregist_device();

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return 0;
}

static int drpai_open(struct inode *inode, struct file *file)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int result = 0;
    struct drpai_priv *priv = drpai_priv;
    unsigned long flags;
    struct drpai_rw_status *drpai_rw_status = 0;

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) major %d minor %d\n", __func__, current->pid, imajor(inode), iminor(inode));

    /* Allocate drpai_rw_status to each file descriptor */
    drpai_rw_status = kzalloc(sizeof(struct drpai_rw_status), GFP_KERNEL);
    if (!drpai_rw_status) 
    {
        result = -ENOMEM;
        goto end;
    }
    /* Initialization flag */
    drpai_rw_status->rw_status  = DRPAI_STATUS_IDLE_RW;
    drpai_rw_status->param_info = NULL;
    INIT_LIST_HEAD(&drpai_rw_status->list);
    atomic_set(&drpai_rw_status->inout_flag, 0);
    DRPAI_DEBUG_PRINT("[%s](pid %d) Generated list %px rw_status %d prev %px next %px\n", __func__, current->pid, &drpai_rw_status->list, drpai_rw_status->rw_status, drpai_rw_status->list.prev, drpai_rw_status->list.next);
    file->private_data = drpai_rw_status;
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:   %d\n", __func__, current->pid, priv->drpai_status.status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT)))
    {
        result = -ETIMEDOUT;
        goto end;
    }

    if(likely((1 == refcount_read(&priv->count)) && (1 == atomic_long_read(&file->f_count))))
    {
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) Initialize DRP-AI\n", __func__, current->pid);

        /* CPG Reset */
        if(drpai_drp_cpg_init())
        {
            result = -EIO;
            goto end;
        }

        /* Initialize DRP-AI */
        drpai_init_device(0);

        /* Reset DRP-AI */
        if(R_DRPAI_SUCCESS != drpai_reset_device(0))
        {
            result = -EIO;
            goto end;
        }

        /* Initialize DRP-AI */
        drpai_init_device(0);

        /* INIT -> IDLE */
        spin_lock_irqsave(&priv->lock, flags);
        priv->drpai_status.status = DRPAI_STATUS_IDLE;
        spin_unlock_irqrestore(&priv->lock, flags);
    }
    /* Increment reference count */
    refcount_inc(&priv->count);

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:   %d\n", __func__, current->pid, priv->drpai_status.status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    result = 0;
    goto end;
end:
    if((-ENOMEM != result) || (-ETIMEDOUT != result))
    {
        up(&priv->sem);
    }
    if((0 != drpai_rw_status) && (0 != result))
    {
        kfree(file->private_data);
        file->private_data = NULL;
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return result;
}

static int drpai_close(struct inode *inode, struct file *file)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) major %d minor %d\n", __func__, current->pid, imajor(inode), iminor(inode));

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return 0;
}

static int drpai_flush(struct file *file, fl_owner_t id)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int result = 0;
    unsigned long flags;
    struct drpai_priv *priv = drpai_priv;
    struct drpai_rw_status *drpai_rw_status = file->private_data;
    struct drpai_rw_status *entry;
    struct list_head *listitr;

    if(unlikely(down_interruptible(&rw_sem))) 
    {
        result = -ERESTART;
        goto end;
    }

    DRPAI_DEBUG_PRINT("[%s](pid %d) HEAD  list %px rw_status %d prev %px next %px\n", __func__, current->pid, &drpai_rw_sentinel->list, drpai_rw_sentinel->rw_status, drpai_rw_sentinel->list.prev, drpai_rw_sentinel->list.next);
    if(!list_empty(&drpai_rw_sentinel->list))
    {
        if((DRPAI_STATUS_ASSIGN   == drpai_rw_status->rw_status) || 
           (DRPAI_STATUS_READ_MEM == drpai_rw_status->rw_status) ||
           (DRPAI_STATUS_WRITE    == drpai_rw_status->rw_status))
           {
                DRPAI_DEBUG_PRINT("[%s](pid %d) Deleted list %px rw_status %d prev %px next %px\n", __func__, current->pid, &drpai_rw_status->list, drpai_rw_status->rw_status, drpai_rw_status->list.prev, drpai_rw_status->list.next);
                list_del(&drpai_rw_status->list);
           }
    }
    up(&rw_sem);

    if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT))) 
    {
        result = -ETIMEDOUT;
        goto end;
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:   %d\n", __func__, current->pid, priv->drpai_status.status);

    if(1 == atomic_long_read(&file->f_count))
    {
        if(2 == refcount_read(&priv->count))
        {
            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start DRP-AI reset\n", __func__, current->pid);
            if(R_DRPAI_SUCCESS != drpai_reset_device(0))
            {
                result = -EIO;
                goto end;
            }

            //CPG clock disable
            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) CPG clock disable\n", __func__, current->pid);
            clk_disable_unprepare(priv->clk_int);
            clk_disable_unprepare(priv->clk_aclk_drp);
            clk_disable_unprepare(priv->clk_mclk);
            clk_disable_unprepare(priv->clk_dclkin);
            clk_disable_unprepare(priv->clk_aclk);  

            /* IDLE -> INIT */
            /* RUN  -> INIT */
            spin_lock_irqsave(&priv->lock, flags);
            priv->drpai_status.status = DRPAI_STATUS_INIT;
            priv->drpai_status.err    = DRPAI_ERRINFO_SUCCESS;
            spin_unlock_irqrestore(&priv->lock, flags);

            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) done DRP-AI reset\n", __func__, current->pid);
        }
        /* Decrement referenece count*/
        refcount_dec(&priv->count);
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:   %d\n", __func__, current->pid, priv->drpai_status.status);

    goto end;
end:
    if((-ERESTART != result) || (-ETIMEDOUT != result))
    {
        up(&priv->sem);
    }
    if(1 == atomic_long_read(&file->f_count))
    {
        /* Free memory */
        if(NULL != drpai_rw_status->param_info)
        {
            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) vfree is called\n", __func__, current->pid);
            vfree(drpai_rw_status->param_info);
            drpai_rw_status->param_info = NULL;
        }
        if(file->private_data) 
        {
            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) kfree is called\n", __func__, current->pid);
            kfree(file->private_data);
            file->private_data = NULL;
        }
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return result;
}

static ssize_t  drpai_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    ssize_t result = 0;
    uint8_t *kbuf = NULL;
    volatile void *p_drpai_cma = 0;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;

    if(unlikely(down_trylock(&rw_sem)))
    {
        result = -ERESTART;
        goto end;
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    /* Check status */
    if (!((DRPAI_STATUS_ASSIGN == drpai_rw_status->rw_status) || 
          (DRPAI_STATUS_WRITE == drpai_rw_status->rw_status) ||
          (DRPAI_STATUS_ASSIGN_PARAM == drpai_rw_status->rw_status) ||
          (DRPAI_STATUS_WRITE_PARAM == drpai_rw_status->rw_status)))
    {
        result = -EACCES;
        goto end;
    }

    /* Check Argument */
    if (NULL == buf)
    {
        result = -EFAULT;
        goto end;
    }
    if (0 == count)
    {
        result = -EINVAL;
        goto end;
    }

    switch(drpai_rw_status->rw_status)
    {
        case DRPAI_STATUS_ASSIGN:
            /* DRPAI_STATUS_ASSIGN -> DRPAI_STATUS_WRITE */
            drpai_rw_status->rw_status = DRPAI_STATUS_WRITE;
            break;
        case DRPAI_STATUS_ASSIGN_PARAM:
            /* DRPAI_STATUS_ASSIGN_PARAM -> DRPAI_STATUS_WRITE_PARAM */
            drpai_rw_status->rw_status = DRPAI_STATUS_WRITE_PARAM;
            break;
        default:
            ; /* Do nothing */
            break;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    switch(drpai_rw_status->rw_status)
    {
        case DRPAI_STATUS_WRITE:
            /* Secure Kbuf area */
            kbuf = vmalloc(count);
            if (NULL == kbuf)
            {
                result = -ENOMEM;
                goto end;
            }
            /* Copy arguments from user space to kernel space */
            if (copy_from_user(kbuf, buf, count))
            {
                result = -EFAULT;
                goto end;
            }

            /* Expand to DRP for CMA */
            p_drpai_cma = phys_to_virt(drpai_rw_status->drpai_data.address + drpai_rw_status->write_count);
            if (p_drpai_cma == 0)
            {
                result = -EFAULT;
                goto end;
            }
            if ( !( drpai_rw_status->drpai_data.size >= (drpai_rw_status->write_count + count) ) )
            {
                count = drpai_rw_status->drpai_data.size - drpai_rw_status->write_count;
            }
            memcpy(p_drpai_cma, kbuf, count);
            drpai_rw_status->write_count = drpai_rw_status->write_count + count;

            /* DRPAI_STATUS_WRITE -> DRPAI_STATUS_IDLE_RW */
            if (drpai_rw_status->drpai_data.size <= drpai_rw_status->write_count)
            {
                p_drpai_cma = phys_to_virt(drpai_rw_status->drpai_data.address);
                if (p_drpai_cma == 0)
                {
                    result = -EFAULT;
                    goto end;
                }
                __flush_dcache_area(p_drpai_cma, drpai_rw_status->drpai_data.size);
                drpai_rw_status->rw_status = DRPAI_STATUS_IDLE_RW;
                DRPAI_DEBUG_PRINT("[%s](pid %d) Deleted list %px rw_status %d prev %px next %px\n", __func__, current->pid, &drpai_rw_status->list, drpai_rw_status->rw_status, drpai_rw_status->list.prev, drpai_rw_status->list.next);
                list_del(&drpai_rw_status->list);
                drpai_rw_status->drpai_data.address = 0x0;
                drpai_rw_status->drpai_data.size    = 0x0;
            }
            result = count;
            break;
        case DRPAI_STATUS_WRITE_PARAM:
            if ( !( drpai_rw_status->assign_param.info_size >= (drpai_rw_status->write_count + count) ) )
            {
                count = drpai_rw_status->assign_param.info_size - drpai_rw_status->write_count;
            }
            /* Copy arguments from user space to kernel space */
            if (copy_from_user(&drpai_rw_status->param_info[drpai_rw_status->write_count], buf, count))
            {
                result = -EFAULT;
                goto end;
            }
            drpai_rw_status->write_count = drpai_rw_status->write_count + count;
            /* DRPAI_STATUS_WRITE_PARAM -> DRPAI_STATUS_IDLE_RW */
            if (drpai_rw_status->assign_param.info_size <= drpai_rw_status->write_count)
            {
                DRPAI_DEBUG_PRINT("[%s](pid %d) Status is changed \n", __func__, current->pid);
                drpai_rw_status->rw_status = DRPAI_STATUS_IDLE_RW;
            }
            result = count;
            break;
        default:
            ; /* Do nothing */
            break;
    }
    goto end;
end:
    if(-ERESTART != result)
    {
        up(&rw_sem);
    }
    if (NULL != kbuf)
    {
        /* Free kbuf */
        vfree(kbuf);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw3:%d\n", __func__, current->pid, drpai_rw_status->rw_status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return result;
}

static ssize_t drpai_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    ssize_t result = 0;
    uint8_t *kbuf = NULL;
    volatile void *p_drpai_cma = 0;
    uint32_t reg_val;
    uint32_t i;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;
    struct drpai_priv *priv = drpai_priv;

    if(unlikely(down_trylock(&rw_sem)))
    {
        result = -ERESTART;
        goto end;
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    /* Check status */
    if (!((DRPAI_STATUS_ASSIGN  == drpai_rw_status->rw_status) ||
        (DRPAI_STATUS_DUMP_REG  == drpai_rw_status->rw_status) ||
        (DRPAI_STATUS_READ_MEM  == drpai_rw_status->rw_status) ||
        (DRPAI_STATUS_READ_REG  == drpai_rw_status->rw_status)))
    {
        result = -EACCES;
        goto end;
    }

    /* Check Argument */
    if (NULL == buf)
    {
        result = -EFAULT;
        goto end;
    }
    if (0 == count)
    {
        result = -EINVAL;
        goto end;
    }

    /* Secure Kbuf area */
    kbuf = vmalloc(count);
    if (NULL == kbuf)
    {
        result = -ENOMEM;
        goto end;
    }

    switch(drpai_rw_status->rw_status)
    {
        case DRPAI_STATUS_ASSIGN:
            /* DRPAI_STATUS_ASSIGN -> DRPAI_STATUS_READ_MEM */
            drpai_rw_status->rw_status = DRPAI_STATUS_READ_MEM;
            break;
        case DRPAI_STATUS_DUMP_REG:
            /* DRPAI_STATUS_DUMP_REG -> DRPAI_STATUS_READ_REG */
            drpai_rw_status->rw_status = DRPAI_STATUS_READ_REG;
            break;
        default:
            ; /* Do nothing */
            break;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    switch(drpai_rw_status->rw_status)
    {
        case DRPAI_STATUS_READ_MEM:
            /* Read DRP-AI memory */
            p_drpai_cma = phys_to_virt(drpai_rw_status->drpai_data.address + drpai_rw_status->read_count);
            if (p_drpai_cma == 0)
            {
                result = -EFAULT;
                goto end;
            }
            if ( !( drpai_rw_status->drpai_data.size >= (drpai_rw_status->read_count + count) ) )
            {
                count = drpai_rw_status->drpai_data.size - drpai_rw_status->read_count;
            }
            memcpy(kbuf, p_drpai_cma, count);
            drpai_rw_status->read_count = drpai_rw_status->read_count + count;

            /* DRPAI_STATUS_READ -> DRPAI_STATUS_IDLE_RW */
            if (drpai_rw_status->drpai_data.size <= drpai_rw_status->read_count)
            {
                drpai_rw_status->rw_status = DRPAI_STATUS_IDLE_RW;
                DRPAI_DEBUG_PRINT("[%s](pid %d) Deleted list %px rw_status %d prev %px next %px\n", __func__, current->pid, &drpai_rw_status->list, drpai_rw_status->rw_status, drpai_rw_status->list.prev, drpai_rw_status->list.next);
                list_del(&drpai_rw_status->list);
                drpai_rw_status->drpai_data.address = 0x0;
                drpai_rw_status->drpai_data.size    = 0x0;
            }
            i = count;
            break;
        case DRPAI_STATUS_READ_REG:
            /* Read DRP-AI register */
            if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT))) 
            {
                result = -ETIMEDOUT;
                goto end;
            }
            for (i = 0; i < count; i+=4)
            {
                if (drp_size > drpai_rw_status->read_count)
                {
                    reg_val = ioread32(g_drp_base_addr[0] + drpai_rw_status->drp_reg_offset_count);
                    *(kbuf + i)     = (uint8_t)reg_val;
                    *(kbuf + i + 1) = (uint8_t)(reg_val >> 8);
                    *(kbuf + i + 2) = (uint8_t)(reg_val >> 16);
                    *(kbuf + i + 3) = (uint8_t)(reg_val >> 24);
                    drpai_rw_status->drp_reg_offset_count+=4;
                }
                else
                {
                    reg_val = ioread32(g_aimac_base_address[0] + drpai_rw_status->aimac_reg_offset_count);
                    *(kbuf + i)     = (uint8_t)reg_val;
                    *(kbuf + i + 1) = (uint8_t)(reg_val >> 8);
                    *(kbuf + i + 2) = (uint8_t)(reg_val >> 16);
                    *(kbuf + i + 3) = (uint8_t)(reg_val >> 24);
                    drpai_rw_status->aimac_reg_offset_count+=4;
                }
                drpai_rw_status->read_count+=4;

                /* DRPAI_STATUS_READ_REG -> DRPAI_STATUS_IDLE_RW */
                if ((drp_size + aimac_size) <= drpai_rw_status->read_count)
                {
                    drpai_rw_status->rw_status = DRPAI_STATUS_IDLE_RW;
                    i+=4;
                    break;
                }
            }
            up(&priv->sem);
            break;
        default:
            ; /* Do nothing */
            break;
    }

    /* Copy arguments from kernel space to user space */
    if (copy_to_user(buf, kbuf, count))
    {
        result = -EFAULT;
        goto end;
    }

    result = i;
    goto end;
end:
    if(-ERESTART != result)
    {
        up(&rw_sem);
    }
    if (NULL != kbuf)
    {
        /* Free kbuf */
        vfree(kbuf);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw3:%d\n", __func__, current->pid, drpai_rw_status->rw_status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return result;
}

static long drpai_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    long result = 0;

    switch (cmd) {
    case DRPAI_ASSIGN:
        DRPAI_DEBUG_PRINT(KERN_INFO "[ioctl(DRPAI_ASSIGN)](pid %d)\n", current->pid);
        result = drpai_ioctl_assign(filp, cmd, arg);
        break;
    case DRPAI_START:
        DRPAI_DEBUG_PRINT(KERN_INFO "[ioctl(DRPAI_START)](pid %d)\n", current->pid);
        result = drpai_ioctl_start(filp, cmd, arg);
        break;
    case DRPAI_RESET:
        DRPAI_DEBUG_PRINT(KERN_INFO "[ioctl(DRPAI_RESET)](pid %d)\n", current->pid);
        result = drpai_ioctl_reset(filp, cmd, arg);
        break;
    case DRPAI_GET_STATUS:
        DRPAI_DEBUG_PRINT(KERN_INFO "[ioctl(DRPAI_GET_STATUS)](pid %d)\n", current->pid);
        result = drpai_ioctl_get_status(filp, cmd, arg);
        break;
    case DRPAI_REG_DUMP:
        DRPAI_DEBUG_PRINT(KERN_INFO "[ioctl(DRPAI_REG_DUMP)](pid %d)\n", current->pid);
        result = drpai_ioctl_reg_dump(filp, cmd, arg);
        break;
    case DRPAI_ASSIGN_PARAM:
        DRPAI_DEBUG_PRINT(KERN_INFO "[ioctl(DRPAI_ASSIGN_PARAM)](pid %d)\n", current->pid);
        result = drpai_ioctl_assign_param(filp, cmd, arg);
        break;
    case DRPAI_PREPOST_CROP:
        DRPAI_DEBUG_PRINT(KERN_INFO "[ioctl(DRPAI_PREPOST_CROP)](pid %d)\n", current->pid);
        result = drpai_ioctl_prepost_crop(filp, cmd, arg);
        break;
    case DRPAI_PREPOST_INADDR:
        DRPAI_DEBUG_PRINT(KERN_INFO "[ioctl(DRPAI_PREPOST_INADDR)](pid %d)\n", current->pid);
        result = drpai_ioctl_prepost_inaddr(filp, cmd, arg);
        break;
    default:
        DRPAI_DEBUG_PRINT(KERN_WARNING "unsupported command %d\n", cmd);
        result = -EFAULT;
        break;
    }
    goto end;

end:
    return result;
}

static unsigned int drpai_poll( struct file* filp, poll_table* wait )
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    unsigned int retmask = 0;
    struct drpai_priv *priv = drpai_priv;
    unsigned long flags;

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);

    spin_lock_irqsave(&priv->lock, flags);

    poll_wait( filp, &drpai_waitq,  wait );

    if (1 == priv->aimac_irq_flag)
    {
        retmask |= ( POLLIN  | POLLRDNORM );
    }

    spin_unlock_irqrestore(&priv->lock, flags);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return retmask;
}

/* ISP */
static irqreturn_t irq_drp_nmlint(int irq, void *dev)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    unsigned long flags;
    struct drpai_priv *priv = drpai_priv;
    drpai_odif_intcnto_t local_odif_intcnto;

    spin_lock_irqsave(&priv->lock, flags);
    /* For success ISP call back*/
    void (*finish_callback)(int);
    finish_callback = priv->isp_finish_loc;
    /* For success ISP call back*/

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:%d\n", __func__, current->pid, priv->drpai_status.status);

    /* DRP normal interrupt processing */
    R_DRPAI_DRP_Nmlint(0, &local_odif_intcnto);

    odif_intcnto.ch0 += local_odif_intcnto.ch0;
    odif_intcnto.ch1 += local_odif_intcnto.ch1;
    odif_intcnto.ch2 += local_odif_intcnto.ch2;
    odif_intcnto.ch3 += local_odif_intcnto.ch3;

    DRPAI_DEBUG_PRINT(KERN_INFO "ODIF_INTCNTO0 : 0x%08X\n", odif_intcnto.ch0);
    DRPAI_DEBUG_PRINT(KERN_INFO "ODIF_INTCNTO1 : 0x%08X\n", odif_intcnto.ch1);
    DRPAI_DEBUG_PRINT(KERN_INFO "ODIF_INTCNTO2 : 0x%08X\n", odif_intcnto.ch2);
    DRPAI_DEBUG_PRINT(KERN_INFO "ODIF_INTCNTO3 : 0x%08X\n", odif_intcnto.ch3);

    DRPAI_DEBUG_PRINT(KERN_INFO "local_ODIF_INTCNTO0 : 0x%08X\n", local_odif_intcnto.ch0);
    DRPAI_DEBUG_PRINT(KERN_INFO "local_ODIF_INTCNTO1 : 0x%08X\n", local_odif_intcnto.ch1);
    DRPAI_DEBUG_PRINT(KERN_INFO "local_ODIF_INTCNTO2 : 0x%08X\n", local_odif_intcnto.ch2);
    DRPAI_DEBUG_PRINT(KERN_INFO "local_ODIF_INTCNTO3 : 0x%08X\n", local_odif_intcnto.ch3);

    if  ((1 == odif_intcnto.ch0) &&
        (1 == odif_intcnto.ch1) &&
        (1 == odif_intcnto.ch2) &&
        (1 == odif_intcnto.ch3))
     {
        /* Internal state update */
        priv->drpai_status.status = DRPAI_STATUS_IDLE;
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:%d\n", __func__, current->pid, priv->drpai_status.status);
        /* For success ISP call back*/
        if(NULL != finish_callback)
        {
            (*finish_callback)(0);
        }
     }
    priv->isp_finish_loc = NULL;
    spin_unlock_irqrestore(&priv->lock, flags);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return IRQ_HANDLED;
}
/* ISP */

static irqreturn_t irq_drp_errint(int irq, void *dev)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    unsigned long flags;
    struct drpai_priv *priv = drpai_priv;

    spin_lock_irqsave(&priv->lock, flags);
/* ISP */
/* For error ISP call back*/
    void (*finish_callback)(int);
    finish_callback = priv->isp_finish_loc;

    if(NULL != finish_callback)
    {
        (*finish_callback)(-5);
    }
/* For error ISP call back*/
/* ISP */

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:%d\n", __func__, current->pid, priv->drpai_status.status);

    /* DRP error interrupt processing */
    R_DRPAI_DRP_Errint(0);

/* ISP */
    /* Internal state update */
    priv->drpai_status.err = DRPAI_ERRINFO_DRP_ERR;
    priv->drpai_status.status = DRPAI_STATUS_IDLE;
    priv->aimac_irq_flag = 1;
    
    /* Wake up the process when it's not ISP mode*/
    if(NULL == finish_callback)
    {
        wake_up_interruptible( &drpai_waitq );
    }
    priv->isp_finish_loc = NULL;
/* ISP */

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:%d\n", __func__, current->pid, priv->drpai_status.status);
    spin_unlock_irqrestore(&priv->lock, flags);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return IRQ_HANDLED;
}

static irqreturn_t irq_mac_nmlint(int irq, void *dev)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    unsigned long flags;
    struct drpai_priv *priv = drpai_priv;

    spin_lock_irqsave(&priv->lock, flags);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:%d\n", __func__, current->pid, priv->drpai_status.status);

    /* AI-MAC normal interrupt processing */
    R_DRPAI_AIMAC_Nmlint(0);

    /* Internal state update */
    priv->drpai_status.status = DRPAI_STATUS_IDLE;
    priv->aimac_irq_flag = 1;
    
    /* Wake up the process */
    wake_up_interruptible( &drpai_waitq );

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:%d\n", __func__, current->pid, priv->drpai_status.status);
    spin_unlock_irqrestore(&priv->lock, flags);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return IRQ_HANDLED;
}
static irqreturn_t irq_mac_errint(int irq, void *dev)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    unsigned long flags;
    struct drpai_priv *priv = drpai_priv;

    spin_lock_irqsave(&priv->lock, flags);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:%d\n", __func__, current->pid, priv->drpai_status.status);

    /* AI-MAC error interrupt processing */
    R_DRPAI_AIMAC_Errint(0);

    /* Internal state update */
    priv->drpai_status.err = DRPAI_ERRINFO_AIMAC_ERR;
    priv->drpai_status.status = DRPAI_STATUS_IDLE;
    priv->aimac_irq_flag = 1;
    
    /* Wake up the process */
    wake_up_interruptible( &drpai_waitq );

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:%d\n", __func__, current->pid, priv->drpai_status.status);
    spin_unlock_irqrestore(&priv->lock, flags);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return IRQ_HANDLED;
}

static int drpai_regist_driver(void)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int alloc_ret = 0;
    int cdev_err = 0;
    dev_t dev;

    printk(KERN_INFO "DRP-AI Driver version : %s\n", DRPAI_DRIVER_VERSION);

    /* Get free major number. */
    alloc_ret = alloc_chrdev_region(&dev, MINOR_BASE, MINOR_NUM, DRPAI_DRIVER_NAME);
    if (alloc_ret != 0) {
        printk(KERN_ERR "alloc_chrdev_region = %d\n", alloc_ret);
        return -1;
    }

    /* Save major number. */
    drpai_major = MAJOR(dev);
    dev = MKDEV(drpai_major, MINOR_BASE);

    /* Initialize cdev and registration handler table. */
    cdev_init(&drpai_cdev, &s_mydevice_fops);
    drpai_cdev.owner = THIS_MODULE;

    /* Registration cdev */
    cdev_err = cdev_add(&drpai_cdev, dev, MINOR_NUM);
    if (cdev_err != 0) {
        printk(KERN_ERR  "cdev_add = %d\n", cdev_err);
        unregister_chrdev_region(dev, MINOR_NUM);
        return -1;
    }

    /* Cleate class "/sys/class/drpai/" */
    drpai_class = class_create(THIS_MODULE, DRPAI_DRIVER_NAME);
    if (IS_ERR(drpai_class)) {
        printk(KERN_ERR  "class_create = %d\n", drpai_class);
        cdev_del(&drpai_cdev);
        unregister_chrdev_region(dev, MINOR_NUM);
        return -1;
    }

    int minor;
    /* Make "/sys/class/drpai/drpai*" */
    for (minor = MINOR_BASE; minor < MINOR_BASE + MINOR_NUM; minor++) {
        drpai_device_array[minor - MINOR_BASE] =
        device_create(drpai_class, NULL, MKDEV(drpai_major, minor), NULL, DRPAI_DRIVER_NAME "%d", minor);
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return 0;
}

static int drpai_regist_device(struct platform_device *pdev)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    struct resource *res;
    struct resource reserved_res;
    struct device_node *np;
    struct drpai_priv *priv;
    struct drpai_rw_status *drpai_rw_status;
    int irq, ret;
    void __iomem *base;

    /* Intialize DRP-AI status to control */
    priv = devm_kzalloc(&pdev->dev, sizeof(struct drpai_priv), GFP_KERNEL);
    if (!priv) {
        dev_err(&pdev->dev, "cannot allocate private data\n");
        return -ENOMEM;
    }

    platform_set_drvdata(pdev, priv);
    priv->pdev = pdev;
    priv->dev_name = dev_name(&pdev->dev);
    spin_lock_init(&priv->lock);
    sema_init(&priv->sem, DRPAI_DEV_NUM);
    refcount_set(&priv->count, 1);
    priv->drpai_status.status = DRPAI_STATUS_INIT; 
    priv->drpai_status.err    = DRPAI_ERRINFO_SUCCESS;
    drpai_priv = priv;
/* ISP */
    /* Call back function pointer initialization */
    drpai_priv->isp_finish_loc = NULL;
/* ISP */

    /* Initialize list head */
    drpai_rw_status = devm_kzalloc(&pdev->dev, sizeof(struct drpai_rw_status), GFP_KERNEL);
    if (!drpai_rw_status) {
        dev_err(&pdev->dev, "cannot allocate sentinel data\n");
        return -ENOMEM;
    }
    drpai_rw_status->rw_status  = HEAD_SENTINEL;
    drpai_rw_status->param_info = NULL;
    INIT_LIST_HEAD(&drpai_rw_status->list);
    atomic_set(&drpai_rw_status->inout_flag, 0);
    drpai_rw_sentinel = drpai_rw_status;
    DRPAI_DEBUG_PRINT("[%s](pid %d) HEAD  list %px rw_status %d prev %px next %px\n", __func__, current->pid, &drpai_rw_sentinel->list, drpai_rw_sentinel->rw_status, drpai_rw_sentinel->list.prev, drpai_rw_sentinel->list.next);

    /* Get reserved memory region from Device tree.*/
    np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
    if (!np) {
        dev_err(&pdev->dev, "No %s specified\n", "memory-region");
        return -ENOMEM;
    }

    /* Convert memory region to a struct resource */
    ret = of_address_to_resource(np, 0, &reserved_res);
    if (ret) {
        dev_err(&pdev->dev, "No memory address assigned to the region\n");
        return -ENOMEM;
    }
    drpai_region_base_addr = reserved_res.start;
    drpai_region_size = resource_size(&reserved_res);
    printk(KERN_INFO "DRP-AI memory region start 0x%08X, size 0x%08X\n", drpai_region_base_addr, drpai_region_size);

    /* Get linux memory region from Device tree.*/
    np = of_parse_phandle(pdev->dev.of_node, "linux-memory-region", 0);
    if (!np) {
        dev_err(&pdev->dev, "No %s specified\n", "linux-memory-region");
        return -ENOMEM;
    }

    /* read linux start address and size */
    ret = of_address_to_resource(np, 0, &reserved_res);
    if (ret) {
        dev_err(&pdev->dev, "No address assigned to the linux-memory-region\n");
        return -ENOMEM;
    }
    drpai_linux_mem_start = reserved_res.start;
    drpai_linux_mem_size = resource_size(&reserved_res);
    printk(KERN_INFO "linux-memory-region start 0x%08X, size 0x%08X\n", drpai_linux_mem_start, drpai_linux_mem_size);

    /* Convert DRP base address from physical to virtual */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        dev_err(&pdev->dev, "cannot get resources (reg)\n");
        return -EINVAL;
    }
    priv->drp_base = devm_ioremap_nocache(&pdev->dev, res->start, resource_size(res));
    if (!priv->drp_base) {
        dev_err(&pdev->dev, "cannot ioremap\n");
        return -EINVAL;
    }
    g_drp_base_addr[0] = priv->drp_base;
    drp_size = resource_size(res);
    printk(KERN_INFO "DRP base address 0x%08X, size 0x%08X\n", res->start, drp_size);

    /* Convert AI-MAC base address from physical to virtual */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    if (!res) {
        dev_err(&pdev->dev, "cannot get resources (reg)\n");
        return -EINVAL;
    }
    priv->aimac_base = devm_ioremap_nocache(&pdev->dev, res->start, resource_size(res));
    if (!priv->aimac_base) {
        dev_err(&pdev->dev, "cannot ioremap\n");
        return -EINVAL;
    }
    g_aimac_base_address[0] = priv->aimac_base;
    aimac_size = resource_size(res);
    printk(KERN_INFO "AI-MAC base address 0x%08X, size 0x%08X\n", res->start, aimac_size);

    /* Registering an interrupt handler */
/* ISP */
    irq = platform_get_irq(pdev, 0);
    ret = devm_request_irq(&pdev->dev, irq, irq_drp_nmlint, 0, "drpa nmlint", priv);
    if (ret) {
        dev_err(&pdev->dev, "Failed to claim IRQ!\n");
        return ret;
    }
/* ISP */
    irq = platform_get_irq(pdev, 1);
    ret = devm_request_irq(&pdev->dev, irq, irq_drp_errint, 0, "drpa errint", priv);
    if (ret) {
        dev_err(&pdev->dev, "Failed to claim IRQ!\n");
        return ret;
    }
    irq = platform_get_irq(pdev, 2);
    ret = devm_request_irq(&pdev->dev, irq, irq_mac_nmlint, 0, "drpa mac_nmlint", priv);
    if (ret) {
        dev_err(&pdev->dev, "Failed to claim IRQ!\n");
        return ret;
    }
    irq = platform_get_irq(pdev, 3);
    ret = devm_request_irq(&pdev->dev, irq, irq_mac_errint, 0, "drpa mac_errint", priv);
    if (ret) {
        dev_err(&pdev->dev, "Failed to claim IRQ!\n");
        return ret;
    }

	/* Get clock controller info */
	priv->clk_int = devm_clk_get(&pdev->dev, "intclk");
	if (IS_ERR(priv->clk_int)) {
		dev_err(&pdev->dev, "missing controller clock");
		return PTR_ERR(priv->clk_int);
	}
	
	priv->clk_aclk_drp = devm_clk_get(&pdev->dev, "aclk_drp");
	if (IS_ERR(priv->clk_aclk_drp)) {
		dev_err(&pdev->dev, "missing controller clock");
		return PTR_ERR(priv->clk_aclk_drp);
	}
	
	priv->clk_mclk = devm_clk_get(&pdev->dev, "mclk");
	if (IS_ERR(priv->clk_mclk)) {
		dev_err(&pdev->dev, "missing controller clock");
		return PTR_ERR(priv->clk_mclk);
	}
	
	priv->clk_dclkin = devm_clk_get(&pdev->dev, "dclkin");
	if (IS_ERR(priv->clk_dclkin)) {
		dev_err(&pdev->dev, "missing controller clock");
		return PTR_ERR(priv->clk_dclkin);
	}
	
	priv->clk_aclk = devm_clk_get(&pdev->dev, "aclk");
	if (IS_ERR(priv->clk_aclk)) {
		dev_err(&pdev->dev, "missing controller clock");
		return PTR_ERR(priv->clk_aclk);
	}

    /* Get reset controller info */
    priv->rstc = devm_reset_control_get(&pdev->dev, NULL);
    if (IS_ERR(priv->rstc))
    {
        DRPAI_DEBUG_PRINT(KERN_INFO "failed to get DRP cpg reset\n");        
    }
    else
    {
        DRPAI_DEBUG_PRINT(KERN_INFO "OK!!!! get DRP cpg reset\n");        
    } 

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return 0;
}

static void drpai_unregist_driver(void)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    DRPAI_DEBUG_PRINT(KERN_INFO "drpai_unregist_driver");

    dev_t dev = MKDEV(drpai_major, MINOR_BASE);

    int minor;
    /* Delete "/sys/class/mydevice/mydevice*". */
    for (minor = MINOR_BASE; minor < MINOR_BASE + MINOR_NUM; minor++) {
        device_destroy(drpai_class, MKDEV(drpai_major, minor));
    }

    /* Destroy "/sys/class/mydevice/". */
    class_destroy(drpai_class);

    /* Delete cdev from kernel. */
    cdev_del(&drpai_cdev);

    /* Unregistration */
    unregister_chrdev_region(dev, MINOR_NUM);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
}

static void drpai_unregist_device(void)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    /* Do nothing */
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
}

static int8_t drpai_reset_device(uint32_t ch)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    int8_t retval;

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);

    /* Reset DRP */
    if(R_DRPAI_SUCCESS != R_DRPAI_DRP_Reset(ch)) {
        goto err_reset;
    }

    /* Reset AI-MAC */
    if(R_DRPAI_SUCCESS != R_DRPAI_AIMAC_Reset(ch)) {
        goto err_reset;
    }

    /* Reset CPG register */
    if(R_DRPAI_SUCCESS != R_DRPAI_CPG_Reset()) {
        goto err_reset;
    }

    retval = R_DRPAI_SUCCESS;
    goto end;
err_reset:
    retval = R_DRPAI_ERR_RESET;
    goto end;
end:
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return retval;
}

static void drpai_init_device(uint32_t ch)
{
	unsigned long flags;
    struct drpai_priv *priv = drpai_priv;
    
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    spin_lock_irqsave(&priv->lock, flags);
    priv->aimac_irq_flag = 1;
    spin_unlock_irqrestore(&priv->lock, flags);
    (void)R_DRPAI_DRP_Open(0);
    (void)R_DRPAI_AIMAC_Open(0);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
}

static long drpai_ioctl_assign(struct file *filp, unsigned int cmd, unsigned long arg)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    long result = 0;
    volatile void *p_virt_address = 0;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;
    struct drpai_rw_status *entry;
    struct list_head *listitr;
    drpai_data_t drpai_data_buf;

    if(unlikely(down_trylock(&rw_sem)))
    {
        result = -ERESTART;
        goto end;
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) (pid %d)\n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    /* Check NULL */
    if (NULL == arg)
    {
        result = -EINVAL;
        goto end;
    }

    /* Check status */
    if (DRPAI_STATUS_IDLE_RW != drpai_rw_status->rw_status)
    {
        result = -EACCES;
        goto end;
    }

    /* Copy arguments from user space to kernel space */
    if (copy_from_user(&drpai_data_buf, (void __user *)arg, sizeof(drpai_data_t)))
    {
        result = -EFAULT;
        goto end;
    }
    /* Check Argument */
    if (0 != (drpai_data_buf.address & DRPAI_64BYTE_ALIGN))
    {
        result = -EINVAL;
        goto end;
    }
    if ((drpai_region_base_addr > drpai_data_buf.address) || 
       ((drpai_region_base_addr + drpai_region_size) <= (drpai_data_buf.address + drpai_data_buf.size)))
    {
        result = -EINVAL;
        goto end;
    }

    /* Check the assigned address */
    DRPAI_DEBUG_PRINT("[%s](pid %d) list %px prev %px next %px\n", __func__, current->pid, &drpai_rw_status->list, drpai_rw_status->list.prev, drpai_rw_status->list.next);
    if(!list_empty(&drpai_rw_sentinel->list))
    {   
        DRPAI_DEBUG_PRINT("[%s](pid %d) List is not empty\n", __func__, current->pid);
        list_for_each(listitr, &drpai_rw_sentinel->list)
        {
            entry = list_entry(listitr, struct drpai_rw_status, list);
            DRPAI_DEBUG_PRINT("[%s](pid %d) rw_status %d list %px prev %px next %px\n", __func__, current->pid, entry->rw_status, &entry->list, entry->list.prev, entry->list.next);
            if(HEAD_SENTINEL != entry->rw_status)
            {
                if( !( (entry->drpai_data.address > (drpai_data_buf.address + drpai_data_buf.size - 1)) ||
                       ((entry->drpai_data.address + entry->drpai_data.size - 1) < drpai_data_buf.address) ))
                {
                    result = -EINVAL;
                    goto end;
                }
            }
        }
    }

    /* Data cache invalidate. DRP-AI W -> CPU R */
    p_virt_address = phys_to_virt(drpai_data_buf.address);
    if (p_virt_address == 0)
    {
        result = -EFAULT;
        goto end;
    }
    __inval_dcache_area(p_virt_address, drpai_data_buf.size);

    /* Initialization of read / write processing variables */
    drpai_rw_status->drpai_data  = drpai_data_buf;
    drpai_rw_status->rw_status   = DRPAI_STATUS_ASSIGN;
    drpai_rw_status->write_count = 0;
    drpai_rw_status->read_count  = 0;
    /* Register assigned status */
    list_add(&drpai_rw_status->list, &drpai_rw_sentinel->list);
    DRPAI_DEBUG_PRINT("[%s](pid %d) Registered list %px prev %px next %px\n", __func__, current->pid, &drpai_rw_status->list, drpai_rw_status->list.prev, drpai_rw_status->list.next);

    goto end;
end:
    if(-ERESTART != result)
    {
        up(&rw_sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

static long drpai_ioctl_start(struct file *filp, unsigned int cmd, unsigned long arg)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    int result = 0;
    drpai_data_t proc[DRPAI_INDEX_NUM];
    volatile void *p_drp_param = 0;
    volatile void *p_drp_desc = 0;
    volatile void *p_aimac_desc = 0;
    struct drpai_priv *priv = drpai_priv;
    unsigned long flags;
    int i;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;

    if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT))) 
    {
        result = -ETIMEDOUT;
        goto end;
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:%d\n", __func__, current->pid, priv->drpai_status.status);

    /* Check NULL */
    if (NULL == arg)
    {
        result = -EINVAL;
        goto end;
    }

    /* Check status */
    spin_lock_irqsave(&priv->lock, flags);
    if (DRPAI_STATUS_RUN == priv->drpai_status.status)
    {
        spin_unlock_irqrestore(&priv->lock, flags);
        result = -EBUSY;
        goto end;
    }
    spin_unlock_irqrestore(&priv->lock, flags);
    /* Copy arguments from user space to kernel space */
    if (copy_from_user(&proc[0], (void __user *)arg, sizeof(proc)))
    {
        result = -EFAULT;
        goto end;
    }
    /* Check Argument */
    for (i = DRPAI_INDEX_DRP_DESC; i < DRPAI_INDEX_NUM; i++)
    {
        if (0 != (proc[i].address & DRPAI_64BYTE_ALIGN))
        {
            result = -EINVAL;
            goto end;
        }
        if ((drpai_region_base_addr > proc[i].address) || 
           ((drpai_region_base_addr + drpai_region_size) <= (proc[i].address + proc[i].size)))
            {
                result = -EINVAL;
                goto end;
            }
    }

    /* Check if input is in linux memory region */
    if(0 == atomic_read(&drpai_rw_status->inout_flag))
    {
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) Use arg variable\n", __func__, current->pid);
        if(0 != drpai_flush_dcache_input_area(&proc[DRPAI_INDEX_INPUT]))
        {
            result = -EFAULT;
            goto end;
        }
        /* Change input address to value specified by user app. */
        if(0 != drp_param_change32(proc[DRPAI_INDEX_DRP_PARAM].address, 0, proc[DRPAI_INDEX_INPUT].address))
        {
            result = -EFAULT;
            goto end;
        }
    }
    
    /* drp_desc.bin */
    p_drp_desc = phys_to_virt(proc[DRPAI_INDEX_DRP_DESC].address);
    if (p_drp_desc == 0)
    {
        result = -EFAULT;
        goto end;
    }
    /* Changed link descriptor of drp_desc.bin */
    if (0 != (proc[DRPAI_INDEX_DRP_DESC].size & 0x0F))
    {
        result = -EINVAL;
        goto end;
    }
    iowrite8(0x08, p_drp_desc + proc[DRPAI_INDEX_DRP_DESC].size - 13);
    __flush_dcache_area(p_drp_desc + proc[DRPAI_INDEX_DRP_DESC].size - 13, 1);

    /* aimac_desc.bin */
    p_aimac_desc = phys_to_virt(proc[DRPAI_INDEX_AIMAC_DESC].address);
    if (p_aimac_desc == 0)
    {
        result = -EFAULT;
        goto end;
    }
    /* Changed link descriptor of drp_desc.bin */
    if (0 != (proc[DRPAI_INDEX_AIMAC_DESC].size & 0x0F))
    {
        result = -EINVAL;
        goto end;
    }
    iowrite8(0x08, p_aimac_desc + proc[DRPAI_INDEX_AIMAC_DESC].size - 13);
    __flush_dcache_area(p_aimac_desc + proc[DRPAI_INDEX_AIMAC_DESC].size - 13, 1);

    spin_lock_irqsave(&priv->lock, flags);
    /* Init drpai_status.err */
    priv->drpai_status.err    = DRPAI_ERRINFO_SUCCESS;
    /* IDLE -> RUN */
    priv->drpai_status.status = DRPAI_STATUS_RUN;
    priv->aimac_irq_flag = 0;
    spin_unlock_irqrestore(&priv->lock, flags);

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:%d\n", __func__, current->pid, priv->drpai_status.status);

    /* Kick */
    (void)R_DRPAI_DRP_Start(0, proc[DRPAI_INDEX_DRP_DESC].address);
    (void)R_DRPAI_AIMAC_Start(0, proc[DRPAI_INDEX_AIMAC_DESC].address);

    goto end;
end:
    if(-ETIMEDOUT != result)
    {
        up(&priv->sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

static long drpai_ioctl_reset(struct file *filp, unsigned int cmd, unsigned long arg)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    long result = 0;
    struct drpai_priv *priv = drpai_priv;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;
    unsigned long flags;
/* ISP */
    void (*finish_callback)(int);
    spin_lock_irqsave(&priv->lock, flags);
    finish_callback = drpai_priv->isp_finish_loc;
    spin_unlock_irqrestore(&priv->lock, flags);
/* ISP */

    if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT))) 
    {
        result = -ETIMEDOUT;
        goto end;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:   %d\n", __func__, current->pid, priv->drpai_status.status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    if(R_DRPAI_SUCCESS != drpai_reset_device(0))
    {
        result = -EIO;
        goto end;
    }
    drpai_init_device(0);

    /* Update internal state */
    spin_lock_irqsave(&priv->lock, flags);
    priv->drpai_status.err    = DRPAI_ERRINFO_RESET;
    priv->drpai_status.status = DRPAI_STATUS_IDLE;

    /* Wake up the process */
    wake_up_interruptible( &drpai_waitq );
    spin_unlock_irqrestore(&priv->lock, flags);

/* ISP */
/* For reset ISP call back*/    
    if(NULL != finish_callback)
    {
        /* ERROR No.: ERESTART*/
        (*finish_callback)(-85);
    }
    spin_lock_irqsave(&priv->lock, flags);    
    drpai_priv->isp_finish_loc = NULL;
    spin_unlock_irqrestore(&priv->lock, flags);
/* For reset ISP call back*/
/* ISP */

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:   %d\n", __func__, current->pid, priv->drpai_status.status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    result = 0;
    goto end;
end:
    if(-ETIMEDOUT != result)
    {
        up(&priv->sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

static long drpai_ioctl_get_status(struct file *filp, unsigned int cmd, unsigned long arg)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    long result = 0;
    drpai_status_t local_drpai_status;
    struct drpai_priv *priv = drpai_priv;
    unsigned long flags;

    if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT))) 
    {
        result = -ETIMEDOUT;
        goto end;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    /* Check NULL */
    if (NULL == arg)
    {
        result = -EINVAL;
        goto end;
    }

    /* Get the internal state of DRP-AI */
    spin_lock_irqsave(&priv->lock, flags);
    (void)R_DRPAI_Status(0, &priv->drpai_status);

    /* Copy arguments from kernel space to user space */
    local_drpai_status = priv->drpai_status;
    spin_unlock_irqrestore(&priv->lock, flags);
    if (copy_to_user((void __user *)arg, &local_drpai_status, sizeof(drpai_status_t)))
    {
        result = -EFAULT;
        goto end;
    }

    /* Check status */
    if (DRPAI_STATUS_RUN == local_drpai_status.status)
    {
        result = -EBUSY;
        goto end;
    }

    /* Check DRP-AI H/W error */
    if ((DRPAI_ERRINFO_DRP_ERR == local_drpai_status.err) || (DRPAI_ERRINFO_AIMAC_ERR == local_drpai_status.err))
    {
        result = -EIO;
        goto end;
    }

    goto end;
end:
    if(-ETIMEDOUT != result)
    {
        up(&priv->sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return result;
}

static long drpai_ioctl_reg_dump(struct file *filp, unsigned int cmd, unsigned long arg)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    long result = 0;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;

    if(unlikely(down_timeout(&rw_sem, MAX_SEM_TIMEOUT)))
    {
        result = -ETIMEDOUT;
        goto end;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) \n", __func__, current->pid);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    /* Check of writing and reading completion of DRP-AI obj file */
    if (DRPAI_STATUS_IDLE_RW != drpai_rw_status->rw_status)
    {
        result = -EACCES;
        goto end;
    }

    /* Initialization of register dump processing variables */
    drpai_rw_status->rw_status              = DRPAI_STATUS_DUMP_REG;
    drpai_rw_status->read_count             = 0;
    drpai_rw_status->drp_reg_offset_count   = 0;
    drpai_rw_status->aimac_reg_offset_count = 0;

    goto end;
end:
    if(-ETIMEDOUT != result)
    {
        up(&rw_sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

static long drpai_ioctl_assign_param(struct file *filp, unsigned int cmd, unsigned long arg)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    long result = 0;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;
    drpai_assign_param_t drpai_assign_param_buf;
    char *vbuf;

    if(unlikely(down_trylock(&rw_sem)))
    {
        result = -ERESTART;
        goto end;
    }
	/* Check NULL */
    if (NULL == arg)
    {
        result = -EINVAL;
        goto end;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);
    /* Check status */
    if (DRPAI_STATUS_IDLE_RW != drpai_rw_status->rw_status)
    {
        result = -EACCES;
        goto end;
    }
    if(drpai_rw_status->param_info)
    {
        result = -EFAULT;
        goto end;
    }

    /* Copy arguments from user space to kernel space */
    if (copy_from_user(&drpai_assign_param_buf, (void __user *)arg, sizeof(drpai_assign_param_t)))
    {
        result = -EFAULT;
        goto end;
    }
    if(0 == drpai_assign_param_buf.obj.size)
    {
        result = -EINVAL;
        goto end;
    }
    if ((drpai_region_base_addr > drpai_assign_param_buf.obj.address) || 
        ((drpai_region_base_addr + drpai_region_size) <= (drpai_assign_param_buf.obj.address + drpai_assign_param_buf.obj.size)))
    {
        result = -EINVAL;
        goto end;
    }
    
    /* Allocate memory for *_param_info.txt */
    vbuf = vmalloc(drpai_assign_param_buf.info_size);
    if(!vbuf){
        result = -EFAULT;
        goto end;
    }

    /* Initialization of read / write processing variables */
    drpai_rw_status->rw_status    = DRPAI_STATUS_ASSIGN_PARAM;
    drpai_rw_status->write_count  = 0;
    drpai_rw_status->assign_param = drpai_assign_param_buf;
    drpai_rw_status->param_info   = vbuf;

end:
    if(-ERESTART != result)
    {
        up(&rw_sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

/* Note: This function change line variable. so if you use, check your variables address */
static int8_t get_param_attr(char *line, char *attr, unsigned long *rvalue)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int8_t result = 0;
    char *ptr_stmp, *ptr_etmp;

    ptr_stmp = strstr(line, attr);
    ptr_stmp += strlen(attr);
    ptr_etmp = strstr(line, ",");
    *ptr_etmp = '\0';
    if(0 != kstrtoul(ptr_stmp, 10, rvalue))
    {
        result = -1;
        goto end;
    }
end:
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}
static int8_t drp_param_change16(uint32_t base, uint32_t offset, uint16_t value)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    int8_t result = 0;
    volatile void *virt_addr = 0;
    virt_addr = phys_to_virt(base + offset);
    if (0 == virt_addr)
    {
        result = -1;
        goto end;
    }
    iowrite16(value, virt_addr);
    __flush_dcache_area(virt_addr, sizeof(value));
end:
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}
static int8_t drp_param_change32(uint32_t base, uint32_t offset, uint32_t value)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int8_t result = 0;
    volatile void *virt_addr = 0;
    virt_addr = phys_to_virt(base + offset);
    if (0 == virt_addr)
    {
        result = -1;
        goto end;
    }
    iowrite32(value, virt_addr);
    __flush_dcache_area(virt_addr, sizeof(value));
end:
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}
static int8_t drpai_flush_dcache_input_area(drpai_data_t *input)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    int8_t result = 0;
    drpai_data_t proc = *input;
    uint32_t flush_addr, flush_size;
    uint32_t input_saddr, input_eaddr, linux_mem_saddr, linux_mem_eaddr;
    volatile void *p_input = 0;

    input_saddr      = proc.address;
    input_eaddr      = proc.address + proc.size - 1;
    linux_mem_saddr  = drpai_linux_mem_start;
    linux_mem_eaddr  = drpai_linux_mem_start + drpai_linux_mem_size - 1;
    if ((input_saddr >= linux_mem_saddr) && 
        (input_eaddr <= linux_mem_eaddr))
    {
        flush_addr = proc.address;
        flush_size = proc.size;
    }
    else if ((input_saddr >= linux_mem_saddr) &&
             (input_saddr <= linux_mem_eaddr) &&
             (input_eaddr >  linux_mem_eaddr))
    {
        flush_addr = proc.address;
        flush_size = (drpai_linux_mem_start + drpai_linux_mem_size) - proc.address;
    }
    else if((input_eaddr >= linux_mem_saddr) &&
            (input_eaddr <= linux_mem_eaddr) &&
            (input_saddr <  linux_mem_saddr))
    {
        flush_addr = drpai_linux_mem_start;
        flush_size = (proc.address + proc.size) - drpai_linux_mem_start;
    }
    else
    {
        flush_addr = 0;
        flush_size = 0;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) flush_addr = 0x%08X, flush_size = 0x%08X\n", __func__, current->pid, flush_addr, flush_size);
    if (0 != flush_size)
    {
        /* Input data area cache flush */
        p_input = phys_to_virt(flush_addr);
        if (0 == p_input)
        {
            result = -1;
            goto end;
        }
        __flush_dcache_area(p_input, flush_size);
    }
end:
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

static long drpai_ioctl_prepost_crop(struct file *filp, unsigned int cmd, unsigned long arg)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    long result = 0;
    struct drpai_priv *priv = drpai_priv;
    unsigned long flags;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;
    drpai_crop_t crop_param_buf;
    char buf[DRP_PARAM_MAX_LINE_LENGTH];
    char *ptr, *prev_ptr;
    unsigned long offset_add0, offset_add1;
    int mode = 0;

    /* Check the internal state of DRP-AI */
    spin_lock_irqsave(&priv->lock, flags);
    if(DRPAI_STATUS_RUN == priv->drpai_status.status)
    {
        spin_unlock_irqrestore(&priv->lock, flags);
        result = -EBUSY;
        goto end;
    }
    spin_unlock_irqrestore(&priv->lock, flags);

    if(unlikely(down_trylock(&rw_sem)))
    {
        result = -ERESTART;
        goto end;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    /* Check NULL */
    if (NULL == arg)
    {
        result = -EINVAL;
        goto end;
    }
    if((NULL == drpai_rw_status->param_info) ||
       (DRPAI_STATUS_ASSIGN_PARAM == drpai_rw_status->rw_status) ||
       (DRPAI_STATUS_WRITE_PARAM  == drpai_rw_status->rw_status))
    {
        result = -EACCES;
        goto end;
    }
    /* Copy arguments from user space to kernel space */
    if (copy_from_user(&crop_param_buf, (void __user *)arg, sizeof(drpai_crop_t)))
    {
        result = -EFAULT;
        goto end;
    }
    /* Check if there is in drpai dedicated area */
    if((drpai_rw_status->assign_param.obj.address != crop_param_buf.obj.address) ||
       (drpai_rw_status->assign_param.obj.size != crop_param_buf.obj.size))
    {
        result = -EINVAL;
        goto end;
    }

    /* Search argument */
    ptr = drpai_rw_status->param_info;
    do
    {
        /* Save current pointer */
        prev_ptr = ptr;

        /* Get 1 line */
        ptr = strchr(ptr, '\n');
        if(NULL == ptr) {
            result = -EFAULT;
            goto end;
        }
        ptr += 1;
        if(ptr - prev_ptr >= DRP_PARAM_MAX_LINE_LENGTH)
        {
            result = -EFAULT;
            goto end;
        }

        /* Copy only 1line to buffer */
        memset(buf, 0, sizeof(buf));
        strncpy(buf, prev_ptr, ptr - prev_ptr);

        if(0 == mode)
        {
            /* Check if there is DRP_LIB_NAME_CROP in this line */
            if(NULL != strstr(buf, DRP_LIB_NAME_CROP))
            {
                mode += 1;
                if(0 != get_param_attr(buf, DRP_PARAM_ATTR_OFFSET_ADD, &offset_add0))
                {
                    result = -EFAULT;
                    goto end;
                }
            }
        }
        else if(1 == mode)
        {
            if(NULL != strstr(buf, DRP_PARAM_ATTR_OFFSET_ADD))
            {
                mode += 1;
                if(0 != get_param_attr(buf, DRP_PARAM_ATTR_OFFSET_ADD, &offset_add1))
                {
                    result = -EFAULT;
                    goto end;
                }
                break;
            }
        }
    } while (ptr);

    DRPAI_DEBUG_PRINT("[%s](pid %d) offset_add0=%d, offset_add1=%d\n", __func__, current->pid, offset_add0, offset_add1);

    /* Change parameters of drp_param.bin to value specified by user app. */
    if(0 != drp_param_change16(crop_param_buf.obj.address, offset_add0 + DRP_PARAM_IMG_OWIDTH, crop_param_buf.img_owidth))
    {
        result = -EFAULT;
        goto end;
    }
    if(0 != drp_param_change16(crop_param_buf.obj.address, offset_add0 + DRP_PARAM_IMG_OHEIGHT, crop_param_buf.img_oheight))
    {
        result = -EFAULT;
        goto end;
    }
    if(0 != drp_param_change16(crop_param_buf.obj.address, offset_add0 + DRP_PARAM_CROP_POS_X, crop_param_buf.pos_x))
    {
        result = -EFAULT;
        goto end;
    }
    if(0 != drp_param_change16(crop_param_buf.obj.address, offset_add0 + DRP_PARAM_CROP_POS_Y, crop_param_buf.pos_y))
    {
        result = -EFAULT;
        goto end;
    }
    if(0 != drp_param_change16(crop_param_buf.obj.address, offset_add1 + DRP_PARAM_IMG_IWIDTH, crop_param_buf.img_owidth))
    {
        result = -EFAULT;
        goto end;
    }
    if(0 != drp_param_change16(crop_param_buf.obj.address, offset_add1 + DRP_PARAM_IMG_IHEIGHT, crop_param_buf.img_oheight))
    {
        result = -EFAULT;
        goto end;
    }

    goto end;
end:
    if((-EBUSY != result) || (-ERESTART != result))
    {
        up(&rw_sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}
static long drpai_ioctl_prepost_inaddr(struct file *filp, unsigned int cmd, unsigned long arg)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    long result = 0;
    struct drpai_priv *priv = drpai_priv;
    struct drpai_rw_status *drpai_rw_status = filp->private_data;
    drpai_inout_t inout_param_buf;
    char buf[DRP_PARAM_MAX_LINE_LENGTH];
    char *ptr, *prev_ptr;
    unsigned long flags;
    unsigned long offset_add;

    /* Check the internal state of DRP-AI */
    spin_lock_irqsave(&priv->lock, flags);
    if(DRPAI_STATUS_RUN == priv->drpai_status.status)
    {
        spin_unlock_irqrestore(&priv->lock, flags);
        result = -EBUSY;
        goto end;
    }
    spin_unlock_irqrestore(&priv->lock, flags);

    if(unlikely(down_trylock(&rw_sem)))
    {
        result = -ERESTART;
        goto end;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw1:%d\n", __func__, current->pid, drpai_rw_status->rw_status);

    /* Check NULL */
    if (NULL == arg)
    {
        result = -EINVAL;
        goto end;
    }
    if((NULL == drpai_rw_status->param_info) ||
       (DRPAI_STATUS_ASSIGN_PARAM == drpai_rw_status->rw_status) ||
       (DRPAI_STATUS_WRITE_PARAM  == drpai_rw_status->rw_status))
    {
        result = -EACCES;
        goto end;
    }
    /* Copy arguments from user space to kernel space */
    if (copy_from_user(&inout_param_buf, (void __user *)arg, sizeof(drpai_inout_t)))
    {
        result = -EFAULT;
        goto end;
    }
    /* Check if there is in drpai dedicated area */
    if((drpai_rw_status->assign_param.obj.address != inout_param_buf.obj.address) ||
       (drpai_rw_status->assign_param.obj.size != inout_param_buf.obj.size))
    {
        result = -EINVAL;
        goto end;
    }

    /* Search argument */
    ptr = drpai_rw_status->param_info;
    do
    {
        /* Save current pointer */
        prev_ptr = ptr;

        /* Get 1 line */
        ptr = strchr(ptr, '\n');
        if(NULL == ptr) {
            result = -EFAULT;
            goto end;
        }
        ptr += 1;
        if(ptr - prev_ptr >= DRP_PARAM_MAX_LINE_LENGTH)
        {
            result = -EFAULT;
            goto end;
        }

        /* Copy only 1line to buffer */
        memset(buf, 0, sizeof(buf));
        strncpy(buf, prev_ptr, ptr - prev_ptr);

        /* Check if there is DRP_PARAM_ATTR_PROP_INPUT in this line */
        if(NULL != strstr(buf, DRP_PARAM_ATTR_PROP_INPUT))
        {
            if(NULL != strstr(buf, inout_param_buf.name))
            {
                if(0 != get_param_attr(buf, DRP_PARAM_ATTR_OFFSET_ADD, &offset_add))
                {
                    result = -EFAULT;
                    goto end;
                }
                break;
            }
        }
    } while (ptr);

    DRPAI_DEBUG_PRINT("[%s](pid %d) offset_add=%d\n", __func__, current->pid, offset_add);

    /* Check if input is in linux memory region */
    if(0 != drpai_flush_dcache_input_area(&inout_param_buf.data))
    {
        result = -EFAULT;
        goto end;
    }
    /* Change parameters of drp_param.bin to value specified by user app. */
    if(0 != drp_param_change32(inout_param_buf.obj.address, offset_add + DRP_PARAM_raddr, inout_param_buf.data.address))
    {
        result = -EFAULT;
        goto end;
    }
    atomic_set(&drpai_rw_status->inout_flag, 1);

    goto end;
end:
    if((-EBUSY != result) || (-ERESTART != result))
    {
        up(&rw_sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status_rw2:%d\n", __func__, current->pid, drpai_rw_status->rw_status);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

static int drpai_drp_cpg_init(void)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int result;
    struct drpai_priv *priv = drpai_priv;
    unsigned long flags;
    int r_data;
    int32_t i = 0;
    bool is_stop = false;

    /* Access clock interface */
    clk_prepare_enable(priv->clk_int);
    clk_prepare_enable(priv->clk_aclk_drp);
    clk_prepare_enable(priv->clk_mclk);
    clk_prepare_enable(priv->clk_dclkin);
    clk_prepare_enable(priv->clk_aclk);

    r_data = reset_control_status(priv->rstc);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) drp reset_control_status before %d \n", __func__, current->pid, r_data);
    
    /* Access reset controller interface */
    reset_control_reset(priv->rstc);

    /* Check reset status */
    i = 0;
    while((RST_MAX_TIMEOUT > i) && (false == is_stop))
    {
        udelay(1);
        i++;
        r_data = reset_control_status(priv->rstc);
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) drp reset_control_status %d \n", __func__, current->pid, r_data);
        if(1 == r_data)
        {
            is_stop = true;
            break;
        }
    }

    i = 0;
    while((RST_MAX_TIMEOUT > i) && (false == is_stop))
    {
        usleep_range(100, 200);
        i++;
        r_data = reset_control_status(priv->rstc);
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) drp reset_control_status %d \n", __func__, current->pid, r_data);
        if(1 == r_data)
        {
            is_stop = true;
            break;
        }
    }

    if(true == is_stop)
    {
        result =  R_DRPAI_SUCCESS;
    }
    else
    {
        result = R_DRPAI_ERR_RESET;
        DRPAI_DEBUG_PRINT(KERN_INFO "%s: CPG Reset failed. Reset Control Status: %d\n", __func__,  r_data);
    }

    goto end;
end:
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

/* ISP */
static int drpai_drp_config_init(void)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int result;
    struct drpai_priv *priv = drpai_priv;
    struct device *dev = &priv->pdev->dev;
    int i;

    /* Get driver workspace from Linux CMA */
    p_dmabuf_vaddr = dma_alloc_coherent(dev, DRPAI_CMA_SIZE, &p_dmabuf_phyaddr, GFP_DMA);
    if (NULL == p_dmabuf_vaddr)
    {
        /* Error -ENOMEM */
        result = -1;
        goto end;
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) dmabuf:0x%08X, dmaphys:0x%08X\n", __func__, current->pid, p_dmabuf_vaddr, p_dmabuf_phyaddr);

    /* 64bytes alignment adjustment */
    if (0 != (p_dmabuf_phyaddr & DRPAI_64BYTE_ALIGN))
    {
        p_dmabuf_vaddr = p_dmabuf_vaddr + (0x40 - (p_dmabuf_phyaddr & DRPAI_64BYTE_ALIGN));
        p_dmabuf_phyaddr = p_dmabuf_phyaddr + (0x40 - (p_dmabuf_phyaddr & DRPAI_64BYTE_ALIGN));
    }

    /* Deploy drp_single_desc */
    memcpy(p_dmabuf_vaddr, &drp_single_desc_bin[0], sizeof(drp_single_desc_bin));

    __flush_dcache_area(p_dmabuf_vaddr, DRPAI_CMA_SIZE);

    result = R_DRPAI_SUCCESS;

    goto end;
end:
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

static void drpai_drp_config_uninit(void)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    struct drpai_priv *priv = drpai_priv;
    struct device *dev = &priv->pdev->dev;

    dma_free_coherent(dev, DRPAI_CMA_SIZE, p_dmabuf_vaddr, p_dmabuf_phyaddr);
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
}

int drpai_open_k(void)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);
    
    int result = 0;
    struct drpai_priv *priv = drpai_priv;
    unsigned long flags;
    
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:   %d\n", __func__, current->pid, priv->drpai_status.status);

    if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT))) 
    {
        result = -ETIMEDOUT;
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) ETIMEDOUT.\n", __func__, current->pid);
        goto end;
    }  

    if(drpai_drp_config_init())
    {
        result = -ENOMEM;
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) ENOMEM.\n", __func__, current->pid);
        goto end;
    }

    if(likely(1 == refcount_read(&priv->count)))
    {
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) Initialize DRP-AI\n", __func__, current->pid);

        /* CPG Reset */
        if(drpai_drp_cpg_init())
        {
            result = -EIO;
            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) DRP CPG HW error EIO.\n", __func__, current->pid);
            goto end;
        }

        /* Initialize DRP-AI */
        drpai_init_device(0);

        /* Reset DRP-AI */
        if(R_DRPAI_SUCCESS != drpai_reset_device(0))
        {
            result = -EIO;
            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) DRP HW error EIO.\n", __func__, current->pid);
            goto end;
        }

        /* Initialize DRP-AI */
        drpai_init_device(0);

        /* INIT -> IDLE */
        spin_lock_irqsave(&priv->lock, flags);
        priv->drpai_status.status = DRPAI_STATUS_IDLE;
        spin_unlock_irqrestore(&priv->lock, flags);
    }

    /* Increment reference count */
    refcount_inc(&priv->count);

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:%d\n", __func__, current->pid, priv->drpai_status.status);
    goto end;
end:
    if(-ETIMEDOUT != result)
    {
        /* Return semaphore when no ETIMEOUT */
        up(&priv->sem);
    }
    if(-EIO == result)
    {
        /* Release Linux CMA when hardware error */
        drpai_drp_config_uninit();
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

int drpai_close_k(void)
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int result = 0;
    struct drpai_priv *priv = drpai_priv;
    unsigned long flags;

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:   %d\n", __func__, current->pid, priv->drpai_status.status);

    if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT))) 
    {
        result = -ETIMEDOUT;
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) ETIMEOUT.\n", __func__, current->pid);
        goto end;
    } 

    /* Check status */
    spin_lock_irqsave(&priv->lock, flags);
    if (DRPAI_STATUS_INIT == priv->drpai_status.status)
    {
        spin_unlock_irqrestore(&priv->lock, flags);
        result = -EACCES;        
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) EACCES.\n", __func__, current->pid);
        goto end;
    }
    spin_unlock_irqrestore(&priv->lock, flags);

    if(2 == refcount_read(&priv->count))
    {
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start DRP-AI reset\n", __func__, current->pid);
        if(R_DRPAI_SUCCESS != drpai_reset_device(0))
        {
            result = -EIO;              
            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) EIO.\n", __func__, current->pid);
            goto end;
        }

        // CPG clock disable
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) CPG clock disable\n", __func__, current->pid);
        clk_disable_unprepare(priv->clk_int);
        clk_disable_unprepare(priv->clk_aclk_drp);
        clk_disable_unprepare(priv->clk_mclk);
        clk_disable_unprepare(priv->clk_dclkin);
        clk_disable_unprepare(priv->clk_aclk);  

        /* IDLE -> INIT */
        /* RUN  -> INIT */
        spin_lock_irqsave(&priv->lock, flags);
        priv->drpai_status.status = DRPAI_STATUS_INIT;
        priv->drpai_status.err    = DRPAI_ERRINFO_SUCCESS;
        spin_unlock_irqrestore(&priv->lock, flags);

        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) done DRP-AI reset\n", __func__, current->pid);
    }
    /* Release Linux CMA */
    drpai_drp_config_uninit();

    /* Decrement referenece count*/
    refcount_dec(&priv->count);

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:   %d\n", __func__, current->pid, priv->drpai_status.status);

    goto end;
end:
    if(-ETIMEDOUT != result)
    {
        /* Return semaphore when no ETIMEOUT */
        up(&priv->sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);

    return result;
}

int drpai_start_k(drpai_data_t *arg, void (*isp_finish)(int result))
{
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) start.\n", __func__, current->pid);

    int result = 0;
    int wait_ret = 0;
    volatile void *p_drp_param = 0;
    volatile void *p_input = 0;
    volatile void *p_drpcfg = 0;
    volatile void *p_weight = 0;
    struct drpai_priv *priv = drpai_priv;
    drpai_data_t *proc_k;
    unsigned long flags;
    int i;

    if(unlikely(down_timeout(&priv->sem, MAX_SEM_TIMEOUT))) 
    {
        result = -ETIMEDOUT;
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) ETIMEOUT.\n", __func__, current->pid);
        goto end;
    }

    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status1:   %d\n", __func__, current->pid, priv->drpai_status.status);

    /* Check H/W Error */
    spin_lock_irqsave(&priv->lock, flags);
    if ((DRPAI_ERRINFO_DRP_ERR == priv->drpai_status.err) || (DRPAI_ERRINFO_AIMAC_ERR == priv->drpai_status.err))
    {
        spin_unlock_irqrestore(&priv->lock, flags);
        result = -EIO;   
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) EIO.\n", __func__, current->pid);
        goto end;
    }
    spin_unlock_irqrestore(&priv->lock, flags);

    /* Check status */
    spin_lock_irqsave(&priv->lock, flags);
    if (DRPAI_STATUS_RUN == priv->drpai_status.status)
    {
        spin_unlock_irqrestore(&priv->lock, flags);
        result = -EBUSY;   
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) EBUSY.\n", __func__, current->pid);
        goto end;
    }
    spin_unlock_irqrestore(&priv->lock, flags);

    /* Check status */
    spin_lock_irqsave(&priv->lock, flags);
    if (DRPAI_STATUS_INIT == priv->drpai_status.status)
    {
        spin_unlock_irqrestore(&priv->lock, flags);
        result = -EACCES;   
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) EACCES.\n", __func__, current->pid);
        goto end;
    }
    spin_unlock_irqrestore(&priv->lock, flags);

    /* Check NULL */
    if (NULL == isp_finish)
    {
        result = -EINVAL; 
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) EINVAL NULL function pointer.\n", __func__, current->pid);
        goto end;
    }
    /* Referring the call back function info from ISP Lib */
    priv->isp_finish_loc = isp_finish;  

    /* Check NULL */
    if (NULL == arg)
    {
        result = -EINVAL; 
        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) EINVAL NULL argument pointer.\n", __func__, current->pid);
        goto end;
    }
    /* Referring the obj file info from ISP Lib */
    proc_k = arg;

    spin_lock_irqsave(&priv->lock, flags);
    odif_intcnto.ch0 = 0;
    odif_intcnto.ch1 = 0;
    odif_intcnto.ch2 = 0;
    odif_intcnto.ch3 = 0;
    spin_unlock_irqrestore(&priv->lock, flags);

    DRPAI_DEBUG_PRINT(KERN_INFO "ODIF_INTCNTO0 : 0x%08X\n", odif_intcnto.ch0);
    DRPAI_DEBUG_PRINT(KERN_INFO "ODIF_INTCNTO1 : 0x%08X\n", odif_intcnto.ch1);
    DRPAI_DEBUG_PRINT(KERN_INFO "ODIF_INTCNTO2 : 0x%08X\n", odif_intcnto.ch2);
    DRPAI_DEBUG_PRINT(KERN_INFO "ODIF_INTCNTO3 : 0x%08X\n", odif_intcnto.ch3);

    /* Check Argument 64-byte*/
    for (i = 0; i < 2; i++)
    {
        if (0 != (proc_k[i].address & DRPAI_64BYTE_ALIGN))
        {
            result = -EINVAL; 
            DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) EINVAL argument. Not 64-byte aligned.\n", __func__, current->pid);
            goto end;
        }
    }

    for (i = 0; i < 1; i++)
    {
        /* DRPcfg address and size settings */
        *(uint32_t*)(p_dmabuf_vaddr + (DRPAI_SGL_DRP_DESC_SIZE * i) + 4) = proc_k[i * 2].address;
        *(uint32_t*)(p_dmabuf_vaddr + (DRPAI_SGL_DRP_DESC_SIZE * i) + 8) = proc_k[i * 2].size;

        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) cfg_address:0x%08X, cfg_size:0x%08X\n", __func__, current->pid, proc_k[i * 2].address, proc_k[i * 2].size);

        /* DRP param address and size settings */
        *(uint32_t*)(p_dmabuf_vaddr + (DRPAI_SGL_DRP_DESC_SIZE * i) + 36) = proc_k[i * 2 + 1].address;
        *(uint32_t*)(p_dmabuf_vaddr + (DRPAI_SGL_DRP_DESC_SIZE * i) + 40) = proc_k[i * 2 + 1].size;

        DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) parm_address:0x%08X, parm_size:0x%08X\n", __func__, current->pid, proc_k[i * 2 + 1].address, proc_k[i * 2 + 1].size);

    }
    __flush_dcache_area(p_dmabuf_vaddr, DRPAI_CMA_SIZE);

    spin_lock_irqsave(&priv->lock, flags);
    /* Init drpai_status.err */
    priv->drpai_status.err = DRPAI_ERRINFO_SUCCESS;

    /* IDLE -> RUN */
    priv->drpai_status.status = DRPAI_STATUS_RUN;
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) status2:   %d\n", __func__, current->pid, priv->drpai_status.status);
    spin_unlock_irqrestore(&priv->lock, flags);

    /* Kick */
    (void)R_DRPAI_DRP_Start(0, p_dmabuf_phyaddr);
    (void)R_DRPAI_AIMAC_Start(0, p_dmabuf_phyaddr + (DRPAI_SGL_DRP_DESC_SIZE));

    goto end;
end:
    if(-ETIMEDOUT != result)
    {
        /* Return semaphore when no ETIMEOUT */
        up(&priv->sem);
    }
    DRPAI_DEBUG_PRINT(KERN_INFO "[%s](pid %d) end.\n", __func__, current->pid);
    
    return result;
}

/* Public to Kernel space */
EXPORT_SYMBOL(drpai_open_k);
EXPORT_SYMBOL(drpai_close_k);
EXPORT_SYMBOL(drpai_start_k);
/* ISP */

module_platform_driver(drpai_platform_driver);
MODULE_DEVICE_TABLE(of, drpai_match);
MODULE_DESCRIPTION("RZ/V2L DRP-AI driver");
MODULE_AUTHOR("Renesas Electronics Corporation");
MODULE_LICENSE("GPL v2");

