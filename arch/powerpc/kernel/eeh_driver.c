/*
 * PCI Error Recovery Driver for RPA-compliant PPC64 platform.
 * Copyright IBM Corp. 2004 2005
 * Copyright Linas Vepstas <linas@linas.org> 2004, 2005
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Send comments and feedback to Linas Vepstas <linas@austin.ibm.com>
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <asm/eeh.h>
#include <asm/eeh_event.h>
#include <asm/ppc-pci.h>
#include <asm/pci-bridge.h>
#include <asm/prom.h>
#include <asm/rtas.h>

struct eeh_rmv_data {
	struct list_head edev_list;
	int removed;
};

static int eeh_result_priority(enum pci_ers_result result)
{
	switch (result) {
	case PCI_ERS_RESULT_NONE:
		return 1;
	case PCI_ERS_RESULT_NO_AER_DRIVER:
		return 2;
	case PCI_ERS_RESULT_RECOVERED:
		return 3;
	case PCI_ERS_RESULT_CAN_RECOVER:
		return 4;
	case PCI_ERS_RESULT_DISCONNECT:
		return 5;
	case PCI_ERS_RESULT_NEED_RESET:
		return 6;
	default:
		WARN_ONCE(1, "Unknown pci_ers_result value: %d\n", (int)result);
		return 0;
	}
};

const char *pci_ers_result_name(enum pci_ers_result result)
{
	switch (result) {
	case PCI_ERS_RESULT_NONE:
		return "none";
	case PCI_ERS_RESULT_CAN_RECOVER:
		return "can recover";
	case PCI_ERS_RESULT_NEED_RESET:
		return "need reset";
	case PCI_ERS_RESULT_DISCONNECT:
		return "disconnect";
	case PCI_ERS_RESULT_RECOVERED:
		return "recovered";
	case PCI_ERS_RESULT_NO_AER_DRIVER:
		return "no AER driver";
	default:
		WARN_ONCE(1, "Unknown result type: %d\n", (int)result);
		return "unknown";
	}
};

static __printf(2, 3) void eeh_edev_info(const struct eeh_dev *edev,
					 const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_INFO "EEH: PE#%x (PCI %s): %pV\n", edev->pe_config_addr,
	       edev->pdev ? dev_name(&edev->pdev->dev) : "none", &vaf);

	va_end(args);
}

static enum pci_ers_result pci_ers_merge_result(enum pci_ers_result old,
						enum pci_ers_result new)
{
	if (eeh_result_priority(new) > eeh_result_priority(old))
		return new;
	return old;
}

static bool eeh_dev_removed(struct eeh_dev *edev)
{
	return !edev || (edev->mode & EEH_DEV_REMOVED);
}

static bool eeh_edev_actionable(struct eeh_dev *edev)
{
	return (edev->pdev && !eeh_dev_removed(edev) &&
		!eeh_pe_passed(edev->pe));
}

/**
 * eeh_pcid_get - Get the PCI device driver
 * @pdev: PCI device
 *
 * The function is used to retrieve the PCI device driver for
 * the indicated PCI device. Besides, we will increase the reference
 * of the PCI device driver to prevent that being unloaded on
 * the fly. Otherwise, kernel crash would be seen.
 */
static inline struct pci_driver *eeh_pcid_get(struct pci_dev *pdev)
{
	if (!pdev || !pdev->driver)
		return NULL;

	if (!try_module_get(pdev->driver->driver.owner))
		return NULL;

	return pdev->driver;
}

/**
 * eeh_pcid_put - Dereference on the PCI device driver
 * @pdev: PCI device
 *
 * The function is called to do dereference on the PCI device
 * driver of the indicated PCI device.
 */
static inline void eeh_pcid_put(struct pci_dev *pdev)
{
	if (!pdev || !pdev->driver)
		return;

	module_put(pdev->driver->driver.owner);
}

/**
 * eeh_disable_irq - Disable interrupt for the recovering device
 * @dev: PCI device
 *
 * This routine must be called when reporting temporary or permanent
 * error to the particular PCI device to disable interrupt of that
 * device. If the device has enabled MSI or MSI-X interrupt, we needn't
 * do real work because EEH should freeze DMA transfers for those PCI
 * devices encountering EEH errors, which includes MSI or MSI-X.
 */
static void eeh_disable_irq(struct eeh_dev *edev)
{
	/* Don't disable MSI and MSI-X interrupts. They are
	 * effectively disabled by the DMA Stopped state
	 * when an EEH error occurs.
	 */
	if (edev->pdev->msi_enabled || edev->pdev->msix_enabled)
		return;

	if (!irq_has_action(edev->pdev->irq))
		return;

	edev->mode |= EEH_DEV_IRQ_DISABLED;
	disable_irq_nosync(edev->pdev->irq);
}

