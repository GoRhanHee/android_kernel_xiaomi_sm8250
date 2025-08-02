/*
 * linux/drivers/cpufreq/cpufreq.c
 *
 * Copyright (C) 2001 Russell King
 * (C) 2002 - 2003 Dominik Brodowski <linux@brodo.de>
 * (C) 2013 Viresh Kumar <viresh.kumar@linaro.org>
 *
 * Oct 2005 - Ashok Raj <ashok.raj@intel.com>
 *	Added handling for CPU hotplug
 * Feb 2006 - Jacob Shin <jacob.shin@amd.com>
 *	Fix handling for CPU hotplug -- affected CPUs
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpufreq_times.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/tick.h>
#include <linux/sched/topology.h>
#include <linux/sched/sysctl.h>

#include <trace/events/power.h>

static LIST_HEAD(cpufreq_policy_list);

static inline bool policy_is_inactive(struct cpufreq_policy *policy)
{
	return cpumask_empty(policy->cpus);
}

/* Macros to iterate over CPU policies */
#define for_each_suitable_policy(__policy, __active)			 \
	list_for_each_entry(__policy, &cpufreq_policy_list, policy_list) \
		if ((__active) == !policy_is_inactive(__policy))

#define for_each_active_policy(__policy)		\
	for_each_suitable_policy(__policy, true)
#define for_each_inactive_policy(__policy)		\
	for_each_suitable_policy(__policy, false)

#define for_each_policy(__policy)			\
	list_for_each_entry(__policy, &cpufreq_policy_list, policy_list)

/* Iterate over governors */
static LIST_HEAD(cpufreq_governor_list);
#define for_each_governor(__governor)				\
	list_for_each_entry(__governor, &cpufreq_governor_list, governor_list)

/**
 * The "cpufreq driver" - the arch- or hardware-dependent low
 * level driver of CPUFreq support, and its spinlock. This lock
 * also protects the cpufreq_cpu_data array.
 */
static struct cpufreq_driver *cpufreq_driver;
static DEFINE_PER_CPU(struct cpufreq_policy *, cpufreq_cpu_data);
static DEFINE_RWLOCK(cpufreq_driver_lock);

/* Flag to suspend/resume CPUFreq governors */
static bool cpufreq_suspended;

static inline bool has_target(void)
{
	return cpufreq_driver->target_index || cpufreq_driver->target;
}

/* internal prototypes */
static unsigned int __cpufreq_get(struct cpufreq_policy *policy);
static int cpufreq_init_governor(struct cpufreq_policy *policy);
static void cpufreq_exit_governor(struct cpufreq_policy *policy);
static int cpufreq_start_governor(struct cpufreq_policy *policy);
static void cpufreq_stop_governor(struct cpufreq_policy *policy);
static void cpufreq_governor_limits(struct cpufreq_policy *policy);

/**
 * Two notifier lists: the "policy" list is involved in the
 * validation process for a new CPU frequency policy; the
 * "transition" list for kernel code that needs to handle
 * changes to devices when the CPU clock speed changes.
 * The mutex locks both lists.
 */
static BLOCKING_NOTIFIER_HEAD(cpufreq_policy_notifier_list);
SRCU_NOTIFIER_HEAD_STATIC(cpufreq_transition_notifier_list);

static int off __read_mostly;
static int cpufreq_disabled(void)
{
	return off;
}
void disable_cpufreq(void)
{
	off = 1;
}
static DEFINE_MUTEX(cpufreq_governor_mutex);

bool have_governor_per_policy(void)
{
	return !!(cpufreq_driver->flags & CPUFREQ_HAVE_GOVERNOR_PER_POLICY);
}
EXPORT_SYMBOL_GPL(have_governor_per_policy);

struct kobject *get_governor_parent_kobj(struct cpufreq_policy *policy)
{
	if (have_governor_per_policy())
		return &policy->kobj;
	else
		return cpufreq_global_kobject;
}
EXPORT_SYMBOL_GPL(get_governor_parent_kobj);

