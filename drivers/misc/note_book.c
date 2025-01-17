/* drivers/video/backlight/rk29_backlight.c
 *
 * Copyright (C) 2009-2011 Rockchip Corporation.
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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/earlysuspend.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/power_supply.h>
#include <linux/adc.h>

#include <asm/io.h>
#include <mach/board.h>
#include <mach/gpio.h>
#include <mach/iomux.h>

/*
 * Debug
 */
#if 0
#define DBG(x...)	printk(KERN_INFO x)
#else
#define DBG(x...)
#endif

struct note_book_gpio_cfg
{
	int notebook_power;      //底坐电源
	int notebook_power_pad;  //底坐对pad供电
	int notebook_charge_pad; //底坐对pad充电

	int notebook_dc_det; //底坐DC检测
	int notebook_charge_ok; //底坐充电OK
	int notebook_low_battery; //底坐电量底
	int notebook_low_battery_led; //底坐电量灯控制
};

struct note_book_conf
{
	int power_status;
	int power_pad_status;
	int power_charge_status;
};

struct note_book_battery
{
	int level;
	int status;
	int voltage;
};


struct note_book
{
	int use;
	int status;
	int adc_val;
	int suspend;
	int battery_low;
	int pad_det_pin;
	int pad_det_level;
	int notebook_pin;
	int notebook_level;
	int notebook_irq;
	int enable_wakeup;
	int battery_level;
	int charge_ok_number;
	int battery_work_status;
	int notebook_irq_status;
	int notebook_detect_irq;
	struct delayed_work work;
	struct adc_client *client;
	struct workqueue_struct *wq;
	struct completion completion;
	int notebook_detect_irq_status;
	struct delayed_work work_detect;
	struct delayed_work battery_work;
	struct early_suspend device_early;
	struct note_book_conf notebook_conf;
	struct note_book_battery note_battery;
	struct note_book_gpio_cfg note_gpio_cfg;
};

extern void rk28_send_wakeup_key( void );
extern void rk29_send_power_key( int state );
extern int set_power_key_interrupt( int status );

static struct note_book *m_notebook = NULL;

#ifdef CONFIG_HAS_EARLYSUSPEND
static struct early_suspend device_early = 
{
	.level = -0xff,
	.suspend = NULL,
	.resume = NULL,
};

static void device_earlysuspend( int status )
{
	struct early_suspend *pos;
	struct list_head *list_head;
	struct early_suspend *earlysuspend_temp;
	
	earlysuspend_temp = &device_early;
	list_head =  earlysuspend_temp->link.prev;
	if ( status == 0 ){
		list_for_each_entry( pos, list_head, link ){
			if ( pos->suspend != NULL ){
				pos->suspend(pos);
			}
		}
	}
	if ( status == 1 ){
		list_for_each_entry_reverse( pos, list_head, link ){
			if ( pos->resume != NULL ){
				pos->resume(pos);
			}
		}
	}
}

static void early_suspend( struct early_suspend *handler )
{
	struct note_book *book = container_of( handler, struct note_book, device_early );
	book->suspend = 1;
}

static void early_resume( struct early_suspend *handler )
{
	struct note_book *book = container_of( handler, struct note_book, device_early );
	book->suspend = 0;
	if ( gpio_get_value( book->pad_det_pin ) == book->pad_det_level ){
		gpio_direction_output( book->note_gpio_cfg.notebook_power, book->notebook_conf.power_status ? GPIO_HIGH : GPIO_LOW );
	}
}
#endif

static void notebook_battery_callback( struct adc_client *client, void *param, int result )
{
	struct note_book *book = (struct note_book *)param;
	book->adc_val = result;
	complete( &book->completion );
}

static unsigned short sync_read_adc( struct note_book *book )
{
	int err;
	init_completion( &book->completion );
	adc_async_read( book->client );
	err = wait_for_completion_timeout( &book->completion, msecs_to_jiffies( 1000 ) );
	if ( err == 0 ){
		return 0;
	}
	return book->adc_val;
}

#if 0
static void bubble_sort( unsigned short *buffer, unsigned short size )
{
	int i;
	int flag;
	int index;
	for ( i = 0; i < size - 1; i++ ){
		flag = 1;
		for ( index = 0; index < size - i - 1; index++ ){
			if ( buffer[index] > buffer[index + 1] ){
				unsigned short value = buffer[index];
				buffer[index] = buffer[index + 1];
				buffer[index + 1] = value;
				flag = 0;
			}
		}
		if ( 1 == flag ){
			break;
		}
	}
}
#endif