/**
 * eeh_enable_irq - Enable interrupt for the recovering device
 * @dev: PCI device
 *
 * This routine must be called to enable interrupt while failed
 * device could be resumed.
 */
static void eeh_enable_irq(struct eeh_dev *edev)
{
	if ((edev->mode) & EEH_DEV_IRQ_DISABLED) {
		edev->mode &= ~EEH_DEV_IRQ_DISABLED;
		/*
		 * FIXME !!!!!
		 *
		 * This is just ass backwards. This maze has
		 * unbalanced irq_enable/disable calls. So instead of
		 * finding the root cause it works around the warning
		 * in the irq_enable code by conditionally calling
		 * into it.
		 *
		 * That's just wrong.The warning in the core code is
		 * there to tell people to fix their asymmetries in
		 * their own code, not by abusing the core information
		 * to avoid it.
		 *
		 * I so wish that the assymetry would be the other way
		 * round and a few more irq_disable calls render that
		 * shit unusable forever.
		 *
		 *	tglx
		 */
		if (irqd_irq_disabled(irq_get_irq_data(edev->pdev->irq)))
			enable_irq(edev->pdev->irq);
	}
}

static void *eeh_dev_save_state(struct eeh_dev *edev, void *userdata)
{
	struct pci_dev *pdev;

	if (!edev)
		return NULL;

	/*
	 * We cannot access the config space on some adapters.
	 * Otherwise, it will cause fenced PHB. We don't save
	 * the content in their config space and will restore
	 * from the initial config space saved when the EEH
	 * device is created.
	 */
	if (edev->pe && (edev->pe->state & EEH_PE_CFG_RESTRICTED))
		return NULL;

	pdev = eeh_dev_to_pci_dev(edev);
	if (!pdev)
		return NULL;

	pci_save_state(pdev);
	return NULL;
}

static void eeh_set_channel_state(struct eeh_pe *root, enum pci_channel_state s)
{
	struct eeh_pe *pe;
	struct eeh_dev *edev, *tmp;

	eeh_for_each_pe(root, pe)
		eeh_pe_for_each_dev(pe, edev, tmp)
			if (eeh_edev_actionable(edev))
				edev->pdev->error_state = s;
}

static void eeh_set_irq_state(struct eeh_pe *root, bool enable)
{
	struct eeh_pe *pe;
	struct eeh_dev *edev, *tmp;

	eeh_for_each_pe(root, pe) {
		eeh_pe_for_each_dev(pe, edev, tmp) {
			if (!eeh_edev_actionable(edev))
				continue;

			if (!eeh_pcid_get(edev->pdev))
				continue;

			if (enable)
				eeh_enable_irq(edev);
			else
				eeh_disable_irq(edev);

			eeh_pcid_put(edev->pdev);
		}
	}
}

typedef enum pci_ers_result (*eeh_report_fn)(struct eeh_dev *,
					     struct pci_driver *);
static void eeh_pe_report_edev(struct eeh_dev *edev, eeh_report_fn fn,
			       enum pci_ers_result *result)
{
	struct pci_driver *driver;
	enum pci_ers_result new_result;

	if (!edev->pdev) {
		eeh_edev_info(edev, "no device");
		return;
	}
	device_lock(&edev->pdev->dev);
	if (eeh_edev_actionable(edev)) {
		driver = eeh_pcid_get(edev->pdev);

		if (!driver)
			eeh_edev_info(edev, "no driver");
		else if (!driver->err_handler)
			eeh_edev_info(edev, "driver not EEH aware");
		else if (edev->mode & EEH_DEV_NO_HANDLER)
			eeh_edev_info(edev, "driver bound too late");
		else {
			new_result = fn(edev, driver);
			eeh_edev_info(edev, "%s driver reports: '%s'",
				      driver->name,
				      pci_ers_result_name(new_result));
			if (result)
				*result = pci_ers_merge_result(*result,
							       new_result);
		}
		if (driver)
			eeh_pcid_put(edev->pdev);
	} else {
		eeh_edev_info(edev, "not actionable (%d,%d,%d)", !!edev->pdev,
			      !eeh_dev_removed(edev), !eeh_pe_passed(edev->pe));
	}
	device_unlock(&edev->pdev->dev);
}