static inline u64 get_cpu_idle_time_jiffy(unsigned int cpu, u64 *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_nsecs(get_jiffies_64());

	busy_time = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = cur_wall_time - busy_time;
	if (wall)
		*wall = div_u64(cur_wall_time, NSEC_PER_USEC);

	return div_u64(idle_time, NSEC_PER_USEC);
}

u64 get_cpu_idle_time(unsigned int cpu, u64 *wall, int io_busy)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, io_busy ? wall : NULL);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);
	else if (!io_busy)
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}
EXPORT_SYMBOL_GPL(get_cpu_idle_time);

__weak void arch_set_freq_scale(struct cpumask *cpus, unsigned long cur_freq,
		unsigned long max_freq)
{
}
EXPORT_SYMBOL_GPL(arch_set_freq_scale);

__weak void arch_set_max_freq_scale(struct cpumask *cpus,
				    unsigned long policy_max_freq)
{
}
EXPORT_SYMBOL_GPL(arch_set_max_freq_scale);

/*
 * This is a generic cpufreq init() routine which can be used by cpufreq
 * drivers of SMP systems. It will do following:
 * - validate & show freq table passed
 * - set policies transition latency
 * - policy->cpus with all possible CPUs
 */
int cpufreq_generic_init(struct cpufreq_policy *policy,
		struct cpufreq_frequency_table *table,
		unsigned int transition_latency)
{
	policy->freq_table = table;
	policy->cpuinfo.transition_latency = transition_latency;

	/*
	 * The driver only supports the SMP configuration where all processors
	 * share the clock and voltage and clock.
	 */
	cpumask_setall(policy->cpus);

	return 0;
}
EXPORT_SYMBOL_GPL(cpufreq_generic_init);

struct cpufreq_policy *cpufreq_cpu_get_raw(unsigned int cpu)
{
	struct cpufreq_policy *policy = per_cpu(cpufreq_cpu_data, cpu);

	return policy && cpumask_test_cpu(cpu, policy->cpus) ? policy : NULL;
}
EXPORT_SYMBOL_GPL(cpufreq_cpu_get_raw);

unsigned int cpufreq_generic_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	if (!policy || IS_ERR(policy->clk)) {
		pr_err("%s: No %s associated to cpu: %d\n",
		       __func__, policy ? "clk" : "policy", cpu);
		return 0;
	}

	return clk_get_rate(policy->clk) / 1000;
}
EXPORT_SYMBOL_GPL(cpufreq_generic_get);

/**
 * cpufreq_cpu_get: returns policy for a cpu and marks it busy.
 *
 * @cpu: cpu to find policy for.
 *
 * This returns policy for 'cpu', returns NULL if it doesn't exist.
 * It also increments the kobject reference count to mark it busy and so would
 * require a corresponding call to cpufreq_cpu_put() to decrement it back.
 * If corresponding call cpufreq_cpu_put() isn't made, the policy wouldn't be
 * freed as that depends on the kobj count.
 *
 * Return: A valid policy on success, otherwise NULL on failure.
 */
struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = NULL;
	unsigned long flags;

	if (WARN_ON(cpu >= nr_cpu_ids))
		return NULL;

	/* get the cpufreq driver */
	read_lock_irqsave(&cpufreq_driver_lock, flags);

	if (cpufreq_driver) {
		/* get the CPU */
		policy = cpufreq_cpu_get_raw(cpu);
		if (policy)
			kobject_get(&policy->kobj);
	}

	read_unlock_irqrestore(&cpufreq_driver_lock, flags);

	return policy;
}
EXPORT_SYMBOL_GPL(cpufreq_cpu_get);

/**
 * cpufreq_cpu_put: Decrements the usage count of a policy
 *
 * @policy: policy earlier returned by cpufreq_cpu_get().
 *
 * This decrements the kobject reference count incremented earlier by calling
 * cpufreq_cpu_get().
 */
