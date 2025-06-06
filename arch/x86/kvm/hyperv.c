/*
 * KVM Microsoft Hyper-V emulation
 *
 * derived from arch/x86/kvm/x86.c
 *
 * Copyright (C) 2006 Qumranet, Inc.
 * Copyright (C) 2008 Qumranet, Inc.
 * Copyright IBM Corporation, 2008
 * Copyright 2010 Red Hat, Inc. and/or its affiliates.
 * Copyright (C) 2015 Andrey Smetanin <asmetanin@virtuozzo.com>
 *
 * Authors:
 *   Avi Kivity   <avi@qumranet.com>
 *   Yaniv Kamay  <yaniv@qumranet.com>
 *   Amit Shah    <amit.shah@qumranet.com>
 *   Ben-Ami Yassour <benami@il.ibm.com>
 *   Andrey Smetanin <asmetanin@virtuozzo.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "x86.h"
#include "lapic.h"
#include "ioapic.h"
#include "hyperv.h"

#include <linux/kvm_host.h>
#include <linux/highmem.h>
#include <linux/sched/cputime.h>
#include <linux/eventfd.h>

#include <asm/apicdef.h>
#include <trace/events/kvm.h>

#include "trace.h"

static inline u64 synic_read_sint(struct kvm_vcpu_hv_synic *synic, int sint)
{
	return atomic64_read(&synic->sint[sint]);
}

static inline int synic_get_sint_vector(u64 sint_value)
{
	if (sint_value & HV_SYNIC_SINT_MASKED)
		return -1;
	return sint_value & HV_SYNIC_SINT_VECTOR_MASK;
}

static bool synic_has_vector_connected(struct kvm_vcpu_hv_synic *synic,
				      int vector)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(synic->sint); i++) {
		if (synic_get_sint_vector(synic_read_sint(synic, i)) == vector)
			return true;
	}
	return false;
}

static bool synic_has_vector_auto_eoi(struct kvm_vcpu_hv_synic *synic,
				     int vector)
{
	int i;
	u64 sint_value;

	for (i = 0; i < ARRAY_SIZE(synic->sint); i++) {
		sint_value = synic_read_sint(synic, i);
		if (synic_get_sint_vector(sint_value) == vector &&
		    sint_value & HV_SYNIC_SINT_AUTO_EOI)
			return true;
	}
	return false;
}

static void synic_update_vector(struct kvm_vcpu_hv_synic *synic,
				int vector)
{
	if (vector < HV_SYNIC_FIRST_VALID_VECTOR)
		return;

	if (synic_has_vector_connected(synic, vector))
		__set_bit(vector, synic->vec_bitmap);
	else
		__clear_bit(vector, synic->vec_bitmap);

	if (synic_has_vector_auto_eoi(synic, vector))
		__set_bit(vector, synic->auto_eoi_bitmap);
	else
		__clear_bit(vector, synic->auto_eoi_bitmap);
}

static int synic_set_sint(struct kvm_vcpu_hv_synic *synic, int sint,
			  u64 data, bool host)
{
	int vector, old_vector;
	bool masked;

	vector = data & HV_SYNIC_SINT_VECTOR_MASK;
	masked = data & HV_SYNIC_SINT_MASKED;

	/*
	 * Valid vectors are 16-255, however, nested Hyper-V attempts to write
	 * default '0x10000' value on boot and this should not #GP. We need to
	 * allow zero-initing the register from host as well.
	 */
	if (vector < HV_SYNIC_FIRST_VALID_VECTOR && !host && !masked)
		return 1;
	/*
	 * Guest may configure multiple SINTs to use the same vector, so
	 * we maintain a bitmap of vectors handled by synic, and a
	 * bitmap of vectors with auto-eoi behavior.  The bitmaps are
	 * updated here, and atomically queried on fast paths.
	 */
	old_vector = synic_read_sint(synic, sint) & HV_SYNIC_SINT_VECTOR_MASK;

	atomic64_set(&synic->sint[sint], data);

	synic_update_vector(synic, old_vector);

	synic_update_vector(synic, vector);

	/* Load SynIC vectors into EOI exit bitmap */
	kvm_make_request(KVM_REQ_SCAN_IOAPIC, synic_to_vcpu(synic));
	return 0;
}

static struct kvm_vcpu *get_vcpu_by_vpidx(struct kvm *kvm, u32 vpidx)
{
	struct kvm_vcpu *vcpu = NULL;
	int i;

	if (vpidx >= KVM_MAX_VCPUS)
		return NULL;

	vcpu = kvm_get_vcpu(kvm, vpidx);
	if (vcpu && vcpu_to_hv_vcpu(vcpu)->vp_index == vpidx)
		return vcpu;
	kvm_for_each_vcpu(i, vcpu, kvm)
		if (vcpu_to_hv_vcpu(vcpu)->vp_index == vpidx)
			return vcpu;
	return NULL;
}

static struct kvm_vcpu_hv_synic *synic_get(struct kvm *kvm, u32 vpidx)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vcpu_hv_synic *synic;

	vcpu = get_vcpu_by_vpidx(kvm, vpidx);
	if (!vcpu)
		return NULL;
	synic = vcpu_to_synic(vcpu);
	return (synic->active) ? synic : NULL;
}

static void synic_clear_sint_msg_pending(struct kvm_vcpu_hv_synic *synic,
					u32 sint)
{
	struct kvm_vcpu *vcpu = synic_to_vcpu(synic);
	struct page *page;
	gpa_t gpa;
	struct hv_message *msg;
	struct hv_message_page *msg_page;

	gpa = synic->msg_page & PAGE_MASK;
	page = kvm_vcpu_gfn_to_page(vcpu, gpa >> PAGE_SHIFT);
	if (is_error_page(page)) {
		vcpu_err(vcpu, "Hyper-V SynIC can't get msg page, gpa 0x%llx\n",
			 gpa);
		return;
	}
	msg_page = kmap_atomic(page);

	msg = &msg_page->sint_message[sint];
	msg->header.message_flags.msg_pending = 0;

	kunmap_atomic(msg_page);
	kvm_release_page_dirty(page);
	kvm_vcpu_mark_page_dirty(vcpu, gpa >> PAGE_SHIFT);
}

static void kvm_hv_notify_acked_sint(struct kvm_vcpu *vcpu, u32 sint)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_vcpu_hv_synic *synic = vcpu_to_synic(vcpu);
	struct kvm_vcpu_hv *hv_vcpu = vcpu_to_hv_vcpu(vcpu);
	struct kvm_vcpu_hv_stimer *stimer;
	int gsi, idx, stimers_pending;

	trace_kvm_hv_notify_acked_sint(vcpu->vcpu_id, sint);

	if (synic->msg_page & HV_SYNIC_SIMP_ENABLE)
		synic_clear_sint_msg_pending(synic, sint);

	/* Try to deliver pending Hyper-V SynIC timers messages */
	stimers_pending = 0;
	for (idx = 0; idx < ARRAY_SIZE(hv_vcpu->stimer); idx++) {
		stimer = &hv_vcpu->stimer[idx];
		if (stimer->msg_pending &&
		    (stimer->config & HV_STIMER_ENABLE) &&
		    HV_STIMER_SINT(stimer->config) == sint) {
			set_bit(stimer->index,
				hv_vcpu->stimer_pending_bitmap);
			stimers_pending++;
		}
	}
	if (stimers_pending)
		kvm_make_request(KVM_REQ_HV_STIMER, vcpu);

	idx = srcu_read_lock(&kvm->irq_srcu);
	gsi = atomic_read(&synic->sint_to_gsi[sint]);
	if (gsi != -1)
		kvm_notify_acked_gsi(kvm, gsi);
	srcu_read_unlock(&kvm->irq_srcu, idx);
}

static void synic_exit(struct kvm_vcpu_hv_synic *synic, u32 msr)
{
	struct kvm_vcpu *vcpu = synic_to_vcpu(synic);
	struct kvm_vcpu_hv *hv_vcpu = &vcpu->arch.hyperv;

	hv_vcpu->exit.type = KVM_EXIT_HYPERV_SYNIC;
	hv_vcpu->exit.u.synic.msr = msr;
	hv_vcpu->exit.u.synic.control = synic->control;
	hv_vcpu->exit.u.synic.evt_page = synic->evt_page;
	hv_vcpu->exit.u.synic.msg_page = synic->msg_page;

	kvm_make_request(KVM_REQ_HV_EXIT, vcpu);
}

