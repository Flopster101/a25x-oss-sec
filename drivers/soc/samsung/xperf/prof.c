/*
 * cpufreq logging driver
 * Jungwook Kim <jwook1.kim@samsung.com>
 * updated: 2021
 */

#include <linux/string.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/cpufreq.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/thermal.h>
#include <asm/topology.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <soc/samsung/gpu_cooling.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <soc/samsung/cal-if.h>
#include <soc/samsung/exynos-devfreq.h>
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/sched/ems/ems.h"
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <soc/samsung/exynos_pm_qos.h>
#include <trace/events/sched.h>
#include <trace/events/ems_debug.h>
#include <soc/samsung/freq-qos-tracer.h>
#include "xperf.h"

#if IS_ENABLED(CONFIG_EXYNOS_UFCC)
extern unsigned int get_cpufreq_max_limit(void);
#endif

#if IS_ENABLED(CONFIG_EXYNOS_AFM)
extern unsigned int get_afm_clipped_freq(int cpu);
#endif

static const char *prefix = "xperf";

#define MAX_TNAMES 16

static uint cal_id_mif = 0;
static uint cal_id_g3d = 0;
static uint devfreq_mif = 0;

static int is_running;
static void *buf = NULL;
static uint polling_ms = 100;
static uint bind_cpu = 0;
static uint pid = 0;
static char tns[MAX_TNAMES][TASK_COMM_LEN + 1] = {"", };
static uint util_thd = 300;
static u32 power_coeffs[MAX_CLUSTER];

int cl_cnt;
struct my_cluster cls[MAX_CLUSTER];

static int init_cls(void)
{
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *cursor;
	int next_cpu = 0;
	int last_cpu;
	int table_size;
	int cnt = 0;
	int i;

	while (next_cpu < num_possible_cpus()) {
		policy = cpufreq_cpu_get(next_cpu);
		if (!policy) {
			next_cpu++;
			continue;
		}
		cls[cnt].siblings = policy->related_cpus;
		last_cpu = cpumask_last(cls[cnt].siblings);
		table_size = 0;
		cpufreq_for_each_entry(cursor, policy->freq_table)
			table_size++;
		cls[cnt].freq_size = table_size;
		cls[cnt].maxfreq = policy->cpuinfo.max_freq;
		cls[cnt].minfreq = policy->cpuinfo.min_freq;
		cls[cnt].first_cpu = next_cpu;
		cls[cnt].last_cpu = last_cpu;
		cls[cnt].freqs = kcalloc(cls[cnt].freq_size, sizeof(struct freq), GFP_KERNEL);
		for (i = 0; i < cls[cnt].freq_size; i++)
			cls[cnt].freqs[i].freq = policy->freq_table[i].frequency;
		cnt++;
		next_cpu = last_cpu + 1;
	}
	cl_cnt = cnt;
	return 0;
}

int get_cl_idx(int cpu)
{
	int index = 0;
	int i;
	for (i = 0; i < cl_cnt; i++) {
		if (cpu <= cls[i].last_cpu) {
			index = i;
			break;
		}
	}
	return index;
}

int get_f_idx(int cl_idx, int freq)
{
	int index = 0;
	int i;
	for (i = 0; i < cls[cl_idx].freq_size; i++) {
		if (cls[cl_idx].freqs[i].freq == freq) {
			index = i;
			break;
		}
	}
	return index;
}

//---------------------------------------
// hooking function
uint cpu_util_avgs[VENDOR_NR_CPUS];
static void update_cpu_util_avg(void *data, struct cfs_rq *cfs_rq)
{
	unsigned int util = READ_ONCE(cfs_rq->avg.util_avg);

	util = (util > 1024) ? 1024 : util;
	cpu_util_avgs[cpu_of(rq_of(cfs_rq))] = util;
}