static void bubble( unsigned short *buffer, unsigned short size )
{
	int i;
	int index;
	for ( i = 0; i < size - 1; i++ ){
		for ( index = 0; index < size - i - 1; index++ ){
			if ( buffer[index + 1] < buffer[index] ){
				unsigned short value = buffer[index];
				buffer[index] = buffer[index + 1];
				buffer[index + 1] = value;
			}
		}
	}
}

static unsigned short average( unsigned short *Buffer, unsigned short Size )
{
	int i;
	int result = 0;
	bubble( Buffer, Size );
	for ( i = 2; i < Size - 2; i++ ){
		result += Buffer[i];
	}
	return result / (Size - 4);
}

struct adc_voltage
{
	unsigned short adc;
	unsigned short vol;
};

struct battery_capacity
{
	unsigned short voltage;
	unsigned short capacity;
};

static const struct adc_voltage m_adc_voltage_table[] = {
	{588, 6000},
	{599, 6100},
	{610, 6200},
	{621, 6300},
	{632, 6400},
	{641, 6500},
	{650, 6600},
	{658, 6700},
	{668, 6800},
	{678, 6900},
	{686, 7000},
	{695, 7100},
	{704, 7200},
	{714, 7300},
	{723, 7400},
	{733, 7500},
	{741, 7600},
	{751, 7700},
	{760, 7800},
	{770, 7900},
	{780, 8000},
	{788, 8100},
	{798, 8200},
	{806, 8300},
	{816, 8400},
};

static const struct battery_capacity m_battery_table[] = {
	{6800, 1},
	{7200, 15},
	{7450, 30},
	{7700, 50},
	{7900, 75},
	{8100, 98},
	{8300, 100},
};

static int calc_voltage_ab( const struct adc_voltage *min, const struct adc_voltage *max, int *a, int *b )
{
	int add;
	int mul;
	int vol;
	int adc;
	if ( NULL == min || NULL == max ){
		return -1;
	}
	vol = max->vol - min->vol;
	adc = max->adc - min->adc;
	mul = (vol * 1000) / adc;
	add = (max->vol * 1000) - (max->adc * mul);
	if ( NULL != a ){
		*a = mul;
	}
	if ( NULL != b ){
		*b = add;
	}
	return 0;
}

static int adc_to_voltage( const struct adc_voltage *table, int size, unsigned short adc )
{
	int i;
	int a = 0;
	int b = 0;
	int err = 0;
	if ( NULL == table || size < 2 ){
		return 0;
	}
	for ( i = 0; i < size; i++ ){
		if ( adc < table[i].adc ){
			break;
		}
	}
	if ( 0 == i ){
		err = calc_voltage_ab( &table[0], &table[1], &a, &b );
	}else if ( i < size ){
		err = calc_voltage_ab( &table[i - 1], &table[i], &a, &b );
	}else{
		err = calc_voltage_ab( &table[size - 2], &table[size - 1], &a, &b );
	}
	if ( 0 != err ){
		return 0;
	}
	i = (adc * a + b);
	err = i % 1000;
	if ( err > 500 ){
		i += 1000;
	}
	return i / 1000;
}

static int calc_capacity_ab( const struct battery_capacity *min, const struct battery_capacity *max, int *a, int *b )
{
	int add;
	int mul;
	int voltage;
	int capacity;
	if ( NULL == min || NULL == max ){
		return -1;
	}
	capacity = max->capacity - min->capacity;
	voltage = max->voltage - min->voltage;
	mul = (capacity * 1000) / voltage;
	add = (max->capacity * 1000) - (max->voltage * mul);
	if ( NULL != a ){
		*a = mul;
	}
	if ( NULL != b ){
		*b = add;
	}
	return 0;	
}

static int voltage_to_capacity( const struct battery_capacity *table, int size, unsigned short voltage )
{
	int i;
	int a = 0;
	int b = 0;
	int err = 0;
	if ( NULL == table || size < 2 ){
		return 0;
	}
	for ( i = 0; i < size; i++ ){
		if ( voltage < table[i].voltage ){
			break;
		}
	}
	if ( 0 == i ){
		err = calc_capacity_ab( &table[0], &table[1], &a, &b );
	}else if ( i < size ){
		err = calc_capacity_ab( &table[i - 1], &table[i], &a, &b );
	}else{
		err = calc_capacity_ab( &table[size - 2], &table[size - 1], &a, &b );
	}
	if ( 0 != err ){
		return 0;
	}
	i = (voltage * a + b);
	err = i % 1000;
	if ( err > 500 ){
		i += 1000;
	}
	return i / 1000;
}

