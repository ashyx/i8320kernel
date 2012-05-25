/*
 * input/touchscreen/omap/omap_ts.c
 *
 * touchscreen input device driver for various TI OMAP boards
 * Copyright (c) 2002 MontaVista Software Inc.
 * Copyright (c) 2004 Texas Instruments, Inc.
 * Cleanup and modularization 2004 by Dirk Behme <dirk.behme@de.bosch.com>
 *
 * Assembled using driver code copyright the companies above.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * History:
 * 12/12/2004    Srinath Modified and intergrated code for H2 and H3
 *
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/suspend.h>
#include <linux/platform_device.h>
#include <linux/semaphore.h>	// ryun
#include <asm/mach-types.h>
#include <linux/regulator/consumer.h>
#include <plat/gpio.h>	//ryun
#include <plat/mux.h>	//ryun 
#include <linux/delay.h>
#include <asm/irq.h>
#include <asm/mach/irq.h>
#include <linux/firmware.h>
#include <linux/time.h>
#include "touchscreen.h"

#include <linux/i2c/twl.h>	// ryun 20091125 
#include <linux/earlysuspend.h>	// ryun 20200107 for early suspend
#include <plat/omap-pm.h>	// ryun 20200107 for touch boost

//************************************************************************************
// value defines
//************************************************************************************
#define TOUCHSCREEN_NAME			"touchscreen"
#define DEFAULT_PRESSURE_UP		0
#define DEFAULT_PRESSURE_DOWN		256

#define __TOUCH_DRIVER_MAJOR_VER__ 3
#define __TOUCH_DRIVER_MINOR_VER__ 1

#define MAX_TOUCH_X_RESOLUTION	480
#define MAX_TOUCH_Y_RESOLUTION	800

#ifdef __TSP_I2C_ERROR_RECOVERY__
#define MAX_HANDLER_COUNT		50000
#endif

#define SYNAPTICS_SLEEP_WAKEUP_ADDRESS		0x20

#define	TOUCHSCREEN_SLEEP_WAKEUP_RETRY_COUNTER		3

#define touch_mt //me add 2012.05.21
#ifdef touch_mt
#define MAX_PRESSURE                (1)
#define MAX_TOOL_WIDTH      (15)
#define MAX_TOUCH_MAJOR     (255)
#define MAX_TOUCH_MINOR     (15)
#endif

//add touch boost funtion //me add 2012.04.14

#define touch_boost
#define VDD1_OPP5_FREQ         600000000
#define VDD1_OPP4_FREQ         550000000
#define VDD1_OPP3_FREQ         500000000
#define VDD1_OPP1_FREQ         250000000

//************************************************************************************
// enum value value
//************************************************************************************

enum EnTouchSleepStatus 
{
	EN_TOUCH_SLEEP_MODE = 0,
	EN_TOUCH_WAKEUP_MODE = 1
}; 

enum EnTouchPowerStatus 
{
	EN_TOUCH_POWEROFF_MODE = 0,
	EN_TOUCH_POWERON_MODE = 1
}; 

#ifdef	__SYNAPTICS_ALWAYS_ACTIVE_MODE__
enum EnTouchDriveStatus 
{
	EN_TOUCH_USE_DOZE_MODE = 0,
	EN_TOUCH_USE_NOSLEEP_MODE = 1
}; 
#endif

//************************************************************************************
// global value
//************************************************************************************


static struct touchscreen_t tsp;
//static struct work_struct tsp_work; //me move
static struct workqueue_struct *tsp_wq;
static int g_touch_onoff_status = EN_TOUCH_POWEROFF_MODE;
static int g_sleep_onoff_status = EN_TOUCH_WAKEUP_MODE;
static int g_enable_touchscreen_handler = 0;	// fixed for i2c timeout error.
static unsigned int g_version_read_addr = 0;
//static unsigned short g_position_read_addr = 0;

//extern int nowplus_enable_touch_pins(int enable);//me add
//struct res_handle *tsp_rhandle = NULL;
struct regulator *tsp_rhandle;//me add

#ifndef __CONFIG_CYPRESS_USE_RELEASE_BIT__
static struct timer_list tsp_timer;
#endif
struct touchscreen_t;

struct touchscreen_t {
	struct input_dev * inputdevice;
#ifndef __CONFIG_CYPRESS_USE_RELEASE_BIT__
	struct timer_list ts_timer;
#endif
	int touched;
	int irq;
	int irq_type;
	int irq_enabled;
//	struct ts_device *dev;//me close
	struct device *dev;//me change
	spinlock_t lock;
	struct early_suspend	early_suspend;// ryun 20200107 for early suspend
        struct work_struct tsp_work;//me move here
#ifdef touch_boost //me add 2012.04.14
	struct timer_list opp_set_timer;	// ryun 20100107 for touch boost
	struct work_struct constraint_wq;
	int opp_high;	// ryun 20100107 for touch boost	
#endif
};

#ifdef CONFIG_HAS_EARLYSUSPEND
void touchscreen_early_suspend(struct early_suspend *h);
void touchscreen_late_resume(struct early_suspend *h);
#endif	/* CONFIG_HAS_EARLYSUSPEND */

void (*touchscreen_read)(struct work_struct *work) = NULL;

static u16 syna_old_x_position = 0;
static u16 syna_old_y_position = 0;
static u16 syna_old_press = 0;//me add 2012.05.22
#if 1 //def touch_mt
static u16 syna_old_z1 = 0;
#endif
static u16 syna_old_x2_position = 0;
static u16 syna_old_y2_position = 0;
static int syna_old_finger_type = 0;
static int finger_switched = 0;

#ifdef __TSP_I2C_ERROR_RECOVERY__
static struct timespec g_last_recovery_time;
static unsigned int g_i2c_error_recovery_count = 0;
static long g_touch_irq_handler_count = 0;
#endif

#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
static unsigned int g_synaptics_unstable_recovery_count = 0;
static unsigned int g_rf_recovery_behavior_status = 1;
static struct timer_list tsp_rf_noise_recovery_timer;
static struct timeval g_last_rf_noise_recovery_time;
#endif

u8 g_synaptics_read_addr = 0;
int g_synaptics_read_cnt = 0;

unsigned char g_pTouchFirmware[SYNAPTICS_FIRMWARE_IMAGE_SIZE];
unsigned int g_FirmwareImageSize = 0;

struct timeval g_current_timestamp;

u8 g_synaptics_device_control_addr = 0;

#ifdef __SYNA_MULTI_TOUCH_SUPPORT__
static int syna_old_old_finger_type=0;
#endif

//************************************************************************************
// extern functions
//************************************************************************************
extern int i2c_tsp_sensor_init(void);
extern int i2c_tsp_sensor_read(u8 reg, u8 *val, unsigned int len );
extern int i2c_tsp_sensor_write_reg(u8 address, int data);


//************************************************************************************
// function defines
//************************************************************************************
static irqreturn_t touchscreen_handler(int irq, void *dev_id);

void touchscreen_enter_sleep(void);
void touchscreen_wake_up(void);

