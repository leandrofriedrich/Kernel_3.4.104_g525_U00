/* drivers/input/accelerometer/gs_mma8452.c
 *
 * Copyright (C) 2010-2011  Huawei.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */


#include <linux/module.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include "linux/hardware_self_adapt.h"
#include <linux/slab.h>
#include <mach/vreg.h>

#ifdef CONFIG_HUAWEI_HW_DEV_DCT
#include <linux/hw_dev_dec.h>
#endif
#include <linux/sensors.h>
//#define GS_DEBUG
//#undef GS_DEBUG 

#ifdef GS_DEBUG
#define GS_DEBUG(fmt, args...) printk(KERN_ERR fmt, ##args)
#else
#define GS_DEBUG(fmt, args...)
#endif

#define GS_POLLING   1

#define MMA8452_DRV_NAME	"gs_mma8452"
#define MMA8452_I2C_ADDR	0x1C
#define MMA8452_ID		0x2A

/* register enum for mma8452 registers */
enum {
	MMA8452_STATUS = 0x00,
	MMA8452_OUT_X_MSB,
	MMA8452_OUT_X_LSB,
	MMA8452_OUT_Y_MSB,
	MMA8452_OUT_Y_LSB,
	MMA8452_OUT_Z_MSB,
	MMA8452_OUT_Z_LSB,
	
	MMA8452_SYSMOD = 0x0B,
	MMA8452_INT_SOURCE,
	MMA8452_WHO_AM_I,
	MMA8452_XYZ_DATA_CFG,
	MMA8452_HP_FILTER_CUTOFF,
	
	MMA8452_PL_STATUS,
	MMA8452_PL_CFG,
	MMA8452_PL_COUNT,
	MMA8452_PL_BF_ZCOMP,
	MMA8452_PL_P_L_THS_REG,
	
	MMA8452_FF_MT_CFG,
	MMA8452_FF_MT_SRC,
	MMA8452_FF_MT_THS,
	MMA8452_FF_MT_COUNT,

	MMA8452_TRANSIENT_CFG = 0x1D,
	MMA8452_TRANSIENT_SRC,
	MMA8452_TRANSIENT_THS,
	MMA8452_TRANSIENT_COUNT,
	
	MMA8452_PULSE_CFG,
	MMA8452_PULSE_SRC,
	MMA8452_PULSE_THSX,
	MMA8452_PULSE_THSY,
	MMA8452_PULSE_THSZ,
	MMA8452_PULSE_TMLT,
	MMA8452_PULSE_LTCY,
	MMA8452_PULSE_WIND,
	
	MMA8452_ASLP_COUNT,
	MMA8452_CTRL_REG1,
	MMA8452_CTRL_REG2,
	MMA8452_CTRL_REG3,
	MMA8452_CTRL_REG4,
	MMA8452_CTRL_REG5,
	
	MMA8452_OFF_X,
	MMA8452_OFF_Y,
	MMA8452_OFF_Z,
	
	MMA8452_REG_END,
};

enum {
	MODE_2G = 0,
	MODE_4G,
	MODE_8G,
};

#define MG_PER_SAMPLE				720            /*HAL: 720=1g*/                       
#define FILTER_SAMPLE_NUMBER		4096           /*256LSB =1g*/  
#define	GPIO_INT1                   19
#define GPIO_INT2                   20
#define GS_TIMRER                    (1000)           /*1000ms*/

#define ECS_IOCTL_READ_ACCEL_XYZ			_IOR(0xA1, 0x06, char[3])
#define ECS_IOCTL_APP_SET_DELAY 			_IOW(0xA1, 0x18, short)
#define ECS_IOCTL_APP_GET_DELAY 			_IOR(0xA1, 0x30, short)
#define ECS_IOCTL_APP_SET_AFLAG 			_IOW(0xA1, 0x13, short)
#define ECS_IOCTL_APP_GET_AFLAG				_IOR(0xA1, 0x14, short)
#define ECS_IOCTL_READ_DEVICEID				_IOR(0xA1, 0x31, char[20])