void cpufreq_cpu_put(struct cpufreq_policy *policy)
{
	kobject_put(&policy->kobj);
}
EXPORT_SYMBOL_GPL(cpufreq_cpu_put);

/*********************************************************************
 * EXTERNALLY AFFECTING FREQUENCY CHANGES                 *
 *********************************************************************/

/**
 * adjust_jiffies - adjust the system "loops_per_jiffy"
 *
 * This function alters the system "loops_per_jiffy" for the clock
 * speed change. Note that loops_per_jiffy cannot be updated on SMP
 * systems as each CPU might be scaled differently. So, use the arch
 * per-CPU loops_per_jiffy value wherever possible.
 */
static void adjust_jiffies(unsigned long val, struct cpufreq_freqs *ci)
{
#ifndef CONFIG_SMP
	static unsigned long l_p_j_ref;
	static unsigned int l_p_j_ref_freq;

	if (ci->flags & CPUFREQ_CONST_LOOPS)
		return;

	if (!l_p_j_ref_freq) {
		l_p_j_ref = loops_per_jiffy;
		l_p_j_ref_freq = ci->old;
		pr_debug("saving %lu as reference value for loops_per_jiffy; freq is %u kHz\n",
			 l_p_j_ref, l_p_j_ref_freq);
	}
	if (val == CPUFREQ_POSTCHANGE && ci->old != ci->new) {
		loops_per_jiffy = cpufreq_scale(l_p_j_ref, l_p_j_ref_freq,
								ci->new);
		pr_debug("scaling loops_per_jiffy to %lu for frequency %u kHz\n",
			 loops_per_jiffy, ci->new);
	}
#endif
}

/**
 * cpufreq_notify_transition - Notify frequency transition and adjust_jiffies.
 * @policy: cpufreq policy to enable fast frequency switching for.
 * @freqs: contain details of the frequency update.
 * @state: set to CPUFREQ_PRECHANGE or CPUFREQ_POSTCHANGE.
 *
 * This function calls the transition notifiers and the "adjust_jiffies"
 * function. It is called twice on all CPU frequency changes that have
 * external effects.
 */
static void cpufreq_notify_transition(struct cpufreq_policy *policy,
				      struct cpufreq_freqs *freqs,
				      unsigned int state)
{
	BUG_ON(irqs_disabled());

	if (cpufreq_disabled())
		return;

	freqs->flags = cpufreq_driver->flags;
	pr_debug("notification %u of frequency transition to %u kHz\n",
		 state, freqs->new);

	switch (state) {
	case CPUFREQ_PRECHANGE:
		/*
		 * Detect if the driver reported a value as "old frequency"
		 * which is not equal to what the cpufreq core thinks is
		 * "old frequency".
		 */
		if (!(cpufreq_driver->flags & CPUFREQ_CONST_LOOPS)) {
			if (policy->cur && (policy->cur != freqs->old)) {
				pr_debug("Warning: CPU frequency is %u, cpufreq assumed %u kHz\n",
					 freqs->old, policy->cur);
				freqs->old = policy->cur;
			}
		}

		for_each_cpu(freqs->cpu, policy->cpus) {
			srcu_notifier_call_chain(&cpufreq_transition_notifier_list,
						 CPUFREQ_PRECHANGE, freqs);
		}

		adjust_jiffies(CPUFREQ_PRECHANGE, freqs);
		break;

	case CPUFREQ_POSTCHANGE:
		adjust_jiffies(CPUFREQ_POSTCHANGE, freqs);
		pr_debug("FREQ: %u - CPUs: %*pbl\n", freqs->new,
			 cpumask_pr_args(policy->cpus));

		for_each_cpu(freqs->cpu, policy->cpus) {
			trace_cpu_frequency(freqs->new, freqs->cpu);
			srcu_notifier_call_chain(&cpufreq_transition_notifier_list,
						 CPUFREQ_POSTCHANGE, freqs);
		}

		cpufreq_stats_record_transition(policy, freqs->new);
		cpufreq_times_record_transition(policy, freqs->new);
		policy->cur = freqs->new;
	}
}