static int synic_set_msr(struct kvm_vcpu_hv_synic *synic,
			 u32 msr, u64 data, bool host)
{
	struct kvm_vcpu *vcpu = synic_to_vcpu(synic);
	int ret;

	if (!synic->active && (!host || data))
		return 1;

	trace_kvm_hv_synic_set_msr(vcpu->vcpu_id, msr, data, host);

	ret = 0;
	switch (msr) {
	case HV_X64_MSR_SCONTROL:
		synic->control = data;
		if (!host)
			synic_exit(synic, msr);
		break;
	case HV_X64_MSR_SVERSION:
		if (!host) {
			ret = 1;
			break;
		}
		synic->version = data;
		break;
	case HV_X64_MSR_SIEFP:
		if ((data & HV_SYNIC_SIEFP_ENABLE) && !host &&
		    !synic->dont_zero_synic_pages)
			if (kvm_clear_guest(vcpu->kvm,
					    data & PAGE_MASK, PAGE_SIZE)) {
				ret = 1;
				break;
			}
		synic->evt_page = data;
		if (!host)
			synic_exit(synic, msr);
		break;
	case HV_X64_MSR_SIMP:
		if ((data & HV_SYNIC_SIMP_ENABLE) && !host &&
		    !synic->dont_zero_synic_pages)
			if (kvm_clear_guest(vcpu->kvm,
					    data & PAGE_MASK, PAGE_SIZE)) {
				ret = 1;
				break;
			}
		synic->msg_page = data;
		if (!host)
			synic_exit(synic, msr);
		break;
	case HV_X64_MSR_EOM: {
		int i;

		if (!synic->active)
			break;

		for (i = 0; i < ARRAY_SIZE(synic->sint); i++)
			kvm_hv_notify_acked_sint(vcpu, i);
		break;
	}
	case HV_X64_MSR_SINT0 ... HV_X64_MSR_SINT15:
		ret = synic_set_sint(synic, msr - HV_X64_MSR_SINT0, data, host);
		break;
	default:
		ret = 1;
		break;
	}
	return ret;
}

static int synic_get_msr(struct kvm_vcpu_hv_synic *synic, u32 msr, u64 *pdata,
			 bool host)
{
	int ret;

	if (!synic->active && !host)
		return 1;

	ret = 0;
	switch (msr) {
	case HV_X64_MSR_SCONTROL:
		*pdata = synic->control;
		break;
	case HV_X64_MSR_SVERSION:
		*pdata = synic->version;
		break;
	case HV_X64_MSR_SIEFP:
		*pdata = synic->evt_page;
		break;
	case HV_X64_MSR_SIMP:
		*pdata = synic->msg_page;
		break;
	case HV_X64_MSR_EOM:
		*pdata = 0;
		break;
	case HV_X64_MSR_SINT0 ... HV_X64_MSR_SINT15:
		*pdata = atomic64_read(&synic->sint[msr - HV_X64_MSR_SINT0]);
		break;
	default:
		ret = 1;
		break;
	}
	return ret;
}

static int synic_set_irq(struct kvm_vcpu_hv_synic *synic, u32 sint)
{
	struct kvm_vcpu *vcpu = synic_to_vcpu(synic);
	struct kvm_lapic_irq irq;
	int ret, vector;

	if (KVM_BUG_ON(!lapic_in_kernel(vcpu), vcpu->kvm))
		return -EINVAL;

	if (sint >= ARRAY_SIZE(synic->sint))
		return -EINVAL;

	vector = synic_get_sint_vector(synic_read_sint(synic, sint));
	if (vector < 0)
		return -ENOENT;

	memset(&irq, 0, sizeof(irq));
	irq.shorthand = APIC_DEST_SELF;
	irq.dest_mode = APIC_DEST_PHYSICAL;
	irq.delivery_mode = APIC_DM_FIXED;
	irq.vector = vector;
	irq.level = 1;

	ret = kvm_irq_delivery_to_apic(vcpu->kvm, vcpu->arch.apic, &irq, NULL);
	trace_kvm_hv_synic_set_irq(vcpu->vcpu_id, sint, irq.vector, ret);
	return ret;
}

int kvm_hv_synic_set_irq(struct kvm *kvm, u32 vpidx, u32 sint)
{
	struct kvm_vcpu_hv_synic *synic;

	synic = synic_get(kvm, vpidx);
	if (!synic)
		return -EINVAL;

	return synic_set_irq(synic, sint);
}

void kvm_hv_synic_send_eoi(struct kvm_vcpu *vcpu, int vector)
{
	struct kvm_vcpu_hv_synic *synic = vcpu_to_synic(vcpu);
	int i;

	trace_kvm_hv_synic_send_eoi(vcpu->vcpu_id, vector);

	for (i = 0; i < ARRAY_SIZE(synic->sint); i++)
		if (synic_get_sint_vector(synic_read_sint(synic, i)) == vector)
			kvm_hv_notify_acked_sint(vcpu, i);
}

static int kvm_hv_set_sint_gsi(struct kvm *kvm, u32 vpidx, u32 sint, int gsi)
{
	struct kvm_vcpu_hv_synic *synic;

	synic = synic_get(kvm, vpidx);
	if (!synic)
		return -EINVAL;

	if (sint >= ARRAY_SIZE(synic->sint_to_gsi))
		return -EINVAL;

	atomic_set(&synic->sint_to_gsi[sint], gsi);
	return 0;
}

void kvm_hv_irq_routing_update(struct kvm *kvm)
{
	struct kvm_irq_routing_table *irq_rt;
	struct kvm_kernel_irq_routing_entry *e;
	u32 gsi;

	irq_rt = srcu_dereference_check(kvm->irq_routing, &kvm->irq_srcu,
					lockdep_is_held(&kvm->irq_lock));

	for (gsi = 0; gsi < irq_rt->nr_rt_entries; gsi++) {
		hlist_for_each_entry(e, &irq_rt->map[gsi], link) {
			if (e->type == KVM_IRQ_ROUTING_HV_SINT)
				kvm_hv_set_sint_gsi(kvm, e->hv_sint.vcpu,
						    e->hv_sint.sint, gsi);
		}
	}
}

static void synic_init(struct kvm_vcpu_hv_synic *synic)
{
	int i;

	memset(synic, 0, sizeof(*synic));
	synic->version = HV_SYNIC_VERSION_1;
	for (i = 0; i < ARRAY_SIZE(synic->sint); i++) {
		atomic64_set(&synic->sint[i], HV_SYNIC_SINT_MASKED);
		atomic_set(&synic->sint_to_gsi[i], -1);
	}
}

static u64 get_time_ref_counter(struct kvm *kvm)
{
	struct kvm_hv *hv = &kvm->arch.hyperv;
	struct kvm_vcpu *vcpu;
	u64 tsc;

	/*
	 * The guest has not set up the TSC page or the clock isn't
	 * stable, fall back to get_kvmclock_ns.
	 */
	if (!hv->tsc_ref.tsc_sequence)
		return div_u64(get_kvmclock_ns(kvm), 100);

	vcpu = kvm_get_vcpu(kvm, 0);
	tsc = kvm_read_l1_tsc(vcpu, rdtsc());
	return mul_u64_u64_shr(tsc, hv->tsc_ref.tsc_scale, 64)
		+ hv->tsc_ref.tsc_offset;
}

static void stimer_mark_pending(struct kvm_vcpu_hv_stimer *stimer,
				bool vcpu_kick)
{
	struct kvm_vcpu *vcpu = stimer_to_vcpu(stimer);

	set_bit(stimer->index,
		vcpu_to_hv_vcpu(vcpu)->stimer_pending_bitmap);
	kvm_make_request(KVM_REQ_HV_STIMER, vcpu);
	if (vcpu_kick)
		kvm_vcpu_kick(vcpu);
}

