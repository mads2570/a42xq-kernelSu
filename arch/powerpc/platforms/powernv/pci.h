/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __POWERNV_PCI_H
#define __POWERNV_PCI_H

#include <linux/iommu.h>
#include <asm/iommu.h>
#include <asm/msi_bitmap.h>

struct pci_dn;

/* Maximum possible number of ATSD MMIO registers per NPU */
#define NV_NMMU_ATSD_REGS 8

enum pnv_phb_type {
	PNV_PHB_IODA1		= 0,
	PNV_PHB_IODA2		= 1,
	PNV_PHB_NPU_NVLINK	= 2,
	PNV_PHB_NPU_OCAPI	= 3,
};

/* Precise PHB model for error management */
enum pnv_phb_model {
	PNV_PHB_MODEL_UNKNOWN,
	PNV_PHB_MODEL_P7IOC,
	PNV_PHB_MODEL_PHB3,
	PNV_PHB_MODEL_NPU,
	PNV_PHB_MODEL_NPU2,
};

#define PNV_PCI_DIAG_BUF_SIZE	8192
#define PNV_IODA_PE_DEV		(1 << 0)	/* PE has single PCI device	*/
#define PNV_IODA_PE_BUS		(1 << 1)	/* PE has primary PCI bus	*/
#define PNV_IODA_PE_BUS_ALL	(1 << 2)	/* PE has subordinate buses	*/
#define PNV_IODA_PE_MASTER	(1 << 3)	/* Master PE in compound case	*/
#define PNV_IODA_PE_SLAVE	(1 << 4)	/* Slave PE in compound case	*/
#define PNV_IODA_PE_VF		(1 << 5)	/* PE for one VF 		*/

/* Indicates operations are frozen for a PE: MMIO in PESTA & DMA in PESTB. */
#define PNV_IODA_STOPPED_STATE	0x8000000000000000

/* Data associated with a PE, including IOMMU tracking etc.. */
struct pnv_phb;
struct pnv_ioda_pe {
	unsigned long		flags;
	struct pnv_phb		*phb;
	int			device_count;

	/* A PE can be associated with a single device or an
	 * entire bus (& children). In the former case, pdev
	 * is populated, in the later case, pbus is.
	 */
#ifdef CONFIG_PCI_IOV
	struct pci_dev          *parent_dev;
#endif
	struct pci_dev		*pdev;
	struct pci_bus		*pbus;

	/* Effective RID (device RID for a device PE and base bus
	 * RID with devfn 0 for a bus PE)
	 */
	unsigned int		rid;

	/* PE number */
	unsigned int		pe_number;

	/* "Base" iommu table, ie, 4K TCEs, 32-bit DMA */
	struct iommu_table_group table_group;

	/* 64-bit TCE bypass region */
	bool			tce_bypass_enabled;
	uint64_t		tce_bypass_base;

	/* MSIs. MVE index is identical for for 32 and 64 bit MSI
	 * and -1 if not supported. (It's actually identical to the
	 * PE number)
	 */
	int			mve_number;

	/* PEs in compound case */
	struct pnv_ioda_pe	*master;
	struct list_head	slaves;

	/* PCI peer-to-peer*/
	int			p2p_initiator_count;

	/* Link in list of PE#s */
	struct list_head	list;
};

#define PNV_PHB_FLAG_EEH	(1 << 0)

struct pnv_phb {
	struct pci_controller	*hose;
	enum pnv_phb_type	type;
	enum pnv_phb_model	model;
	u64			hub_id;
	u64			opal_id;
	int			flags;
	void __iomem		*regs;
	u64			regs_phys;
	int			initialized;
	spinlock_t		lock;

#ifdef CONFIG_DEBUG_FS
	int			has_dbgfs;
	struct dentry		*dbgfs;
#endif

#ifdef CONFIG_PCI_MSI
	unsigned int		msi_base;
	unsigned int		msi32_support;
	struct msi_bitmap	msi_bmp;
#endif
	int (*msi_setup)(struct pnv_phb *phb, struct pci_dev *dev,
			 unsigned int hwirq, unsigned int virq,
			 unsigned int is_64, struct msi_msg *msg);
	void (*dma_dev_setup)(struct pnv_phb *phb, struct pci_dev *pdev);
	void (*fixup_phb)(struct pci_controller *hose);
	int (*init_m64)(struct pnv_phb *phb);
	void (*reserve_m64_pe)(struct pci_bus *bus,
			       unsigned long *pe_bitmap, bool all);
	struct pnv_ioda_pe *(*pick_m64_pe)(struct pci_bus *bus, bool all);
	int (*get_pe_state)(struct pnv_phb *phb, int pe_no);
	void (*freeze_pe)(struct pnv_phb *phb, int pe_no);
	int (*unfreeze_pe)(struct pnv_phb *phb, int pe_no, int opt);

	struct {
		/* Global bridge info */
		unsigned int		total_pe_num;
		unsigned int		reserved_pe_idx;
		unsigned int		root_pe_idx;
		bool			root_pe_populated;

		/* 32-bit MMIO window */
		unsigned int		m32_size;
		unsigned int		m32_segsize;
		unsigned int		m32_pci_base;

		/* 64-bit MMIO window */
		unsigned int		m64_bar_idx;
		unsigned long		m64_size;
		unsigned long		m64_segsize;
		unsigned long		m64_base;
		unsigned long		m64_bar_alloc;

		/* IO ports */
		unsigned int		io_size;
		unsigned int		io_segsize;
		unsigned int		io_pci_base;

		/* PE allocation */
		struct mutex		pe_alloc_mutex;
		unsigned long		*pe_alloc;
		struct pnv_ioda_pe	*pe_array;