void touchscreen_poweroff(void);
void touchscreen_poweron(void);
void set_touch_i2c_mode_init(void);
void synaptics_touchscreen_start_triggering(void);
#ifdef __TSP_I2C_ERROR_RECOVERY__
void touch_ic_recovery();
#endif
#ifdef __SYNAPTICS_ALWAYS_ACTIVE_MODE__
static int synaptics_set_drive_mode_bit(enum EnTouchDriveStatus drive_status);
#endif

// never call irq handler
void synaptics_touchscreen_start_triggering(void)
{
	u8 *data;
	int ret = 0;

	data = kmalloc(g_synaptics_read_cnt, GFP_KERNEL);
	if(data == NULL)
	{
		printk("[TSP][ERROR] %s() kmalloc fail\n", __FUNCTION__ );
		return;
	}
	ret = i2c_tsp_sensor_read(g_synaptics_read_addr, data, g_synaptics_read_cnt);
	if(ret != 0)
	{
		printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
	}
	kfree(data);

#ifdef __SYNAPTICS_ALWAYS_ACTIVE_MODE__
	synaptics_set_drive_mode_bit(EN_TOUCH_USE_NOSLEEP_MODE);
#endif

}

void synaptics_get_page_address(unsigned short *position_start_addr, unsigned short *firmware_revision_addr)
{
	u8 data;
	int i=0, ret=0;;
	unsigned short base_addr = 0xEE;
	int getcount=0;


	for(i=0 ; i<10 ; i++)
	{
		ret = i2c_tsp_sensor_read(base_addr, &data, 1);
		if(ret != 0)
		{
			printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
			return;
		}
		
		if(data == 0x0)
			udelay(1000);
		else
			break;
	}
	if(i == 10)
	{
		printk("[TSP][ERROR] i2c read fail\n");
		return;
	}
	
	for(i=0 ; i<10 ; i++)
	{
		ret = i2c_tsp_sensor_read(base_addr, &data, 1);
		if(ret != 0)
		{
			printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
			return;
		}
		
		if(data == 0x01)
		{
			(*firmware_revision_addr) = base_addr - 5;
			ret = i2c_tsp_sensor_read((*firmware_revision_addr), &data, 1);
			if(ret != 0)
			{
				printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
				return;
			}
			(*firmware_revision_addr) = data+3;
			base_addr -= 6;
			getcount++;
		}
		else if(data == 0x11)
		{
			(*position_start_addr) = base_addr - 2;

			base_addr -= 6;
			getcount++;
		}

		if(getcount == 2)
			break;
	}
}

static void issp_request_firmware(char* update_file_name)
{
	int idx_src = 0;
	int idx_dst = 0;
	int line_no = 0;
	int dummy_no = 0;
	char buf[2];
		
	struct device *dev = &tsp.inputdevice->dev;
	const struct firmware * fw_entry;

	request_firmware(&fw_entry, update_file_name, dev);
//	printk("[TSP][DEBUG] firmware size = %d\n", fw_entry->size);

	do {
		g_pTouchFirmware[idx_dst] = fw_entry->data[idx_src];
		idx_src++;
		idx_dst++;
	} while (idx_src < g_FirmwareImageSize-2);
	//end value '0xFFFF'
	g_pTouchFirmware[g_FirmwareImageSize-2]=0xFF;
	g_pTouchFirmware[g_FirmwareImageSize-1]=0xFF;
}

static int touch_firmware_update(char* firmware_file_name)
{
	printk("[TSP] firmware file name : %s\n", firmware_file_name);
	
	printk("[TSP] start update synaptics touch firmware !!\n");
	g_FirmwareImageSize = SYNAPTICS_FIRMWARE_IMAGE_SIZE;

	if(g_pTouchFirmware == NULL)
	{
		printk("[TSP][ERROR] %s() kmalloc fail !! \n", __FUNCTION__);
		return -1;
	}

	if(g_enable_touchscreen_handler == 1)
	{
		g_enable_touchscreen_handler = 0;
		free_irq(tsp.irq, &tsp);
	}

	issp_request_firmware(firmware_file_name);

	
	g_FirmwareImageSize = 0;

	touchscreen_poweroff();
	touchscreen_poweron();

	synaptics_touchscreen_start_triggering();

	return 0;
}

#ifdef touch_boost //me add 2012.04.14

static void tsc_timer_out (unsigned long v)
	{
		schedule_work(&(tsp.constraint_wq));
		return;
	}

void tsc_remove_constraint_handler(struct work_struct *work)
{
		omap_pm_set_min_mpu_freq(tsp.dev, VDD1_OPP1_FREQ);
                tsp.opp_high = 0;
} 

#endif
#ifndef __CONFIG_CYPRESS_USE_RELEASE_BIT__
static void tsp_timer_handler(unsigned long data)
{         
        #ifdef touch_mt
        input_mt_sync(tsp.inputdevice);
        input_sync(tsp.inputdevice);
        #else
	input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
	input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
	input_sync(tsp.inputdevice);
        #endif
	//printk( "[TSP] timer : up\n");
}
#endif
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__

static void tsp_rf_noise_recovery_timer_handler(unsigned long data)
{
	if(
		(g_last_rf_noise_recovery_time.tv_sec > g_current_timestamp.tv_sec) || 
		((g_last_rf_noise_recovery_time.tv_sec == g_current_timestamp.tv_sec) 
			&& (g_last_rf_noise_recovery_time.tv_usec  > g_current_timestamp.tv_usec))
	)
	{
                #ifdef touch_mt
                 input_mt_sync(tsp.inputdevice);
                 input_sync(tsp.inputdevice);
                #else
		input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
		input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
		input_sync(tsp.inputdevice);
                #endif
		printk("[TSP][WARNING] == recovery timer : up\n");
	}

	del_timer(&tsp_rf_noise_recovery_timer);
}

#endif

#ifdef __SYNAPTICS_ALWAYS_ACTIVE_MODE__
static int synaptics_set_drive_mode_bit(enum EnTouchDriveStatus drive_status)
{
	int data = 0, ret = 0;
	
	ret = i2c_tsp_sensor_read((u8)g_synaptics_device_control_addr, (u8*)&data, 1);
	if(ret != 0)
	{
		printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
		return -1;
	}

	printk( "%s() data before = 0x%x\n", __FUNCTION__, data);
	
	if(drive_status == EN_TOUCH_USE_DOZE_MODE) // clear bit
	{
		data &=	0xfb;	// and 1111 1011
	}else if(drive_status == EN_TOUCH_USE_NOSLEEP_MODE) // set 1
	{
		data |= 0x04;		// or 0000 0100
	}

	printk( "%s() data after = 0x%x\n", __FUNCTION__, data);	
	
	if(g_synaptics_device_control_addr != 0)
	{
		if((ret = i2c_tsp_sensor_write_reg(g_synaptics_device_control_addr, data)) != 0)
		{
			printk("[TSP][ERROR] %s() i2c_tsp_sensor_write_reg error : %d\n", __FUNCTION__ , ret);
			return -1;
		}
	}else{
		printk("[TSP][ERROR] %s() g_synaptics_device_control_addr is not setted\n", __FUNCTION__);
		return -1;
	}
	return 1;
}

#endif // __SYNAPTICS_ALWAYS_ACTIVE_MODE__


/**************************************************
//	Device Control register
//          7                6           5    4   3         2            1      0
// | Configured | Report Rate | - | - | - | No Sleep | Sleep Mode |
**************************************************/