static void stimer_cleanup(struct kvm_vcpu_hv_stimer *stimer)
{
	struct kvm_vcpu *vcpu = stimer_to_vcpu(stimer);

	trace_kvm_hv_stimer_cleanup(stimer_to_vcpu(stimer)->vcpu_id,
				    stimer->index);

	hrtimer_cancel(&stimer->timer);
	clear_bit(stimer->index,
		  vcpu_to_hv_vcpu(vcpu)->stimer_pending_bitmap);
	stimer->msg_pending = false;
	stimer->exp_time = 0;
}

static enum hrtimer_restart stimer_timer_callback(struct hrtimer *timer)
{
	struct kvm_vcpu_hv_stimer *stimer;

	stimer = container_of(timer, struct kvm_vcpu_hv_stimer, timer);
	trace_kvm_hv_stimer_callback(stimer_to_vcpu(stimer)->vcpu_id,
				     stimer->index);
	stimer_mark_pending(stimer, true);

	return HRTIMER_NORESTART;
}

/*
 * stimer_start() assumptions:
 * a) stimer->count is not equal to 0
 * b) stimer->config has HV_STIMER_ENABLE flag
 */
static int stimer_start(struct kvm_vcpu_hv_stimer *stimer)
{
	u64 time_now;
	ktime_t ktime_now;

	time_now = get_time_ref_counter(stimer_to_vcpu(stimer)->kvm);
	ktime_now = ktime_get();

	if (stimer->config & HV_STIMER_PERIODIC) {
		if (stimer->exp_time) {
			if (time_now >= stimer->exp_time) {
				u64 remainder;

				div64_u64_rem(time_now - stimer->exp_time,
					      stimer->count, &remainder);
				stimer->exp_time =
					time_now + (stimer->count - remainder);
			}
		} else
			stimer->exp_time = time_now + stimer->count;

		trace_kvm_hv_stimer_start_periodic(
					stimer_to_vcpu(stimer)->vcpu_id,
					stimer->index,
					time_now, stimer->exp_time);

		hrtimer_start(&stimer->timer,
			      ktime_add_ns(ktime_now,
					   100 * (stimer->exp_time - time_now)),
			      HRTIMER_MODE_ABS);
		return 0;
	}
	stimer->exp_time = stimer->count;
	if (time_now >= stimer->count) {
		/*
		 * Expire timer according to Hypervisor Top-Level Functional
		 * specification v4(15.3.1):
		 * "If a one shot is enabled and the specified count is in
		 * the past, it will expire immediately."
		 */
		stimer_mark_pending(stimer, false);
		return 0;
	}

	trace_kvm_hv_stimer_start_one_shot(stimer_to_vcpu(stimer)->vcpu_id,
					   stimer->index,
					   time_now, stimer->count);

	hrtimer_start(&stimer->timer,
		      ktime_add_ns(ktime_now, 100 * (stimer->count - time_now)),
		      HRTIMER_MODE_ABS);
	return 0;
}

static int stimer_set_config(struct kvm_vcpu_hv_stimer *stimer, u64 config,
			     bool host)
{
	struct kvm_vcpu *vcpu = stimer_to_vcpu(stimer);
	struct kvm_vcpu_hv_synic *synic = vcpu_to_synic(vcpu);

	if (!synic->active && (!host || config))
		return 1;

	trace_kvm_hv_stimer_set_config(stimer_to_vcpu(stimer)->vcpu_id,
				       stimer->index, config, host);

	stimer_cleanup(stimer);
	if ((stimer->config & HV_STIMER_ENABLE) && HV_STIMER_SINT(config) == 0)
		config &= ~HV_STIMER_ENABLE;
	stimer->config = config;
	stimer_mark_pending(stimer, false);
	return 0;
}

static int stimer_set_count(struct kvm_vcpu_hv_stimer *stimer, u64 count,
			    bool host)
{
	struct kvm_vcpu *vcpu = stimer_to_vcpu(stimer);
	struct kvm_vcpu_hv_synic *synic = vcpu_to_synic(vcpu);

	if (!synic->active && (!host || count))
		return 1;

	trace_kvm_hv_stimer_set_count(stimer_to_vcpu(stimer)->vcpu_id,
				      stimer->index, count, host);

	stimer_cleanup(stimer);
	stimer->count = count;
	if (stimer->count == 0)
		stimer->config &= ~HV_STIMER_ENABLE;
	else if (stimer->config & HV_STIMER_AUTOENABLE)
		stimer->config |= HV_STIMER_ENABLE;
	stimer_mark_pending(stimer, false);
	return 0;
}

static int stimer_get_config(struct kvm_vcpu_hv_stimer *stimer, u64 *pconfig)
{
	*pconfig = stimer->config;
	return 0;
}

static int stimer_get_count(struct kvm_vcpu_hv_stimer *stimer, u64 *pcount)
{
	*pcount = stimer->count;
	return 0;
}

static int synic_deliver_msg(struct kvm_vcpu_hv_synic *synic, u32 sint,
			     struct hv_message *src_msg)
{
	struct kvm_vcpu *vcpu = synic_to_vcpu(synic);
	struct page *page;
	gpa_t gpa;
	struct hv_message *dst_msg;
	int r;
	struct hv_message_page *msg_page;

	if (!(synic->msg_page & HV_SYNIC_SIMP_ENABLE))
		return -ENOENT;

	gpa = synic->msg_page & PAGE_MASK;
	page = kvm_vcpu_gfn_to_page(vcpu, gpa >> PAGE_SHIFT);
	if (is_error_page(page))
		return -EFAULT;

	msg_page = kmap_atomic(page);
	dst_msg = &msg_page->sint_message[sint];
	if (sync_cmpxchg(&dst_msg->header.message_type, HVMSG_NONE,
			 src_msg->header.message_type) != HVMSG_NONE) {
		dst_msg->header.message_flags.msg_pending = 1;
		r = -EAGAIN;
	} else {
		memcpy(&dst_msg->u.payload, &src_msg->u.payload,
		       src_msg->header.payload_size);
		dst_msg->header.message_type = src_msg->header.message_type;
		dst_msg->header.payload_size = src_msg->header.payload_size;
		r = synic_set_irq(synic, sint);
		if (r >= 1)
			r = 0;
		else if (r == 0)
			r = -EFAULT;
	}
	kunmap_atomic(msg_page);
	kvm_release_page_dirty(page);
	kvm_vcpu_mark_page_dirty(vcpu, gpa >> PAGE_SHIFT);
	return r;
}

static int stimer_send_msg(struct kvm_vcpu_hv_stimer *stimer)
{
	struct kvm_vcpu *vcpu = stimer_to_vcpu(stimer);
	struct hv_message *msg = &stimer->msg;
	struct hv_timer_message_payload *payload =
			(struct hv_timer_message_payload *)&msg->u.payload;

	payload->expiration_time = stimer->exp_time;
	payload->delivery_time = get_time_ref_counter(vcpu->kvm);
	return synic_deliver_msg(vcpu_to_synic(vcpu),
				 HV_STIMER_SINT(stimer->config), msg);
}

static void stimer_expiration(struct kvm_vcpu_hv_stimer *stimer)
{
	int r;

	stimer->msg_pending = true;
	r = stimer_send_msg(stimer);
	trace_kvm_hv_stimer_expiration(stimer_to_vcpu(stimer)->vcpu_id,
				       stimer->index, r);
	if (!r) {
		stimer->msg_pending = false;
		if (!(stimer->config & HV_STIMER_PERIODIC))
			stimer->config &= ~HV_STIMER_ENABLE;
	}
}

void kvm_hv_process_stimers(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_hv *hv_vcpu = vcpu_to_hv_vcpu(vcpu);
	struct kvm_vcpu_hv_stimer *stimer;
	u64 time_now, exp_time;
	int i;

	for (i = 0; i < ARRAY_SIZE(hv_vcpu->stimer); i++)
		if (test_and_clear_bit(i, hv_vcpu->stimer_pending_bitmap)) {
			stimer = &hv_vcpu->stimer[i];
			if (stimer->config & HV_STIMER_ENABLE) {
				exp_time = stimer->exp_time;

				if (exp_time) {
					time_now =
						get_time_ref_counter(vcpu->kvm);
					if (time_now >= exp_time)
						stimer_expiration(stimer);
				}

				if ((stimer->config & HV_STIMER_ENABLE) &&
				    stimer->count) {
					if (!stimer->msg_pending)
						stimer_start(stimer);
				} else
					stimer_cleanup(stimer);
			}
		}
}