static int note_book_config( struct note_book *book, int status )
{
	printk( "note_book_config = %d, power_en = %d\n", status, book->notebook_conf.power_status );
	if ( status ){
		gpio_direction_output( book->note_gpio_cfg.notebook_power, book->notebook_conf.power_status ? GPIO_HIGH : GPIO_LOW );
		msleep( 100 );
		if ( GPIO_HIGH == gpio_get_value( book->note_gpio_cfg.notebook_low_battery ) ){
			book->battery_low = 0;
		}else{
			book->battery_low = 1;
		}
		//pad 连接底坐
	}else{
		//pad 与底坐断开
		book->battery_low = 0;
		gpio_direction_output( book->note_gpio_cfg.notebook_charge_pad, GPIO_LOW );
		mdelay( 100 );
		gpio_direction_output( book->note_gpio_cfg.notebook_power_pad, GPIO_LOW );
		mdelay( 100 );
		gpio_direction_output( book->note_gpio_cfg.notebook_power, GPIO_LOW );
		mdelay( 100 );
	}
	return 0;
}


static irqreturn_t note_book_interrupt( int irq, void *dev_id )
{
	struct note_book *book = (struct note_book *)dev_id;
	book->notebook_irq_status = 0;
	disable_irq_nosync( book->notebook_irq );
	queue_delayed_work( book->wq, &book->work, msecs_to_jiffies(1000) );
	return IRQ_HANDLED;
}

static irqreturn_t note_book_detect_interrupt( int irq, void *dev_id )
{
	struct note_book *book = (struct note_book *)dev_id;
	book->notebook_detect_irq_status = 0;
	disable_irq_nosync( book->notebook_detect_irq );
	queue_delayed_work( book->wq, &book->work_detect, msecs_to_jiffies(1000) );
	return IRQ_HANDLED;
}

static void notebook_work( struct work_struct *work )
{
	int status;
	struct note_book *book  = container_of( to_delayed_work(work), struct note_book, work );
	if ( 0 == book->use ){
		//如果驱动没有被初始化，禁止使用，解决插底坐不能重启
		return;
	}
	status = gpio_get_value( book->notebook_pin );
	printk( "notebook_work = %d\n", status );
	if ( book->status != status ){
		book->status = status;
		//device_earlysuspend( status );
		printk( "notebook_work %d\n", status );
		if ( status == book->notebook_level ){
			//休眠
			set_power_key_interrupt( 0 );
			if ( 0 == book->suspend ){
				rk29_send_power_key( 1 );
				rk29_send_power_key( 0 );
			}
		}else{
			//唤醒
			rk28_send_wakeup_key( );
			set_power_key_interrupt( 1 );
		}
		if ( status ){
			irq_set_irq_type( book->notebook_irq, IRQF_TRIGGER_FALLING ); //下降沿
		}else{
			irq_set_irq_type( book->notebook_irq, IRQF_TRIGGER_RISING ); //上升沿
		}
	}
	if ( 0 == book->notebook_irq_status ){
		book->notebook_irq_status = 1;
		enable_irq( book->notebook_irq );
	}
}

static void notebook_work_detect( struct work_struct *work )
{
	int status;
	struct note_book *book  = container_of( to_delayed_work(work), struct note_book, work_detect );
	status = gpio_get_value( book->pad_det_pin );
	if ( 0 == book->use ){
		//如果驱动没有被初始化，禁止使用，解决插底坐不能重启
		return;
	}
	printk( "notebook_work_detect = %d\n", status );
	if ( status ){
		irq_set_irq_type( book->notebook_irq, IRQF_TRIGGER_FALLING ); //下降沿
	}else{
		irq_set_irq_type( book->notebook_irq, IRQF_TRIGGER_RISING ); //上升沿
	}
	if ( 0 == book->notebook_detect_irq_status ){
		book->notebook_detect_irq_status = 1;
		enable_irq( book->notebook_detect_irq );
	}
	if ( 0 == book->battery_work_status ){
		queue_delayed_work( book->wq, &book->battery_work, msecs_to_jiffies(2000) );
	}
	if ( status == book->pad_det_level ){
		if ( 0 == book->notebook_irq_status ){
			book->notebook_irq_status = 1;
			enable_irq( book->notebook_irq );
		}
	}
	note_book_config( book, status == book->pad_det_level );
}