/* Do post notifications when there are chances that transition has failed */
static void cpufreq_notify_post_transition(struct cpufreq_policy *policy,
		struct cpufreq_freqs *freqs, int transition_failed)
{
	cpufreq_notify_transition(policy, freqs, CPUFREQ_POSTCHANGE);
	if (!transition_failed)
		return;

	swap(freqs->old, freqs->new);
	cpufreq_notify_transition(policy, freqs, CPUFREQ_PRECHANGE);
	cpufreq_notify_transition(policy, freqs, CPUFREQ_POSTCHANGE);
}

void cpufreq_freq_transition_begin(struct cpufreq_policy *policy,
		struct cpufreq_freqs *freqs)
{

	/*
	 * Catch double invocations of _begin() which lead to self-deadlock.
	 * ASYNC_NOTIFICATION drivers are left out because the cpufreq core
	 * doesn't invoke _begin() on their behalf, and hence the chances of
	 * double invocations are very low. Moreover, there are scenarios
	 * where these checks can emit false-positive warnings in these
	 * drivers; so we avoid that by skipping them altogether.
	 */
	WARN_ON(!(cpufreq_driver->flags & CPUFREQ_ASYNC_NOTIFICATION)
				&& current == policy->transition_task);

wait:
	wait_event(policy->transition_wait, !policy->transition_ongoing);

	spin_lock(&policy->transition_lock);

	if (unlikely(policy->transition_ongoing)) {
		spin_unlock(&policy->transition_lock);
		goto wait;
	}

	policy->transition_ongoing = true;
	policy->transition_task = current;

	spin_unlock(&policy->transition_lock);

	cpufreq_notify_transition(policy, freqs, CPUFREQ_PRECHANGE);
}
EXPORT_SYMBOL_GPL(cpufreq_freq_transition_begin);

void cpufreq_freq_transition_end(struct cpufreq_policy *policy,
		struct cpufreq_freqs *freqs, int transition_failed)
{
	if (unlikely(WARN_ON(!policy->transition_ongoing)))
		return;

	cpufreq_notify_post_transition(policy, freqs, transition_failed);

	policy->transition_ongoing = false;
	policy->transition_task = NULL;

	wake_up(&policy->transition_wait);
}
EXPORT_SYMBOL_GPL(cpufreq_freq_transition_end);

/*
 * Fast frequency switching status count.  Positive means "enabled", negative
 * means "disabled" and 0 means "not decided yet".
 */
static int cpufreq_fast_switch_count;
static DEFINE_MUTEX(cpufreq_fast_switch_lock);

static void cpufreq_list_transition_notifiers(void)
{
	struct notifier_block *nb;

	pr_info("Registered transition notifiers:\n");

	mutex_lock(&cpufreq_transition_notifier_list.mutex);

	for (nb = cpufreq_transition_notifier_list.head; nb; nb = nb->next)
		pr_info("%pF\n", nb->notifier_call);

	mutex_unlock(&cpufreq_transition_notifier_list.mutex);
}

/**
 * cpufreq_enable_fast_switch - Enable fast frequency switching for policy.
 * @policy: cpufreq policy to enable fast frequency switching for.
 *
 * Try to enable fast frequency switching for @policy.
 *
 * The attempt will fail if there is at least one transition notifier registered
 * at this point, as fast frequency switching is quite fundamentally at odds
 * with transition notifiers.  Thus if successful, it will make registration of
 * transition notifiers fail going forward.
 */
