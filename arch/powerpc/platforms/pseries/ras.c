/*
 * Copyright (C) 2001 Dave Engebretsen IBM Corporation
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/reboot.h>
#include <linux/irq_work.h>

#include <asm/machdep.h>
#include <asm/rtas.h>
#include <asm/firmware.h>

#include "pseries.h"

static unsigned char ras_log_buf[RTAS_ERROR_LOG_MAX];
static DEFINE_SPINLOCK(ras_log_buf_lock);

static int ras_check_exception_token;

static void mce_process_errlog_event(struct irq_work *work);
static struct irq_work mce_errlog_process_work = {
	.func = mce_process_errlog_event,
};

#define EPOW_SENSOR_TOKEN	9
#define EPOW_SENSOR_INDEX	0

/* EPOW events counter variable */
static int num_epow_events;

static irqreturn_t ras_hotplug_interrupt(int irq, void *dev_id);
static irqreturn_t ras_epow_interrupt(int irq, void *dev_id);
static irqreturn_t ras_error_interrupt(int irq, void *dev_id);


/*
 * Enable the hotplug interrupt late because processing them may touch other
 * devices or systems (e.g. hugepages) that have not been initialized at the
 * subsys stage.
 */
int __init init_ras_hotplug_IRQ(void)
{
	struct device_node *np;

	/* Hotplug Events */
	np = of_find_node_by_path("/event-sources/hot-plug-events");
	if (np != NULL) {
		if (dlpar_workqueue_init() == 0)
			request_event_sources_irqs(np, ras_hotplug_interrupt,
						   "RAS_HOTPLUG");
		of_node_put(np);
	}

	return 0;
}
machine_late_initcall(pseries, init_ras_hotplug_IRQ);

/*
 * Initialize handlers for the set of interrupts caused by hardware errors
 * and power system events.
 */
static int __init init_ras_IRQ(void)
{
	struct device_node *np;

	ras_check_exception_token = rtas_token("check-exception");

	/* Internal Errors */
	np = of_find_node_by_path("/event-sources/internal-errors");
	if (np != NULL) {
		request_event_sources_irqs(np, ras_error_interrupt,
					   "RAS_ERROR");
		of_node_put(np);
	}

	/* EPOW Events */
	np = of_find_node_by_path("/event-sources/epow-events");
	if (np != NULL) {
		request_event_sources_irqs(np, ras_epow_interrupt, "RAS_EPOW");
		of_node_put(np);
	}

	return 0;
}
machine_subsys_initcall(pseries, init_ras_IRQ);

#define EPOW_SHUTDOWN_NORMAL				1
#define EPOW_SHUTDOWN_ON_UPS				2
#define EPOW_SHUTDOWN_LOSS_OF_CRITICAL_FUNCTIONS	3
#define EPOW_SHUTDOWN_AMBIENT_TEMPERATURE_TOO_HIGH	4

static void handle_system_shutdown(char event_modifier)
{
	switch (event_modifier) {
	case EPOW_SHUTDOWN_NORMAL:
		pr_emerg("Power off requested\n");
		orderly_poweroff(true);
		break;

	case EPOW_SHUTDOWN_ON_UPS:
		pr_emerg("Loss of system power detected. System is running on"
			 " UPS/battery. Check RTAS error log for details\n");
		break;

	case EPOW_SHUTDOWN_LOSS_OF_CRITICAL_FUNCTIONS:
		pr_emerg("Loss of system critical functions detected. Check"
			 " RTAS error log for details\n");
		orderly_poweroff(true);
		break;

	case EPOW_SHUTDOWN_AMBIENT_TEMPERATURE_TOO_HIGH:
		pr_emerg("High ambient temperature detected. Check RTAS"
			 " error log for details\n");
		orderly_poweroff(true);
		break;

	default:
		pr_err("Unknown power/cooling shutdown event (modifier = %d)\n",
			event_modifier);
	}
}

struct epow_errorlog {
	unsigned char sensor_value;
	unsigned char event_modifier;
	unsigned char extended_modifier;
	unsigned char reserved;
	unsigned char platform_reason;
};

#define EPOW_RESET			0
#define EPOW_WARN_COOLING		1
#define EPOW_WARN_POWER			2
#define EPOW_SYSTEM_SHUTDOWN		3
#define EPOW_SYSTEM_HALT		4
#define EPOW_MAIN_ENCLOSURE		5
#define EPOW_POWER_OFF			7