struct gs_data {
	uint16_t addr;
	struct i2c_client *client;
	struct input_dev *input_dev;
	int use_irq;
	struct mutex  mlock;
	struct hrtimer timer;
	struct work_struct  work;	
	uint32_t flags;
	struct early_suspend early_suspend;
};
/* DATA_CTRL_REG: controls the output data rate of the part */
#define ODR6_25F        0x30   //Period  160ms
#define ODR12_5F        0x28   //Period   80ms
#define ODR50F          0x20   //Period   20ms
#define ODR100F         0x18   //Period   10ms
#define ODR200F         0x10   //Period    5ms
#define ODR400F         0x08   //Period  2.5ms
#define ODR800F         0x00   //Period 1.25ms
#define ODR_MASK        0xC7
#define ACTIVE          0x01   //active bit
/*This is the classcial Delay_time from framework and the units is ms*/
#define DELAY_FASTEST  10
#define DELAY_GAME     20
#define DELAY_UI       68
#define DELAY_NORMAL  200
#define DELAY_ERROR 10000
/*
 * The following table lists the maximum appropriate poll interval for each
 * available output data rate.
 * Make sure the status still have proper timer.
 */
 
static const struct {
	unsigned int cutoff;
	u8 mask;
} mma_odr_table[] = {
	{ DELAY_FASTEST, ODR200F},
	{ DELAY_GAME,    ODR100F},
	{ DELAY_UI,       ODR50F},
	{ DELAY_NORMAL, ODR6_25F},
	{ DELAY_ERROR,  ODR6_25F},
};
static struct gs_data  *this_gs_data;

static struct workqueue_struct *gs_wq;
static signed short compass_sensor_data[3];
static char gs_device_id[] = MMA8452_DRV_NAME;

extern struct input_dev *sensor_dev;
static atomic_t mma_status_flag;
#ifdef CONFIG_MELFAS_UPDATE_TS_FIRMWARE
extern struct gs_data *TS_updateFW_gs_data;
#endif

static int accel_delay = GS_TIMRER;     /*1s*/

static atomic_t a_flag;

#ifdef CONFIG_HAS_EARLYSUSPEND
static void gs_early_suspend(struct early_suspend *h);
static void gs_late_resume(struct early_suspend *h);
#endif
static compass_gs_position_type  compass_gs_position=COMPASS_TOP_GS_TOP;
static inline int reg_read(struct gs_data *gs , int reg);
static int mma8452_debug_mask;
module_param_named(mma8452_debug, mma8452_debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);

#define mma8452_DBG(x...) do {\
    if (mma8452_debug_mask) \
        printk(KERN_DEBUG x);\
    } while (0)
#define mma8452_PRINT_PER_TIMES 100
unsigned int mma8452_times = 0;

void mma8452_print_debug(int start_reg,int end_reg)
{
        int reg, ret;

        for(reg = start_reg ; reg <= end_reg ; reg ++)
        {                
			/* read reg value */
            ret = reg_read(this_gs_data,reg);
			/* print reg info */
            mma8452_DBG("mma8452 reg 0x%x values 0x%x\n",reg,ret);
         }

}

/**************************************************************************************/
static inline int reg_read(struct gs_data *gs , int reg)
{
    int val;

    mutex_lock(&gs->mlock);

    val = i2c_smbus_read_byte_data(gs->client, reg);
    if (val < 0)
    {
        printk(KERN_ERR "MMA8452 chip i2c %s failed! reg=0x%x, value=0x%x\n", __FUNCTION__, reg, val);
    }

    mutex_unlock(&gs->mlock);

    return val;
}
static inline int reg_write(struct gs_data *gs, int reg, uint8_t val)
{
    int ret;

    mutex_lock(&gs->mlock);
    ret = i2c_smbus_write_byte_data(gs->client, reg, val);
    if(ret < 0)
    {
        printk(KERN_ERR "MMA8452 chip i2c %s failed! reg=0x%x, value=0x%x, ret=%d\n", __FUNCTION__, reg, val, ret);
    }
    mutex_unlock(&gs->mlock);

    return ret;
}

/**************************************************************************************/

static int gs_data_to_compass(signed short accel_data [3])
{
	memset((void*)accel_data, 0, sizeof(accel_data));
	accel_data[0]=compass_sensor_data[0];
	accel_data[1]=compass_sensor_data[1];
	accel_data[2]=compass_sensor_data[2];
	return 0;
}
static void gs_mma_update_odr(struct gs_data  *gs)
{
	int i;
	int ret = 0;
	short time_reg;
	for (i = 0; i < ARRAY_SIZE(mma_odr_table); i++) 
	{
		time_reg = mma_odr_table[i].mask;
		if (accel_delay <= mma_odr_table[i].cutoff)
		{
			accel_delay = mma_odr_table[i].cutoff;
			break;
		}
	}
	printk("Update G-sensor Odr ,delay_time is %d\n",accel_delay);
	ret  = reg_write(gs, MMA8452_CTRL_REG1, 0x00);
	time_reg = time_reg | ACTIVE;
	ret  = reg_write(gs, MMA8452_CTRL_REG1, time_reg);
	if(ret < 0)
	{
		printk("register write failed is gs_mma_update_odr\n ");
	}
}
/**************************************************************************************/