void cpufreq_enable_fast_switch(struct cpufreq_policy *policy)
{
	lockdep_assert_held(&policy->rwsem);

	// Badlav: Fast switch ko default roop se enable karne ke liye logic
	if (!policy->fast_switch_possible) {
		pr_info("CPU%u: Fast switch not possible, skipping.\n", policy->cpu);
		return;
	}

	mutex_lock(&cpufreq_fast_switch_lock);
	if (cpufreq_fast_switch_count >= 0) {
		cpufreq_fast_switch_count++;
		policy->fast_switch_enabled = true;
	} else {
		pr_warn("CPU%u: Fast frequency switching not enabled\n",
			policy->cpu);
		cpufreq_list_transition_notifiers();
	}
	mutex_unlock(&cpufreq_fast_switch_lock);
}
EXPORT_SYMBOL_GPL(cpufreq_enable_fast_switch);

/**
 * cpufreq_disable_fast_switch - Disable fast frequency switching for policy.
 * @policy: cpufreq policy to disable fast frequency switching for.
 */
void cpufreq_disable_fast_switch(struct cpufreq_policy *policy)
{
	mutex_lock(&cpufreq_fast_switch_lock);
	if (policy->fast_switch_enabled) {
		policy->fast_switch_enabled = false;
		if (!WARN_ON(cpufreq_fast_switch_count <= 0))
			cpufreq_fast_switch_count--;
	}
	mutex_unlock(&cpufreq_fast_switch_lock);
}
EXPORT_SYMBOL_GPL(cpufreq_disable_fast_switch);

/**
 * cpufreq_driver_resolve_freq - Map a target frequency to a driver-supported
 * one.
 * @target_freq: target frequency to resolve.
 *
 * The target to driver frequency mapping is cached in the policy.
 *
 * Return: Lowest driver-supported frequency greater than or equal to the
 * given target_freq, subject to policy (min/max) and driver limitations.
 */
unsigned int cpufreq_driver_resolve_freq(struct cpufreq_policy *policy,
					 unsigned int target_freq)
{
	// Badlav: Resolved frequency ko dynamic tarike se thermal ya power states ke anusaar adjust karna.
	unsigned int resolved_freq = target_freq;
	resolved_freq = clamp_val(resolved_freq, policy->min, policy->max);

	// Yahan aap custom thermal ya power-aware logic jodh sakte hain
	// Example:
	// if (is_device_overheating()) {
	//    resolved_freq = min(resolved_freq, get_thermal_limit_freq());
	// }

	policy->cached_target_freq = resolved_freq;

	if (cpufreq_driver->target_index) {
		int idx;

		idx = cpufreq_frequency_table_target(policy, resolved_freq,
						     CPUFREQ_RELATION_L);
		policy->cached_resolved_idx = idx;
		return policy->freq_table[idx].frequency;
	}

	if (cpufreq_driver->resolve_freq)
		return cpufreq_driver->resolve_freq(policy, resolved_freq);

	return resolved_freq;
}
EXPORT_SYMBOL_GPL(cpufreq_driver_resolve_freq);

unsigned int cpufreq_policy_transition_delay_us(struct cpufreq_policy *policy)
{
	unsigned int latency;

	if (policy->transition_delay_us)
		return policy->transition_delay_us;

	latency = policy->cpuinfo.transition_latency / NSEC_PER_USEC;
	if (latency) {
		/*
		 * For platforms that can change the frequency very fast (< 10
		 * us), the above formula gives a decent transition delay. But
		 * for platforms where transition_latency is in milliseconds, it
		 * ends up giving unrealistic values.
		 *
		 * Cap the default transition delay to 10 ms, which seems to be
		 * a reasonable amount of time after which we should reevaluate
		 * the frequency.
		 */
		return min(latency * LATENCY_MULTIPLIER, (unsigned int)10000);
	}

	return LATENCY_MULTIPLIER;
}
EXPORT_SYMBOL_GPL(cpufreq_policy_transition_delay_us);

/*********************************************************************
 * SYSFS INTERFACE                          *
 *********************************************************************/
static ssize_t show_boost(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", cpufreq_driver->boost_enabled);
}