static void rtas_parse_epow_errlog(struct rtas_error_log *log)
{
	struct pseries_errorlog *pseries_log;
	struct epow_errorlog *epow_log;
	char action_code;
	char modifier;

	pseries_log = get_pseries_errorlog(log, PSERIES_ELOG_SECT_ID_EPOW);
	if (pseries_log == NULL)
		return;

	epow_log = (struct epow_errorlog *)pseries_log->data;
	action_code = epow_log->sensor_value & 0xF;	/* bottom 4 bits */
	modifier = epow_log->event_modifier & 0xF;	/* bottom 4 bits */

	switch (action_code) {
	case EPOW_RESET:
		if (num_epow_events) {
			pr_info("Non critical power/cooling issue cleared\n");
			num_epow_events--;
		}
		break;

	case EPOW_WARN_COOLING:
		pr_info("Non-critical cooling issue detected. Check RTAS error"
			" log for details\n");
		break;

	case EPOW_WARN_POWER:
		pr_info("Non-critical power issue detected. Check RTAS error"
			" log for details\n");
		break;

	case EPOW_SYSTEM_SHUTDOWN:
		handle_system_shutdown(epow_log->event_modifier);
		break;

	case EPOW_SYSTEM_HALT:
		pr_emerg("Critical power/cooling issue detected. Check RTAS"
			 " error log for details. Powering off.\n");
		orderly_poweroff(true);
		break;

	case EPOW_MAIN_ENCLOSURE:
	case EPOW_POWER_OFF:
		pr_emerg("System about to lose power. Check RTAS error log "
			 " for details. Powering off immediately.\n");
		emergency_sync();
		kernel_power_off();
		break;

	default:
		pr_err("Unknown power/cooling event (action code  = %d)\n",
			action_code);
	}

	/* Increment epow events counter variable */
	if (action_code != EPOW_RESET)
		num_epow_events++;
}

static irqreturn_t ras_hotplug_interrupt(int irq, void *dev_id)
{
	struct pseries_errorlog *pseries_log;
	struct pseries_hp_errorlog *hp_elog;

	spin_lock(&ras_log_buf_lock);

	rtas_call(ras_check_exception_token, 6, 1, NULL,
		  RTAS_VECTOR_EXTERNAL_INTERRUPT, virq_to_hw(irq),
		  RTAS_HOTPLUG_EVENTS, 0, __pa(&ras_log_buf),
		  rtas_get_error_log_max());

	pseries_log = get_pseries_errorlog((struct rtas_error_log *)ras_log_buf,
					   PSERIES_ELOG_SECT_ID_HOTPLUG);
	hp_elog = (struct pseries_hp_errorlog *)pseries_log->data;

	/*
	 * Since PCI hotplug is not currently supported on pseries, put PCI
	 * hotplug events on the ras_log_buf to be handled by rtas_errd.
	 */
	if (hp_elog->resource == PSERIES_HP_ELOG_RESOURCE_MEM ||
	    hp_elog->resource == PSERIES_HP_ELOG_RESOURCE_CPU)
		queue_hotplug_event(hp_elog, NULL, NULL);
	else
		log_error(ras_log_buf, ERR_TYPE_RTAS_LOG, 0);

	spin_unlock(&ras_log_buf_lock);
	return IRQ_HANDLED;
}

/* Handle environmental and power warning (EPOW) interrupts. */
static irqreturn_t ras_epow_interrupt(int irq, void *dev_id)
{
	int status;
	int state;
	int critical;

	status = rtas_get_sensor_fast(EPOW_SENSOR_TOKEN, EPOW_SENSOR_INDEX,
				      &state);

	if (state > 3)
		critical = 1;		/* Time Critical */
	else
		critical = 0;

	spin_lock(&ras_log_buf_lock);

	status = rtas_call(ras_check_exception_token, 6, 1, NULL,
			   RTAS_VECTOR_EXTERNAL_INTERRUPT,
			   virq_to_hw(irq),
			   RTAS_EPOW_WARNING,
			   critical, __pa(&ras_log_buf),
				rtas_get_error_log_max());

	log_error(ras_log_buf, ERR_TYPE_RTAS_LOG, 0);

	rtas_parse_epow_errlog((struct rtas_error_log *)ras_log_buf);

	spin_unlock(&ras_log_buf_lock);
	return IRQ_HANDLED;
}