static int synaptics_write_sleep_bit(enum EnTouchSleepStatus sleep_status)
{
	int data = 0, ret = 0;
	
	ret = i2c_tsp_sensor_read((u8)g_synaptics_device_control_addr, (u8*)&data, 1);
	if(ret != 0)
	{
		printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
		return -1;
	}

	printk( "%s() data before = 0x%x\n", __FUNCTION__, data);

#ifdef __SYNAPTICS_ALWAYS_ACTIVE_MODE__
	if(sleep_status == EN_TOUCH_SLEEP_MODE) // set 1
	{
		data &=	0xf8;	// and 1111 1000 - clear 0~2
		data |= 0x01;		// or 0000 0001
	}else if(sleep_status == EN_TOUCH_WAKEUP_MODE) // set 0
	{
		data &=	0xfc;	// and 1111 1100
		data |= 0x04;		// or 0000 0100
	}
#else
	if(sleep_status == EN_TOUCH_SLEEP_MODE) // set 1
	{
		data &=	0xfc; // and 1111 1100
		data |= 0x01;		// or 0000 0001
	}else if(sleep_status == EN_TOUCH_WAKEUP_MODE) // set 0
	{
		data &=	0xfc; // and 1111 1100
	}
#endif

	printk( "%s() data after = 0x%x\n", __FUNCTION__, data);	
	
	if(g_synaptics_device_control_addr != 0)
	{
		if((ret = i2c_tsp_sensor_write_reg(g_synaptics_device_control_addr, data)) != 0)
		{
			printk("[TSP][ERROR] %s() i2c_tsp_sensor_write_reg error : %d\n", __FUNCTION__ , ret);
			return -1;
		}
	}else{
		printk("[TSP][ERROR] %s() g_synaptics_device_control_addr is not setted\n", __FUNCTION__);
		return -1;
	}
	return 1;
}


void touchscreen_enter_sleep(void)
{
	int ret=0, ii=0;

	if(g_sleep_onoff_status == EN_TOUCH_WAKEUP_MODE)
	{
		
		if(g_enable_touchscreen_handler == 1)
		{
			g_enable_touchscreen_handler = 0;
			free_irq(tsp.irq, &tsp);
		}else{
			printk( "[TSP][WARNING] %s() handler is already disabled \n", __FUNCTION__);
		}
		

		g_sleep_onoff_status = EN_TOUCH_SLEEP_MODE;

		//[synaptics] normal mode -> sleep mode

		// must be success !!
		for(ii=0 ; ii<TOUCHSCREEN_SLEEP_WAKEUP_RETRY_COUNTER ; ii++)
		{
			if(synaptics_write_sleep_bit(EN_TOUCH_SLEEP_MODE) == -1)
			{
				printk("[TSP][ERROR] %s() sleep bit control error\n", __FUNCTION__);
			}else
			{
				break;
			}
		}

		if(ii == TOUCHSCREEN_SLEEP_WAKEUP_RETRY_COUNTER)
			goto error_return;
		printk("[TSP] touchscreen enter sleep !\n");
	}else{
		printk( "[TSP][WARNING] %s() call but g_sleep_onoff_status = %d !\n", __FUNCTION__, g_sleep_onoff_status);
	}
	
	return;
	
error_return:
	
	return;
	
}

void touchscreen_poweroff(void)
{
	int ret = 0;
	
	if(g_touch_onoff_status == EN_TOUCH_POWERON_MODE)
	{
		
		if(g_enable_touchscreen_handler == 1)
		{
			g_enable_touchscreen_handler = 0;
			free_irq(tsp.irq, &tsp);
		}else{
			printk( "[TSP][WARNING] %s() handler is already disabled \n", __FUNCTION__);
		}
		

		g_touch_onoff_status = EN_TOUCH_POWEROFF_MODE;
#if 0
		ret = resource_release(tsp_rhandle);
		if(ret < 0)
		{
			printk("[TSP] %s() touchscreen resource release fail : %d\n", __FUNCTION__, ret);
			goto error_return;
		}
		udelay(10);
#endif

		printk("[TSP] touchscreen power off !\n");
	}else{
		printk("[TSP] %s() call but g_touch_onoff_status = %d !\n", __FUNCTION__, g_touch_onoff_status);
	}
	
	
	return;

error_return:
	
	return;

}



void set_touch_i2c_mode_init(void)
{
	//omap3430_pad_set_configs(synaptics_touch_i2c_gpio_init, ARRAY_SIZE(synaptics_touch_i2c_gpio_init));
	//omap_set_gpio_direction(OMAP_GPIO_TOUCH_IRQ, GPIO_DIR_INPUT);
	return;
}

void touchscreen_wake_up(void)
{
	int ret=0, ii=0;
	
	set_touch_i2c_mode_init();

	udelay(100);
	
	if(g_sleep_onoff_status == EN_TOUCH_SLEEP_MODE)
	{

		//[synaptics] sleep mode -> normal mode
		for(ii=0 ; ii<TOUCHSCREEN_SLEEP_WAKEUP_RETRY_COUNTER ; ii++)
		{
			if(synaptics_write_sleep_bit(EN_TOUCH_WAKEUP_MODE) == -1)
			{
				printk("[TSP][ERROR] %s() sleep bit control error\n", __FUNCTION__);
			}else
				break;
		}
		if(ii == TOUCHSCREEN_SLEEP_WAKEUP_RETRY_COUNTER)
			goto error_return;
		synaptics_touchscreen_start_triggering();
		
		if(g_enable_touchscreen_handler == 0)
		{
			g_enable_touchscreen_handler = 1;
			if (request_irq(tsp.irq, touchscreen_handler, tsp.irq_type, TOUCHSCREEN_NAME, &tsp))	
			{
				printk("[TSP][ERROR] %s() Could not allocate touchscreen IRQ!\n", __FUNCTION__);
				tsp.irq = -1;
				input_free_device(tsp.inputdevice);
				goto error_return_2;
			}else
				printk( "[TSP] %s() success register touchscreen IRQ!\n", __FUNCTION__);
		}else{
			printk( "[TSP][WARNING] %s() handler is already enabled \n", __FUNCTION__);
		}
		
		
		g_sleep_onoff_status = EN_TOUCH_WAKEUP_MODE;
		printk("[TSP] touchscreen_wake_up() success!!\n");
	}else{
		printk( "[TSP][WARNING] %s() call but g_sleep_onoff_status = %d !\n", __FUNCTION__, g_sleep_onoff_status);
	}
	
	return;

error_return:
	
	return;

error_return_2:
	
	return;
}