static void eeh_pe_report(const char *name, struct eeh_pe *root,
			  eeh_report_fn fn, enum pci_ers_result *result)
{
	struct eeh_pe *pe;
	struct eeh_dev *edev, *tmp;

	pr_info("EEH: Beginning: '%s'\n", name);
	eeh_for_each_pe(root, pe) eeh_pe_for_each_dev(pe, edev, tmp)
		eeh_pe_report_edev(edev, fn, result);
	if (result)
		pr_info("EEH: Finished:'%s' with aggregate recovery state:'%s'\n",
			name, pci_ers_result_name(*result));
	else
		pr_info("EEH: Finished:'%s'", name);
}

/**
 * eeh_report_error - Report pci error to each device driver
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * Report an EEH error to each device driver.
 */
static enum pci_ers_result eeh_report_error(struct eeh_dev *edev,
					    struct pci_driver *driver)
{
	enum pci_ers_result rc;
	struct pci_dev *dev = edev->pdev;

	if (!driver->err_handler->error_detected)
		return PCI_ERS_RESULT_NONE;

	eeh_edev_info(edev, "Invoking %s->error_detected(IO frozen)",
		      driver->name);
	rc = driver->err_handler->error_detected(dev, pci_channel_io_frozen);

	edev->in_error = true;
	pci_uevent_ers(dev, PCI_ERS_RESULT_NONE);
	return rc;
}

/**
 * eeh_report_mmio_enabled - Tell drivers that MMIO has been enabled
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * Tells each device driver that IO ports, MMIO and config space I/O
 * are now enabled.
 */
static enum pci_ers_result eeh_report_mmio_enabled(struct eeh_dev *edev,
						   struct pci_driver *driver)
{
	if (!driver->err_handler->mmio_enabled)
		return PCI_ERS_RESULT_NONE;
	eeh_edev_info(edev, "Invoking %s->mmio_enabled()", driver->name);
	return driver->err_handler->mmio_enabled(edev->pdev);
}

/**
 * eeh_report_reset - Tell device that slot has been reset
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * This routine must be called while EEH tries to reset particular
 * PCI device so that the associated PCI device driver could take
 * some actions, usually to save data the driver needs so that the
 * driver can work again while the device is recovered.
 */
static enum pci_ers_result eeh_report_reset(struct eeh_dev *edev,
					    struct pci_driver *driver)
{
	if (!driver->err_handler->slot_reset || !edev->in_error)
		return PCI_ERS_RESULT_NONE;
	eeh_edev_info(edev, "Invoking %s->slot_reset()", driver->name);
	return driver->err_handler->slot_reset(edev->pdev);
}

static void *eeh_dev_restore_state(struct eeh_dev *edev, void *userdata)
{
	struct pci_dev *pdev;

	if (!edev)
		return NULL;

	/*
	 * The content in the config space isn't saved because
	 * the blocked config space on some adapters. We have
	 * to restore the initial saved config space when the
	 * EEH device is created.
	 */
	if (edev->pe && (edev->pe->state & EEH_PE_CFG_RESTRICTED)) {
		if (list_is_last(&edev->list, &edev->pe->edevs))
			eeh_pe_restore_bars(edev->pe);

		return NULL;
	}

	pdev = eeh_dev_to_pci_dev(edev);
	if (!pdev)
		return NULL;

	pci_restore_state(pdev);
	return NULL;
}

/**
 * eeh_report_resume - Tell device to resume normal operations
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * This routine must be called to notify the device driver that it
 * could resume so that the device driver can do some initialization
 * to make the recovered device work again.
 */
static enum pci_ers_result eeh_report_resume(struct eeh_dev *edev,
					     struct pci_driver *driver)
{
	if (!driver->err_handler->resume || !edev->in_error)
		return PCI_ERS_RESULT_NONE;

	eeh_edev_info(edev, "Invoking %s->resume()", driver->name);
	driver->err_handler->resume(edev->pdev);

	pci_uevent_ers(edev->pdev, PCI_ERS_RESULT_RECOVERED);
#ifdef CONFIG_PCI_IOV
	if (eeh_ops->notify_resume && eeh_dev_to_pdn(edev))
		eeh_ops->notify_resume(eeh_dev_to_pdn(edev));
#endif
	return PCI_ERS_RESULT_NONE;
}

/**
 * eeh_report_failure - Tell device driver that device is dead.
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * This informs the device driver that the device is permanently
 * dead, and that no further recovery attempts will be made on it.
 */
static enum pci_ers_result eeh_report_failure(struct eeh_dev *edev,
					      struct pci_driver *driver)
{
	enum pci_ers_result rc;

	if (!driver->err_handler->error_detected)
		return PCI_ERS_RESULT_NONE;

	eeh_edev_info(edev, "Invoking %s->error_detected(permanent failure)",
		      driver->name);
	rc = driver->err_handler->error_detected(edev->pdev,
						 pci_channel_io_perm_failure);