//---------------------------------------
// thread main
static int cpufreq_log_thread(void *data)
{
	int i;
	int ret = 0;
	uint buf_size;
	struct thermal_zone_device *tz;
	int temp[3], temp_g3d, temp_bat;
	const char *tz_names[] = { "BIG", "LITTLE", "MID" };
	uint siop, ocp;
	int cpu;
	uint cpu_cur[MAX_CLUSTER];
	uint cpu_max[MAX_CLUSTER];
	uint mif, gpu;
	int gpu_util;
	int grp_start, grp_num;
	int cpu_power;
	int power_total;
	int cl_idx;
	int f_idx;
	int freq;
	int try_alloc_cnt = 3;
	struct pid *pgrp;
	struct task_struct *p;
	static int task_num_cpus[VENDOR_NR_CPUS] = {0,};
	uint util;

	if (is_running) {
		pr_info("[%s] already running!!\n", prefix);
		return 0;
	}

	// alloc buf
	buf_size = KMALLOC_MAX_SIZE;
	if (buf)
		vfree(buf);
	while (try_alloc_cnt-- > 0) {
		buf = vmalloc(buf_size);
		if (!buf) {
			pr_info("[%s] kmalloc failed: try again...%d\n", prefix, try_alloc_cnt);
			buf_size -= (1024*1024);
			continue;
		} else {
			break;
		}
	}
	if (try_alloc_cnt <= 0) {
		pr_info("[%s] kmalloc failed: buf_size: %d\n", prefix, buf_size);
		return 0;
	} else {
		memset(buf, 0, buf_size);
		pr_info("[%s] kmalloc ok. buf=%p, buf_size=%d\n", prefix, buf, buf_size);
	}

	// start
	is_running = 1;
	pr_info("[%s] cpufreq log start\n", prefix);

	//---------------------
	// header
	//---------------------

	// temperature
	ret += snprintf(buf + ret, buf_size - ret, "01-temp_BIG ");
	ret += snprintf(buf + ret, buf_size - ret, "01-temp_LITTLE ");
	if (cl_cnt > 2)
		ret += snprintf(buf + ret, buf_size - ret, "01-temp_MID ");
	ret += snprintf(buf + ret, buf_size - ret, "01-temp_G3D ");
	ret += snprintf(buf + ret, buf_size - ret, "01-temp_BAT ");

	// cpufreq
	grp_start = 2;
	for (i = cl_cnt-1; i >= 0; i--) {
		grp_num = grp_start + (cl_cnt-1 - i);
		cpu = cls[i].first_cpu;
		ret += snprintf(buf + ret, buf_size - ret, "0%d-cpu%d_max ", grp_num, cpu);
		ret += snprintf(buf + ret, buf_size - ret, "0%d-cpu%d_cur ", grp_num, cpu);
		if (i == (cl_cnt-1)) { // if big cluster
			ret += snprintf(buf + ret, buf_size - ret, "0%d-cpu%d_siop ", grp_num, cpu);
			ret += snprintf(buf + ret, buf_size - ret, "0%d-cpu%d_ocp ", grp_num, cpu);
		}
	}

	// mif, gpu
	ret += snprintf(buf + ret, buf_size - ret, "05-mif_cur 06-gpu_util 06-gpu_cur ");

	// cpu util
	for_each_possible_cpu(cpu) {
		ret += snprintf(buf + ret, buf_size - ret, "08-util_cpu%d ", cpu);
	}

	// cpu power
	for_each_possible_cpu(cpu) {
		ret += snprintf(buf + ret, buf_size - ret, "09-power_cpu%d ", cpu);
	}
	ret += snprintf(buf + ret, buf_size - ret, "09-power_total ");

	// task multi
	for_each_possible_cpu(cpu) {
		ret += snprintf(buf + ret, buf_size - ret, "10-task_cpu%d ", cpu);
	}

	// tune mode
	ret += snprintf(buf + ret, buf_size - ret, "11-ems_mode 11-ems_level ");

	ret -= 1;
	ret += snprintf(buf + ret, buf_size - ret, "\n");

	//---------------------
	// body
	//---------------------
	while (is_running) {
		// cpu temperature
		for (i = 0; i < cl_cnt; i++) {
			tz = thermal_zone_get_zone_by_name(tz_names[i]);
			thermal_zone_get_temp(tz, &temp[i]);
			temp[i] = (temp[i] < 0)? 0 : temp[i] / 1000;
			ret += snprintf(buf + ret, buf_size - ret, "%d ", temp[i]);
		}
		// gpu temperature
		tz = thermal_zone_get_zone_by_name("G3D");
		thermal_zone_get_temp(tz, &temp_g3d);
		temp_g3d = (temp_g3d < 0)? 0 : temp_g3d / 1000;
		ret += snprintf(buf + ret, buf_size - ret, "%d ", temp_g3d);

		// siop, ocp
#if IS_ENABLED(CONFIG_EXYNOS_UFCC)
		siop = get_cpufreq_max_limit() / 1000;
		siop = (siop > 4000)? 0 : siop;
#else
		siop = 0;
#endif

#if IS_ENABLED(CONFIG_EXYNOS_AFM)
		ocp = get_afm_clipped_freq( cls[cl_cnt-1].last_cpu ) / 1000;
		ocp = (ocp > 4000)? 0 : ocp;
#else
		ocp = 0;
#endif
		// battery
		tz = thermal_zone_get_zone_by_name("battery");
		thermal_zone_get_temp(tz, &temp_bat);
		temp_bat = (temp_bat < 0) ? 0 : temp_bat / 1000;
		ret += snprintf(buf + ret, buf_size - ret, "%d ", temp_bat);

		// cpufreq
		for (i = cl_cnt - 1; i >= 0; i--) {
			cpu = cls[i].first_cpu;
			cpu_max[i] = cpufreq_quick_get_max(cpu) / 1000;
			cpu_cur[i] = cpufreq_quick_get(cpu) / 1000;
			ret += snprintf(buf + ret, buf_size - ret, "%d %d ", cpu_max[i], cpu_cur[i]);
			if (i == (cl_cnt-1)) {
				ret += snprintf(buf + ret, buf_size - ret, "%d %d ", siop, ocp);
			}
		}
		// mif
		mif = (uint)exynos_devfreq_get_domain_freq(devfreq_mif) / 1000;

		// gpu
		gpu_util = gpu_dvfs_get_utilization();

		//gpu_util = 0;
		gpu = (uint)cal_dfs_cached_get_rate(cal_id_g3d) / 1000;
		ret += snprintf(buf + ret, buf_size - ret, "%d %d %d ", mif, gpu_util, gpu);

		// cpu util
		for_each_possible_cpu(cpu)
			ret += snprintf(buf + ret, buf_size - ret, "%d ", cpu_util_avgs[cpu]);

		// cpu power
		power_total = 0;
		for_each_possible_cpu(cpu) {
			unsigned int util_ratio, power;
			util_ratio = (cpu_util_avgs[cpu] * 100) / 1024;
			cl_idx = get_cl_idx(cpu);
			freq = cpu_cur[cl_idx] * 1000;
			f_idx = get_f_idx(cl_idx, freq);
#if IS_ENABLED(CONFIG_SCHED_EMS)
			power = cls[cl_idx].freqs[f_idx].dyn_power + et_freq_to_spower(cpu, freq);
#else
			power = cls[cl_idx].freqs[f_idx].dyn_power;
#endif
			cpu_power = util_ratio * power / 100;
			power_total += cpu_power;
			ret += snprintf(buf + ret, buf_size - ret, "%d ", cpu_power);
		}
		ret += snprintf(buf + ret, buf_size - ret, "%d ", power_total);

		// task multi
		for (i = 0; i <= cls[cl_cnt-1].last_cpu; i++)
			task_num_cpus[i] = 0;

		pgrp = find_vpid(pid);
		do_each_pid_thread(pgrp, PIDTYPE_TGID, p) {
			if (p->state == TASK_RUNNING) {
				util = p->se.avg.util_avg;
				if (util > util_thd) {
					for (i = 0; i < MAX_TNAMES; i++) {
						if (strlen(tns[i]) > 1) {
							if (!strncmp(p->comm, tns[i], strlen(tns[i]))) {
								pr_info("[%s] util_thd=%d util=%d tid=%d cpu=%d tns[%d]=%s p->comm=%s", prefix, util_thd, util, p->pid, p->cpu, i, tns[i], p->comm);
								task_num_cpus[p->cpu] = i + 1;		// tnames index + 1
							}
						}
					}
				}
			}
		} while_each_pid_thread(pgrp, PIDTYPE_TGID, p);

		for_each_possible_cpu(cpu)
			ret += snprintf(buf + ret, buf_size - ret, "%d ", task_num_cpus[cpu]);

		// tune mode
		ret += snprintf(buf + ret, buf_size - ret, "%d %d ", emstune_get_cur_mode(), emstune_get_cur_level());

		ret -= 1;
		ret += snprintf(buf + ret, buf_size - ret, "\n");

		// check buf size
		if ( ret + 256 > buf_size ) {
			pr_info("[%s] buf_size: %d + 256 > %d!!", prefix, ret, buf_size);
			break;
		}
		msleep(polling_ms);
	}

	return 0;
}