void touchscreen_poweron(void)
{
	int ret = 0;

	set_touch_i2c_mode_init();

	udelay(100);

	
	if(g_touch_onoff_status == EN_TOUCH_POWEROFF_MODE)
	{
		printk("[TSP] synaptics touchscreen power on\n");
		//synaptics_set_GPIO_direction_out();
		// set gpio low
		//gpio_direction_output(OMAP_GPIO_TOUCH_IRQ, 0);
		//gpio_direction_output(OMAP_GPIO_TSP_SCL, 0);
		//gpio_direction_output(OMAP_GPIO_TSP_SDA, 0);
		//udelay(50);
	#if 0	
		if(tsp_rhandle != NULL)
		{
			ret = resource_request(tsp_rhandle, T2_VAUX2_2V80);
			if(ret < 0)
			{
				printk("[TSP][ERROR] %s() resource_request fail : %d\n", __FUNCTION__, ret);
				goto error_return;
			}
		}
		else
		{
			printk("[TSP][ERROR] enable_tsp_pins() - tsp_rhandle is NULL\n");
			goto error_return;
		}
	#endif	
                //nowplus_enable_touch_pins(1);//me add
		set_touch_i2c_mode_init();
		mdelay(200);

		synaptics_touchscreen_start_triggering();

		if(g_enable_touchscreen_handler == 0)
		{
			g_enable_touchscreen_handler = 1;
			if (request_irq(tsp.irq, touchscreen_handler, tsp.irq_type, TOUCHSCREEN_NAME, &tsp))	
			{
				printk("[TSP][ERROR] %s() Could not allocate touchscreen IRQ!\n", __FUNCTION__);
				tsp.irq = -1;
				input_free_device(tsp.inputdevice);
				goto error_return_2;
			}else
				printk( "[TSP] %s() success register touchscreen IRQ!\n", __FUNCTION__);
		}else{
			printk( "[TSP][WARNING] %s() handler is already enabled \n", __FUNCTION__);
		}
				
		g_touch_onoff_status = EN_TOUCH_POWERON_MODE;
		g_sleep_onoff_status = EN_TOUCH_WAKEUP_MODE;
	}else{
		printk("[TSP] %s() call but g_touch_onoff_status = %d !\n", __FUNCTION__, g_touch_onoff_status);
	}
	
	return;

error_return:
	return;

error_return_2:
	
	return;

}

#ifdef __TSP_I2C_ERROR_RECOVERY__
void touch_ic_recovery()
{
	printk("[TSP] try to touch IC reset...\n");
	g_last_recovery_time = current_kernel_time();
	g_i2c_error_recovery_count++;
	touchscreen_poweroff();
	touchscreen_poweron();
	printk("[TSP] complete to reset touch IC...\n");
}
#endif


#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
static void touch_RF_nosie_recovery(int wx1, int wy1, int wx2, int wy2, int finger_type)
{
	printk("[TSP][WARNING] == touch is unstable state.!! (%d, %d, %d, %d), finger_type(%d)\n", wx1, wy1, wx2, wy2, finger_type);
	do_gettimeofday(&g_last_rf_noise_recovery_time);
	
	if(g_rf_recovery_behavior_status == 1) {
		printk("[TSP][WARNING] == reset touch chip baseband by 'rezero'.\n");
		i2c_tsp_sensor_write_reg(0x67, 0x1);
	}
	else if(g_rf_recovery_behavior_status == 2) {
		printk("[TSP][WARNING] == reset touch chip baseband by 'reset'.\n");
		i2c_tsp_sensor_write_reg(0x66, 0x1);
	}
	else //g_rf_recovery_behavior_status == 0
		printk("[TSP][WARNING] == NO reset touch chip baseband.\n");

	del_timer(&tsp_rf_noise_recovery_timer);
	tsp_rf_noise_recovery_timer.expires = (jiffies + msecs_to_jiffies(60));
	add_timer(&tsp_rf_noise_recovery_timer);

	g_synaptics_unstable_recovery_count++;

}
#endif

/******************************************************
******************   FUNCTION NOTICE   ********************
*
*****  synaptics_touchscreen_read(struct work_struct *work)    *****
*
* never call this function in irq handler directly.
* have to call "kfree()", If you want to "return" suddenly.
*
*******************************************************/
#if 1 //def touch_mt
void synaptics_touchscreen_read(struct work_struct *work)
{
	int ret = 0;
	u8 *data;
	u16 x_position = 0;
	u16 y_position = 0;
	//u16 x2_position = 0;
	//u16 y2_position = 0;
	int press = 0;
	//int press2 = 0;
	int finger_type = 0;
#ifdef touch_boost//me add 2012.04.14//	added for touchscreen boost,samsung customisation
        struct touchscreen_t *ts = container_of(work,
					struct touchscreen_t, tsp_work);
	
		if (timer_pending(&ts->opp_set_timer))
			del_timer(&ts->opp_set_timer);
		omap_pm_set_min_mpu_freq(ts->dev, VDD1_OPP3_FREQ);
		mod_timer(&ts->opp_set_timer, jiffies + (1000 * HZ) / 1000);

#endif
#ifdef __CONFIG_SYNAPTICS_MULTI_TOUCH__
	int muilti_touch_id = 0;
#endif
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
	u8 WX1 = 0;
	u8 WY1 = 0;
	//u8 WX2 = 0;
	//u8 WY2 = 0;
#ifdef touch_mt
        u8 z1 = 0;//me add 2012.05.21
	//u8 z2 = 0;//me add 2012.05.21
#endif
#endif
	do_gettimeofday(&g_current_timestamp);
//	if (touch_debug_status) {
//		printk("[TSP][DEBUG]read start time : %ld sec, %6ld microsec\n", g_current_timestamp.tv_sec, g_current_timestamp.tv_usec);
//	}

	data = kmalloc(g_synaptics_read_cnt, GFP_KERNEL);
	if(data == NULL)
	{
		printk("[TSP][ERROR] %s() kmalloc fail\n", __FUNCTION__ );
		kfree(data);
                
		return;
	}

	ret = i2c_tsp_sensor_read(g_synaptics_read_addr, data, g_synaptics_read_cnt);
	if(ret != 0)
	{
		printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
		kfree(data);
                
		return;
	}

	press = (data[0x15-g_synaptics_read_addr] & 0x03);
	x_position = data[0x16-g_synaptics_read_addr];
	x_position = (x_position << 4) | (data[0x18-g_synaptics_read_addr] & 0x0f);
	y_position = data[0x17-g_synaptics_read_addr];
	y_position = (y_position << 4) | (data[0x18-g_synaptics_read_addr]  >> 4);
	//press2 = ((data[0x15-g_synaptics_read_addr] & 0x0C)>>2);
	//x2_position = data[0x1B-g_synaptics_read_addr];
	//x2_position = (x2_position << 4) | (data[0x1D-g_synaptics_read_addr] & 0x0f);
	//y2_position = data[0x1C-g_synaptics_read_addr];
	//y2_position = (y2_position << 4) | (data[0x1D-g_synaptics_read_addr]  >> 4);
	
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
	WX1 = (data[0x19-g_synaptics_read_addr] & 0x0F);
	WY1 = (data[0x19-g_synaptics_read_addr] >> 4);
#ifdef touch_mt
        z1 = data[0x1A-g_synaptics_read_addr];//me add 2012.05.21
#endif
	//WX2 = (data[0x1E - g_synaptics_read_addr] & 0x0F);
	//WY2 = (data[0x1E - g_synaptics_read_addr] >> 4);
#ifdef touch_mt
        //z2 = data[0x1F-g_synaptics_read_addr];//me add 2012.05.21
#endif

#endif
#ifdef touch_mt
if((x_position == syna_old_x_position) && (y_position == syna_old_y_position)&& (z1 == syna_old_z1)&& (press == syna_old_press)) 
                               {
					kfree(data);
                                                                  
					return;
				}
                syna_old_x_position = x_position;
	 	syna_old_y_position = y_position;
                syna_old_press = press;
                syna_old_z1 = z1;



                                input_report_abs(tsp.inputdevice, ABS_MT_TOUCH_MAJOR,z1);
			        input_report_abs(tsp.inputdevice, ABS_MT_WIDTH_MAJOR,15);
                                input_report_abs(tsp.inputdevice, ABS_MT_POSITION_X, x_position);
				input_report_abs(tsp.inputdevice, ABS_MT_POSITION_Y, y_position);
                                input_mt_sync(tsp.inputdevice);
                                input_sync(tsp.inputdevice);

#else
if (press == 1 ) {
if((x_position == syna_old_x_position) && (y_position == syna_old_y_position)&& (press == syna_old_press)) 
                               {
					kfree(data);
                                                                  
					return;
				}
                syna_old_x_position = x_position;
	 	syna_old_y_position = y_position;
                syna_old_press = press;
                input_report_abs(tsp.inputdevice, ABS_X, x_position);
	        input_report_abs(tsp.inputdevice, ABS_Y, y_position);
                input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_DOWN);//me add
                input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_DOWN);
	        input_sync(tsp.inputdevice);
}else if (press == 0) {
if((x_position == syna_old_x_position) && (y_position == syna_old_y_position)&& (press == syna_old_press)) 
                               {
					kfree(data);
                                                                  
					return;
				}
                syna_old_x_position = x_position;
	 	syna_old_y_position = y_position;
                syna_old_press = press;
                input_report_abs(tsp.inputdevice, ABS_X, x_position);
	        input_report_abs(tsp.inputdevice, ABS_Y, y_position);
                input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
		input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
		input_sync(tsp.inputdevice);

}
#endif
 
