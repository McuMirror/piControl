/*
 * revpi_core.c - RevPi Core specific functions
 *
 * Copyright (C) 2017 KUNBUS GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2) as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>	// included for all kernel modules
#include <linux/kernel.h>	// included for KERN_INFO
#include <linux/init.h>		// included for __init and __exit macros
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <asm/uaccess.h>
#include <asm/elf.h>
#include <linux/semaphore.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/delay.h>
#include <linux/rtmutex.h>
#include <linux/sem.h>
#include <linux/gpio.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/thermal.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <asm/div64.h>

#include "common_define.h"
#include "project.h"

#include <bsp/ksz8851/ksz8851.h>
#include <bsp/spi/spi.h>

#include "revpi_common.h"
#include "revpi_core.h"

static const struct kthread_prio revpi_core_kthread_prios[] = {
	/* spi pump to RevPi Gateways */
	{ .comm = "spi0",		.prio = MAX_USER_RT_PRIO/2 + 4 },
	{ }
};

static struct gpiod_lookup_table revpi_core_gpios = {
	.dev_id = "piControl0",
	.table  = { GPIO_LOOKUP_IDX("pinctrl-bcm2835", 42, "Sniff1A", 0, GPIO_ACTIVE_HIGH),	// 1 links
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835", 43, "Sniff1B", 0, GPIO_ACTIVE_HIGH),	// 1 rechts
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835", 28, "Sniff2A", 0, GPIO_ACTIVE_HIGH),	// 2 links
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835", 29, "Sniff2B", 0, GPIO_ACTIVE_HIGH),	// 2 rechts
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835", 35, "KSZ0",    0, GPIO_ACTIVE_HIGH),	// KSZ CS rechts
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835", 36, "KSZ1",    0, GPIO_ACTIVE_HIGH),	// KSZ CS links
	},
};

// im Schaltplan heißen die Pins 1B und 2B, da sie links sind, müssten sie aber 1A und 1B heißen.
// hier in der Software werden sie mit A benannt
static struct gpiod_lookup_table revpi_connect_gpios = {
	.dev_id = "piControl0",
	.table  = { GPIO_LOOKUP_IDX("pinctrl-bcm2835", 43, "Sniff1A",	0, GPIO_ACTIVE_HIGH),	// 1 links
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835", 29, "Sniff2A",	0, GPIO_ACTIVE_HIGH),	// 2 links
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835", 35, "KSZ0",	0, GPIO_ACTIVE_HIGH),	// KSZ CS links
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835",  0, "X2_DI",	0, GPIO_ACTIVE_HIGH),	// Digital In auf X2 Stecker
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835",  1, "X2_DO",	0, GPIO_ACTIVE_HIGH),	// Digital Out (Relais) auf X2 Stecker
		    GPIO_LOOKUP_IDX("pinctrl-bcm2835", 42, "WDTrigger", 0, GPIO_ACTIVE_HIGH),	// Watchdog trigger
	},
};

static struct module *piSpiModule;

SRevPiCore piCore_g;


static enum hrtimer_restart piControlGateTimer(struct hrtimer *pTimer)
{
	up(&piCore_g.gateSem);
	return HRTIMER_NORESTART;
}