static struct task_struct *task;
static void cpufreq_log_start(void)
{
	if (is_running) {
		pr_err("[%s] already running!!\n", prefix);
		return;
	}
	task = kthread_create(cpufreq_log_thread, NULL, "xperf%u", 0);
	kthread_bind(task, bind_cpu);
	wake_up_process(task);
	return;
}

static void cpufreq_log_stop(void)
{
	is_running = 0;
	pr_info("[%s] cpufreq profile done\n", prefix);
}

//===========================================================
// sysfs nodes
//===========================================================

#define DEF_NODE(name) \
	static ssize_t show_##name(struct kobject *k, struct kobj_attribute *attr, char *buf) { \
		int ret = 0; \
		ret += sprintf(buf + ret, "%d\n", name); \
		return ret; } \
	static ssize_t store_##name(struct kobject *k, struct kobj_attribute *attr, const char *buf, size_t count) { \
		if (sscanf(buf, "%d", &name) != 1) \
			return -EINVAL; \
		return count; } \
	static struct kobj_attribute name##_attr = __ATTR(name, 0644, show_##name, store_##name);

DEF_NODE(polling_ms)
DEF_NODE(bind_cpu)
DEF_NODE(pid)
DEF_NODE(util_thd)

static ssize_t show_tnames(struct kobject *k, struct kobj_attribute *attr, char *buf)
{
	int ret = 0, i;
	for (i = 0; i < MAX_TNAMES; i++)
		ret += sprintf(buf + ret, "%d:%s\n", i, tns[i]);
	return ret;
}
static ssize_t store_tnames(struct kobject *k, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int i;
	char tbuf[1000], *_tbuf, *_ttmp;

	if (sscanf(buf, "%999s", tbuf) != 1)
		return -EINVAL;
	_tbuf = tbuf;

	i = 0;
	while ((_ttmp = strsep(&_tbuf, ",")) != NULL && i < MAX_TNAMES) {
		strncpy(tns[i], _ttmp, TASK_COMM_LEN);
		i++;
	}
	return count;
}
static struct kobj_attribute tnames_attr = __ATTR(tnames, 0644, show_tnames, store_tnames);