void kvm_hv_vcpu_uninit(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_hv *hv_vcpu = vcpu_to_hv_vcpu(vcpu);
	int i;

	for (i = 0; i < ARRAY_SIZE(hv_vcpu->stimer); i++)
		stimer_cleanup(&hv_vcpu->stimer[i]);
}

bool kvm_hv_assist_page_enabled(struct kvm_vcpu *vcpu)
{
	if (!(vcpu->arch.hyperv.hv_vapic & HV_X64_MSR_VP_ASSIST_PAGE_ENABLE))
		return false;
	return vcpu->arch.pv_eoi.msr_val & KVM_MSR_ENABLED;
}
EXPORT_SYMBOL_GPL(kvm_hv_assist_page_enabled);

bool kvm_hv_get_assist_page(struct kvm_vcpu *vcpu,
			    struct hv_vp_assist_page *assist_page)
{
	if (!kvm_hv_assist_page_enabled(vcpu))
		return false;
	return !kvm_read_guest_cached(vcpu->kvm, &vcpu->arch.pv_eoi.data,
				      assist_page, sizeof(*assist_page));
}
EXPORT_SYMBOL_GPL(kvm_hv_get_assist_page);

static void stimer_prepare_msg(struct kvm_vcpu_hv_stimer *stimer)
{
	struct hv_message *msg = &stimer->msg;
	struct hv_timer_message_payload *payload =
			(struct hv_timer_message_payload *)&msg->u.payload;

	memset(&msg->header, 0, sizeof(msg->header));
	msg->header.message_type = HVMSG_TIMER_EXPIRED;
	msg->header.payload_size = sizeof(*payload);

	payload->timer_index = stimer->index;
	payload->expiration_time = 0;
	payload->delivery_time = 0;
}