static int get_ac_status( struct note_book *book )
{
	if ( gpio_get_value( book->note_gpio_cfg.notebook_dc_det ) == GPIO_LOW ){
		return 1;
	}else{
		return 0;
	}
}

static int get_charge_status( struct note_book *book )
{
	if ( gpio_get_value( book->note_gpio_cfg.notebook_charge_ok ) == GPIO_HIGH ){
		return 1;
	}else{
		return 0;
	}
}

static int notebook_get_battery_status( struct note_book *book )
{
	int i;
	int current_voltage;
	unsigned short adc_value;
	unsigned short adc_buffer[8] = {0};
	for ( i = 0; i < sizeof(adc_buffer) / sizeof(adc_buffer[0]); i++ ){
		adc_buffer[i] = sync_read_adc( book );
		mdelay( 1 );
	}
	adc_value = average( adc_buffer, sizeof(adc_buffer) / sizeof(adc_buffer[0]) );
	//printk( "notebook adc_value = %d\n", adc_value );
	adc_value += 256;
	if ( get_ac_status( book ) ){
		if ( 1 == get_charge_status( book ) ){
			if ( book->charge_ok_number < 10 ){
				book->charge_ok_number++;
			}
		}else{
			book->charge_ok_number = 0;
		}
		if ( 1 == get_charge_status( book ) && book->charge_ok_number >= 3 ){
			book->note_battery.status = POWER_SUPPLY_STATUS_FULL; //充完
		}else{
			book->note_battery.status = POWER_SUPPLY_STATUS_CHARGING; //正在充电
			//adc_value -= 22; //测试实际数据,恒流充电 
		}
	}else{
		book->charge_ok_number = 0;
		book->note_battery.status = POWER_SUPPLY_STATUS_DISCHARGING;
	}
	/*get present voltage*/
	current_voltage = adc_to_voltage( m_adc_voltage_table, sizeof(m_adc_voltage_table) / sizeof(m_adc_voltage_table[0]), adc_value );
	book->note_battery.voltage = current_voltage;
	/*calc battery capacity*/
	if ( get_ac_status( book ) ){
		int array_size = 0;
		if ( POWER_SUPPLY_STATUS_FULL == book->note_battery.status ){
			//充满
			book->note_battery.level = 100;
		}else{
			//没有充满
			array_size = sizeof(m_battery_table) / sizeof(m_battery_table[0]);
			book->note_battery.level = voltage_to_capacity( m_battery_table, array_size , current_voltage );
			//printk( "book->note_battery.level = %d\n", book->note_battery.level );
			if ( book->note_battery.level > 99 ){
				book->note_battery.level = 99;
			}
			if ( book->note_battery.level < 1 ){
				book->note_battery.level = 1;
			}
			if ( -1 == book->battery_level ){
				book->battery_level = book->note_battery.level;
			}
			//消抖
			if ( book->note_battery.level < book->battery_level ){
				book->note_battery.level = book->battery_level;
			}else{
				book->battery_level = book->note_battery.level;
			}
		}
		//printk( "book->note_battery.level = %d, book->battery_level = %d\n", book->note_battery.level, book->battery_level );
	}else{
		int array_size = sizeof(m_battery_table) / sizeof(m_battery_table[0]);
		book->note_battery.level = voltage_to_capacity( m_battery_table, array_size, current_voltage );
		//printk( "book->note_battery.level = %d\n", book->note_battery.level );
		if ( book->note_battery.level > 99 ){
			book->note_battery.level = 99;
		}
		//printk( "aa--book->note_battery.level = %d, book->battery_level = %d\n", book->note_battery.level, book->battery_level );
		if ( book->note_battery.level < 0 ){
			book->note_battery.level = 0;
		}
		if ( -1 == book->battery_level ){
			book->battery_level = book->note_battery.level;
		}
		//消抖
		if ( book->note_battery.level < book->battery_level ){
			book->battery_level = book->note_battery.level;
		}else{
			book->note_battery.level = book->battery_level;
		}
		//printk( "aaa--book->note_battery.level = %d, book->battery_level = %d\n", book->note_battery.level, book->battery_level );
	}
	return 0;
}