//-------------------------------------------------------------------
static int init_cpu_power(void)
{
	u64 f, v, c, p;
	struct device *dev;
	unsigned long f_hz;
	int cnt, i;

	for (cnt = 0; cnt < cl_cnt; cnt++) {
		cls[cnt].coeff = power_coeffs[cnt];
		dev = get_cpu_device(cls[cnt].first_cpu);
		for (i = 0; i < cls[cnt].freq_size; i++) {
			struct dev_pm_opp *opp;
			f_hz = cls[cnt].freqs[i].freq * 1000;
			opp = dev_pm_opp_find_freq_ceil(dev, &f_hz);
			cls[cnt].freqs[i].volt = dev_pm_opp_get_voltage(opp) / 1000;	// uV -> mV
			f = cls[cnt].freqs[i].freq / 1000;	// kHz -> MHz
			v = cls[cnt].freqs[i].volt;
			c = cls[cnt].coeff;
			p = c * v * v * f;
			p = p / 1000000000;
			cls[cnt].freqs[i].dyn_power = p;	// dynamic power

#if IS_ENABLED(CONFIG_SCHED_EMS)
			p = et_freq_to_spower(cls[cnt].first_cpu, cls[cnt].freqs[i].freq);
#else
			p = cls[cnt].coeff * cls[cnt].freqs[i].freq;
#endif
			cls[cnt].freqs[i].sta_power = p;	// static power
		}
	}
	return 0;
}

// power
static ssize_t show_power(struct kobject *k, struct kobj_attribute *attr, char *b)
{
	int i, j;
	int ret = 0;
	for (i = 0; i < cl_cnt; i++) {
		for (j = 0; j < cls[i].freq_size; j++)
			ret += sprintf(b + ret, "%u %u %u\n",
					cls[i].freqs[j].freq,
					cls[i].freqs[j].dyn_power,
					cls[i].freqs[j].sta_power);
		ret += sprintf(b + ret, "\n");
	}
	b[ret] = '\0';
	return ret;
}
static ssize_t store_power(struct kobject *k, struct kobj_attribute *attr, const char *b, size_t count)
{
	return count;
}
static struct kobj_attribute power_attr = __ATTR(power, 0644, show_power, store_power);