	pci_uevent_ers(edev->pdev, PCI_ERS_RESULT_DISCONNECT);
	return rc;
}

static void *eeh_add_virt_device(void *data, void *userdata)
{
	struct pci_driver *driver;
	struct eeh_dev *edev = (struct eeh_dev *)data;
	struct pci_dev *dev = eeh_dev_to_pci_dev(edev);
	struct pci_dn *pdn = eeh_dev_to_pdn(edev);

	if (!(edev->physfn)) {
		pr_warn("%s: EEH dev %04x:%02x:%02x.%01x not for VF\n",
			__func__, pdn->phb->global_number, pdn->busno,
			PCI_SLOT(pdn->devfn), PCI_FUNC(pdn->devfn));
		return NULL;
	}

	driver = eeh_pcid_get(dev);
	if (driver) {
		if (driver->err_handler) {
			eeh_pcid_put(dev);
			return NULL;
		}
		eeh_pcid_put(dev);
	}

#ifdef CONFIG_PCI_IOV
	pci_iov_add_virtfn(edev->physfn, pdn->vf_index);
#endif
	return NULL;
}

static void *eeh_rmv_device(struct eeh_dev *edev, void *userdata)
{
	struct pci_driver *driver;
	struct pci_dev *dev = eeh_dev_to_pci_dev(edev);
	struct eeh_rmv_data *rmv_data = (struct eeh_rmv_data *)userdata;
	int *removed = rmv_data ? &rmv_data->removed : NULL;

	/*
	 * Actually, we should remove the PCI bridges as well.
	 * However, that's lots of complexity to do that,
	 * particularly some of devices under the bridge might
	 * support EEH. So we just care about PCI devices for
	 * simplicity here.
	 */
	if (!dev || (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE))
		return NULL;

	/*
	 * We rely on count-based pcibios_release_device() to
	 * detach permanently offlined PEs. Unfortunately, that's
	 * not reliable enough. We might have the permanently
	 * offlined PEs attached, but we needn't take care of
	 * them and their child devices.
	 */
	if (eeh_dev_removed(edev))
		return NULL;

	if (removed) {
		if (eeh_pe_passed(edev->pe))
			return NULL;
		driver = eeh_pcid_get(dev);
		if (driver) {
			if (driver->err_handler &&
			    driver->err_handler->error_detected &&
			    driver->err_handler->slot_reset) {
				eeh_pcid_put(dev);
				return NULL;
			}
			eeh_pcid_put(dev);
		}
	}

	/* Remove it from PCI subsystem */
	pr_debug("EEH: Removing %s without EEH sensitive driver\n",
		 pci_name(dev));
	edev->bus = dev->bus;
	edev->mode |= EEH_DEV_DISCONNECTED;
	if (removed)
		(*removed)++;

	if (edev->physfn) {
#ifdef CONFIG_PCI_IOV
		struct pci_dn *pdn = eeh_dev_to_pdn(edev);

		pci_iov_remove_virtfn(edev->physfn, pdn->vf_index);
		edev->pdev = NULL;
#endif
		if (rmv_data)
			list_add(&edev->rmv_list, &rmv_data->edev_list);
	} else {
		pci_lock_rescan_remove();
		pci_stop_and_remove_bus_device(dev);
		pci_unlock_rescan_remove();
	}

	return NULL;
}

static void *eeh_pe_detach_dev(struct eeh_pe *pe, void *userdata)
{
	struct eeh_dev *edev, *tmp;

	eeh_pe_for_each_dev(pe, edev, tmp) {
		if (!(edev->mode & EEH_DEV_DISCONNECTED))
			continue;

		edev->mode &= ~(EEH_DEV_DISCONNECTED | EEH_DEV_IRQ_DISABLED);
		eeh_rmv_from_parent_pe(edev);
	}

	return NULL;
}

/*
 * Explicitly clear PE's frozen state for PowerNV where
 * we have frozen PE until BAR restore is completed. It's
 * harmless to clear it for pSeries. To be consistent with
 * PE reset (for 3 times), we try to clear the frozen state
 * for 3 times as well.
 */
static void *__eeh_clear_pe_frozen_state(struct eeh_pe *pe, void *flag)
{
	bool clear_sw_state = *(bool *)flag;
	int i, rc = 1;

	for (i = 0; rc && i < 3; i++)
		rc = eeh_unfreeze_pe(pe, clear_sw_state);

	/* Stop immediately on any errors */
	if (rc) {
		pr_warn("%s: Failure %d unfreezing PHB#%x-PE#%x\n",
			__func__, rc, pe->phb->global_number, pe->addr);
		return (void *)pe;
	}

	return NULL;
}