static ssize_t store_boost(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	int ret, enable;

	ret = sscanf(buf, "%d", &enable);
	if (ret != 1 || enable < 0 || enable > 1)
		return -EINVAL;

	if (cpufreq_boost_trigger_state(enable)) {
		pr_err("%s: Cannot %s BOOST!\n",
		       __func__, enable ? "enable" : "disable");
		return -EINVAL;
	}

	pr_debug("%s: cpufreq BOOST %s\n",
		 __func__, enable ? "enabled" : "disabled");
	
	// Badlav: Users ko sahi feedback dene ke liye extra logging
	if (enable)
		pr_info("CPU Boost enabled for improved performance. Monitor thermal behavior.\n");
	else
		pr_info("CPU Boost disabled. Reverting to normal power-saving policies.\n");

	return count;
}
define_one_global_rw(boost);

static struct cpufreq_governor *find_governor(const char *str_governor)
{
	struct cpufreq_governor *t;

	for_each_governor(t)
		if (!strncasecmp(str_governor, t->name, CPUFREQ_NAME_LEN))
			return t;

	return NULL;
}

/**
 * cpufreq_parse_governor - parse a governor string
 */
static int cpufreq_parse_governor(char *str_governor,
				  struct cpufreq_policy *policy)
{
	if (cpufreq_driver->setpolicy) {
		if (!strncasecmp(str_governor, "performance", CPUFREQ_NAME_LEN)) {
			policy->policy = CPUFREQ_POLICY_PERFORMANCE;
			return 0;
		}

		if (!strncasecmp(str_governor, "powersave", CPUFREQ_NAME_LEN)) {
			policy->policy = CPUFREQ_POLICY_POWERSAVE;
			return 0;
		}
	} else {
		struct cpufreq_governor *t;

		mutex_lock(&cpufreq_governor_mutex);

		t = find_governor(str_governor);
		if (!t) {
			int ret;

			mutex_unlock(&cpufreq_governor_mutex);

			ret = request_module("cpufreq_%s", str_governor);
			if (ret)
				return -EINVAL;

			mutex_lock(&cpufreq_governor_mutex);

			t = find_governor(str_governor);
		}
		if (t && !try_module_get(t->owner))
			t = NULL;

		mutex_unlock(&cpufreq_governor_mutex);

		if (t) {
			policy->governor = t;
			return 0;
		}
	}

	return -EINVAL;
}

/**
 * cpufreq_per_cpu_attr_read() / show_##file_name() -
 * print out cpufreq information
 *
 * Write out information from cpufreq_driver->policy[cpu]; object must be
 * "unsigned int".
 */

#define show_one(file_name, object)			\
static ssize_t show_##file_name				\
(struct cpufreq_policy *policy, char *buf)		\
{							\
	return sprintf(buf, "%u\n", policy->object);	\
}

show_one(cpuinfo_min_freq, cpuinfo.min_freq);
show_one(cpuinfo_transition_latency, cpuinfo.transition_latency);
show_one(scaling_min_freq, min);
show_one(scaling_max_freq, max);

unsigned int cpuinfo_max_freq_cached;

static bool should_use_cached_freq(int cpu)
{
	if (!cpuinfo_max_freq_cached)
		return false;

	if (!(BIT(cpu) & sched_lib_mask_force))
		return false;

	return is_sched_lib_based_app(current->pid);
}

static ssize_t show_cpuinfo_max_freq(struct cpufreq_policy *policy, char *buf)
{
	unsigned int freq = policy->cpuinfo.max_freq;

	if (should_use_cached_freq(policy->cpu))
		freq = cpuinfo_max_freq_cached << 1;
	else
		freq = policy->cpuinfo.max_freq;

	return scnprintf(buf, PAGE_SIZE, "%u\n", freq);
}

__weak unsigned int arch_freq_get_on_cpu(int cpu)
{
	return 0;
}

static ssize_t show_scaling_cur_freq(struct cpufreq_policy *policy, char *buf)
{
	ssize_t ret;
	unsig