static void notebook_control_power( struct note_book *book )
{
	int dc_in = 0;
	if ( gpio_get_value( book->pad_det_pin ) == book->pad_det_level ){
		//printk( "notebook_low_battery = %d, notebook_dc_det = %d\n", gpio_get_value( book->note_gpio_cfg.notebook_low_battery ), gpio_get_value(book->note_gpio_cfg.notebook_dc_det) );
		gpio_direction_output( book->note_gpio_cfg.notebook_power, book->notebook_conf.power_status ? GPIO_HIGH : GPIO_LOW );
		msleep( 100 );
		if ( GPIO_LOW == gpio_get_value(book->note_gpio_cfg.notebook_dc_det) ){
			dc_in = 1;
			gpio_direction_input( book->note_gpio_cfg.notebook_low_battery_led );
		}
		if ( dc_in || GPIO_HIGH == gpio_get_value( book->note_gpio_cfg.notebook_low_battery ) ){
			if ( 0 == dc_in && book->battery_low == 1 ){
				//printk( "notebook_low_battery GPIO_LOW not charge\n" );
			}else{
				book->battery_low = 0;
				gpio_direction_output( book->note_gpio_cfg.notebook_power_pad, book->notebook_conf.power_pad_status ? GPIO_HIGH : GPIO_LOW );
				gpio_direction_output( book->note_gpio_cfg.notebook_charge_pad, book->notebook_conf.power_charge_status ? GPIO_HIGH : GPIO_LOW );
			}
		}else{
			//printk( "notebook_low_battery GPIO_LOW power_pad = %d, charge_pad = %d\n", book->notebook_conf.power_pad_status, book->notebook_conf.power_charge_status );
			book->battery_low = 1;
			gpio_direction_output( book->note_gpio_cfg.notebook_charge_pad, GPIO_LOW );
			gpio_direction_output( book->note_gpio_cfg.notebook_power_pad, GPIO_LOW );
			gpio_direction_output( book->note_gpio_cfg.notebook_low_battery_led, GPIO_HIGH );
		}
	}
}

static void notebook_battery_work( struct work_struct *work )
{
	struct note_book *book  = container_of( to_delayed_work(work), struct note_book, battery_work );
	if ( 0 == book->use ){
		return;
	}
	notebook_control_power( book );
	if ( gpio_get_value( book->pad_det_pin ) == book->pad_det_level ){
		notebook_get_battery_status( book );
		queue_delayed_work( book->wq, &book->battery_work, msecs_to_jiffies(2000) );
	}else{
		book->battery_work_status = 0;
	}
}

static volatile void jbsmdelay( int delay )
{
	volatile int i;
	volatile int index;
	volatile int value = 0;
	for ( i = 0; i < delay; i++ ){
		for ( index = 0; index < 0x10000; index++ ){
			value = index + i;
		}
	}
}

static void notebook_shutdown( struct platform_device *pdev )
{
	struct note_book *book = platform_get_drvdata( pdev );
	if ( NULL != book ){
		book->use = 0;
		cancel_delayed_work_sync( &book->battery_work );
		gpio_direction_output( book->note_gpio_cfg.notebook_charge_pad, GPIO_LOW );
		jbsmdelay( 1000 );
		gpio_direction_output( book->note_gpio_cfg.notebook_power_pad, GPIO_LOW );
		jbsmdelay( 1000 );
		gpio_direction_output( book->note_gpio_cfg.notebook_power, GPIO_LOW );
		jbsmdelay( 1000 );
		printk( "notebook_shutdown\n" );
	}	
}

static int notebook_suspend( struct platform_device *pdev, pm_message_t state )
{
	struct note_book *book = platform_get_drvdata( pdev );
	if ( NULL != book ){
		if ( gpio_get_value( book->pad_det_pin ) == book->pad_det_level ){
			gpio_direction_output( book->note_gpio_cfg.notebook_power, GPIO_LOW );
		}
		if ( gpio_get_value( book->notebook_pin ) == book->notebook_level ){
			enable_irq_wake( book->notebook_irq );
			book->enable_wakeup = 1;
		}
	}
	return 0;
}

static int notebook_resume( struct platform_device *pdev )
{
	struct note_book *book = platform_get_drvdata( pdev );
	book->battery_level = -1;
	if ( NULL != book ){
		if ( book->enable_wakeup ){
			disable_irq_wake( book->notebook_irq );
			book->enable_wakeup = 0;
		}
	}
	return 0;
}

static int notebook_open( struct inode *inode, struct file *file )
{
	return 0;
}

static int notebook_release( struct inode *inode, struct file *file )
{
	return 0;
}

static ssize_t notebook_read( struct file *file, char __user *buf, size_t count, loff_t *offset )
{
	return 0;
}

enum notebook_ioctl
{
	POWER,
	CHARGE,
	DEVICE,
	POWERSUPPLY,
	BATTERYINFO,
	PADDCSTATUS,
	PADBATTERYLOW,
};


struct battery_info_buffer
{
	int level;
	int status;
	int voltage;
};