static int eeh_clear_pe_frozen_state(struct eeh_pe *pe,
				     bool clear_sw_state)
{
	void *rc;

	rc = eeh_pe_traverse(pe, __eeh_clear_pe_frozen_state, &clear_sw_state);
	if (!rc)
		eeh_pe_state_clear(pe, EEH_PE_ISOLATED);

	return rc ? -EIO : 0;
}

int eeh_pe_reset_and_recover(struct eeh_pe *pe)
{
	int ret;

	/* Bail if the PE is being recovered */
	if (pe->state & EEH_PE_RECOVERING)
		return 0;

	/* Put the PE into recovery mode */
	eeh_pe_state_mark(pe, EEH_PE_RECOVERING);

	/* Save states */
	eeh_pe_dev_traverse(pe, eeh_dev_save_state, NULL);

	/* Issue reset */
	ret = eeh_pe_reset_full(pe);
	if (ret) {
		eeh_pe_state_clear(pe, EEH_PE_RECOVERING);
		return ret;
	}

	/* Unfreeze the PE */
	ret = eeh_clear_pe_frozen_state(pe, true);
	if (ret) {
		eeh_pe_state_clear(pe, EEH_PE_RECOVERING);
		return ret;
	}

	/* Restore device state */
	eeh_pe_dev_traverse(pe, eeh_dev_restore_state, NULL);

	/* Clear recovery mode */
	eeh_pe_state_clear(pe, EEH_PE_RECOVERING);

	return 0;
}

/**
 * eeh_reset_device - Perform actual reset of a pci slot
 * @driver_eeh_aware: Does the device's driver provide EEH support?
 * @pe: EEH PE
 * @bus: PCI bus corresponding to the isolcated slot
 * @rmv_data: Optional, list to record removed devices
 *
 * This routine must be called to do reset on the indicated PE.
 * During the reset, udev might be invoked because those affected
 * PCI devices will be removed and then added.
 */
static int eeh_reset_device(struct eeh_pe *pe, struct pci_bus *bus,
			    struct eeh_rmv_data *rmv_data,
			    bool driver_eeh_aware)
{
	time64_t tstamp;
	int cnt, rc;
	struct eeh_dev *edev;

	/* pcibios will clear the counter; save the value */
	cnt = pe->freeze_count;
	tstamp = pe->tstamp;

	/*
	 * We don't remove the corresponding PE instances because
	 * we need the information afterwords. The attached EEH
	 * devices are expected to be attached soon when calling
	 * into pci_hp_add_devices().
	 */
	eeh_pe_state_mark(pe, EEH_PE_KEEP);
	if (driver_eeh_aware || (pe->type & EEH_PE_VF)) {
		eeh_pe_dev_traverse(pe, eeh_rmv_device, rmv_data);
	} else {
		pci_lock_rescan_remove();
		pci_hp_remove_devices(bus);
		pci_unlock_rescan_remove();
	}

	/*
	 * Reset the pci controller. (Asserts RST#; resets config space).
	 * Reconfigure bridges and devices. Don't try to bring the system
	 * up if the reset failed for some reason.
	 *
	 * During the reset, it's very dangerous to have uncontrolled PCI
	 * config accesses. So we prefer to block them. However, controlled
	 * PCI config accesses initiated from EEH itself are allowed.
	 */
	rc = eeh_pe_reset_full(pe);
	if (rc)
		return rc;

	pci_lock_rescan_remove();

	/* Restore PE */
	eeh_ops->configure_bridge(pe);
	eeh_pe_restore_bars(pe);

	/* Clear frozen state */
	rc = eeh_clear_pe_frozen_state(pe, false);
	if (rc) {
		pci_unlock_rescan_remove();
		return rc;
	}

	/* Give the system 5 seconds to finish running the user-space
	 * hotplug shutdown scripts, e.g. ifdown for ethernet.  Yes,
	 * this is a hack, but if we don't do this, and try to bring
	 * the device up before the scripts have taken it down,
	 * potentially weird things happen.
	 */
	if (!driver_eeh_aware || rmv_data->removed) {
		pr_info("EEH: Sleep 5s ahead of %s hotplug\n",
			(driver_eeh_aware ? "partial" : "complete"));
		ssleep(5);

		/*
		 * The EEH device is still connected with its parent
		 * PE. We should disconnect it so the binding can be
		 * rebuilt when adding PCI devices.
		 */
		edev = list_first_entry(&pe->edevs, struct eeh_dev, list);
		eeh_pe_traverse(pe, eeh_pe_detach_dev, NULL);
		if (pe->type & EEH_PE_VF) {
			eeh_add_virt_device(edev, NULL);
		} else {
			if (!driver_eeh_aware)
				eeh_pe_state_clear(pe, EEH_PE_PRI_BUS);
			pci_hp_add_devices(bus);
		}
	}
	eeh_pe_state_clear(pe, EEH_PE_KEEP);

	pe->tstamp = tstamp;
	pe->freeze_count = cnt;

	pci_unlock_rescan_remove();
	return 0;
}