/*
 * Handle hardware error interrupts.
 *
 * RTAS check-exception is called to collect data on the exception.  If
 * the error is deemed recoverable, we log a warning and return.
 * For nonrecoverable errors, an error is logged and we stop all processing
 * as quickly as possible in order to prevent propagation of the failure.
 */
static irqreturn_t ras_error_interrupt(int irq, void *dev_id)
{
	struct rtas_error_log *rtas_elog;
	int status;
	int fatal;

	spin_lock(&ras_log_buf_lock);

	status = rtas_call(ras_check_exception_token, 6, 1, NULL,
			   RTAS_VECTOR_EXTERNAL_INTERRUPT,
			   virq_to_hw(irq),
			   RTAS_INTERNAL_ERROR, 1 /* Time Critical */,
			   __pa(&ras_log_buf),
				rtas_get_error_log_max());

	rtas_elog = (struct rtas_error_log *)ras_log_buf;

	if (status == 0 &&
	    rtas_error_severity(rtas_elog) >= RTAS_SEVERITY_ERROR_SYNC)
		fatal = 1;
	else
		fatal = 0;

	/* format and print the extended information */
	log_error(ras_log_buf, ERR_TYPE_RTAS_LOG, fatal);

	if (fatal) {
		pr_emerg("Fatal hardware error detected. Check RTAS error"
			 " log for details. Powering off immediately\n");
		emergency_sync();
		kernel_power_off();
	} else {
		pr_err("Recoverable hardware error detected\n");
	}

	spin_unlock(&ras_log_buf_lock);
	return IRQ_HANDLED;
}

/*
 * Some versions of FWNMI place the buffer inside the 4kB page starting at
 * 0x7000. Other versions place it inside the rtas buffer. We check both.
 * Minimum size of the buffer is 16 bytes.
 */
#define VALID_FWNMI_BUFFER(A) \
	((((A) >= 0x7000) && ((A) <= 0x8000 - 16)) || \
	(((A) >= rtas.base) && ((A) <= (rtas.base + rtas.size - 16))))

static inline struct rtas_error_log *fwnmi_get_errlog(void)
{
	return (struct rtas_error_log *)local_paca->mce_data_buf;
}

/*
 * Get the error information for errors coming through the
 * FWNMI vectors.  The pt_regs' r3 will be updated to reflect
 * the actual r3 if possible, and a ptr to the error log entry
 * will be returned if found.
 *
 * Use one buffer mce_data_buf per cpu to store RTAS error.
 *
 * The mce_data_buf does not have any locks or protection around it,
 * if a second machine check comes in, or a system reset is done
 * before we have logged the error, then we will get corruption in the
 * error log.  This is preferable over holding off on calling
 * ibm,nmi-interlock which would result in us checkstopping if a
 * second machine check did come in.
 */
static struct rtas_error_log *fwnmi_get_errinfo(struct pt_regs *regs)
{
	unsigned long *savep;
	struct rtas_error_log *h;

	/* Mask top two bits */
	regs->gpr[3] &= ~(0x3UL << 62);

	if (!VALID_FWNMI_BUFFER(regs->gpr[3])) {
		printk(KERN_ERR "FWNMI: corrupt r3 0x%016lx\n", regs->gpr[3]);
		return NULL;
	}

	savep = __va(regs->gpr[3]);
	regs->gpr[3] = be64_to_cpu(savep[0]);	/* restore original r3 */

	h = (struct rtas_error_log *)&savep[1];
	/* Use the per cpu buffer from paca to store rtas error log */
	memset(local_paca->mce_data_buf, 0, RTAS_ERROR_LOG_MAX);
	if (!rtas_error_extended(h)) {
		memcpy(local_paca->mce_data_buf, h, sizeof(__u64));
	} else {
		int len, error_log_length;

		error_log_length = 8 + rtas_error_extended_log_length(h);
		len = min_t(int, error_log_length, RTAS_ERROR_LOG_MAX);
		memcpy(local_paca->mce_data_buf, h, len);
	}

	return (struct rtas_error_log *)local_paca->mce_data_buf;
}