		/* M32 & IO segment maps */
		unsigned int		*m64_segmap;
		unsigned int		*m32_segmap;
		unsigned int		*io_segmap;

		/* DMA32 segment maps - IODA1 only */
		unsigned int		dma32_count;
		unsigned int		*dma32_segmap;

		/* IRQ chip */
		int			irq_chip_init;
		struct irq_chip		irq_chip;

		/* Sorted list of used PE's based
		 * on the sequence of creation
		 */
		struct list_head	pe_list;
		struct mutex            pe_list_mutex;

		/* Reverse map of PEs, indexed by {bus, devfn} */
		unsigned int		pe_rmap[0x10000];
	} ioda;

	/* PHB and hub diagnostics */
	unsigned int		diag_data_size;
	u8			*diag_data;

	/* Nvlink2 data */
	struct npu {
		int index;
		__be64 *mmio_atsd_regs[NV_NMMU_ATSD_REGS];
		unsigned int mmio_atsd_count;

		/* Bitmask for MMIO register usage */
		unsigned long mmio_atsd_usage;

		/* Do we need to explicitly flush the nest mmu? */
		bool nmmu_flush;
	} npu;

	int p2p_target_count;
};

extern struct pci_ops pnv_pci_ops;

void pnv_pci_dump_phb_diag_data(struct pci_controller *hose,
				unsigned char *log_buff);
int pnv_pci_cfg_read(struct pci_dn *pdn,
		     int where, int size, u32 *val);
int pnv_pci_cfg_write(struct pci_dn *pdn,
		      int where, int size, u32 val);
extern struct iommu_table *pnv_pci_table_alloc(int nid);

extern void pnv_pci_init_ioda_hub(struct device_node *np);
extern void pnv_pci_init_ioda2_phb(struct device_node *np);
extern void pnv_pci_init_npu_phb(struct device_node *np);
extern void pnv_pci_init_npu2_opencapi_phb(struct device_node *np);
extern void pnv_pci_reset_secondary_bus(struct pci_dev *dev);
extern int pnv_eeh_phb_reset(struct pci_controller *hose, int option);

extern void pnv_pci_dma_dev_setup(struct pci_dev *pdev);
extern void pnv_pci_dma_bus_setup(struct pci_bus *bus);
extern int pnv_setup_msi_irqs(struct pci_dev *pdev, int nvec, int type);
extern void pnv_teardown_msi_irqs(struct pci_dev *pdev);
extern struct pnv_ioda_pe *pnv_ioda_get_pe(struct pci_dev *dev);
extern void pnv_set_msi_irq_chip(struct pnv_phb *phb, unsigned int virq);
extern void pnv_pci_ioda2_set_bypass(struct pnv_ioda_pe *pe, bool enable);
extern int pnv_eeh_post_init(void);

extern void pe_level_printk(const struct pnv_ioda_pe *pe, const char *level,
			    const char *fmt, ...);
#define pe_err(pe, fmt, ...)					\
	pe_level_printk(pe, KERN_ERR, fmt, ##__VA_ARGS__)
#define pe_warn(pe, fmt, ...)					\
	pe_level_printk(pe, KERN_WARNING, fmt, ##__VA_ARGS__)
#define pe_info(pe, fmt, ...)					\
	pe_level_printk(pe, KERN_INFO, fmt, ##__VA_ARGS__)

/* Nvlink functions */
extern void pnv_npu_try_dma_set_bypass(struct pci_dev *gpdev, bool bypass);
extern void pnv_pci_ioda2_tce_invalidate_entire(struct pnv_phb *phb, bool rm);
extern struct pnv_ioda_pe *pnv_pci_npu_setup_iommu(struct pnv_ioda_pe *npe);
extern long pnv_npu_set_window(struct pnv_ioda_pe *npe, int num,
		struct iommu_table *tbl);
extern long pnv_npu_unset_window(struct pnv_ioda_pe *npe, int num);
extern void pnv_npu_take_ownership(struct pnv_ioda_pe *npe);
extern void pnv_npu_release_ownership(struct pnv_ioda_pe *npe);
extern int pnv_npu2_init(struct pnv_phb *phb);

/* pci-ioda-tce.c */
#define POWERNV_IOMMU_DEFAULT_LEVELS	2
#define POWERNV_IOMMU_MAX_LEVELS	5

extern int pnv_tce_build(struct iommu_table *tbl, long index, long npages,
		unsigned long uaddr, enum dma_data_direction direction,
		unsigned long attrs);
extern void pnv_tce_free(struct iommu_table *tbl, long index, long npages);
extern int pnv_tce_xchg(struct iommu_table *tbl, long index,
		unsigned long *hpa, enum dma_data_direction *direction,
		bool alloc);
extern __be64 *pnv_tce_useraddrptr(struct iommu_table *tbl, long index,
		bool alloc);
extern unsigned long pnv_tce_get(struct iommu_table *tbl, long index);

extern long pnv_pci_ioda2_table_alloc_pages(int nid, __u64 bus_offset,
		__u32 page_shift, __u64 window_size, __u32 levels,
		bool alloc_userspace_copy, struct iommu_table *tbl);
extern void pnv_pci_ioda2_table_free_pages(struct iommu_table *tbl);

extern long pnv_pci_link_table_and_group(int node, int num,
		struct iommu_table *tbl,
		struct iommu_table_group *table_group);
extern void pnv_pci_unlink_table_and_group(struct iommu_table *tbl,
		struct iommu_table_group *table_group);
extern void pnv_pci_setup_iommu_table(struct iommu_table *tbl,
		void *tce_mem, u64 tce_size,
		u64 dma_offset, unsigned int page_shift);

#endif /* __POWERNV_PCI_H */