static void stimer_init(struct kvm_vcpu_hv_stimer *stimer, int timer_index)
{
	memset(stimer, 0, sizeof(*stimer));
	stimer->index = timer_index;
	hrtimer_init(&stimer->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	stimer->timer.function = stimer_timer_callback;
	stimer_prepare_msg(stimer);
}

void kvm_hv_vcpu_init(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_hv *hv_vcpu = vcpu_to_hv_vcpu(vcpu);
	int i;

	synic_init(&hv_vcpu->synic);

	bitmap_zero(hv_vcpu->stimer_pending_bitmap, HV_SYNIC_STIMER_COUNT);
	for (i = 0; i < ARRAY_SIZE(hv_vcpu->stimer); i++)
		stimer_init(&hv_vcpu->stimer[i], i);
}

void kvm_hv_vcpu_postcreate(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_hv *hv_vcpu = vcpu_to_hv_vcpu(vcpu);

	hv_vcpu->vp_index = kvm_vcpu_get_idx(vcpu);
}

int kvm_hv_activate_synic(struct kvm_vcpu *vcpu, bool dont_zero_synic_pages)
{
	struct kvm_vcpu_hv_synic *synic = vcpu_to_synic(vcpu);

	/*
	 * Hyper-V SynIC auto EOI SINT's are
	 * not compatible with APICV, so deactivate APICV
	 */
	kvm_vcpu_deactivate_apicv(vcpu);
	synic->active = true;
	synic->dont_zero_synic_pages = dont_zero_synic_pages;
	return 0;
}

static bool kvm_hv_msr_partition_wide(u32 msr)
{
	bool r = false;

	switch (msr) {
	case HV_X64_MSR_GUEST_OS_ID:
	case HV_X64_MSR_HYPERCALL:
	case HV_X64_MSR_REFERENCE_TSC:
	case HV_X64_MSR_TIME_REF_COUNT:
	case HV_X64_MSR_CRASH_CTL:
	case HV_X64_MSR_CRASH_P0 ... HV_X64_MSR_CRASH_P4:
	case HV_X64_MSR_RESET:
	case HV_X64_MSR_REENLIGHTENMENT_CONTROL:
	case HV_X64_MSR_TSC_EMULATION_CONTROL:
	case HV_X64_MSR_TSC_EMULATION_STATUS:
		r = true;
		break;
	}

	return r;
}

static int kvm_hv_msr_get_crash_data(struct kvm_vcpu *vcpu,
				     u32 index, u64 *pdata)
{
	struct kvm_hv *hv = &vcpu->kvm->arch.hyperv;
	size_t size = ARRAY_SIZE(hv->hv_crash_param);

	if (WARN_ON_ONCE(index >= size))
		return -EINVAL;

	*pdata = hv->hv_crash_param[array_index_nospec(index, size)];
	return 0;
}

static int kvm_hv_msr_get_crash_ctl(struct kvm_vcpu *vcpu, u64 *pdata)
{
	struct kvm_hv *hv = &vcpu->kvm->arch.hyperv;

	*pdata = hv->hv_crash_ctl;
	return 0;
}

static int kvm_hv_msr_set_crash_ctl(struct kvm_vcpu *vcpu, u64 data, bool host)
{
	struct kvm_hv *hv = &vcpu->kvm->arch.hyperv;

	if (host)
		hv->hv_crash_ctl = data & HV_X64_MSR_CRASH_CTL_NOTIFY;

	if (!host && (data & HV_X64_MSR_CRASH_CTL_NOTIFY)) {

		vcpu_debug(vcpu, "hv crash (0x%llx 0x%llx 0x%llx 0x%llx 0x%llx)\n",
			  hv->hv_crash_param[0],
			  hv->hv_crash_param[1],
			  hv->hv_crash_param[2],
			  hv->hv_crash_param[3],
			  hv->hv_crash_param[4]);

		/* Send notification about crash to user space */
		kvm_make_request(KVM_REQ_HV_CRASH, vcpu);
	}

	return 0;
}

static int kvm_hv_msr_set_crash_data(struct kvm_vcpu *vcpu,
				     u32 index, u64 data)
{
	struct kvm_hv *hv = &vcpu->kvm->arch.hyperv;
	size_t size = ARRAY_SIZE(hv->hv_crash_param);

	if (WARN_ON_ONCE(index >= size))
		return -EINVAL;

	hv->hv_crash_param[array_index_nospec(index, size)] = data;
	return 0;
}

/*
 * The kvmclock and Hyper-V TSC page use similar formulas, and converting
 * between them is possible:
 *
 * kvmclock formula:
 *    nsec = (ticks - tsc_timestamp) * tsc_to_system_mul * 2^(tsc_shift-32)
 *           + system_time
 *
 * Hyper-V formula:
 *    nsec/100 = ticks * scale / 2^64 + offset
 *
 * When tsc_timestamp = system_time = 0, offset is zero in the Hyper-V formula.
 * By dividing the kvmclock formula by 100 and equating what's left we get:
 *    ticks * scale / 2^64 = ticks * tsc_to_system_mul * 2^(tsc_shift-32) / 100
 *            scale / 2^64 =         tsc_to_system_mul * 2^(tsc_shift-32) / 100
 *            scale        =         tsc_to_system_mul * 2^(32+tsc_shift) / 100
 *
 * Now expand the kvmclock formula and divide by 100:
 *    nsec = ticks * tsc_to_system_mul * 2^(tsc_shift-32)
 *           - tsc_timestamp * tsc_to_system_mul * 2^(tsc_shift-32)
 *           + system_time
 *    nsec/100 = ticks * tsc_to_system_mul * 2^(tsc_shift-32) / 100
 *               - tsc_timestamp * tsc_to_system_mul * 2^(tsc_shift-32) / 100
 *               + system_time / 100
 *
 * Replace tsc_to_system_mul * 2^(tsc_shift-32) / 100 by scale / 2^64:
 *    nsec/100 = ticks * scale / 2^64
 *               - tsc_timestamp * scale / 2^64
 *               + system_time / 100
 *
 * Equate with the Hyper-V formula so that ticks * scale / 2^64 cancels out:
 *    offset = system_time / 100 - tsc_timestamp * scale / 2^64
 *
 * These two equivalencies are implemented in this function.
 */
static bool compute_tsc_page_parameters(struct pvclock_vcpu_time_info *hv_clock,
					HV_REFERENCE_TSC_PAGE *tsc_ref)
{
	u64 max_mul;

	if (!(hv_clock->flags & PVCLOCK_TSC_STABLE_BIT))
		return false;

	/*
	 * check if scale would overflow, if so we use the time ref counter
	 *    tsc_to_system_mul * 2^(tsc_shift+32) / 100 >= 2^64
	 *    tsc_to_system_mul / 100 >= 2^(32-tsc_shift)
	 *    tsc_to_system_mul >= 100 * 2^(32-tsc_shift)
	 */
	max_mul = 100ull << (32 - hv_clock->tsc_shift);
	if (hv_clock->tsc_to_system_mul >= max_mul)
		return false;

	/*
	 * Otherwise compute the scale and offset according to the formulas
	 * derived above.
	 */
	tsc_ref->tsc_scale =
		mul_u64_u32_div(1ULL << (32 + hv_clock->tsc_shift),
				hv_clock->tsc_to_system_mul,
				100);

	tsc_ref->tsc_offset = hv_clock->system_time;
	do_div(tsc_ref->tsc_offset, 100);
	tsc_ref->tsc_offset -=
		mul_u64_u64_shr(hv_clock->tsc_timestamp, tsc_ref->tsc_scale, 64);
	return true;
}

void kvm_hv_setup_tsc_page(struct kvm *kvm,
			   struct pvclock_vcpu_time_info *hv_clock)
{
	struct kvm_hv *hv = &kvm->arch.hyperv;
	u32 tsc_seq;
	u64 gfn;

	BUILD_BUG_ON(sizeof(tsc_seq) != sizeof(hv->tsc_ref.tsc_sequence));
	BUILD_BUG_ON(offsetof(HV_REFERENCE_TSC_PAGE, tsc_sequence) != 0);

	if (!(hv->hv_tsc_page & HV_X64_MSR_TSC_REFERENCE_ENABLE))
		return;

	mutex_lock(&kvm->arch.hyperv.hv_lock);
	if (!(hv->hv_tsc_page & HV_X64_MSR_TSC_REFERENCE_ENABLE))
		goto out_unlock;

	gfn = hv->hv_tsc_page >> HV_X64_MSR_TSC_REFERENCE_ADDRESS_SHIFT;
	/*
	 * Because the TSC parameters only vary when there is a
	 * change in the master clock, do not bother with caching.
	 */
	if (unlikely(kvm_read_guest(kvm, gfn_to_gpa(gfn),
				    &tsc_seq, sizeof(tsc_seq))))
		goto out_unlock;

	/*
	 * While we're computing and writing the parameters, force the
	 * guest to use the time reference count MSR.
	 */
	hv->tsc_ref.tsc_sequence = 0;
	if (kvm_write_guest(kvm, gfn_to_gpa(gfn),
			    &hv->tsc_ref, sizeof(hv->tsc_ref.tsc_sequence)))
		goto out_unlock;

	if (!compute_tsc_page_parameters(hv_clock, &hv->tsc_ref))
		goto out_unlock;

	/* Ensure sequence is zero before writing the rest of the struct.  */
	smp_wmb();
	if (kvm_write_guest(kvm, gfn_to_gpa(gfn), &hv->tsc_ref, sizeof(hv->tsc_ref)))
		goto out_unlock;

	/*
	 * Now switch to the TSC page mechanism by writing the sequence.
	 */
	tsc_seq++;
	if (tsc_seq == 0xFFFFFFFF || tsc_seq == 0)
		tsc_seq = 1;

	/* Write the struct entirely before the non-zero sequence.  */
	smp_wmb();

	hv->tsc_ref.tsc_sequence = tsc_seq;
	kvm_write_guest(kvm, gfn_to_gpa(gfn),
			&hv->tsc_ref, sizeof(hv->tsc_ref.tsc_sequence));
out_unlock:
	mutex_unlock(&kvm->arch.hyperv.hv_lock);
}

static int kvm_hv_set_msr_pw(struct kvm_vcpu *vcpu, u32 msr, u64 data,
			     bool host)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_hv *hv = &kvm->arch.hyperv;

	switch (msr) {
	case HV_X64_MSR_GUEST_OS_ID:
		hv->hv_guest_os_id = data;
		/* setting guest os id to zero disables hypercall page */
		if (!hv->hv_guest_os_id)
			hv->hv_hypercall &= ~HV_X64_MSR_HYPERCALL_ENABLE;
		break;
	case HV_X64_MSR_HYPERCALL: {
		u64 gfn;
		unsigned long addr;
		u8 instructions[4];

		/* if guest os id is not set hypercall should remain disabled */
		if (!hv->hv_guest_os_id)
			break;
		if (!(data & HV_X64_MSR_HYPERCALL_ENABLE)) {
			hv->hv_hypercall = data;
			break;
		}
		gfn = data >> HV_X64_MSR_HYPERCALL_PAGE_ADDRESS_SHIFT;
		addr = gfn_to_hva(kvm, gfn);
		if (kvm_is_error_hva(addr))
			return 1;
		kvm_x86_ops->patch_hypercall(vcpu, instructions);
		((unsigned char *)instructions)[3] = 0xc3; /* ret */
		if (__copy_to_user((void __user *)addr, instructions, 4))
			return 1;
		hv->hv_hypercall = data;
		mark_page_dirty(kvm, gfn);
		break;
	}
	case HV_X64_MSR_REFERENCE_TSC:
		hv->hv_tsc_page = data;
		if (hv->hv_tsc_page & HV_X64_MSR_TSC_REFERENCE_ENABLE)
			kvm_make_request(KVM_REQ_MASTERCLOCK_UPDATE, vcpu);
		break;
	case HV_X64_MSR_CRASH_P0 ... HV_X64_MSR_CRASH_P4:
		return kvm_hv_msr_set_crash_data(vcpu,
						 msr - HV_X64_MSR_CRASH_P0,
						 data);
	case HV_X64_MSR_CRASH_CTL:
		return kvm_hv_msr_set_crash_ctl(vcpu, data, host);
	case HV_X64_MSR_RESET:
		if (data == 1) {
			vcpu_debug(vcpu, "hyper-v reset requested\n");
			kvm_make_request(KVM_REQ_HV_RESET, vcpu);
		}
		break;
	case HV_X64_MSR_REENLIGHTENMENT_CONTROL:
		hv->hv_reenlightenment_control = data;
		break;
	case HV_X64_MSR_TSC_EMULATION_CONTROL:
		hv->hv_tsc_emulation_control = data;
		break;
	case HV_X64_MSR_TSC_EMULATION_STATUS:
		hv->hv_tsc_emulation_status = data;
		break;
	case HV_X64_MSR_TIME_REF_COUNT:
		/* read-only, but still ignore it if host-initiated */
		if (!host)
			return 1;
		break;
	default:
		vcpu_unimpl(vcpu, "Hyper-V uhandled wrmsr: 0x%x data 0x%llx\n",
			    msr, data);
		return 1;
	}
	return 0;
}

/* Calculate cpu time spent by current task in 100ns units */
static u64 current_task_runtime_100ns(void)
{
	u64 utime, stime;

	task_cputime_adjusted(current, &utime, &stime);

	return div_u64(utime + stime, 100);
}