union note_args_buffer
{
	int value;
	struct battery_info_buffer battery_info;
}; 

struct note_args
{
	enum notebook_ioctl cmd;
	union note_args_buffer buffer;
};	

extern int rk3066_get_battery_low( void );
extern int rk3066_battery_get_dc_status( void );

static int get_data( struct note_book *book, struct note_args *info )
{
	switch ( info->cmd )
	{
	case POWER:
		info->buffer.value = book->notebook_conf.power_status;
		break;
	case CHARGE:
		info->buffer.value = book->notebook_conf.power_charge_status;
		break;
	case DEVICE:
		if ( gpio_get_value( book->pad_det_pin ) == book->pad_det_level ){
			info->buffer.value = 1;
		}else{
			info->buffer.value = 0;
		}
		break;
	case POWERSUPPLY:
		info->buffer.value = book->notebook_conf.power_pad_status;
		break;
	case BATTERYINFO:
		if ( gpio_get_value( book->pad_det_pin ) != book->pad_det_level ){
			printk( "get_data not get battery info\n" );
			return -1;
		}
		//printk( "info->buffer.battery_info.level = %d\n", book->note_battery.level );
		info->buffer.battery_info.level = 88; //book->note_battery.level;
		info->buffer.battery_info.voltage = book->note_battery.voltage;
		switch ( book->note_battery.status )
		{
		case POWER_SUPPLY_STATUS_FULL:
			info->buffer.battery_info.status = 2;
			break;
		case POWER_SUPPLY_STATUS_CHARGING:
			info->buffer.battery_info.status = 1;
			break;
		case POWER_SUPPLY_STATUS_DISCHARGING:
			info->buffer.battery_info.status = 0;
			break;
		default:
			printk( "get_data battery status Unknown\n" );
			return -1;
		}
		//printk( "info->buffer.battery_info.status = %d\n", info->buffer.battery_info.status );
		break;
	case PADDCSTATUS:
		info->buffer.value = rk3066_battery_get_dc_status( );
		break;
	case PADBATTERYLOW:
		info->buffer.value = rk3066_get_battery_low( );
		break;
	default:
		printk( "get_data other cmd not Support\n" );
		return -1;
	}
	return 0;
}

static int set_data( struct note_book *book, struct note_args *info )
{
	printk( "set_data cmd = %d, %d\n", info->cmd, info->buffer.value );
	switch ( info->cmd )
	{
	case POWER:
		book->notebook_conf.power_status = info->buffer.value;
		break;
	case CHARGE:
		book->notebook_conf.power_charge_status = info->buffer.value;
		break;
	case POWERSUPPLY:
		book->notebook_conf.power_pad_status = info->buffer.value;
		break;
	case BATTERYINFO:
		printk( "notebook_ioctl cmd faild\n" );
		return -1;
	default:
		printk( "set_data other cmd not Support\n" );
		return -1;
	}
	return 0;
}

static ssize_t notebook_write( struct file *file, const char __user *buf, size_t count, loff_t *offset )
{
	struct note_book *book = m_notebook;
	if ( 0x19840818 != count || NULL != buf ){
		return count;
	}
	if ( NULL != book ){
		if ( 0 == book->use ){
			book->use = 1;
			printk ( "notebook_open fast!!!!!!\r\n" );
			if ( gpio_get_value( book->pad_det_pin ) == book->pad_det_level ){
				printk ( "notebook_open queue_delayed_work!!!!!!\r\n" );
				queue_delayed_work( book->wq, &book->work_detect, msecs_to_jiffies(0) );
			}
		}
	}
	return count;
}

static long notebook_ioctl( struct file *file, unsigned int cmd, unsigned long arg )
{
	struct note_book *book = m_notebook;
	if ( NULL == book ){
		return -1;
	}
	switch( cmd )
	{
	case 0:
		if ( NULL != (char __user *)arg ){
			struct note_args info;
			memset( &info, 0, sizeof(struct note_args) );
			if ( copy_from_user( &info, (struct note_args __user *)arg, sizeof(struct note_args) ) ){
				printk( "notebook_ioctl copy_from_user faild\n" );
				return -1;
			}
			//printk( "notebook_ioctl cmd_index = 0x%x\n", info.cmd );
			if ( -1 == get_data( book, &info ) ){
				return -1;
			}
			if ( copy_to_user( (struct note_args __user *)arg, &info, sizeof(struct note_args) ) ){
				printk( "notebook_ioctl copy_to_user faild\n" );
				return -1;
			}
		}
		break;
	case 1: //set
		if ( NULL != (char __user *)arg ){
			struct note_args info;
			memset( &info, 0, sizeof(struct note_args) );
			if ( copy_from_user( &info, (struct note_args __user *)arg, sizeof(struct note_args) ) ){
				printk( "notebook_ioctl copy_from_user faild\n" );
				return -1;
			}
			if ( -1 == set_data( book, &info ) ){
				return -1;
			}
			notebook_control_power( book );
			if ( gpio_get_value( book->pad_det_pin ) == book->pad_det_level ){
				notebook_get_battery_status( book );
			}
		}
		break;
	default:
		break;
	}
	return 0;
}