// volt
static ssize_t show_volt(struct kobject *k, struct kobj_attribute *attr, char *b)
{
	int i, j;
	int ret = 0;
	for (i = 0; i < cl_cnt; i++) {
		for (j = 0; j < cls[i].freq_size; j++)
			ret += sprintf(b + ret, "%u %u\n", cls[i].freqs[j].freq, cls[i].freqs[j].volt);
		ret += sprintf(b + ret, "\n");
	}
	b[ret] = '\0';
	return ret;
}
static ssize_t store_volt(struct kobject *k, struct kobj_attribute *attr, const char *b, size_t count)
{
	return count;
}
static struct kobj_attribute volt_attr = __ATTR(volt, 0644, show_volt, store_volt);

// init
static ssize_t show_init(struct kobject *k, struct kobj_attribute *attr, char *b)
{
	return 0;
}
static ssize_t store_init(struct kobject *k, struct kobj_attribute *attr, const char *b, size_t count)
{
	init_cpu_power();
	return count;
}
static struct kobj_attribute init_attr = __ATTR(init, 0644, show_init, store_init);

// run
static ssize_t show_run(struct kobject *k, struct kobj_attribute *attr, char *b)
{
	return 0;
}
static ssize_t store_run(struct kobject *k, struct kobj_attribute *attr, const char *b, size_t count)
{
	int run = 0;
	if (sscanf(b, "%d", &run) != 1)
		return -EINVAL;
	if (run)
		cpufreq_log_start();
	else
		cpufreq_log_stop();
	return count;
}
static struct kobj_attribute run_attr = __ATTR(run, 0644, show_run, store_run);

static struct kobject *prof_kobj;
static struct attribute *prof_attrs[] = {
	&run_attr.attr,
	&polling_ms_attr.attr,
	&bind_cpu_attr.attr,
	&pid_attr.attr,
	&power_attr.attr,
	&volt_attr.attr,
	&init_attr.attr,
	&tnames_attr.attr,
	&util_thd_attr.attr,
	NULL
};
static struct attribute_group prof_group = {
	.attrs = prof_attrs,
};

//================================
// debugfs

static int result_seq_show(struct seq_file *file, void *iter)
{
       if (is_running) {
               seq_printf(file, "NO RESULT\n");
       } else {
               seq_printf(file, "%s", (char *)buf);    // PRINT RESULT
       }
       return 0;
}
static ssize_t result_seq_write(struct file *file, const char __user *buffer, size_t count, loff_t *off)
{
       return count;
}
static int result_debugfs_open(struct inode *inode, struct file *file)
{
       return single_open(file, result_seq_show, inode->i_private);
}
static struct file_operations result_debugfs_fops = {
       .owner          = THIS_MODULE,
       .open           = result_debugfs_open,
       .write          = result_seq_write,
       .read           = seq_read,
       .llseek         = seq_lseek,
       .release        = single_release,
};

/*--------------------------------------*/
// MAIN

int xperf_prof_init(struct platform_device *pdev, struct kobject *kobj)
{
	struct device_node *dn = pdev->dev.of_node;
	struct dentry *root, *d;
	int ret = 0;

	WARN_ON(register_trace_pelt_cfs_tp(update_cpu_util_avg, NULL));

	of_property_read_u32(dn, "cal-id-cpucl0", &cls[0].cal_id);
	of_property_read_u32(dn, "cal-id-cpucl1", &cls[1].cal_id);
	of_property_read_u32(dn, "cal-id-cpucl2", &cls[2].cal_id);
	of_property_read_u32(dn, "cal-id-mif", &cal_id_mif);
	of_property_read_u32(dn, "cal-id-g3d", &cal_id_g3d);
	of_property_read_u32(dn, "devfreq-mif", &devfreq_mif);

	init_cls();	// get cl_cnt

	if (of_property_read_u32_array(dn, "power-coeff", power_coeffs, cl_cnt) < 0)
		return -ENODATA;

	init_cpu_power();

	// sysfs: normal nodes
	prof_kobj = kobject_create_and_add("prof", kobj);

	if (!prof_kobj) {
		pr_info("[%s] create node failed: %s\n", prefix, __FILE__);
		return -EINVAL;
	}

	ret = sysfs_create_group(prof_kobj, &prof_group);
	if (ret) {
		pr_info("[%s] create group failed: %s\n", prefix, __FILE__);
		return -EINVAL;
	}

	// debugfs: only for result
	root = debugfs_create_dir("xperf_prof", NULL);
	if (!root) {
		printk("%s: create debugfs\n", __FILE__);
		return -ENOMEM;
	}
	d = debugfs_create_file("result", S_IRUSR, root,
				(unsigned int *)0,
				&result_debugfs_fops);
	if (!d)
		return -ENOMEM;

	return 0;
}