#ifdef touch_mt
printk( "[TSP][UP][M->S]type:%d, x=%d, y=%d ,z1=%d ,maxpressure=%d\n", press, x_position, y_position, z1 ,max(WX1 , WY1));
#else
printk( "[TSP][UP][M->S]type:%d, x=%d, y=%d\n", press, x_position, y_position);
#endif
 //printk( "[TSP][UP][M->S]type:%d, x2=%d, y2=%d ,z2=%d ,maxpressure=%d\n", press2, x2_position, y2_position, z2 ,max(WX2 , WY2));       
	kfree(data);
}

#else

void synaptics_touchscreen_read(struct work_struct *work)
{
	int ret = 0;
	u8 *data;
	u16 x_position = 0;
	u16 y_position = 0;
	u16 x2_position = 0;
	u16 y2_position = 0;
	int press = 0;
	int press2 = 0;
	int finger_type = 0;
#ifdef touch_boost//me add 2012.04.14//	added for touchscreen boost,samsung customisation
        struct touchscreen_t *ts = container_of(work,
					struct touchscreen_t, tsp_work);
	
		if (timer_pending(&ts->opp_set_timer))
			del_timer(&ts->opp_set_timer);
		omap_pm_set_min_mpu_freq(ts->dev, VDD1_OPP3_FREQ);
		mod_timer(&ts->opp_set_timer, jiffies + (1000 * HZ) / 1000);

#endif
#ifdef __CONFIG_SYNAPTICS_MULTI_TOUCH__
	int muilti_touch_id = 0;
#endif
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
	u8 WX1 = 0;
	u8 WY1 = 0;
	u8 WX2 = 0;
	u8 WY2 = 0;
#ifdef touch_mt
        u8 z1 = 0;//me add 2012.05.21
	u8 z2 = 0;//me add 2012.05.21
#endif
#endif
	do_gettimeofday(&g_current_timestamp);
//	if (touch_debug_status) {
//		printk("[TSP][DEBUG]read start time : %ld sec, %6ld microsec\n", g_current_timestamp.tv_sec, g_current_timestamp.tv_usec);
//	}

	data = kmalloc(g_synaptics_read_cnt, GFP_KERNEL);
	if(data == NULL)
	{
		printk("[TSP][ERROR] %s() kmalloc fail\n", __FUNCTION__ );
		kfree(data);
                
		return;
	}

	ret = i2c_tsp_sensor_read(g_synaptics_read_addr, data, g_synaptics_read_cnt);
	if(ret != 0)
	{
		printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
		kfree(data);
                
		return;
	}

	press = (data[0x15-g_synaptics_read_addr] & 0x03);
	x_position = data[0x16-g_synaptics_read_addr];
	x_position = (x_position << 4) | (data[0x18-g_synaptics_read_addr] & 0x0f);
	y_position = data[0x17-g_synaptics_read_addr];
	y_position = (y_position << 4) | (data[0x18-g_synaptics_read_addr]  >> 4);
	press2 = ((data[0x15-g_synaptics_read_addr] & 0x0C)>>2);
	x2_position = data[0x1B-g_synaptics_read_addr];
	x2_position = (x2_position << 4) | (data[0x1D-g_synaptics_read_addr] & 0x0f);
	y2_position = data[0x1C-g_synaptics_read_addr];
	y2_position = (y2_position << 4) | (data[0x1D-g_synaptics_read_addr]  >> 4);

	if((press == 0) && (press2 == 0)) {
		finger_type=0;
	}
	else if((press == 1) && (press2 == 0)) {
		finger_type=1;
		finger_switched=0;
	}
	else if((press == 0) && (press2 == 1)) {
		finger_type=2;
		finger_switched=1;
	}
	else if((press == 1) && (press2 == 1))
		finger_type=3;
	else if((press == 2) && (press2 == 2))
		finger_type=4;
	else if((press == 0) && (press2 == 2)) {
		finger_type=5;
		finger_switched=1;
	}
	else if((press == 2) && (press2 == 0)) {
		finger_type=6;
		finger_switched=0;
	}
	else
		finger_type=7;
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
	WX1 = (data[0x19-g_synaptics_read_addr] & 0x0F);
	WY1 = (data[0x19-g_synaptics_read_addr] >> 4);
#ifdef touch_mt
        z1 = data[0x1A-g_synaptics_read_addr];//me add 2012.05.21
#endif
	WX2 = (data[0x1E - g_synaptics_read_addr] & 0x0F);
	WY2 = (data[0x1E - g_synaptics_read_addr] >> 4);
#ifdef touch_mt
        z2 = data[0x1F-g_synaptics_read_addr];//me add 2012.05.21
#endif

#endif

/////////////////////////////////////////////	 press event processing start   //////////////////////////////////////////////////

	if ((finger_type > 0) && (finger_type < 7))		// press
	{
		//same point filtering
		if( finger_type == syna_old_finger_type) 
		{
			if((x_position == syna_old_x_position) && (y_position == syna_old_y_position))
			{
				if((x2_position == syna_old_x2_position) && (y2_position == syna_old_y2_position)) {
					kfree(data);
                                        //
	                                
					return;
				}
			}
		}
		//end same point filtering

		syna_old_x_position = x_position;
	 	syna_old_y_position = y_position;
		syna_old_x2_position = x2_position;
		syna_old_y2_position = y2_position;

#ifdef __SYNA_MULTI_TOUCH_SUPPORT__
		//making release event(2finger => 1finger)
		if((syna_old_finger_type==3) || (syna_old_finger_type==4))
		{
			if((finger_type == 2) || (finger_type == 5)) {
				if((syna_old_old_finger_type == 1) || (syna_old_old_finger_type == 6)) {
					input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
					input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
					input_sync(tsp.inputdevice);
					//printk( "[TSP][UP][M->S]type:%d, x=%d, y=%d\n", finger_type, x_position, y_position);
				}
			}
			else if((finger_type == 1) || (finger_type == 6)) {
				if((syna_old_old_finger_type == 2) || (syna_old_old_finger_type == 5)) {
					input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
					input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
					input_sync(tsp.inputdevice);
					//printk( "[TSP][UP][M->S]type:%d, x=%d, y=%d\n", finger_type, x2_position, y2_position);
				}
			}
		}
		//2finger => 1finger making release event done
#endif //__SYNA_MULTI_TOUCH_SUPPORT__

		//send press event
		if((finger_type == 1) || (finger_type == 2) || (finger_type == 5) || (finger_type == 6)) {
#ifdef __MAKE_MISSED_UP_EVENT__
			if(finger_type == 1) {
				if(syna_old_finger_type == 2) {
                                        #ifdef touch_mt
                                        input_mt_sync(tsp.inputdevice);
                                        input_sync(tsp.inputdevice);
                                        #else
                                        input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
					input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
					input_sync(tsp.inputdevice);
					//printk( "[TSP][UP][F]type:%d, x=%d, y=%d\n", finger_type, x2_position, y2_position);
                                        #endif
				}
			}
			else if(finger_type == 2) {
				if(syna_old_finger_type == 1) {
                                         #ifdef touch_mt
                                        input_mt_sync(tsp.inputdevice);
                                        input_sync(tsp.inputdevice);
                                        #else
					input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
					input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
					input_sync(tsp.inputdevice);
					//printk( "[TSP][UP][F]type:%d, x=%d, y=%d\n", finger_type, x_position, y_position);
                                        #endif
				}
			}
#endif
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
			if((WX1 > 0xa) || (WY1 > 0xa) || (WX2 > 0xa) || (WY2 > 0xa)) {
				touch_RF_nosie_recovery(WX1, WY1, WX2, WY2, finger_type);
				if(g_rf_recovery_behavior_status !=0) {
					kfree(data); 
                                        
					return;
				}
			}
#endif	// __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
			if(finger_switched == 0) {
                                 #ifdef touch_mt
                                input_report_abs(tsp.inputdevice, ABS_MT_TOUCH_MAJOR,z1);
			        input_report_abs(tsp.inputdevice, ABS_MT_WIDTH_MAJOR,max(WX1 , WY1));
                                input_report_abs(tsp.inputdevice, ABS_MT_POSITION_X, x_position);
				input_report_abs(tsp.inputdevice, ABS_MT_POSITION_Y, y_position);
                                #else
				input_report_abs(tsp.inputdevice, ABS_X, x_position);
				input_report_abs(tsp.inputdevice, ABS_Y, y_position);
                                #endif
			}
			else {
                                 #ifdef touch_mt
                                input_report_abs(tsp.inputdevice, ABS_MT_TOUCH_MAJOR,z2);
			        input_report_abs(tsp.inputdevice, ABS_MT_WIDTH_MAJOR,max(WX2 , WY2));
                                input_report_abs(tsp.inputdevice, ABS_MT_POSITION_X, x2_position);
				input_report_abs(tsp.inputdevice, ABS_MT_POSITION_Y, y2_position);
                                #else
				input_report_abs(tsp.inputdevice, ABS_X, x2_position);
				input_report_abs(tsp.inputdevice, ABS_Y, y2_position);
                                #endif
			}
                                #ifdef touch_mt
                                 input_mt_sync(tsp.inputdevice);
                                 input_sync(tsp.inputdevice);
                                 #else
			         input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_DOWN);//me add
                                 input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_DOWN);
			         input_sync(tsp.inputdevice);
                                 #endif
#if 0 //me change
			if(finger_switched == 0)
				//printk( "[TSP][DOWN]type:%d, x=%d, y=%d\n", finger_type, x_position, y_position);
			else if(finger_switched == 1)
				//printk( "[TSP][DOWN]type:%d, x=%d, y=%d\n", finger_type, x2_position, y2_position);
#endif
		}
		else if((finger_type == 3) || (finger_type == 4)) {
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
//change rf_noise recovery excution condition of multi touch ( W value > 0xa => 0x0 )
			touch_RF_nosie_recovery(WX1, WY1, WX2, WY2, finger_type);
			if(g_rf_recovery_behavior_status !=0) {
				kfree(data);
                                
				return;
			}
#endif	// __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
			printk( "[TSP][DOWN][WARNING]type:%d, Multi touch is not supported.\n", finger_type);
		}
	}
		//end press event((finger_type > 0) && (finger_type < 7))