static int kvm_hv_set_msr(struct kvm_vcpu *vcpu, u32 msr, u64 data, bool host)
{
	struct kvm_vcpu_hv *hv_vcpu = &vcpu->arch.hyperv;

	switch (msr) {
	case HV_X64_MSR_VP_INDEX: {
		struct kvm_hv *hv = &vcpu->kvm->arch.hyperv;
		int vcpu_idx = kvm_vcpu_get_idx(vcpu);
		u32 new_vp_index = (u32)data;

		if (!host || new_vp_index >= KVM_MAX_VCPUS)
			return 1;

		if (new_vp_index == hv_vcpu->vp_index)
			return 0;

		/*
		 * The VP index is initialized to vcpu_index by
		 * kvm_hv_vcpu_postcreate so they initially match.  Now the
		 * VP index is changing, adjust num_mismatched_vp_indexes if
		 * it now matches or no longer matches vcpu_idx.
		 */
		if (hv_vcpu->vp_index == vcpu_idx)
			atomic_inc(&hv->num_mismatched_vp_indexes);
		else if (new_vp_index == vcpu_idx)
			atomic_dec(&hv->num_mismatched_vp_indexes);

		hv_vcpu->vp_index = new_vp_index;
		break;
	}
	case HV_X64_MSR_VP_ASSIST_PAGE: {
		u64 gfn;
		unsigned long addr;

		if (!(data & HV_X64_MSR_VP_ASSIST_PAGE_ENABLE)) {
			hv_vcpu->hv_vapic = data;
			if (kvm_lapic_enable_pv_eoi(vcpu, 0, 0))
				return 1;
			break;
		}
		gfn = data >> HV_X64_MSR_VP_ASSIST_PAGE_ADDRESS_SHIFT;
		addr = kvm_vcpu_gfn_to_hva(vcpu, gfn);
		if (kvm_is_error_hva(addr))
			return 1;
		if (__clear_user((void __user *)addr, PAGE_SIZE))
			return 1;
		hv_vcpu->hv_vapic = data;
		kvm_vcpu_mark_page_dirty(vcpu, gfn);
		if (kvm_lapic_enable_pv_eoi(vcpu,
					    gfn_to_gpa(gfn) | KVM_MSR_ENABLED,
					    sizeof(struct hv_vp_assist_page)))
			return 1;
		break;
	}
	case HV_X64_MSR_EOI:
		return kvm_hv_vapic_msr_write(vcpu, APIC_EOI, data);
	case HV_X64_MSR_ICR:
		return kvm_hv_vapic_msr_write(vcpu, APIC_ICR, data);
	case HV_X64_MSR_TPR:
		return kvm_hv_vapic_msr_write(vcpu, APIC_TASKPRI, data);
	case HV_X64_MSR_VP_RUNTIME:
		if (!host)
			return 1;
		hv_vcpu->runtime_offset = data - current_task_runtime_100ns();
		break;
	case HV_X64_MSR_SCONTROL:
	case HV_X64_MSR_SVERSION:
	case HV_X64_MSR_SIEFP:
	case HV_X64_MSR_SIMP:
	case HV_X64_MSR_EOM:
	case HV_X64_MSR_SINT0 ... HV_X64_MSR_SINT15:
		return synic_set_msr(vcpu_to_synic(vcpu), msr, data, host);
	case HV_X64_MSR_STIMER0_CONFIG:
	case HV_X64_MSR_STIMER1_CONFIG:
	case HV_X64_MSR_STIMER2_CONFIG:
	case HV_X64_MSR_STIMER3_CONFIG: {
		int timer_index = (msr - HV_X64_MSR_STIMER0_CONFIG)/2;

		return stimer_set_config(vcpu_to_stimer(vcpu, timer_index),
					 data, host);
	}
	case HV_X64_MSR_STIMER0_COUNT:
	case HV_X64_MSR_STIMER1_COUNT:
	case HV_X64_MSR_STIMER2_COUNT:
	case HV_X64_MSR_STIMER3_COUNT: {
		int timer_index = (msr - HV_X64_MSR_STIMER0_COUNT)/2;

		return stimer_set_count(vcpu_to_stimer(vcpu, timer_index),
					data, host);
	}
	case HV_X64_MSR_TSC_FREQUENCY:
	case HV_X64_MSR_APIC_FREQUENCY:
		/* read-only, but still ignore it if host-initiated */
		if (!host)
			return 1;
		break;
	default:
		vcpu_unimpl(vcpu, "Hyper-V uhandled wrmsr: 0x%x data 0x%llx\n",
			    msr, data);
		return 1;
	}

	return 0;
}

static int kvm_hv_get_msr_pw(struct kvm_vcpu *vcpu, u32 msr, u64 *pdata)
{
	u64 data = 0;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_hv *hv = &kvm->arch.hyperv;

	switch (msr) {
	case HV_X64_MSR_GUEST_OS_ID:
		data = hv->hv_guest_os_id;
		break;
	case HV_X64_MSR_HYPERCALL:
		data = hv->hv_hypercall;
		break;
	case HV_X64_MSR_TIME_REF_COUNT:
		data = get_time_ref_counter(kvm);
		break;
	case HV_X64_MSR_REFERENCE_TSC:
		data = hv->hv_tsc_page;
		break;
	case HV_X64_MSR_CRASH_P0 ... HV_X64_MSR_CRASH_P4:
		return kvm_hv_msr_get_crash_data(vcpu,
						 msr - HV_X64_MSR_CRASH_P0,
						 pdata);
	case HV_X64_MSR_CRASH_CTL:
		return kvm_hv_msr_get_crash_ctl(vcpu, pdata);
	case HV_X64_MSR_RESET:
		data = 0;
		break;
	case HV_X64_MSR_REENLIGHTENMENT_CONTROL:
		data = hv->hv_reenlightenment_control;
		break;
	case HV_X64_MSR_TSC_EMULATION_CONTROL:
		data = hv->hv_tsc_emulation_control;
		break;
	case HV_X64_MSR_TSC_EMULATION_STATUS:
		data = hv->hv_tsc_emulation_status;
		break;
	default:
		vcpu_unimpl(vcpu, "Hyper-V unhandled rdmsr: 0x%x\n", msr);
		return 1;
	}

	*pdata = data;
	return 0;
}

static int kvm_hv_get_msr(struct kvm_vcpu *vcpu, u32 msr, u64 *pdata,
			  bool host)
{
	u64 data = 0;
	struct kvm_vcpu_hv *hv_vcpu = &vcpu->arch.hyperv;

	switch (msr) {
	case HV_X64_MSR_VP_INDEX:
		data = hv_vcpu->vp_index;
		break;
	case HV_X64_MSR_EOI:
		return kvm_hv_vapic_msr_read(vcpu, APIC_EOI, pdata);
	case HV_X64_MSR_ICR:
		return kvm_hv_vapic_msr_read(vcpu, APIC_ICR, pdata);
	case HV_X64_MSR_TPR:
		return kvm_hv_vapic_msr_read(vcpu, APIC_TASKPRI, pdata);
	case HV_X64_MSR_VP_ASSIST_PAGE:
		data = hv_vcpu->hv_vapic;
		break;
	case HV_X64_MSR_VP_RUNTIME:
		data = current_task_runtime_100ns() + hv_vcpu->runtime_offset;
		break;
	case HV_X64_MSR_SCONTROL:
	case HV_X64_MSR_SVERSION:
	case HV_X64_MSR_SIEFP:
	case HV_X64_MSR_SIMP:
	case HV_X64_MSR_EOM:
	case HV_X64_MSR_SINT0 ... HV_X64_MSR_SINT15:
		return synic_get_msr(vcpu_to_synic(vcpu), msr, pdata, host);
	case HV_X64_MSR_STIMER0_CONFIG:
	case HV_X64_MSR_STIMER1_CONFIG:
	case HV_X64_MSR_STIMER2_CONFIG:
	case HV_X64_MSR_STIMER3_CONFIG: {
		int timer_index = (msr - HV_X64_MSR_STIMER0_CONFIG)/2;

		return stimer_get_config(vcpu_to_stimer(vcpu, timer_index),
					 pdata);
	}
	case HV_X64_MSR_STIMER0_COUNT:
	case HV_X64_MSR_STIMER1_COUNT:
	case HV_X64_MSR_STIMER2_COUNT:
	case HV_X64_MSR_STIMER3_COUNT: {
		int timer_index = (msr - HV_X64_MSR_STIMER0_COUNT)/2;

		return stimer_get_count(vcpu_to_stimer(vcpu, timer_index),
					pdata);
	}
	case HV_X64_MSR_TSC_FREQUENCY:
		data = (u64)vcpu->arch.virtual_tsc_khz * 1000;
		break;
	case HV_X64_MSR_APIC_FREQUENCY:
		data = APIC_BUS_FREQUENCY;
		break;
	default:
		vcpu_unimpl(vcpu, "Hyper-V unhandled rdmsr: 0x%x\n", msr);
		return 1;
	}
	*pdata = data;
	return 0;
}

