// SPDX-License-Identifier: GPL-2.0
/*
 * Mali KMD compatibility stub
 *
 * Provides /sys/module/mali_kbase/ for userspace tooling compatibility.
 * The real Mali KMD logic lives in versioned modules
 * (mali_kbase_r32p1, mali_kbase_r38p1, mali_kbase_r44p1, mali_kbase_r54p3).
 *
 * On init, loads the version selected by mali.version= cmdline
 * so the redirector is populated before external consumers probe.
 */
#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/sysfs.h>

MODULE_DESCRIPTION("Mali KMD compatibility stub");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: mali_kbase_r32p1 mali_kbase_r38p1 mali_kbase_r44p1 mali_kbase_r54p3");

extern char mali_selected_version[];

static const char *mali_kbase_version;

static ssize_t version_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 mali_kbase_version ? mali_kbase_version : "unknown");
}
static struct kobj_attribute version_attr = __ATTR_RO(version);

/* Proxies for r54p3-only module params, created when r54p3 is selected. */
extern int kbase_large_page_conf_get(char *buffer);
extern bool kbase_fully_backed_gpf_memory_get(void);

static ssize_t large_page_conf_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	int (*get_fn)(char *buffer);
	int ret = -ENODEV;

	get_fn = symbol_get(kbase_large_page_conf_get);
	if (get_fn) {
		ret = get_fn(buf);
		symbol_put(kbase_large_page_conf_get);
	}

	return ret;
}
static struct kobj_attribute large_page_conf_attr = __ATTR_RO(large_page_conf);

static ssize_t fully_backed_gpf_memory_show(struct kobject *kobj,
					    struct kobj_attribute *attr, char *buf)
{
	bool (*get_fn)(void);
	int ret = -ENODEV;

	get_fn = symbol_get(kbase_fully_backed_gpf_memory_get);
	if (get_fn) {
		ret = scnprintf(buf, PAGE_SIZE, "%c\n", get_fn() ? 'Y' : 'N');
		symbol_put(kbase_fully_backed_gpf_memory_get);
	}

	return ret;
}
static struct kobj_attribute fully_backed_gpf_memory_attr =
	__ATTR_RO(fully_backed_gpf_memory);

static int __init mali_kbase_compat_init(void)
{
	char mod_name[64];

	snprintf(mod_name, sizeof(mod_name), "mali_kbase_%s",
		 mali_selected_version[0] ? mali_selected_version : "r38p1");
	request_module(mod_name);

	if (strcmp(mali_selected_version, "r32p1") == 0)
		mali_kbase_version = "r32p1-01bet2 (UK version 11.31)";
	else if (strcmp(mali_selected_version, "r44p1") == 0)
		mali_kbase_version = "r44p1-01eac0 (UK version 11.39)";
	else if (strcmp(mali_selected_version, "r54p3") == 0)
		mali_kbase_version = "r54p3-01eac0 (UK version 11.46)";
	else
		mali_kbase_version = "r38p1-01eac0 (UK version 11.35)";

	sysfs_create_file(&THIS_MODULE->mkobj.kobj, &version_attr.attr);
	if (strcmp(mali_selected_version, "r54p3") == 0) {
		sysfs_create_file(&THIS_MODULE->mkobj.kobj,
				  &large_page_conf_attr.attr);
		sysfs_create_file(&THIS_MODULE->mkobj.kobj,
				  &fully_backed_gpf_memory_attr.attr);
	}
	return 0;
}
module_init(mali_kbase_compat_init);

static void __exit mali_kbase_compat_exit(void)
{
	sysfs_remove_file(&THIS_MODULE->mkobj.kobj, &version_attr.attr);
	sysfs_remove_file(&THIS_MODULE->mkobj.kobj, &large_page_conf_attr.attr);
	sysfs_remove_file(&THIS_MODULE->mkobj.kobj,
			  &fully_backed_gpf_memory_attr.attr);
}
module_exit(mali_kbase_compat_exit);