static int gs_mma8452_open(struct inode *inode, struct file *file)
{	
	/*gs active mode, modify the adc frequency to 50HZ*/
	reg_write(this_gs_data, MMA8452_CTRL_REG1, 0x21);
	atomic_set(&mma_status_flag, GS_RESUME);
	if (this_gs_data->use_irq)
		enable_irq(this_gs_data->client->irq);
	else
		hrtimer_start(&this_gs_data->timer, ktime_set(0, 500000000), HRTIMER_MODE_REL);

	return nonseekable_open(inode, file);
}

static int gs_mma8452_release(struct inode *inode, struct file *file)
{
	/*gs standby mode*/
	reg_write(this_gs_data, MMA8452_CTRL_REG1, 0x20); 
	atomic_set(&mma_status_flag, GS_SUSPEND);
	if (this_gs_data->use_irq)
		disable_irq(this_gs_data->client->irq);
	else
		hrtimer_cancel(&this_gs_data->timer);

	accel_delay = GS_TIMRER;	  

	return 0;
}

static long
gs_mma8452_ioctl(struct file *file, unsigned int cmd,
	   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	signed short accel_buf[3];
	short flag;
	
	switch (cmd) 
	{
		case ECS_IOCTL_APP_SET_AFLAG:     /*set open acceleration sensor flag*/
			if (copy_from_user(&flag, argp, sizeof(flag)))
				return -EFAULT;
			break;
				
		case ECS_IOCTL_APP_SET_DELAY:
			if (copy_from_user(&flag, argp, sizeof(flag)))
				return -EFAULT;
			break;
		
			default:
				break;
	}
	switch (cmd) 
	{
		case ECS_IOCTL_APP_SET_AFLAG:
			atomic_set(&a_flag, flag);
			break;
			
		case ECS_IOCTL_APP_GET_AFLAG:  /*get open acceleration sensor flag*/
			flag = atomic_read(&a_flag);
			break;
			
		case ECS_IOCTL_APP_SET_DELAY:
			if(flag)
				accel_delay = flag;
			else
				accel_delay = 10;   /*10ms*/
			gs_mma_update_odr(this_gs_data);
			break;
			
		case ECS_IOCTL_APP_GET_DELAY:
			flag = accel_delay;
			break;
			
		case ECS_IOCTL_READ_ACCEL_XYZ:
			gs_data_to_compass(accel_buf);
			break;
			
		default:
			break;
	}
	switch (cmd) 
	{
		case ECS_IOCTL_APP_GET_AFLAG:
			if (copy_to_user(argp, &flag, sizeof(flag)))
				return -EFAULT;
			break;

		case ECS_IOCTL_APP_GET_DELAY:
			if (copy_to_user(argp, &flag, sizeof(flag)))
				return -EFAULT;
			break;
			
		case ECS_IOCTL_READ_ACCEL_XYZ:
			if (copy_to_user(argp, &accel_buf, sizeof(accel_buf)))
				return -EFAULT;
			break;
			
		case ECS_IOCTL_READ_DEVICEID:
			if (copy_to_user(argp, gs_device_id, sizeof(gs_device_id)))
				return -EFAULT;
			break;
			
		default:
			break;
	}
	return 0;
}

static struct file_operations gs_mma8452_fops = {
	.owner = THIS_MODULE,
	.open = gs_mma8452_open,
	.release = gs_mma8452_release,
	.unlocked_ioctl = gs_mma8452_ioctl,
};

static struct miscdevice gsensor_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "accel",
	.fops = &gs_mma8452_fops,
};