int kvm_hv_set_msr_common(struct kvm_vcpu *vcpu, u32 msr, u64 data, bool host)
{
	if (kvm_hv_msr_partition_wide(msr)) {
		int r;

		mutex_lock(&vcpu->kvm->arch.hyperv.hv_lock);
		r = kvm_hv_set_msr_pw(vcpu, msr, data, host);
		mutex_unlock(&vcpu->kvm->arch.hyperv.hv_lock);
		return r;
	} else
		return kvm_hv_set_msr(vcpu, msr, data, host);
}

int kvm_hv_get_msr_common(struct kvm_vcpu *vcpu, u32 msr, u64 *pdata, bool host)
{
	if (kvm_hv_msr_partition_wide(msr)) {
		int r;

		mutex_lock(&vcpu->kvm->arch.hyperv.hv_lock);
		r = kvm_hv_get_msr_pw(vcpu, msr, pdata);
		mutex_unlock(&vcpu->kvm->arch.hyperv.hv_lock);
		return r;
	} else
		return kvm_hv_get_msr(vcpu, msr, pdata, host);
}

static __always_inline int get_sparse_bank_no(u64 valid_bank_mask, int bank_no)
{
	int i = 0, j;

	if (!(valid_bank_mask & BIT_ULL(bank_no)))
		return -1;

	for (j = 0; j < bank_no; j++)
		if (valid_bank_mask & BIT_ULL(j))
			i++;

	return i;
}

static u64 kvm_hv_flush_tlb(struct kvm_vcpu *current_vcpu, u64 ingpa,
			    u16 rep_cnt, bool ex)
{
	struct kvm *kvm = current_vcpu->kvm;
	struct kvm_vcpu_hv *hv_current = &current_vcpu->arch.hyperv;
	struct hv_tlb_flush_ex flush_ex;
	struct hv_tlb_flush flush;
	struct kvm_vcpu *vcpu;
	unsigned long vcpu_bitmap[BITS_TO_LONGS(KVM_MAX_VCPUS)] = {0};
	unsigned long valid_bank_mask = 0;
	u64 sparse_banks[64];
	int sparse_banks_len, i;
	bool all_cpus;

	if (!ex) {
		if (unlikely(kvm_read_guest(kvm, ingpa, &flush, sizeof(flush))))
			return HV_STATUS_INVALID_HYPERCALL_INPUT;

		trace_kvm_hv_flush_tlb(flush.processor_mask,
				       flush.address_space, flush.flags);

		sparse_banks[0] = flush.processor_mask;

		/*
		 * Work around possible WS2012 bug: it sends hypercalls
		 * with processor_mask = 0x0 and HV_FLUSH_ALL_PROCESSORS clear,
		 * while also expecting us to flush something and crashing if
		 * we don't. Let's treat processor_mask == 0 same as
		 * HV_FLUSH_ALL_PROCESSORS.
		 */
		all_cpus = (flush.flags & HV_FLUSH_ALL_PROCESSORS) ||
			flush.processor_mask == 0;
	} else {
		if (unlikely(kvm_read_guest(kvm, ingpa, &flush_ex,
					    sizeof(flush_ex))))
			return HV_STATUS_INVALID_HYPERCALL_INPUT;

		trace_kvm_hv_flush_tlb_ex(flush_ex.hv_vp_set.valid_bank_mask,
					  flush_ex.hv_vp_set.format,
					  flush_ex.address_space,
					  flush_ex.flags);

		valid_bank_mask = flush_ex.hv_vp_set.valid_bank_mask;
		all_cpus = flush_ex.hv_vp_set.format !=
			HV_GENERIC_SET_SPARSE_4K;

		sparse_banks_len = bitmap_weight(&valid_bank_mask, 64) *
			sizeof(sparse_banks[0]);

		if (!sparse_banks_len && !all_cpus)
			goto ret_success;

		if (!all_cpus &&
		    kvm_read_guest(kvm,
				   ingpa + offsetof(struct hv_tlb_flush_ex,
						    hv_vp_set.bank_contents),
				   sparse_banks,
				   sparse_banks_len))
			return HV_STATUS_INVALID_HYPERCALL_INPUT;
	}

	cpumask_clear(&hv_current->tlb_lush);

	kvm_for_each_vcpu(i, vcpu, kvm) {
		struct kvm_vcpu_hv *hv = &vcpu->arch.hyperv;
		int bank = hv->vp_index / 64, sbank = 0;

		if (!all_cpus) {
			/* Banks >64 can't be represented */
			if (bank >= 64)
				continue;

			/* Non-ex hypercalls can only address first 64 vCPUs */
			if (!ex && bank)
				continue;

			if (ex) {
				/*
				 * Check is the bank of this vCPU is in sparse
				 * set and get the sparse bank number.
				 */
				sbank = get_sparse_bank_no(valid_bank_mask,
							   bank);

				if (sbank < 0)
					continue;
			}

			if (!(sparse_banks[sbank] & BIT_ULL(hv->vp_index % 64)))
				continue;
		}

		/*
		 * vcpu->arch.cr3 may not be up-to-date for running vCPUs so we
		 * can't analyze it here, flush TLB regardless of the specified
		 * address space.
		 */
		__set_bit(i, vcpu_bitmap);
	}

	kvm_make_vcpus_request_mask(kvm,
				    KVM_REQ_TLB_FLUSH | KVM_REQUEST_NO_WAKEUP,
				    vcpu_bitmap, &hv_current->tlb_lush);

ret_success:
	/* We always do full TLB flush, set rep_done = rep_cnt. */
	return (u64)HV_STATUS_SUCCESS |
		((u64)rep_cnt << HV_HYPERCALL_REP_COMP_OFFSET);
}

bool kvm_hv_hypercall_enabled(struct kvm *kvm)
{
	return READ_ONCE(kvm->arch.hyperv.hv_hypercall) & HV_X64_MSR_HYPERCALL_ENABLE;
}

static void kvm_hv_hypercall_set_result(struct kvm_vcpu *vcpu, u64 result)
{
	bool longmode;

	longmode = is_64_bit_mode(vcpu);
	if (longmode)
		kvm_register_write(vcpu, VCPU_REGS_RAX, result);
	else {
		kvm_register_write(vcpu, VCPU_REGS_RDX, result >> 32);
		kvm_register_write(vcpu, VCPU_REGS_RAX, result & 0xffffffff);
	}
}

static int kvm_hv_hypercall_complete(struct kvm_vcpu *vcpu, u64 result)
{
	kvm_hv_hypercall_set_result(vcpu, result);
	++vcpu->stat.hypercalls;
	return kvm_skip_emulated_instruction(vcpu);
}

static int kvm_hv_hypercall_complete_userspace(struct kvm_vcpu *vcpu)
{
	return kvm_hv_hypercall_complete(vcpu, vcpu->run->hyperv.u.hcall.result);
}

static u16 kvm_hvcall_signal_event(struct kvm_vcpu *vcpu, bool fast, u64 param)
{
	struct eventfd_ctx *eventfd;

	if (unlikely(!fast)) {
		int ret;
		gpa_t gpa = param;

		if ((gpa & (__alignof__(param) - 1)) ||
		    offset_in_page(gpa) + sizeof(param) > PAGE_SIZE)
			return HV_STATUS_INVALID_ALIGNMENT;

		ret = kvm_vcpu_read_guest(vcpu, gpa, &param, sizeof(param));
		if (ret < 0)
			return HV_STATUS_INVALID_ALIGNMENT;
	}

	/*
	 * Per spec, bits 32-47 contain the extra "flag number".  However, we
	 * have no use for it, and in all known usecases it is zero, so just
	 * report lookup failure if it isn't.
	 */
	if (param & 0xffff00000000ULL)
		return HV_STATUS_INVALID_PORT_ID;
	/* remaining bits are reserved-zero */
	if (param & ~KVM_HYPERV_CONN_ID_MASK)
		return HV_STATUS_INVALID_HYPERCALL_INPUT;

	/* the eventfd is protected by vcpu->kvm->srcu, but conn_to_evt isn't */
	rcu_read_lock();
	eventfd = idr_find(&vcpu->kvm->arch.hyperv.conn_to_evt, param);
	rcu_read_unlock();
	if (!eventfd)
		return HV_STATUS_INVALID_PORT_ID;

	eventfd_signal(eventfd, 1);
	return HV_STATUS_SUCCESS;
}