/* Call this when done with the data returned by FWNMI_get_errinfo.
 * It will release the saved data area for other CPUs in the
 * partition to receive FWNMI errors.
 */
static void fwnmi_release_errinfo(void)
{
	int ret = rtas_call(rtas_token("ibm,nmi-interlock"), 0, 1, NULL);
	if (ret != 0)
		printk(KERN_ERR "FWNMI: nmi-interlock failed: %d\n", ret);
}

int pSeries_system_reset_exception(struct pt_regs *regs)
{
#ifdef __LITTLE_ENDIAN__
	/*
	 * Some firmware byteswaps SRR registers and gives incorrect SRR1. Try
	 * to detect the bad SRR1 pattern here. Flip the NIP back to correct
	 * endian for reporting purposes. Unfortunately the MSR can't be fixed,
	 * so clear it. It will be missing MSR_RI so we won't try to recover.
	 */
	if ((be64_to_cpu(regs->msr) &
			(MSR_LE|MSR_RI|MSR_DR|MSR_IR|MSR_ME|MSR_PR|
			 MSR_ILE|MSR_HV|MSR_SF)) == (MSR_DR|MSR_SF)) {
		regs->nip = be64_to_cpu((__be64)regs->nip);
		regs->msr = 0;
	}
#endif

	if (fwnmi_active) {
		struct rtas_error_log *errhdr = fwnmi_get_errinfo(regs);
		if (errhdr) {
			/* XXX Should look at FWNMI information */
		}
		fwnmi_release_errinfo();
	}

	if (smp_handle_nmi_ipi(regs))
		return 1;

	return 0; /* need to perform reset */
}

/*
 * Process MCE rtas errlog event.
 */
static void mce_process_errlog_event(struct irq_work *work)
{
	struct rtas_error_log *err;

	err = fwnmi_get_errlog();
	log_error((char *)err, ERR_TYPE_RTAS_LOG, 0);
}

/*
 * See if we can recover from a machine check exception.
 * This is only called on power4 (or above) and only via
 * the Firmware Non-Maskable Interrupts (fwnmi) handler
 * which provides the error analysis for us.
 *
 * Return 1 if corrected (or delivered a signal).
 * Return 0 if there is nothing we can do.
 */
static int recover_mce(struct pt_regs *regs, struct rtas_error_log *err)
{
	int recovered = 0;
	int disposition = rtas_error_disposition(err);

	if (!(regs->msr & MSR_RI)) {
		/* If MSR_RI isn't set, we cannot recover */
		recovered = 0;

	} else if (disposition == RTAS_DISP_FULLY_RECOVERED) {
		/* Platform corrected itself */
		recovered = 1;

	} else if (disposition == RTAS_DISP_LIMITED_RECOVERY) {
		/* Platform corrected itself but could be degraded */
		printk(KERN_ERR "MCE: limited recovery, system may "
		       "be degraded\n");
		recovered = 1;

	} else if (user_mode(regs) && !is_global_init(current) &&
		   rtas_error_severity(err) == RTAS_SEVERITY_ERROR_SYNC) {

		/*
		 * If we received a synchronous error when in userspace
		 * kill the task. Firmware may report details of the fail
		 * asynchronously, so we can't rely on the target and type
		 * fields being valid here.
		 */
		printk(KERN_ERR "MCE: uncorrectable error, killing task "
		       "%s:%d\n", current->comm, current->pid);

		_exception(SIGBUS, regs, BUS_MCEERR_AR, regs->nip);
		recovered = 1;
	}

	/* Queue irq work to log this rtas event later. */
	irq_work_queue(&mce_errlog_process_work);

	return recovered;
}

/*
 * Handle a machine check.
 *
 * Note that on Power 4 and beyond Firmware Non-Maskable Interrupts (fwnmi)
 * should be present.  If so the handler which called us tells us if the
 * error was recovered (never true if RI=0).
 *
 * On hardware prior to Power 4 these exceptions were asynchronous which
 * means we can't tell exactly where it occurred and so we can't recover.
 */
int pSeries_machine_check_exception(struct pt_regs *regs)
{
	struct rtas_error_log *errp;

	if (fwnmi_active) {
		errp = fwnmi_get_errinfo(regs);
		fwnmi_release_errinfo();
		if (errp && recover_mce(regs, errp))
			return 1;
	}

	return 0;
}