static void gs_work_func(struct work_struct *work)
{
    int status = 0; 
    int x = 0;
    int y = 0;
    int z = 0;
    u8 udata[2]={0};
    
    struct gs_data *gs = container_of(work, struct gs_data, work);
    int sesc = accel_delay/1000;
    int nsesc = (accel_delay%1000)*1000000;
       
    status = reg_read(gs, MMA8452_STATUS ); /* read status */
    
    if(status & (1<<3))
    {
        udata[0] = reg_read(gs, MMA8452_OUT_X_MSB ); /* read status */
        GS_DEBUG("%s:A_x h 0x%x \n", __FUNCTION__, udata[0]);
        udata[1] = reg_read(gs, MMA8452_OUT_X_LSB ); /* read status */
        GS_DEBUG("%s:A_x l 0x%x \n", __FUNCTION__, udata[1]);
        x = ((udata[0])<<4)|udata[1]>>4;

        udata[0]= 0;
        udata[1]= 0;
        udata[0] = reg_read(gs, MMA8452_OUT_Y_MSB ); /* read status */
        GS_DEBUG("%s:A_y h 0x%x \n", __FUNCTION__, udata[0]);
        udata[1] = reg_read(gs, MMA8452_OUT_Y_LSB ); /* read status */
        GS_DEBUG("%s:A_y l 0x%x \n", __FUNCTION__, udata[1]);
        y = ((udata[0])<<4)|udata[1]>>4;

        udata[0]= 0;
        udata[1]= 0;
        udata[0] = reg_read(gs, MMA8452_OUT_Z_MSB ); /* read status */
        GS_DEBUG("%s:A_z h 0x%x \n", __FUNCTION__, udata[0]);
        udata[1] = reg_read(gs, MMA8452_OUT_Y_LSB ); /* read status */
        GS_DEBUG("%s:A_z l 0x%x \n", __FUNCTION__, udata[1]);
        z = ((udata[0])<<4)|udata[1]>>4;
        
        mma8452_DBG("Gs_mma8452:A  x:%d y:%d z:%d sec:%d nsec:%d\n", x, y, z, sesc, nsesc);
     
        if(x&0x800)/**/
        {
            x -= 4096;      /*2??s complement 12-bit numbers*/  
        }
                    
        if(y&0x800)/**/
        {
            y -= 4096;      /*2??s complement 12-bit numbers*/ 
        }
    
        if(z&0x800)
        {
            z -= 4096;      /*2??s complement 12-bit numbers*/   
        }

        memset((void*)compass_sensor_data, 0, sizeof(compass_sensor_data));
        //compass_sensor_data[0]= -x;
        //compass_sensor_data[1]= -y;   
        //compass_sensor_data[2]= -z;
        
        /*(Decimal value/ 4096) * 4.0 g,For (0g ~+2.0g)*/   
        x = (MG_PER_SAMPLE*40*(s16)x)/FILTER_SAMPLE_NUMBER/10;           
        y = (MG_PER_SAMPLE*40*(s16)y)/FILTER_SAMPLE_NUMBER/10;
        z = (MG_PER_SAMPLE*40*(s16)z)/FILTER_SAMPLE_NUMBER/10;
    
        /*report different values by machines*/
        if((compass_gs_position==COMPASS_TOP_GS_BOTTOM)||(compass_gs_position==COMPASS_BOTTOM_GS_BOTTOM)||(compass_gs_position==COMPASS_NONE_GS_BOTTOM))
        {
            //inverse
            x *=(-1);
            y *=(-1);
        }       
        else
        {    
            /*
            if((compass_gs_position==0)||(compass_gs_position==2))
            */
            //obverse
            
            y *=(-1);
            z *=(-1);
        }

        input_report_abs(gs->input_dev, ABS_X, x);//cross x,y adapter hal sensors_akm8973.c         
        input_report_abs(gs->input_dev, ABS_Y, y);          
        input_report_abs(gs->input_dev, ABS_Z, z);
        input_sync(gs->input_dev);

        compass_sensor_data[0]= -x;
        compass_sensor_data[1]= y;  
        compass_sensor_data[2]= -z;
        

    }
    else
    {
        printk(KERN_ERR"%s, line %d:MMA8452_CTRL_REG1 is 0x%x ,status=0x%x\n",__func__, __LINE__,reg_read(gs, MMA8452_CTRL_REG1),status);
    }
    if(mma8452_debug_mask)
    {
        /* print reg info in such times */
        if(!(++mma8452_times%mma8452_PRINT_PER_TIMES))
        {
            /* count return to 0 */
            mma8452_times = 0;
            mma8452_print_debug(MMA8452_STATUS,MMA8452_OUT_Z_LSB);
            mma8452_print_debug(MMA8452_SYSMOD,MMA8452_FF_MT_COUNT);
            mma8452_print_debug(MMA8452_TRANSIENT_CFG,MMA8452_REG_END);
        }
    }
    if (gs->use_irq)
    {
        enable_irq(gs->client->irq);
    }
    else
    {
        if(GS_RESUME == atomic_read(&mma_status_flag))
            if (0 != hrtimer_start(&gs->timer, ktime_set(sesc, nsesc), HRTIMER_MODE_REL) )
                printk(KERN_ERR "%s, line %d: hrtimer_start fail! sec=%d, nsec=%d\n", __func__, __LINE__, sesc, nsesc);
    }
}


