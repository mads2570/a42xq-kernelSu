/*
 *  arch/m68k/mvme147/config.c
 *
 *  Copyright (C) 1996 Dave Frascone [chaos@mindspring.com]
 *  Cloned from        Richard Hirst [richard@sleepie.demon.co.uk]
 *
 * Based on:
 *
 *  Copyright (C) 1993 Hamish Macdonald
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file README.legal in the main directory of this archive
 * for more details.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/genhd.h>
#include <linux/rtc.h>
#include <linux/interrupt.h>

#include <asm/bootinfo.h>
#include <asm/bootinfo-vme.h>
#include <asm/byteorder.h>
#include <asm/pgtable.h>
#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/traps.h>
#include <asm/machdep.h>
#include <asm/mvme147hw.h>


static void mvme147_get_model(char *model);
extern void mvme147_sched_init(irq_handler_t handler);
extern u32 mvme147_gettimeoffset(void);
extern int mvme147_hwclk (int, struct rtc_time *);
extern void mvme147_reset (void);


static int bcd2int (unsigned char b);


int __init mvme147_parse_bootinfo(const struct bi_record *bi)
{
	uint16_t tag = be16_to_cpu(bi->tag);
	if (tag == BI_VME_TYPE || tag == BI_VME_BRDINFO)
		return 0;
	else
		return 1;
}

void mvme147_reset(void)
{
	pr_info("\r\n\nCalled mvme147_reset\r\n");
	m147_pcc->watchdog = 0x0a;	/* Clear timer */
	m147_pcc->watchdog = 0xa5;	/* Enable watchdog - 100ms to reset */
	while (1)
		;
}

static void mvme147_get_model(char *model)
{
	sprintf(model, "Motorola MVME147");
}

/*
 * This function is called during kernel startup to initialize
 * the mvme147 IRQ handling routines.
 */

void __init mvme147_init_IRQ(void)
{
	m68k_setup_user_interrupt(VEC_USER, 192);
}

void __init config_mvme147(void)
{
	mach_max_dma_address	= 0x01000000;
	mach_sched_init		= mvme147_sched_init;
	mach_init_IRQ		= mvme147_init_IRQ;
	arch_gettimeoffset	= mvme147_gettimeoffset;
	mach_hwclk		= mvme147_hwclk;
	mach_reset		= mvme147_reset;
	mach_get_model		= mvme147_get_model;

	/* Board type is only set by newer versions of vmelilo/tftplilo */
	if (!vme_brdtype)
		vme_brdtype = VME_TYPE_MVME147;
}


/* Using pcc tick timer 1 */

static irqreturn_t mvme147_timer_int (int irq, void *dev_id)
{
	irq_handler_t timer_routine = dev_id;
	unsigned long flags;

	local_irq_save(flags);
	m147_pcc->t1_int_cntrl = PCC_TIMER_INT_CLR;
	m147_pcc->t1_int_cntrl = PCC_INT_ENAB|PCC_LEVEL_TIMER1;
	timer_routine(0, NULL);
	local_irq_restore(flags);

	return IRQ_HANDLED;
}


void mvme147_sched_init (irq_handler_t timer_routine)
{
	if (request_irq(PCC_IRQ_TIMER1, mvme147_timer_int, 0, "timer 1",
			timer_routine))
		pr_err("Couldn't register timer interrupt\n");

	/* Init the clock with a value */
	/* our clock goes off every 6.25us */
	m147_pcc->t1_preload = PCC_TIMER_PRELOAD;
	m147_pcc->t1_cntrl = 0x0;	/* clear timer */
	m147_pcc->t1_cntrl = 0x3;	/* start timer */
	m147_pcc->t1_int_cntrl = PCC_TIMER_INT_CLR;  /* clear pending ints */
	m147_pcc->t1_int_cntrl = PCC_INT_ENAB|PCC_LEVEL_TIMER1;
}

/* This is always executed with interrupts disabled.  */
/* XXX There are race hazards in this code XXX */
u32 mvme147_gettimeoffset(void)
{
	volatile unsigned short *cp = (volatile unsigned short *)0xfffe1012;
	unsigned short n;

	n = *cp;
	while (n != *cp)
		n = *cp;

	n -= PCC_TIMER_PRELOAD;
	return ((unsigned long)n * 25 / 4) * 1000;
}

static int bcd2int (unsigned char b)
{
	return ((b>>4)*10 + (b&15));
}

int mvme147_hwclk(int op, struct rtc_time *t)
{
#warning check me!
	if (!op) {
		m147_rtc->ctrl = RTC_READ;
		t->tm_year = bcd2int (m147_rtc->bcd_year);
		t->tm_mon  = bcd2int(m147_rtc->bcd_mth) - 1;
		t->tm_mday = bcd2int (m147_rtc->bcd_dom);
		t->tm_hour = bcd2int (m147_rtc->bcd_hr);
		t->tm_min  = bcd2int (m147_rtc->bcd_min);
		t->tm_sec  = bcd2int (m147_rtc->bcd_sec);
		m147_rtc->ctrl = 0;
		if (t->tm_year < 70)
			t->tm_year += 100;
	}
	return 0;
}