/* The longest amount of time to wait for a pci device
 * to come back on line, in seconds.
 */
#define MAX_WAIT_FOR_RECOVERY 300

/**
 * eeh_handle_normal_event - Handle EEH events on a specific PE
 * @pe: EEH PE - which should not be used after we return, as it may
 * have been invalidated.
 *
 * Attempts to recover the given PE.  If recovery fails or the PE has failed
 * too many times, remove the PE.
 *
 * While PHB detects address or data parity errors on particular PCI
 * slot, the associated PE will be frozen. Besides, DMA's occurring
 * to wild addresses (which usually happen due to bugs in device
 * drivers or in PCI adapter firmware) can cause EEH error. #SERR,
 * #PERR or other misc PCI-related errors also can trigger EEH errors.
 *
 * Recovery process consists of unplugging the device driver (which
 * generated hotplug events to userspace), then issuing a PCI #RST to
 * the device, then reconfiguring the PCI config space for all bridges
 * & devices under this slot, and then finally restarting the device
 * drivers (which cause a second set of hotplug events to go out to
 * userspace).
 */
void eeh_handle_normal_event(struct eeh_pe *pe)
{
	struct pci_bus *bus;
	struct eeh_dev *edev, *tmp;
	struct eeh_pe *tmp_pe;
	int rc = 0;
	enum pci_ers_result result = PCI_ERS_RESULT_NONE;
	struct eeh_rmv_data rmv_data = {LIST_HEAD_INIT(rmv_data.edev_list), 0};

	bus = eeh_pe_bus_get(pe);
	if (!bus) {
		pr_err("%s: Cannot find PCI bus for PHB#%x-PE#%x\n",
			__func__, pe->phb->global_number, pe->addr);
		return;
	}

	eeh_pe_state_mark(pe, EEH_PE_RECOVERING);

	eeh_pe_update_time_stamp(pe);
	pe->freeze_count++;
	if (pe->freeze_count > eeh_max_freezes) {
		pr_err("EEH: PHB#%x-PE#%x has failed %d times in the last hour and has been permanently disabled.\n",
		       pe->phb->global_number, pe->addr,
		       pe->freeze_count);
		goto hard_fail;
	}
	pr_warn("EEH: This PCI device has failed %d times in the last hour and will be permanently disabled after %d failures.\n",
		pe->freeze_count, eeh_max_freezes);

	eeh_for_each_pe(pe, tmp_pe)
		eeh_pe_for_each_dev(tmp_pe, edev, tmp)
			edev->mode &= ~EEH_DEV_NO_HANDLER;

	/* Walk the various device drivers attached to this slot through
	 * a reset sequence, giving each an opportunity to do what it needs
	 * to accomplish the reset.  Each child gets a report of the
	 * status ... if any child can't handle the reset, then the entire
	 * slot is dlpar removed and added.
	 *
	 * When the PHB is fenced, we have to issue a reset to recover from
	 * the error. Override the result if necessary to have partially
	 * hotplug for this case.
	 */
	pr_info("EEH: Notify device drivers to shutdown\n");
	eeh_set_channel_state(pe, pci_channel_io_frozen);
	eeh_set_irq_state(pe, false);
	eeh_pe_report("error_detected(IO frozen)", pe, eeh_report_error,
		      &result);
	if ((pe->type & EEH_PE_PHB) &&
	    result != PCI_ERS_RESULT_NONE &&
	    result != PCI_ERS_RESULT_NEED_RESET)
		result = PCI_ERS_RESULT_NEED_RESET;

	/* Get the current PCI slot state. This can take a long time,
	 * sometimes over 300 seconds for certain systems.
	 */
	rc = eeh_ops->wait_state(pe, MAX_WAIT_FOR_RECOVERY*1000);
	if (rc < 0 || rc == EEH_STATE_NOT_SUPPORT) {
		pr_warn("EEH: Permanent failure\n");
		goto hard_fail;
	}

	/* Since rtas may enable MMIO when posting the error log,
	 * don't post the error log until after all dev drivers
	 * have been informed.
	 */
	pr_info("EEH: Collect temporary log\n");
	eeh_slot_error_detail(pe, EEH_LOG_TEMP);

	/* If all device drivers were EEH-unaware, then shut
	 * down all of the device drivers, and hope they
	 * go down willingly, without panicing the system.
	 */
	if (result == PCI_ERS_RESULT_NONE) {
		pr_info("EEH: Reset with hotplug activity\n");
		rc = eeh_reset_device(pe, bus, NULL, false);
		if (rc) {
			pr_warn("%s: Unable to reset, err=%d\n",
				__func__, rc);
			goto hard_fail;
		}
	}

	/* If all devices reported they can proceed, then re-enable MMIO */
	if (result == PCI_ERS_RESULT_CAN_RECOVER) {
		pr_info("EEH: Enable I/O for affected devices\n");
		rc = eeh_pci_enable(pe, EEH_OPT_THAW_MMIO);

		if (rc < 0)
			goto hard_fail;
		if (rc) {
			result = PCI_ERS_RESULT_NEED_RESET;
		} else {
			pr_info("EEH: Notify device drivers to resume I/O\n");
			eeh_pe_report("mmio_enabled", pe,
				      eeh_report_mmio_enabled, &result);
		}
	}

	/* If all devices reported they can proceed, then re-enable DMA */
	if (result == PCI_ERS_RESULT_CAN_RECOVER) {
		pr_info("EEH: Enabled DMA for affected devices\n");
		rc = eeh_pci_enable(pe, EEH_OPT_THAW_DMA);

		if (rc < 0)
			goto hard_fail;
		if (rc) {
			result = PCI_ERS_RESULT_NEED_RESET;
		} else {
			/*
			 * We didn't do PE reset for the case. The PE
			 * is still in frozen state. Clear it before
			 * resuming the PE.
			 */
			eeh_pe_state_clear(pe, EEH_PE_ISOLATED);
			result = PCI_ERS_RESULT_RECOVERED;
		}
	}

	/* If any device has a hard failure, then shut off everything. */
	if (result == PCI_ERS_RESULT_DISCONNECT) {
		pr_warn("EEH: Device driver gave up\n");
		goto hard_fail;
	}

	/* If any device called out for a reset, then reset the slot */
	if (result == PCI_ERS_RESULT_NEED_RESET) {
		pr_info("EEH: Reset without hotplug activity\n");
		rc = eeh_reset_device(pe, bus, &rmv_data, true);
		if (rc) {
			pr_warn("%s: Cannot reset, err=%d\n",
				__func__, rc);
			goto hard_fail;
		}

		pr_info("EEH: Notify device drivers "
			"the completion of reset\n");
		result = PCI_ERS_RESULT_NONE;
		eeh_set_channel_state(pe, pci_channel_io_normal);
		eeh_set_irq_state(pe, true);
		eeh_pe_report("slot_reset", pe, eeh_report_reset, &result);
	}

	/* All devices should claim they have recovered by now. */
	if ((result != PCI_ERS_RESULT_RECOVERED) &&
	    (result != PCI_ERS_RESULT_NONE)) {
		pr_warn("EEH: Not recovered\n");
		goto hard_fail;
	}

	/*
	 * For those hot removed VFs, we should add back them after PF get
	 * recovered properly.
	 */
	list_for_each_entry_safe(edev, tmp, &rmv_data.edev_list, rmv_list) {
		eeh_add_virt_device(edev, NULL);
		list_del(&edev->rmv_list);
	}

	/* Tell all device drivers that they can resume operations */
	pr_info("EEH: Notify device driver to resume\n");
	eeh_set_channel_state(pe, pci_channel_io_normal);
	eeh_set_irq_state(pe, true);
	eeh_pe_report("resume", pe, eeh_report_resume, NULL);
	eeh_for_each_pe(pe, tmp_pe) {
		eeh_pe_for_each_dev(tmp_pe, edev, tmp) {
			edev->mode &= ~EEH_DEV_NO_HANDLER;
			edev->in_error = false;
		}
	}

	pr_info("EEH: Recovery successful.\n");
	goto final;

hard_fail:
	/*
	 * About 90% of all real-life EEH failures in the field
	 * are due to poorly seated PCI cards. Only 10% or so are
	 * due to actual, failed cards.
	 */
	pr_err("EEH: Unable to recover from failure from PHB#%x-PE#%x.\n"
	       "Please try reseating or replacing it\n",
		pe->phb->global_number, pe->addr);

	eeh_slot_error_detail(pe, EEH_LOG_PERM);

	/* Notify all devices that they're about to go down. */
	eeh_set_channel_state(pe, pci_channel_io_perm_failure);
	eeh_set_irq_state(pe, false);
	eeh_pe_report("error_detected(permanent failure)", pe,
		      eeh_report_failure, NULL);

	/* Mark the PE to be removed permanently */
	eeh_pe_state_mark(pe, EEH_PE_REMOVED);

	/*
	 * Shut down the device drivers for good. We mark
	 * all removed devices correctly to avoid access
	 * the their PCI config any more.
	 */
	if (pe->type & EEH_PE_VF) {
		eeh_pe_dev_traverse(pe, eeh_rmv_device, NULL);
		eeh_pe_dev_mode_mark(pe, EEH_DEV_REMOVED);
	} else {
		eeh_pe_state_clear(pe, EEH_PE_PRI_BUS);
		eeh_pe_dev_mode_mark(pe, EEH_DEV_REMOVED);

		pci_lock_rescan_remove();
		pci_hp_remove_devices(bus);
		pci_unlock_rescan_remove();
		/* The passed PE should no longer be used */
		return;
	}
final:
	eeh_pe_state_clear(pe, EEH_PE_RECOVERING);
}