static int piGateThread(void *data)
{
	//TODO int value = 0;
	ktime_t time;
	INT8U i8uLastState[2];
	int i;
	s64 interval;
#ifdef VERBOSE
	INT16U val;
	val = 0;
#endif

	hrtimer_init(&piCore_g.gateTimer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	piCore_g.gateTimer.function = piControlGateTimer;
	i8uLastState[0] = 0;
	i8uLastState[1] = 0;

	//TODO down(&piDev.gateSem);
	pr_info("number of CPUs: %d\n", NR_CPUS);
	if (NR_CPUS == 1) {
		// use a longer interval time on CM1
		interval = INTERVAL_PI_GATE + INTERVAL_PI_GATE;
	} else {
		interval = INTERVAL_PI_GATE;
	}

	time = hrtimer_cb_get_time(&piCore_g.gateTimer);

	/* start after one second */
	time.tv64 += HZ;

	pr_info("mGate thread started\n");

	while (!kthread_should_stop()) {
		time.tv64 += interval;

		hrtimer_start(&piCore_g.gateTimer, time, HRTIMER_MODE_ABS);
		down(&piCore_g.gateSem);

		if (isRunning()) {
			if (piDev_g.stopIO == false) {
				my_rt_mutex_lock(&piDev_g.lockPI);
				if (piDev_g.machine_type == REVPI_CORE) {
					if (piCore_g.i8uRightMGateIdx != REV_PI_DEV_UNDEF) {
						memcpy(piCore_g.ai8uOutput,
						       piDev_g.ai8uPI +
						       RevPiDevice_getDev(piCore_g.i8uRightMGateIdx)->i16uOutputOffset,
						       RevPiDevice_getDev(piCore_g.i8uRightMGateIdx)->sId.i16uFBS_OutputLength);
					}
				}
				if (piCore_g.i8uLeftMGateIdx != REV_PI_DEV_UNDEF) {
					memcpy(piCore_g.ai8uOutput + KB_PD_LEN,
					       piDev_g.ai8uPI +
					       RevPiDevice_getDev(piCore_g.i8uLeftMGateIdx)->i16uOutputOffset,
					       RevPiDevice_getDev(piCore_g.i8uLeftMGateIdx)->sId.i16uFBS_OutputLength);
				}
				rt_mutex_unlock(&piDev_g.lockPI);
			}
		}

		MODGATECOM_run();

		if (MODGATECOM_has_fatal_error()) {
			// stop the thread if an fatal error occurred
			pr_err("mGate exit thread because of fatal error 0x%08x\n", MODGATECOM_has_fatal_error());
			return -1;
		}

		if (piCore_g.i8uRightMGateIdx == REV_PI_DEV_UNDEF
		    && AL_Data_s[0].i8uState >= MODGATE_ST_RUN_NO_DATA && i8uLastState[0] < MODGATE_ST_RUN_NO_DATA) {
			// das mGate wurde beim Scan nicht erkannt, PiBridgeEth Kommunikation ist aber möglich
			// suche den Konfigeintrag dazu
			pr_info("search for right mGate %d\n", AL_Data_s[0].OtherID.i16uModulType);
			for (i = 0; i < RevPiDevice_getDevCnt(); i++) {
				if (RevPiDevice_getDev(i)->sId.i16uModulType ==
				    (AL_Data_s[0].OtherID.i16uModulType | PICONTROL_NOT_CONNECTED)
				    && RevPiDevice_getDev(i)->i8uAddress >= REV_PI_DEV_FIRST_RIGHT) {
					pr_info("found mGate %d\n", i);
					RevPiDevice_getDev(i)->i8uActive = 1;
					RevPiDevice_getDev(i)->sId.i16uModulType &= PICONTROL_NOT_CONNECTED_MASK;
					piCore_g.i8uRightMGateIdx = i;
					break;
				}
			}
		}
		if (piCore_g.i8uLeftMGateIdx == REV_PI_DEV_UNDEF
		    && AL_Data_s[1].i8uState >= MODGATE_ST_RUN_NO_DATA && i8uLastState[1] < MODGATE_ST_RUN_NO_DATA) {
			// das mGate wurde beim Scan nicht erkannt, PiBridgeEth Kommunikation ist aber möglich
			// suche den Konfigeintrag dazu
			pr_info("search for left mGate %d\n", AL_Data_s[1].OtherID.i16uModulType);
			for (i = 0; i < RevPiDevice_getDevCnt(); i++) {

				if (RevPiDevice_getDev(i)->sId.i16uModulType ==
				    (AL_Data_s[1].OtherID.i16uModulType | PICONTROL_NOT_CONNECTED)
				    && RevPiDevice_getDev(i)->i8uAddress < REV_PI_DEV_FIRST_RIGHT) {
					pr_info("found mGate %d\n", i);
					RevPiDevice_getDev(i)->i8uActive = 1;
					RevPiDevice_getDev(i)->sId.i16uModulType &= PICONTROL_NOT_CONNECTED_MASK;
					piCore_g.i8uLeftMGateIdx = i;
					break;
				}
			}
		}

		i8uLastState[0] = AL_Data_s[0].i8uState;
		i8uLastState[1] = AL_Data_s[1].i8uState;

		if (isRunning()) {
			if (piDev_g.stopIO == false) {
				my_rt_mutex_lock(&piDev_g.lockPI);
				if (piDev_g.machine_type == REVPI_CORE) {
					if (piCore_g.i8uRightMGateIdx != REV_PI_DEV_UNDEF) {
						memcpy(piDev_g.ai8uPI +
						       RevPiDevice_getDev(piCore_g.i8uRightMGateIdx)->i16uInputOffset, piCore_g.ai8uInput,
						       RevPiDevice_getDev(piCore_g.i8uRightMGateIdx)->sId.i16uFBS_InputLength);
					}
				}
				if (piCore_g.i8uLeftMGateIdx != REV_PI_DEV_UNDEF) {
					memcpy(piDev_g.ai8uPI +
					       RevPiDevice_getDev(piCore_g.i8uLeftMGateIdx)->i16uInputOffset,
					       piCore_g.ai8uInput + KB_PD_LEN,
					       RevPiDevice_getDev(piCore_g.i8uLeftMGateIdx)->sId.i16uFBS_InputLength);
				}
				rt_mutex_unlock(&piDev_g.lockPI);
			}
		}

#ifdef VERBOSE
		val++;
		if (val >= 200) {
			val = 0;
			if (piCore_g.i8uRightMGateIdx != REV_PI_DEV_UNDEF) {
				pr_info("right  %02x %02x   %d %d\n",
					*(piDev_g.ai8uPI +
					  RevPiDevice.dev[piCore_g.i8uRightMGateIdx].i16uInputOffset),
					*(piDev_g.ai8uPI +
					  RevPiDevice.dev[piCore_g.i8uRightMGateIdx].i16uOutputOffset),
					RevPiDevice.dev[piCore_g.i8uRightMGateIdx].i16uInputOffset,
					RevPiDevice.dev[piCore_g.i8uRightMGateIdx].i16uOutputOffset);
			} else {
				pr_info("right  no device\n");
			}

			if (piCore_g.i8uLeftMGateIdx != REV_PI_DEV_UNDEF) {
				pr_info("left %02x %02x   %d %d\n",
					*(piDev_g.ai8uPI +
					  RevPiDevice.dev[piCore_g.i8uLeftMGateIdx].i16uInputOffset),
					*(piDev_g.ai8uPI +
					  RevPiDevice.dev[piCore_g.i8uLeftMGateIdx].i16uOutputOffset),
					RevPiDevice.dev[piCore_g.i8uLeftMGateIdx].i16uInputOffset,
					RevPiDevice.dev[piCore_g.i8uLeftMGateIdx].i16uOutputOffset);
			} else {
				pr_info("left   no device\n");
			}
		}
#endif
	}

	pr_info("mGate exit\n");
	return 0;
}

static enum hrtimer_restart piIoTimer(struct hrtimer *pTimer)
{
	up(&piCore_g.ioSem);
	return HRTIMER_NORESTART;
}

static int piIoThread(void *data)
{
	//TODO int value = 0;
	ktime_t time;
	ktime_t now;
	s64 tDiff;

	hrtimer_init(&piCore_g.ioTimer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	piCore_g.ioTimer.function = piIoTimer;

	pr_info("piIO thread started\n");

	now = hrtimer_cb_get_time(&piCore_g.ioTimer);

	PiBridgeMaster_Reset();

	while (!kthread_should_stop()) {
		if (PiBridgeMaster_Run() < 0)
			break;

		time = now;
		now = hrtimer_cb_get_time(&piCore_g.ioTimer);

		time = ktime_sub(now, time);
		piCore_g.image.drv.i8uIOCycle = ktime_to_ms(time);

		if (!ktime_equal(piDev_g.tLastOutput1, piDev_g.tLastOutput2)) {
			tDiff = ktime_to_ns(ktime_sub(piDev_g.tLastOutput1, piDev_g.tLastOutput2));
			tDiff = tDiff << 1;	// multiply by 2
			if (ktime_to_ns(ktime_sub(now, piDev_g.tLastOutput1)) > tDiff && isRunning()) {
				int i;
				// the outputs were not written by logiCAD for more than twice the normal period
				// the logiRTS must have been stopped or crashed
				// -> set all outputs to 0
				pr_info("logiRTS timeout, set all output to 0\n");
				if (piDev_g.stopIO == false) {
					my_rt_mutex_lock(&piDev_g.lockPI);
					for (i = 0; i < piDev_g.cl->i16uNumEntries; i++) {
						uint16_t len = piDev_g.cl->ent[i].i16uLength;
						uint16_t addr = piDev_g.cl->ent[i].i16uAddr;

						if (len >= 8) {
							len /= 8;
							memset(piDev_g.ai8uPI + addr, 0, len);
						} else {
							uint8_t val;
							uint8_t mask = piDev_g.cl->ent[i].i8uBitMask;

							val = piDev_g.ai8uPI[addr];
							val &= ~mask;
							piDev_g.ai8uPI[addr] = val;
						}
					}
					rt_mutex_unlock(&piDev_g.lockPI);
				}
				piDev_g.tLastOutput1 = ktime_set(0, 0);
				piDev_g.tLastOutput2 = ktime_set(0, 0);
			}
		}

		revpi_check_timeout();

		if (piCore_g.eBridgeState == piBridgeInit) {
			time.tv64 += INTERVAL_RS485;
		} else {
			time.tv64 += INTERVAL_IO_COMM;
		}

		if ((now.tv64 - time.tv64) > 0) {
			// the call of PiBridgeMaster_Run() needed more time than the INTERVAL
			// -> wait an additional ms
			//pr_info("%d ms too late, state %d\n", (int)((now.tv64 - time.tv64) >> 20), piCore_g.eBridgeState);
			time.tv64 = now.tv64 + INTERVAL_ADDITIONAL;
		}

		hrtimer_start(&piCore_g.ioTimer, time, HRTIMER_MODE_ABS);
		down(&piCore_g.ioSem);	// wait for timer
	}

	RevPiDevice_finish();

	pr_info("piIO exit\n");
	return 0;
}


int revpi_core_get_spi_speed(void)
{
	// return the speed in MHz of the SPI bus to the KSZ8851
	if (piDev_g.machine_type == REVPI_CORE) {
		return 20000000;	// 20 MHz on Core
	} else {
		return 10000000;	// 10 MHz on Connect
	}
}

int revpi_core_init(void)
{
	struct sched_param param;
	INT32U i32uRv;
	int ret = 0;

	piCore_g.i8uLeftMGateIdx = REV_PI_DEV_UNDEF;
	piCore_g.i8uRightMGateIdx = REV_PI_DEV_UNDEF;

	if (piDev_g.machine_type == REVPI_CORE) {
		// the Core has two modular gateway ports
		piCore_g.abMGateActive[0] = true;
		piCore_g.abMGateActive[1] = true;
		gpiod_add_lookup_table(&revpi_core_gpios);
	} else {
		// the connect has only a only one modular gateway port on the left
		piCore_g.abMGateActive[0] = false;
		piCore_g.abMGateActive[1] = true;
		gpiod_add_lookup_table(&revpi_connect_gpios);
	}

	rt_mutex_init(&piCore_g.lockUserTel);
	sema_init(&piCore_g.semUserTel, 0);
	piCore_g.pendingUserTel = false;

	rt_mutex_init(&piCore_g.lockGateTel);
	sema_init(&piCore_g.semGateTel, 0);
	piCore_g.pendingGateTel = false;

	rt_mutex_init(&piCore_g.lockBridgeState);
	sema_init(&piCore_g.ioSem, 0);

	piCore_g.gpio_sniff1a = gpiod_get(piDev_g.dev, "Sniff1A", GPIOD_IN);
	if (IS_ERR(piCore_g.gpio_sniff1a)) {
		pr_err("cannot acquire gpio sniff 1a\n");
		return PTR_ERR(piCore_g.gpio_sniff1a);
	}
	piCore_g.gpio_sniff2a = gpiod_get(piDev_g.dev, "Sniff2A", GPIOD_IN);
	if (IS_ERR(piCore_g.gpio_sniff2a)) {
		pr_err("cannot acquire gpio sniff 2a\n");
		return PTR_ERR(piCore_g.gpio_sniff2a);
	}
	if (piDev_g.machine_type == REVPI_CORE) {
		piCore_g.gpio_sniff1b = gpiod_get(piDev_g.dev, "Sniff1B", GPIOD_IN);
		if (IS_ERR(piCore_g.gpio_sniff1b)) {
			pr_err("cannot acquire gpio sniff 1b\n");
			return PTR_ERR(piCore_g.gpio_sniff1b);
		}
		piCore_g.gpio_sniff2b = gpiod_get(piDev_g.dev, "Sniff2B", GPIOD_IN);
		if (IS_ERR(piCore_g.gpio_sniff2b)) {
			pr_err("cannot acquire gpio sniff 2b\n");
			return PTR_ERR(piCore_g.gpio_sniff2b);
		}
	}
	if (piDev_g.machine_type == REVPI_CONNECT) {
		piCore_g.gpio_x2di = gpiod_get(piDev_g.dev, "X2_DI", GPIOD_IN);
		if (IS_ERR(piCore_g.gpio_x2di)) {
			pr_err("cannot acquire gpio x2 di\n");
			return PTR_ERR(piCore_g.gpio_x2di);
		}
		piCore_g.gpio_x2do = gpiod_get(piDev_g.dev, "X2_DO", GPIOD_OUT_LOW);
		if (IS_ERR(piCore_g.gpio_x2do)) {
			pr_err("cannot acquire gpio x2 do\n");
			return PTR_ERR(piCore_g.gpio_x2do);
		}
		piCore_g.gpio_wdtrigger = gpiod_get(piDev_g.dev, "WDTrigger", GPIOD_OUT_LOW);
		if (IS_ERR(piCore_g.gpio_wdtrigger)) {
			pr_err("cannot acquire gpio watchdog trigger\n");
			return PTR_ERR(piCore_g.gpio_wdtrigger);
		}
	}

	piDev_g.init_step = 11;

	if (piIoComm_init()) {
		pr_err("open serial port failed\n");
		return -EFAULT;
	}
	piDev_g.init_step = 12;

	i32uRv = MODGATECOM_init(piCore_g.ai8uInput, KB_PD_LEN, piCore_g.ai8uOutput, KB_PD_LEN, &EthDrvKSZ8851_g);
	if (i32uRv != MODGATECOM_NO_ERROR) {
		pr_err("MODGATECOM_init error %#08x\n", i32uRv);
		return -EFAULT;
	}
	piDev_g.init_step = 14;

	/* run threads */
	sema_init(&piCore_g.gateSem, 0);
	piCore_g.pGateThread = kthread_run(&piGateThread, NULL, "piControl Gate");
	if (IS_ERR(piCore_g.pGateThread)) {
		pr_err("kthread_run(gate) failed\n");
		ret = PTR_ERR(piCore_g.pGateThread);
		return ret;
	}
	param.sched_priority = RT_PRIO_GATE;
	ret = sched_setscheduler(piCore_g.pGateThread, SCHED_FIFO, &param);
	if (ret) {
		pr_err("cannot set rt prio of gate thread\n");
		goto err_stop_gate_thread;
	}

	piCore_g.pUartThread = kthread_run(&UartThreadProc, (void *)NULL, "piControl Uart");
	if (IS_ERR(piCore_g.pUartThread)) {
		pr_err("kthread_run(uart) failed\n");
		ret = PTR_ERR(piCore_g.pUartThread);
		goto err_stop_gate_thread;
	}
	param.sched_priority = RT_PRIO_UART;
	sched_setscheduler(piCore_g.pUartThread, SCHED_FIFO, &param);
	if (ret) {
		pr_err("cannot set rt prio of uart thread\n");
		goto err_stop_uart_thread;
	}

	piCore_g.pIoThread = kthread_run(&piIoThread, NULL, "piControl I/O");
	if (IS_ERR(piCore_g.pIoThread)) {
		pr_err("kthread_run(io) failed\n");
		ret = PTR_ERR(piCore_g.pIoThread);
		goto err_stop_uart_thread;
	}
	param.sched_priority = RT_PRIO_BRIDGE;
	ret = sched_setscheduler(piCore_g.pIoThread, SCHED_FIFO, &param);
	if (ret) {
		pr_err("cannot set rt prio of io thread\n");
		goto err_stop_io_thread;
	}

	ret = set_kthread_prios(revpi_core_kthread_prios);
	if (ret)
		goto err_stop_io_thread;

	return ret;

err_stop_io_thread:
	kthread_stop(piCore_g.pIoThread);
err_stop_uart_thread:
	kthread_stop(piCore_g.pUartThread);
err_stop_gate_thread:
	kthread_stop(piCore_g.pGateThread);

	return ret;
}

void revpi_core_fini(void)
{
	// the IoThread cannot be stopped
	if (!IS_ERR_OR_NULL(piCore_g.pIoThread))
		kthread_stop(piCore_g.pIoThread);
	if (!IS_ERR_OR_NULL(piCore_g.pUartThread))
		kthread_stop(piCore_g.pUartThread);
	if (!IS_ERR_OR_NULL(piCore_g.pGateThread))
		kthread_stop(piCore_g.pGateThread);

	if (piDev_g.init_step >= 14) {
		/* unregister spi */
		BSP_SPI_RWPERI_deinit(0);
	}

	if (piDev_g.init_step >= 12) {
		piIoComm_finish();
	}

	/* reset GPIO direction */
	if (piDev_g.init_step >= 11) {
		if (!IS_ERR_OR_NULL(piCore_g.gpio_sniff1a)) {
			piIoComm_writeSniff1A(enGpioValue_Low, enGpioMode_Input);
			gpiod_put(piCore_g.gpio_sniff1a);
		}
		if (!IS_ERR_OR_NULL(piCore_g.gpio_sniff1b)) {
			piIoComm_writeSniff1B(enGpioValue_Low, enGpioMode_Input);
			gpiod_put(piCore_g.gpio_sniff1b);
		};
		if (!IS_ERR_OR_NULL(piCore_g.gpio_sniff2a)) {
			piIoComm_writeSniff2A(enGpioValue_Low, enGpioMode_Input);
			gpiod_put(piCore_g.gpio_sniff2a);
		};
		if (!IS_ERR_OR_NULL(piCore_g.gpio_sniff2b)) {
			piIoComm_writeSniff2B(enGpioValue_Low, enGpioMode_Input);
			gpiod_put(piCore_g.gpio_sniff2b);
		}
	}

	if (piSpiModule) {
		module_put(piSpiModule);
		piSpiModule = NULL;
	}
}