/////////////////////////////////////////////    release event processing start   //////////////////////////////////////////////////
	else if(finger_type == 0)
	{
		if((syna_old_finger_type == 1) || (syna_old_finger_type == 2) || (syna_old_finger_type == 5) || (syna_old_finger_type == 6)) {
                                      #ifdef touch_mt
                                       input_mt_sync(tsp.inputdevice);
                                       input_sync(tsp.inputdevice);
                                      #else
			               input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
					input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
			                input_sync(tsp.inputdevice);
                                       #endif
#if 0 //me change			
                       if(finger_switched == 0)
				printk( "[TSP][UP]type:%d, x=%d, y=%d\n", finger_type, x_position, y_position);
			else if(finger_switched == 1)
				printk( "[TSP][UP]type:%d, x=%d, y=%d\n", finger_type, x2_position, y2_position);
#endif
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
			if((WX1 > 0xa) || (WY1 > 0xa) || (WX2 > 0xa) || (WY2 > 0xa)) {
				touch_RF_nosie_recovery(WX1, WY1, WX2, WY2, finger_type);
				if(g_rf_recovery_behavior_status !=0) {
					kfree(data);
                                        
					return;
				}
			}
#endif	//__SYNAPTICS_UNSTABLE_TSP_RECOVERY__
		}
		else if((syna_old_finger_type == 3) || (syna_old_finger_type == 4)) {
#ifndef __SYNA_MULTI_TOUCH_SUPPORT__
                         #ifdef touch_mt
                                        input_mt_sync(tsp.inputdevice);
                                        input_sync(tsp.inputdevice);
                                        #else
			input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
					input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
			input_sync(tsp.inputdevice);
                        #endif
			//printk( "[TSP][UP][WARNING]type:%d, Multi touch is not supported. But send release event.\n", finger_type);
#else //__SYNA_MULTI_TOUCH_SUPPORT__
			//making two release event(2finger => release at the sametime)
			//1st finger up event
			input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
					input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
			input_sync(tsp.inputdevice);

			//2nd finger down event
			if((syna_old_old_finger_type == 1) || (syna_old_old_finger_type == 6)) {
				input_report_abs(tsp.inputdevice, ABS_X, x2_position);
				input_report_abs(tsp.inputdevice, ABS_Y, y2_position);
			}
			else if((syna_old_old_finger_type == 2) || (syna_old_old_finger_type == 5)) {
				input_report_abs(tsp.inputdevice, ABS_X, x_position);
				input_report_abs(tsp.inputdevice, ABS_Y, y_position);
			}
			input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_DOWN);
			input_sync(tsp.inputdevice);

			//2nd finger up event
			input_report_key(tsp.inputdevice, BTN_TOUCH, DEFAULT_PRESSURE_UP);//me add
					input_report_abs(tsp.inputdevice, ABS_PRESSURE, DEFAULT_PRESSURE_UP);
			input_sync(tsp.inputdevice);
			if((syna_old_old_finger_type == 1) || (syna_old_old_finger_type == 6)) {
				printk( "[TSP][UP][M->R]type:%d, x=%d, y=%d\n", finger_type, x_position, y_position);
				printk( "[TSP][DOWN][M->R]type:%d, x=%d, y=%d\n", finger_type, x2_position, y2_position);
				printk( "[TSP][UP][M->R]type:%d, x=%d, y=%d\n", finger_type, x2_position, y2_position);
			}
			else if((syna_old_old_finger_type == 2) || (syna_old_old_finger_type == 5)) {
				printk( "[TSP][UP][M->R]type:%d, x=%d, y=%d\n", finger_type, x2_position, y2_position);
				printk( "[TSP][DOWN][M->R]type:%d, x=%d, y=%d\n", finger_type, x_position, y_position);
				printk( "[TSP][UP][M->R]type:%d, x=%d, y=%d\n", finger_type, x_position, y_position);
			}
#endif //__SYNA_MULTI_TOUCH_SUPPORT__
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
			if((WX1 > 0xa) || (WY1 > 0xa) || (WX2 > 0xa) || (WY2 > 0xa)) {
				touch_RF_nosie_recovery(WX1, WY1, WX2, WY2, finger_type);
				if(g_rf_recovery_behavior_status !=0) {
					kfree(data);
                                        
					return;
				}
			}
#endif	// __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
		}

		finger_switched = 0;
		syna_old_x_position = -1;
	 	syna_old_y_position = -1;
		syna_old_x2_position = -1;
		syna_old_y2_position = -1;
	}