static enum hrtimer_restart gs_timer_func(struct hrtimer *timer)
{
	struct gs_data *gs = container_of(timer, struct gs_data, timer);		
	queue_work(gs_wq, &gs->work);
	//hrtimer_start(&gs->timer, ktime_set(0, 512500000), HRTIMER_MODE_REL);
	return HRTIMER_NORESTART;
}

#ifndef GS_POLLING 	
static irqreturn_t gs_irq_handler(int irq, void *dev_id)
{
	struct gs_data *gs = dev_id;
	disable_irq(gs->client->irq);
	queue_work(gs_wq, &gs->work);
	return IRQ_HANDLED;
}

static int gs_config_int_pin(void)
{
	int err;

	err = gpio_request(GPIO_INT1, "gpio_gs_int");
	if (err) 
	{
		printk(KERN_ERR "gpio_request failed for st gs int\n");
		return -1;
	}	

	err = gpio_configure(GPIO_INT1, GPIOF_INPUT | IRQF_TRIGGER_HIGH);
	if (err) 
	{
		gpio_free(GPIO_INT1);
		printk(KERN_ERR "gpio_config failed for gs int HIGH\n");
		return -1;
	}     

	return 0;
}

static void gs_free_int(void)
{
	gpio_free(GPIO_INT1);
}
#endif /*GS_POLLING*/