/**
 * eeh_handle_special_event - Handle EEH events without a specific failing PE
 *
 * Called when an EEH event is detected but can't be narrowed down to a
 * specific PE.  Iterates through possible failures and handles them as
 * necessary.
 */
void eeh_handle_special_event(void)
{
	struct eeh_pe *pe, *phb_pe, *tmp_pe;
	struct eeh_dev *edev, *tmp_edev;
	struct pci_bus *bus;
	struct pci_controller *hose;
	unsigned long flags;
	int rc;


	do {
		rc = eeh_ops->next_error(&pe);

		switch (rc) {
		case EEH_NEXT_ERR_DEAD_IOC:
			/* Mark all PHBs in dead state */
			eeh_serialize_lock(&flags);

			/* Purge all events */
			eeh_remove_event(NULL, true);

			list_for_each_entry(hose, &hose_list, list_node) {
				phb_pe = eeh_phb_pe_get(hose);
				if (!phb_pe) continue;

				eeh_pe_state_mark(phb_pe, EEH_PE_ISOLATED);
			}

			eeh_serialize_unlock(flags);

			break;
		case EEH_NEXT_ERR_FROZEN_PE:
		case EEH_NEXT_ERR_FENCED_PHB:
		case EEH_NEXT_ERR_DEAD_PHB:
			/* Mark the PE in fenced state */
			eeh_serialize_lock(&flags);

			/* Purge all events of the PHB */
			eeh_remove_event(pe, true);

			if (rc == EEH_NEXT_ERR_DEAD_PHB)
				eeh_pe_state_mark(pe, EEH_PE_ISOLATED);
			else
				eeh_pe_state_mark(pe,
					EEH_PE_ISOLATED | EEH_PE_RECOVERING);

			eeh_serialize_unlock(flags);

			break;
		case EEH_NEXT_ERR_NONE:
			return;
		default:
			pr_warn("%s: Invalid value %d from next_error()\n",
				__func__, rc);
			return;
		}

		/*
		 * For fenced PHB and frozen PE, it's handled as normal
		 * event. We have to remove the affected PHBs for dead
		 * PHB and IOC
		 */
		if (rc == EEH_NEXT_ERR_FROZEN_PE ||
		    rc == EEH_NEXT_ERR_FENCED_PHB) {
			eeh_handle_normal_event(pe);
		} else {
			pci_lock_rescan_remove();
			list_for_each_entry(hose, &hose_list, list_node) {
				phb_pe = eeh_phb_pe_get(hose);
				if (!phb_pe ||
				    !(phb_pe->state & EEH_PE_ISOLATED) ||
				    (phb_pe->state & EEH_PE_RECOVERING))
					continue;

				eeh_for_each_pe(pe, tmp_pe)
					eeh_pe_for_each_dev(tmp_pe, edev, tmp_edev)
						edev->mode &= ~EEH_DEV_NO_HANDLER;

				/* Notify all devices to be down */
				eeh_pe_state_clear(pe, EEH_PE_PRI_BUS);
				eeh_set_channel_state(pe, pci_channel_io_perm_failure);
				eeh_pe_report(
					"error_detected(permanent failure)", pe,
					eeh_report_failure, NULL);
				bus = eeh_pe_bus_get(phb_pe);
				if (!bus) {
					pr_err("%s: Cannot find PCI bus for "
					       "PHB#%x-PE#%x\n",
					       __func__,
					       pe->phb->global_number,
					       pe->addr);
					break;
				}
				pci_hp_remove_devices(bus);
			}
			pci_unlock_rescan_remove();
		}

		/*
		 * If we have detected dead IOC, we needn't proceed
		 * any more since all PHBs would have been removed
		 */
		if (rc == EEH_NEXT_ERR_DEAD_IOC)
			break;
	} while (rc != EEH_NEXT_ERR_NONE);
}