static struct file_operations m_notebook_fops = {
	.owner = THIS_MODULE,
	.open = notebook_open,
	.read = notebook_read,
	.write = notebook_write,
	.release = notebook_release,
	.unlocked_ioctl = notebook_ioctl
};

static struct miscdevice m_notebook_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "notebook",
	.fops = &m_notebook_fops
};

static int notebook_probe( struct platform_device *pdev )
{		
	int ret = 0;
	struct note_book *book = NULL;
	struct adc_client *client = NULL;
	struct rk3066_notebook_platform_data *pdata = pdev->dev.platform_data;
	printk( "notebook_probe\n" );
	mdelay( 100 );
	if ( NULL != pdata ){
		if ( pdata->io_init ){
			pdata->io_init( );
		}
	}
	book = kzalloc( sizeof(struct note_book), GFP_KERNEL );
	client = adc_register( 2, notebook_battery_callback, book );
	if ( !client ){
		kfree( book );
		printk("adc register failed");
		return -1;
	}	
	book->client = client;
	
	book->use = 0;
	book->battery_level = -1;
	book->charge_ok_number = 0;
	book->notebook_pin = pdata->notebook_pin;
	book->notebook_level = pdata->notebook_level;
	book->pad_det_pin = pdata->pad_det_pin;
	book->pad_det_level = pdata->pad_det_level;
	
	book->note_gpio_cfg.notebook_power = pdata->notebook_power;
	book->note_gpio_cfg.notebook_power_pad = pdata->notebook_power_pad;
	book->note_gpio_cfg.notebook_charge_pad = pdata->notebook_charge_pad;
	
	book->note_gpio_cfg.notebook_dc_det = pdata->notebook_dc_det;
	book->note_gpio_cfg.notebook_charge_ok = pdata->notebook_charge_ok;
	book->note_gpio_cfg.notebook_low_battery = pdata->notebook_low_battery;
	book->note_gpio_cfg.notebook_low_battery_led = pdata->notebook_low_battery_led;
	
	book->notebook_conf.power_status = 0;
	book->notebook_conf.power_pad_status = 0;
	book->notebook_conf.power_charge_status = 0;
	memset( &book->note_battery, 0, sizeof(struct note_book_battery) );

	book->suspend = 0;
	book->enable_wakeup = 0;
	book->notebook_irq_status = 1;
	book->battery_work_status = 0;
	book->notebook_detect_irq_status = 1;
	book->notebook_irq = gpio_to_irq( book->notebook_pin );
	book->notebook_detect_irq = gpio_to_irq( book->pad_det_pin );
	book->status = gpio_get_value( book->notebook_pin );
	book->wq = create_singlethread_workqueue( "note_book_wq" );
	INIT_DELAYED_WORK( &book->work, notebook_work );
	INIT_DELAYED_WORK( &book->work_detect, notebook_work_detect );
	INIT_DELAYED_WORK( &book->battery_work, notebook_battery_work );
	m_notebook = book;
	printk( "NoteBook pad_det_pin = %d, notebook_pin = %d\n", gpio_get_value( book->pad_det_pin ), gpio_get_value( book->notebook_pin ) );
	if ( gpio_get_value( book->notebook_pin ) ==  book->notebook_level ){
		//如果在开机状态检测到合屏,则关机
		printk( "notebook_probe notebook is not open..\n" );
		mdelay( 1000 );
		pm_power_off( );
		mdelay( 1000 );
		return 0;
	}
	platform_set_drvdata( pdev, book );
	printk("RK3066 NoteBook Driver Initialized.\n");
	return ret;
}