int kvm_hv_hypercall(struct kvm_vcpu *vcpu)
{
	u64 param, ingpa, outgpa, ret = HV_STATUS_SUCCESS;
	uint16_t code, rep_idx, rep_cnt;
	bool fast, longmode, rep;

	/*
	 * hypercall generates UD from non zero cpl and real mode
	 * per HYPER-V spec
	 */
	if (kvm_x86_ops->get_cpl(vcpu) != 0 || !is_protmode(vcpu)) {
		kvm_queue_exception(vcpu, UD_VECTOR);
		return 1;
	}

	longmode = is_64_bit_mode(vcpu);

	if (!longmode) {
		param = ((u64)kvm_register_read(vcpu, VCPU_REGS_RDX) << 32) |
			(kvm_register_read(vcpu, VCPU_REGS_RAX) & 0xffffffff);
		ingpa = ((u64)kvm_register_read(vcpu, VCPU_REGS_RBX) << 32) |
			(kvm_register_read(vcpu, VCPU_REGS_RCX) & 0xffffffff);
		outgpa = ((u64)kvm_register_read(vcpu, VCPU_REGS_RDI) << 32) |
			(kvm_register_read(vcpu, VCPU_REGS_RSI) & 0xffffffff);
	}
#ifdef CONFIG_X86_64
	else {
		param = kvm_register_read(vcpu, VCPU_REGS_RCX);
		ingpa = kvm_register_read(vcpu, VCPU_REGS_RDX);
		outgpa = kvm_register_read(vcpu, VCPU_REGS_R8);
	}
#endif

	code = param & 0xffff;
	fast = !!(param & HV_HYPERCALL_FAST_BIT);
	rep_cnt = (param >> HV_HYPERCALL_REP_COMP_OFFSET) & 0xfff;
	rep_idx = (param >> HV_HYPERCALL_REP_START_OFFSET) & 0xfff;
	rep = !!(rep_cnt || rep_idx);

	trace_kvm_hv_hypercall(code, fast, rep_cnt, rep_idx, ingpa, outgpa);

	switch (code) {
	case HVCALL_NOTIFY_LONG_SPIN_WAIT:
		if (unlikely(rep)) {
			ret = HV_STATUS_INVALID_HYPERCALL_INPUT;
			break;
		}
		kvm_vcpu_on_spin(vcpu, true);
		break;
	case HVCALL_SIGNAL_EVENT:
		if (unlikely(rep)) {
			ret = HV_STATUS_INVALID_HYPERCALL_INPUT;
			break;
		}
		ret = kvm_hvcall_signal_event(vcpu, fast, ingpa);
		if (ret != HV_STATUS_INVALID_PORT_ID)
			break;
		/* maybe userspace knows this conn_id: fall through */
	case HVCALL_POST_MESSAGE:
		/* don't bother userspace if it has no way to handle it */
		if (unlikely(rep || !vcpu_to_synic(vcpu)->active)) {
			ret = HV_STATUS_INVALID_HYPERCALL_INPUT;
			break;
		}
		vcpu->run->exit_reason = KVM_EXIT_HYPERV;
		vcpu->run->hyperv.type = KVM_EXIT_HYPERV_HCALL;
		vcpu->run->hyperv.u.hcall.input = param;
		vcpu->run->hyperv.u.hcall.params[0] = ingpa;
		vcpu->run->hyperv.u.hcall.params[1] = outgpa;
		vcpu->arch.complete_userspace_io =
				kvm_hv_hypercall_complete_userspace;
		return 0;
	case HVCALL_FLUSH_VIRTUAL_ADDRESS_LIST:
		if (unlikely(fast || !rep_cnt || rep_idx)) {
			ret = HV_STATUS_INVALID_HYPERCALL_INPUT;
			break;
		}
		ret = kvm_hv_flush_tlb(vcpu, ingpa, rep_cnt, false);
		break;
	case HVCALL_FLUSH_VIRTUAL_ADDRESS_SPACE:
		if (unlikely(fast || rep)) {
			ret = HV_STATUS_INVALID_HYPERCALL_INPUT;
			break;
		}
		ret = kvm_hv_flush_tlb(vcpu, ingpa, rep_cnt, false);
		break;
	case HVCALL_FLUSH_VIRTUAL_ADDRESS_LIST_EX:
		if (unlikely(fast || !rep_cnt || rep_idx)) {
			ret = HV_STATUS_INVALID_HYPERCALL_INPUT;
			break;
		}
		ret = kvm_hv_flush_tlb(vcpu, ingpa, rep_cnt, true);
		break;
	case HVCALL_FLUSH_VIRTUAL_ADDRESS_SPACE_EX:
		if (unlikely(fast || rep)) {
			ret = HV_STATUS_INVALID_HYPERCALL_INPUT;
			break;
		}
		ret = kvm_hv_flush_tlb(vcpu, ingpa, rep_cnt, true);
		break;
	default:
		ret = HV_STATUS_INVALID_HYPERCALL_CODE;
		break;
	}

	return kvm_hv_hypercall_complete(vcpu, ret);
}

void kvm_hv_init_vm(struct kvm *kvm)
{
	mutex_init(&kvm->arch.hyperv.hv_lock);
	idr_init(&kvm->arch.hyperv.conn_to_evt);
}

void kvm_hv_destroy_vm(struct kvm *kvm)
{
	struct eventfd_ctx *eventfd;
	int i;

	idr_for_each_entry(&kvm->arch.hyperv.conn_to_evt, eventfd, i)
		eventfd_ctx_put(eventfd);
	idr_destroy(&kvm->arch.hyperv.conn_to_evt);
}

static int kvm_hv_eventfd_assign(struct kvm *kvm, u32 conn_id, int fd)
{
	struct kvm_hv *hv = &kvm->arch.hyperv;
	struct eventfd_ctx *eventfd;
	int ret;

	eventfd = eventfd_ctx_fdget(fd);
	if (IS_ERR(eventfd))
		return PTR_ERR(eventfd);

	mutex_lock(&hv->hv_lock);
	ret = idr_alloc(&hv->conn_to_evt, eventfd, conn_id, conn_id + 1,
			GFP_KERNEL);
	mutex_unlock(&hv->hv_lock);

	if (ret >= 0)
		return 0;

	if (ret == -ENOSPC)
		ret = -EEXIST;
	eventfd_ctx_put(eventfd);
	return ret;
}

static int kvm_hv_eventfd_deassign(struct kvm *kvm, u32 conn_id)
{
	struct kvm_hv *hv = &kvm->arch.hyperv;
	struct eventfd_ctx *eventfd;

	mutex_lock(&hv->hv_lock);
	eventfd = idr_remove(&hv->conn_to_evt, conn_id);
	mutex_unlock(&hv->hv_lock);

	if (!eventfd)
		return -ENOENT;

	synchronize_srcu(&kvm->srcu);
	eventfd_ctx_put(eventfd);
	return 0;
}

int kvm_vm_ioctl_hv_eventfd(struct kvm *kvm, struct kvm_hyperv_eventfd *args)
{
	if ((args->flags & ~KVM_HYPERV_EVENTFD_DEASSIGN) ||
	    (args->conn_id & ~KVM_HYPERV_CONN_ID_MASK))
		return -EINVAL;

	if (args->flags == KVM_HYPERV_EVENTFD_DEASSIGN)
		return kvm_hv_eventfd_deassign(kvm, args->conn_id);
	return kvm_hv_eventfd_assign(kvm, args->conn_id, args->fd);
}