//////////////////////////////////////////	 unknown finger type  processing start   ///////////////////////////////////////////////
	else
	{
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
		if((WX1 > 0xa) || (WY1 > 0xa) || (WX2 > 0xa) || (WY2 > 0xa)) {
			touch_RF_nosie_recovery(WX1, WY1, WX2, WY2, finger_type);
			if(g_rf_recovery_behavior_status !=0) {
				kfree(data);
                                
				return;
			}
		}
#endif	// __SYNAPTICS_UNSTABLE_TSP_RECOVERY__

		//printk("[TSP][ERROR] Unknown finger_type : %d\n", finger_type);
		//printk( "[TSP][ERROR] press : %d, press2 : %d\n",press, press2);
	}
	syna_old_finger_type = finger_type;
#ifdef __SYNA_MULTI_TOUCH_SUPPORT__
		if((syna_old_finger_type != 3) && (syna_old_finger_type != 4))
			syna_old_old_finger_type = finger_type;
#endif
        //
	
	kfree(data);
}
#endif

static irqreturn_t touchscreen_handler(int irq, void *dev_id)
{
printk("[TSP] touchscreen_handler----------- !! \n");
#ifdef __TSP_I2C_ERROR_RECOVERY__
	if(g_touch_irq_handler_count < MAX_HANDLER_COUNT)
		g_touch_irq_handler_count++;
	else
		g_touch_irq_handler_count = 0;
#endif
	
	queue_work(tsp_wq, &tsp.tsp_work);

#ifndef __CONFIG_CYPRESS_USE_RELEASE_BIT__
	del_timer(&tsp_timer);
	tsp_timer.expires  = (jiffies + msecs_to_jiffies(60));
	add_timer(&tsp_timer);
#endif

	return IRQ_HANDLED;
}

void touchscreen_poweronoff(int onoff)
{
	printk( "[TSP] %s(%d)\n", __FUNCTION__, onoff);
	if (onoff)
	{
		touchscreen_wake_up();
	}
	else
	{
		touchscreen_enter_sleep();
	}
	
	return;
}
EXPORT_SYMBOL(touchscreen_poweronoff);

static int __init touchscreen_probe(struct platform_device *pdev)
{
	int ret = 0;
        u8 data[2] = {0, };
	printk("[TSP] touchscreen_probe !! \n");

	// init target dependent value
	touchscreen_read = synaptics_touchscreen_read;
	g_version_read_addr = 0x75;
	g_synaptics_read_addr = 0x13;
	g_synaptics_read_cnt = 13;
	g_synaptics_device_control_addr = 0x23;

	memset(&tsp, 0, sizeof(tsp));
    
        tsp.dev = &pdev->dev;//me add 2012.04.14
	
	tsp.inputdevice = input_allocate_device();

	if (!tsp.inputdevice)
	{
		printk("[TSP][ERROR] input_allocate_device fail \n");
		return -ENOMEM;
	}

	spin_lock_init(&tsp.lock);

	/* request irq */
	if (tsp.irq != -1)
	{
		tsp.irq = OMAP_GPIO_IRQ(OMAP_GPIO_TOUCH_IRQ);
		tsp.irq_type = IRQF_TRIGGER_FALLING;

#ifndef CONFIG_LATE_HANDLER_ENABLE
		if (request_irq(tsp.irq, touchscreen_handler, tsp.irq_type, TOUCHSCREEN_NAME, &tsp))	
		{
			printk("[TSP][ERROR] Could not allocate touchscreen IRQ!\n");
			tsp.irq = -1;
			input_free_device(tsp.inputdevice);
			return -EINVAL;
		}

		g_enable_touchscreen_handler = 1;
		
#endif // CONFIG_LATE_HANDLER_ENABLE

		tsp.irq_enabled = 1;
	}
	
	tsp.inputdevice->name = TOUCHSCREEN_NAME;
	tsp.inputdevice->id.bustype = BUS_I2C;
	tsp.inputdevice->id.vendor  = 0;
	tsp.inputdevice->id.product =0;
 	tsp.inputdevice->id.version =0;
#ifdef touch_mt
	set_bit(EV_SYN, tsp.inputdevice->evbit);
	set_bit(EV_KEY, tsp.inputdevice->evbit);
	set_bit(EV_ABS, tsp.inputdevice->evbit);

	input_set_abs_params(tsp.inputdevice, ABS_MT_POSITION_X, 0,
					MAX_TOUCH_X_RESOLUTION, 0, 0);
	input_set_abs_params(tsp.inputdevice, ABS_MT_POSITION_Y, 0,
					MAX_TOUCH_Y_RESOLUTION, 0, 0);
	input_set_abs_params(tsp.inputdevice, ABS_MT_TOUCH_MAJOR, 0,
						MAX_TOUCH_MAJOR, 0, 0);
	input_set_abs_params(tsp.inputdevice, ABS_MT_WIDTH_MAJOR, 0, 30, 0, 0);

#else
        tsp.inputdevice->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) | BIT_MASK(EV_SYN);//BIT(EV_ABS);me change
	tsp.inputdevice->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
        input_set_abs_params(tsp.inputdevice, ABS_X, 0, MAX_TOUCH_X_RESOLUTION, 0, 0);
        input_set_abs_params(tsp.inputdevice, ABS_Y, 0, MAX_TOUCH_Y_RESOLUTION, 0, 0);
        input_set_abs_params(tsp.inputdevice, ABS_PRESSURE, 0, 1, 0, 0);