int rk3066_notebook_interface_init( void )
{
	int ret;
	struct note_book *book = m_notebook;
	if ( NULL == book ){
		return -1;
	}
	gpio_direction_output( book->note_gpio_cfg.notebook_charge_pad, GPIO_LOW );
	mdelay( 100 );
	if ( gpio_get_value( book->pad_det_pin ) == book->pad_det_level && 1 == rk3066_get_battery_low( ) ){
		//如果接了底坐，平板电量底, 打开底坐供电
		book->notebook_conf.power_pad_status = 1;
		gpio_direction_output( book->note_gpio_cfg.notebook_power_pad, GPIO_HIGH );
		mdelay( 100 );
		if ( 0 == get_ac_status(book) && GPIO_LOW == gpio_get_value( book->note_gpio_cfg.notebook_low_battery ) ){
			//如果底坐没有接AC, 并且底坐电池电量底
			printk( "notebook_probe battery low power off..\n" );
			pm_power_off( );
			mdelay( 1000 );
			return 0;
		}
	}else{
		gpio_direction_output( book->note_gpio_cfg.notebook_power_pad, GPIO_LOW );
	}
	mdelay( 100 );
	gpio_direction_output( book->note_gpio_cfg.notebook_power, GPIO_LOW );
	mdelay( 100 );

	//pad 检测中断
	if ( gpio_get_value( book->pad_det_pin ) ){
		printk( "note_book_detect_interrupt IRQF_TRIGGER_FALLING\n" );
		ret = request_irq( book->notebook_detect_irq, note_book_detect_interrupt, IRQF_TRIGGER_FALLING, "note_book_detect", (void *)book );
	}else{
		printk( "note_book_detect_interrupt IRQF_TRIGGER_RISING\n" );
		ret = request_irq( book->notebook_detect_irq, note_book_detect_interrupt, IRQF_TRIGGER_RISING, "note_book_detect", (void *)book );
	}
	//合屏检测中断
	if ( gpio_get_value( book->notebook_pin ) ){
		printk( "note_book_interrupt IRQF_TRIGGER_FALLING\n" );
		ret = request_irq( book->notebook_irq, note_book_interrupt, IRQF_TRIGGER_FALLING, "note_book", (void *)book );
	}else{
		printk( "note_book_interrupt IRQF_TRIGGER_RISING\n" );
		ret = request_irq( book->notebook_irq, note_book_interrupt, IRQF_TRIGGER_RISING, "note_book", (void *)book );
	}
#if 0
	if ( gpio_get_value( book->pad_det_pin ) != book->pad_det_level ){
		//如果没有接底坐,关闭合屏检测中断
		book->notebook_irq_status = 0;
		disable_irq_nosync( book->notebook_irq );
	}
#else
	//驱动没有被初始化，禁止使用，解决插底坐不能重启
	disable_irq_nosync( book->notebook_irq );
	book->notebook_irq_status = 0;
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
	book->device_early.suspend = early_suspend;
	book->device_early.resume = early_resume;
	book->device_early.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN - 2;
	register_early_suspend( &book->device_early );//xsf
#endif
	ret = misc_register( &m_notebook_misc );
	if ( ret ){
		printk( "misc_register err\n" );
	}
	return 0;
}

static struct platform_driver rk3066_notebook_driver = {
	.probe    = notebook_probe,
	.shutdown = notebook_shutdown,
	.suspend  = notebook_suspend,
	.resume   = notebook_resume,
	.driver   = {
		.name   = "rk3066-notebook",
		.owner  = THIS_MODULE,
	},
};

int __init rk3066_notebook_init(void)
{
	return platform_driver_register( &rk3066_notebook_driver );
}

void __exit rk3066_notebook_exit( void )
{
	platform_driver_unregister( &rk3066_notebook_driver );
}

int get_mach_is_notebook( void )
{
	int result = 0;
	if ( m_notebook == NULL ){
		printk( "m_notebook get_mach_is_notebook is null\n" );
	}
	if ( gpio_get_value( m_notebook->pad_det_pin ) == m_notebook->pad_det_level ) {
		if ( gpio_get_value( m_notebook->notebook_pin ) == m_notebook->notebook_level ){
			result = 1;
		}
	}
	printk( "get_mach_is_notebook = %d, %d, %d\n", result, gpio_get_value( m_notebook->pad_det_pin ), gpio_get_value( m_notebook->notebook_pin ) );
	return result;
}

int notebook_is_connect( void )
{
	int result = 0;
	if ( m_notebook == NULL ){
		printk( "m_notebook notebook_is_connect is null\n" );
	}
	if ( gpio_get_value( m_notebook->pad_det_pin ) == m_notebook->pad_det_level ) {
		result = 1;
	}
	return result;
}

module_init(rk3066_notebook_init);

module_exit(rk3066_notebook_exit);