static int gs_probe(
	struct i2c_client *client, const struct i2c_device_id *id)
{	
	int ret = 0;
	s32 result = 0;
	struct gs_data *gs;
	struct gs_platform_data *pdata = NULL;
	/*delete 19 lines*/
	    
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		printk(KERN_ERR "gs_mma8452_probe: need I2C_FUNC_I2C\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}
	
	/*turn on the power*/
	pdata = client->dev.platform_data;
	if (pdata){
#ifdef CONFIG_ARCH_MSM7X30
		if(pdata->gs_power != NULL){
			ret = pdata->gs_power(IC_PM_ON);
			if(ret < 0 ){
				goto err_check_functionality_failed;
			}
		}
#endif
		if(pdata->adapt_fn != NULL){
			ret = pdata->adapt_fn();
			if(ret > 0){
				client->addr = pdata->slave_addr;//actual address
				printk(KERN_INFO "%s:change i2c addr to actrual address = %d\n", __FUNCTION__, pdata->slave_addr);
				if(client->addr == 0){
					printk(KERN_ERR "%s: bad i2c address = %d\n", __FUNCTION__, client->addr);
					ret = -EFAULT;
					goto err_power_failed;
				}
			}
		}
		if(pdata->get_compass_gs_position != NULL){
			compass_gs_position=pdata->get_compass_gs_position();
		}

		if(pdata->init_flag != NULL){
			if(*(pdata->init_flag)){
				printk(KERN_ERR "gs_mma8452 probe failed, because the othe gsensor has been probed.\n");
				ret = -ENODEV;
				goto err_power_failed;
			}
		}
	}
	
	printk(KERN_INFO "%s:check freescale mma8452 chip ID\n", __FUNCTION__);
	result = i2c_smbus_read_byte_data(client, MMA8452_WHO_AM_I);

	if (0 > result) {	//compare the address value 
		dev_err(&client->dev,"read chip ID 0x%x is not equal to 0x%x!\n", result,MMA8452_ID);
		printk(KERN_INFO "read chip ID failed\n");
		result = -ENODEV;
		goto err_power_failed;
	}
	
#ifndef   GS_POLLING 	
	ret = gs_config_int_pin();
	if(ret <0)
	{
		goto err_power_failed;
	}
#endif

	gs = kzalloc(sizeof(*gs), GFP_KERNEL);
	if (gs == NULL) {
		ret = -ENOMEM;
		goto err_alloc_data_failed;
	}
	mutex_init(&gs->mlock);

	INIT_WORK(&gs->work, gs_work_func);
	gs->client = client;
	i2c_set_clientdata(client, gs);

	/* Initialize the MMA8452 chip */
	ret = reg_write(gs, MMA8452_CTRL_REG2, 0x40);   /*software reset chip*/
	if (ret < 0) {
		printk(KERN_ERR "%s:i2c_smbus_write_byte_data failed\n", __FUNCTION__);
		/* fail? */
		goto err_detect_failed;
	}
	mdelay(2);
	ret |= reg_write(gs, MMA8452_CTRL_REG1, 0x20);    /*standby mode*/
	ret |= reg_write(gs, MMA8452_XYZ_DATA_CFG, MODE_2G);  /*Full scale range is 2g*/
	ret |= reg_write(gs, MMA8452_CTRL_REG2, 0); 
	ret |= reg_write(gs, MMA8452_CTRL_REG3, 0x03); /*Interrupt polarity ACTIVE high*/
	ret |= reg_write(gs, MMA8452_CTRL_REG4, 0); /* disable interrupt,default value is 0x00 */
	ret |= reg_write(gs, MMA8452_CTRL_REG5, 0); /* Interrupt is routed to INT2 pin,default value is 0x00 */
	if (ret < 0) {
		printk(KERN_ERR "%s:i2c Initialize the MMA8452 chip failed, err=%d\n",__FUNCTION__, ret);
		/* fail? */
		goto err_detect_failed;
	}
	atomic_set(&mma_status_flag, GS_SUSPEND);
	#ifdef CONFIG_HUAWEI_HW_DEV_DCT
	/* detect current device successful, set the flag as present */
	set_hw_dev_flag(DEV_I2C_G_SENSOR);
	#endif
	if (sensor_dev == NULL)
	{
		gs->input_dev = input_allocate_device();
		if (gs->input_dev == NULL) {
			ret = -ENOMEM;
			printk(KERN_ERR "%s: Failed to allocate input device\n",__FUNCTION__);
			goto err_input_dev_alloc_failed;
		}
		
		gs->input_dev->name = "sensors";
		sensor_dev = gs->input_dev;
		
	}else{
		gs->input_dev = sensor_dev;
	}
	
	gs->input_dev->id.vendor = GS_MMA8452;
	
	set_bit(EV_ABS,gs->input_dev->evbit);
	/* < DTS20111208XXXXX  liujinggang 20111208 begin */
	/* modify for ES-version*/
	input_set_abs_params(gs->input_dev, ABS_X, -11520, 11520, 0, 0);
	input_set_abs_params(gs->input_dev, ABS_Y, -11520, 11520, 0, 0);
	input_set_abs_params(gs->input_dev, ABS_Z, -11520, 11520, 0, 0);
	/* DTS20111208XXXXX  liujinggang 20111208 end > */
	set_bit(EV_SYN,gs->input_dev->evbit);

	gs->input_dev->id.bustype = BUS_I2C;
	
	input_set_drvdata(gs->input_dev, gs);
	
	ret = input_register_device(gs->input_dev);
	if (ret) {
		printk(KERN_ERR "gs_probe: Unable to register %s input device\n", gs->input_dev->name);
		goto err_input_register_device_failed;
	}
	
	ret = misc_register(&gsensor_device);
	if (ret) {
		printk(KERN_ERR "gs_probe: gsensor_device register failed\n");
		goto err_misc_device_register_failed;
	}

#ifndef   GS_POLLING 
	if (client->irq) {
		ret = request_irq(client->irq, gs_irq_handler, 0, client->name, gs);
		
		if (ret == 0)
			gs->use_irq = 1;
		else
			dev_err(&client->dev, "request_irq failed\n");
	}
#endif 

	if (!gs->use_irq) {
		hrtimer_init(&gs->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		gs->timer.function = gs_timer_func;
	}
	
#ifdef CONFIG_HAS_EARLYSUSPEND
	gs->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	gs->early_suspend.suspend = gs_early_suspend;
	gs->early_suspend.resume = gs_late_resume;
	register_early_suspend(&gs->early_suspend);
#endif

    gs_wq = create_singlethread_workqueue("gs_wq");
    if (!gs_wq)
    {
        ret = -ENOMEM;
        printk(KERN_ERR "%s, line %d: create_singlethread_workqueue fail!\n", __func__, __LINE__);
        goto err_create_workqueue_failed;
    }
    this_gs_data =gs;

    //set_st303_gs_support(true);
    if(pdata && pdata->init_flag)
        *(pdata->init_flag) = 1;
    ret = set_sensor_input(ACC, gs->input_dev->dev.kobj.name);
    if (ret) {
        dev_err(&client->dev, "%s set_sensor_input failed\n", __func__);
        goto err_create_workqueue_failed;
    }
    printk(KERN_INFO "gs_probe: Start MMA8452  in %s mode\n", gs->use_irq ? "interrupt" : "polling");

#ifdef CONFIG_MELFAS_UPDATE_TS_FIRMWARE
    TS_updateFW_gs_data = this_gs_data;
#endif

    set_sensors_list(G_SENSOR);
    return 0;

err_create_workqueue_failed:
#ifdef CONFIG_HAS_EARLYSUSPEND
    unregister_early_suspend(&gs->early_suspend);
#endif

    if (gs->use_irq)
    {
        free_irq(client->irq, gs);
    }
    else
    {
        hrtimer_cancel(&gs->timer);
    }
err_misc_device_register_failed:
	misc_deregister(&gsensor_device);

err_input_register_device_failed:
	input_free_device(gs->input_dev);

err_input_dev_alloc_failed:
err_detect_failed:
	kfree(gs);
err_alloc_data_failed:
#ifndef   GS_POLLING 
	gs_free_int();
#endif
/*turn down the power*/
err_power_failed:
#ifdef CONFIG_ARCH_MSM7X30
	if(pdata->gs_power != NULL){
		pdata->gs_power(IC_PM_OFF);
	}
#endif
err_check_functionality_failed:
	return ret;
}

static int gs_remove(struct i2c_client *client)
{
	struct gs_data *gs = i2c_get_clientdata(client);
#ifdef CONFIG_HAS_EARLYSUSPEND
    unregister_early_suspend(&gs->early_suspend);
#endif
	if (gs->use_irq)
		free_irq(client->irq, gs);
	else
		hrtimer_cancel(&gs->timer);
	misc_deregister(&gsensor_device);
	input_unregister_device(gs->input_dev);
	kfree(gs);
	return 0;
}

static int gs_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int ret;
	struct gs_data *gs = i2c_get_clientdata(client);
	atomic_set(&mma_status_flag, GS_SUSPEND);
	if (gs->use_irq)
		disable_irq(client->irq);
	else
		hrtimer_cancel(&gs->timer);
	ret = cancel_work_sync(&gs->work);
	if (ret && gs->use_irq) 
		enable_irq(client->irq);
	
	reg_write(gs, MMA8452_CTRL_REG1, 0x20); /* power down */
	return 0;
}

static int gs_resume(struct i2c_client *client)
{
	struct gs_data *gs = i2c_get_clientdata(client);
	/*gs active mode, modify the adc frequency to 200HZ until update ODR*/
	reg_write(gs, MMA8452_CTRL_REG1, ODR200F | ACTIVE);
	atomic_set(&mma_status_flag, GS_RESUME);
	if (!gs->use_irq)
		hrtimer_start(&gs->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	else
		enable_irq(client->irq);
	

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void gs_early_suspend(struct early_suspend *h)
{
	struct gs_data *gs;
	gs = container_of(h, struct gs_data, early_suspend);
	gs_suspend(gs->client, PMSG_SUSPEND);
}

static void gs_late_resume(struct early_suspend *h)
{
	struct gs_data *gs;
	gs = container_of(h, struct gs_data, early_suspend);
	gs_resume(gs->client);
}
#endif

static const struct i2c_device_id gs_id[] = {
	{ MMA8452_DRV_NAME, 0 },
	{ }
};

static struct i2c_driver gs_driver = {
	.probe		=gs_probe,
	.remove		= gs_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend	= gs_suspend,
	.resume		= gs_resume,
#endif
	.id_table	= gs_id,
	.driver = {
		.name	=MMA8452_DRV_NAME,
	},
};

static int __devinit gs_mma8452_init(void)
{
	return i2c_add_driver(&gs_driver);
}

static void __exit gs_mma8452_exit(void)
{
	i2c_del_driver(&gs_driver);
	if (gs_wq)
		destroy_workqueue(gs_wq);
}

/* modify the order of init */
module_init(gs_mma8452_init);
module_exit(gs_mma8452_exit);

MODULE_DESCRIPTION("gs_mma8452 Driver");
MODULE_LICENSE("GPL");