#endif
	tsp_wq = create_singlethread_workqueue("tsp_wq");
	INIT_WORK(&tsp.tsp_work, touchscreen_read);
#ifdef touch_boost
  	init_timer(&tsp.opp_set_timer);
  	tsp.opp_set_timer.data = (unsigned long)&tsp; 
  	tsp.opp_set_timer.function = tsc_timer_out;
	INIT_WORK(&(tsp.constraint_wq), tsc_remove_constraint_handler);
#endif
#ifndef __CONFIG_CYPRESS_USE_RELEASE_BIT__
	init_timer(&tsp_timer);
	tsp_timer.expires = (jiffies + msecs_to_jiffies(60));
	tsp_timer.function = tsp_timer_handler;
#endif
#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
	init_timer(&tsp_rf_noise_recovery_timer);
	tsp_rf_noise_recovery_timer.expires = (jiffies + msecs_to_jiffies(60));
	tsp_rf_noise_recovery_timer.function = tsp_rf_noise_recovery_timer_handler;
#endif
	ret = input_register_device(tsp.inputdevice);
#ifdef CONFIG_HAS_EARLYSUSPEND
	tsp.early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	tsp.early_suspend.suspend = touchscreen_early_suspend;
	tsp.early_suspend.resume = touchscreen_late_resume;
	register_early_suspend(&tsp.early_suspend);
#endif	/* CONFIG_HAS_EARLYSUSPEND */
#if 0//me add
	tsp_rhandle = regulator_get(&pdev->dev, "vaux2");//resource_get("TSP", "t2_vaux2");
	if (tsp_rhandle == NULL)
	{
		printk(KERN_ERR "[TSP][ERROR] : Failed to get tsp power resources !! \n");
		return -ENODEV;
	}
        regulator_enable(tsp_rhandle);
#endif
	i2c_tsp_sensor_init();
#if 0 //me close 2012.04.14	
	ret = device_create_file(&tsp.inputdevice->dev, &dev_attr_issp);

#ifdef CONFIG_LATE_HANDLER_ENABLE
	ret = device_create_file(&tsp.inputdevice->dev, &dev_attr_tsp_enable);
#endif

	ret = device_create_file(&tsp.inputdevice->dev, &dev_attr_touchcontrol);
	ret = device_create_file(&tsp.inputdevice->dev, &dev_attr_get_register);
	ret = device_create_file(&tsp.inputdevice->dev, &dev_attr_set_register);

#ifdef __SYNAPTICS_UNSTABLE_TSP_RECOVERY__
	ret = device_create_file(&tsp.inputdevice->dev, &dev_attr_rf_recovery_behavior);
#endif
#endif //me close
#ifndef CONFIG_LATE_HANDLER_ENABLE
	ret = i2c_tsp_sensor_read(g_version_read_addr, data, 2);
	if(ret != 0)
	{
		printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
	}

	printk("[TSP] touch driver version : %d.%d, firmware S/W Revision Info = %x\n", 
		__TOUCH_DRIVER_MAJOR_VER__,__TOUCH_DRIVER_MINOR_VER__, data[0]);
#endif
	printk("[TSP] success %s() !\n", __FUNCTION__);
#if 1  //me add
        //nowplus_enable_touch_pins(1);//me add
        touchscreen_poweron();
        ret = i2c_tsp_sensor_read(g_version_read_addr, data, 2);
		if(ret != 0)
		{
			printk("[TSP][ERROR] %s() i2c_tsp_sensor_read error : %d\n", __FUNCTION__ , ret);
#ifdef __TSP_I2C_ERROR_RECOVERY__
			touch_ic_recovery();
#endif
		}
		printk( "[TSP] touch driver version : %d.%d, firmware S/W Revision Info = %x\n", 
			__TOUCH_DRIVER_MAJOR_VER__,__TOUCH_DRIVER_MINOR_VER__, data[0]);

		synaptics_touchscreen_start_triggering();
#endif
	return 0;
}

static int touchscreen_remove(struct platform_device *pdev)
{

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&tsp.early_suspend);
#endif	/* CONFIG_HAS_EARLYSUSPEND */

	input_unregister_device(tsp.inputdevice);

	if (tsp.irq != -1)
	{
		
		if(g_enable_touchscreen_handler == 1)
		{
			free_irq(tsp.irq, &tsp);
			g_enable_touchscreen_handler = 0;
		}else{
			printk( "[TSP][WARNING] %s() handler is already disabled \n", __FUNCTION__);
		}
		
	}
#if 0 //me add
                regulator_disable(tsp_rhandle);
		regulator_put(tsp_rhandle);
#endif
#if 0
	resource_put(tsp_rhandle);
	tsp_rhandle = NULL;
#endif
	return 0;
}


static int touchscreen_suspend(struct platform_device *pdev, pm_message_t state)
{
// touch power is controled only by sysfs.
	printk("touchscreen_suspend : touch power off\n");
        touchscreen_poweronoff(0);
        //nowplus_enable_touch_pins(0);//me add
	return 0;
}

static int touchscreen_resume(struct platform_device *pdev)
{
// touch power is controled only by sysfs.
	printk("touchscreen_resume : touch power on\n");
        //nowplus_enable_touch_pins(1);//me add
        touchscreen_poweronoff(1);
        return 0;
}

static void touchscreen_device_release(struct device *dev)
{
	/* Nothing */
}

static struct platform_driver touchscreen_driver = {
	.probe 		= touchscreen_probe,
	.remove 	= touchscreen_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND		// 20100113 ryun 
	.suspend 	= &touchscreen_suspend,
	.resume 	= &touchscreen_resume,
#endif	
	.driver = {
		.name	= TOUCHSCREEN_NAME,
	},
};

static struct platform_device touchscreen_device = {
	.name 		= TOUCHSCREEN_NAME,
	.id 		= -1,
	.dev = {
		.release 	= touchscreen_device_release,
	},
};

#ifdef CONFIG_HAS_EARLYSUSPEND
void touchscreen_early_suspend(struct early_suspend *h)
{
//	melfas_ts_suspend(PMSG_SUSPEND);
	touchscreen_suspend(&touchscreen_device, PMSG_SUSPEND);
}

void touchscreen_late_resume(struct early_suspend *h)
{
//	melfas_ts_resume();
	touchscreen_resume(&touchscreen_device);
}
#endif	/* CONFIG_HAS_EARLYSUSPEND */

static int __init touchscreen_init(void)
{
	int ret;

	ret = platform_device_register(&touchscreen_device);
	if (ret != 0)
		return -ENODEV;

	ret = platform_driver_register(&touchscreen_driver);
	if (ret != 0) {
		platform_device_unregister(&touchscreen_device);
		return -ENODEV;
	}

	return 0;
}

static void __exit touchscreen_exit(void)
{
	platform_driver_unregister(&touchscreen_driver);
	platform_device_unregister(&touchscreen_device);
}

module_init(touchscreen_init);
module_exit(touchscreen_exit);

MODULE_LICENSE("GPL");
