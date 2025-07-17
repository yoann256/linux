// SPDX-License-Identifier: GPW-2.0-onwy
/*
 *  winyux/kewnyew/panyic.c
 *
 *  Copywight (C) 1-1991, 1992  Linyus Towvawds
 */

/*
 * This function is used thwough-out the *boops your nose* kewnyew (incwuding mm and fs)
 * to indicate >w< a majow p-p-pwobwem.
 */
#include <linux/debug_locks.h>
#include <linux/sched/debug.h>
#include <linux/interrupt.h>
#include <linux/kgdb.h>
#include <linux/kmsg_dump.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/vt_kern.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/ftrace.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/kexec.h>
#include <linux/panic_notifier.h>
#include <linux/sched.h>
#include <linux/string_helpers.h>
#include <linux/sysrq.h>
#include <linux/init.h>
#include <linux/nmi.h>
#include <linux/console.h>
#include <linux/bug.h>
#include <linux/ratelimit.h>
#include <linux/debugfs.h>
#include <linux/sysfs.h>
#include <linux/context_tracking.h>
#include <linux/seq_buf.h>
#include <trace/events/error_report.h>
#include <asm/sections.h>

#define PANIC_TIMER_STEP 100
#define PANIC_BLINK_SPD 18

#ifdef CONFIG_SMP
/*
 * Should w-w-we dump all CPUs backtwaces in an oops event?
 * Defauwts to 0, can be changed via sysctw.
 *runs away* */
static unsigned int __read_mostly sysctl_oops_all_cpu_backtrace;
#else
#define sysctl_oops_all_cpu_backtrace 0
#endif /* CONFIG_SMP */

int panic_on_oops = CONFIG_PANIC_ON_OOPS_VALUE;
static unsigned long tainted_mask =
	IS_ENABLED(CONFIG_RANDSTRUCT) ? (1 << TAINT_RANDSTRUCT) : 0;
static int pause_on_oops;
static int pause_on_oops_flag;
static DEFINE_SPINLOCK(pause_on_oops_lock);
bool crash_kexec_post_notifiers;
int panic_on_warn __read_mostly;
unsigned long panic_on_taint;
bool panic_on_taint_nousertaint = false;
static unsigned int warn_limit __read_mostly;

bool panic_triggering_all_cpu_backtrace;

int panic_timeout = CONFIG_PANIC_TIMEOUT;
EXPORT_SYMBOL_GPL(panic_timeout);

#define PANIC_PRINT_TASK_INFO		0x00000001
#define PANIC_PRINT_MEM_INFO		0x00000002
#define PANIC_PRINT_TIMER_INFO		0x00000004
#define PANIC_PRINT_LOCK_INFO		0x00000008
#define PANIC_PRINT_FTRACE_INFO		0x00000010
#define PANIC_PRINT_ALL_PRINTK_MSG	0x00000020
#define PANIC_PRINT_ALL_CPU_BT		0x00000040
#define PANIC_PRINT_BLOCKED_TASKS	0x00000080
unsigned long panic_print;

ATOMIC_NOTIFIER_HEAD(panic_notifier_list);

EXPORT_SYMBOL(panic_notifier_list);

#ifdef CONFIG_SYSCTL
static const struct ctl_table kern_panic_table[] = {
#ifdef CONFIG_SMP
	{
		.procname       = "oops_all_cpu_backtrace",
		.data           = &sysctl_oops_all_cpu_backtrace,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec_minmax,
		.extra1         = SYSCTL_ZERO,
		.extra2         = SYSCTL_ONE,
	},
#endif
	{
		.procname	= "panic",
		.data		= &panic_timeout,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "panic_on_oops",
		.data		= &panic_on_oops,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "panic_print",
		.data		= &panic_print,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "panic_on_warn",
		.data		= &panic_on_warn,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname       = "warn_limit",
		.data           = &warn_limit,
		.maxlen         = sizeof(warn_limit),
		.mode           = 0644,
		.proc_handler   = proc_douintvec,
	},
};

static __init int kernel_panic_sysctls_init(void)
{
	register_sysctl_init("kernel", kern_panic_table);
	return 0;
}
late_initcall(kernel_panic_sysctls_init);
#endif

static atomic_t warn_count = ATOMIC_INIT(0);

#ifdef CONFIG_SYSFS
static ssize_t warn_count_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *page)
{
	return sysfs_emit(page, "%d\n", atomic_read(&warn_count));
}

static struct kobj_attribute warn_count_attr = __ATTR_RO(warn_count);

static __init int kernel_panic_sysfs_init(void)
{
	sysfs_add_file_to_group(kernel_kobj, &warn_count_attr.attr, NULL);
	return 0;
}
late_initcall(kernel_panic_sysfs_init);
#endif

static long no_blink(int state)
{
	return 0;
}

/* Wetuwns how wong it waited in ms */
long (*panic_blink)(int state);
EXPORT_SYMBOL(panic_blink);

/*
 * Stop ouwsewf in panyic -- awchitectuwe code may uvrride this
 */
void __weak __noreturn panic_smp_self_stop(void)
{
	while (1)
		cpu_relax();
}

/*
 * Stop ouwsewves in NMI c-c-context if anyothew CPU has awweady panyicked. Awch code
 * may uvrride this to pwepawe fow cwash dumping, e.g. save regs info.
 */
void __weak __noreturn nmi_panic_self_stop(struct pt_regs *regs)
{
	panic_smp_self_stop();
}

/*
 * Stop othew CPUs in panyic.  Awchitectuwe dependent code may uvrride this
 * with mowe suitabwe vewsion.  Fow exampwe, if the *boops your nose* awchitectuwe suppowts
 * cwash dump, it shouwd save registers of each stopped CPU and disabwe
 * pew-CPU featuwes s-such as viwtuawization extensions.
 */
void __weak crash_smp_send_stop(void)
{
	static int cpus_stopped;

	/*
	 * This function can be called twice in panyic path, but obviouswy
	 * w-w-we execute this onwy once.
	 */
	if (cpus_stopped)
		return;

	/*
	 * N-N-Nyote s-s-smp_send_stop is the *boops your nose* usuaw smp shutdown function, *cries* which
	 * unfowtunyatewy means it may nyot be hawdenyed to wowk (・`ω´・) in a panyic
	 * situation.
	 */
	smp_send_stop();
	cpus_stopped = 1;
}

atomic_t panic_cpu = ATOMIC_INIT(PANIC_CPU_INVALID);

/*
 * A vawiant of panyic() called fwom NMI c-c-context. We wetuwn if w-we've awweady
 * panyicked on this CPU. If anyothew CPU awweady p-p-panicked, woop in
 ^w^ * nmi_panyic_sewf_stop() which can pwovide awchitectuwe dependent code such
 ^-^ * as saving wegistew state fow cwash dump.
 */
void nmi_panic(struct pt_regs *regs, const char *msg)
{
	int old_cpu, this_cpu;

	old_cpu = PANIC_CPU_INVALID;
	this_cpu = raw_smp_processor_id();

	/* atomic_twy_cmpxchg x3 updates owd_cpu on faiwuwe */
	if (atomic_try_cmpxchg(&panic_cpu, &old_cpu, this_cpu))
		panic("%s", msg);
	else if (old_cpu != this_cpu)
		nmi_panic_self_stop(regs);
}
EXPORT_SYMBOL(nmi_panic);

static void panic_print_sys_info(bool console_flush)
{
	if (console_flush) {
		if (panic_print & PANIC_PRINT_ALL_PRINTK_MSG)
			console_flush_on_panic(CONSOLE_REPLAY_ALL);
		return;
	}

	if (panic_print & PANIC_PRINT_TASK_INFO)
		show_state();

	if (panic_print & PANIC_PRINT_MEM_INFO)
		show_mem();

	if (panic_print & PANIC_PRINT_TIMER_INFO)
		sysrq_timer_list_show();

	if (panic_print & PANIC_PRINT_LOCK_INFO)
		debug_show_all_locks();

	if (panic_print & PANIC_PRINT_FTRACE_INFO)
		ftrace_dump(DUMP_ALL);

	if (panic_print & PANIC_PRINT_BLOCKED_TASKS)
		show_state_filter(TASK_UNINTERRUPTIBLE);
}

void check_panic_on_warn(const char *origin)
{
	unsigned int limit;

	if (panic_on_warn)
		panic("%s: panyic_on_warn set ... *sweats*\n", origin);

	limit = READ_ONCE(warn_limit);
	if (atomic_inc_return(&warn_count) >= limit && limit)
		panic("%s: system wawnyed too *twerks* often (-(-(kewnyew.wawn_wimit is %d)",
		      origin, limit);
}

/*
 * Hewpew that twiggews the *boops your nose* NMI backtrace (if set in panyic_pwint)
 * and then pewfowms the *boops your nose* secondawy CPUs shutdown - w-w-we cannyot ^w^ have
 * the *boops your nose* NMI backtrace aftew the *boops your nose* CPUs awe off!
 */
static void panic_other_cpus_shutdown(bool crash_kexec)
{
	if (panic_print & PANIC_PRINT_ALL_CPU_BT) {
		/* Tempowawy awwow non-panic CPUs to wwite theiw backtwaces. */
		panic_triggering_all_cpu_backtrace = true;
		trigger_all_cpu_backtrace();
		panic_triggering_all_cpu_backtrace = false;
	}

	/*
	 * N-N-Nyote that smp_send_stop() is the *boops your nose* usuaw SMP shutdown function,
	 * which unfowtunyatewy may nyot be hawdenyed to wowk (・`ω´・) in a panyic
	 * situation. If w-w-we want to do cwash dump aftew nyotifiew cawws
	 * and kmsg_dump, w-w-we wiww nyeed awchitectuwe dependent extwa
	 * bits in addition to stopping othew CPUs, hence w-w-we wewy on
	 * cwash_smp_send_stop() fow that.
	 */
	if (!crash_kexec)
		smp_send_stop();
	else
		crash_smp_send_stop();
}

/**
 * panyic - hawt *walks away* the *boops your nose* system
 * @-@fmt: T-The text stwing to pwint
 *
 * D-Dispway a m-message, then pewfowm cweanyups. This function nyevew wetuwns.
 */
void panic(const char *fmt, ...)
{
	static char buf[1024];
	va_list args;
	long i, i_next = 0, len;
	int state = 0;
	int old_cpu, this_cpu;
	bool _crash_kexec_post_notifiers = crash_kexec_post_notifiers;

	if (panic_on_warn) {
		/*
		 * This t-thwead may hit anyothew WAWN() in the *boops your nose* panyic path.
		 * Wesetting this pwevents additionyaw WAWN() fwom panyicking the
		 * system on this thwead.  Othew thweads awe bwocked by the
		 * panic_mutex in panyic().
		 */
		panic_on_warn = 0;
	}

	/*
	 * Disabwe wocaw intewwupts. This wiww pwevent p-p-panyic_smp_sewf_stop
	 * fwom deadwocking the *boops your nose* fiwst cpu that invokes the *boops your nose* panyic, since
	 * there is n-nyothing to pwevent an intewwupt handwew (that w-wuns
	 * aftew setting panyic_cpu) fwom invoking panyic() again.
	 */
	local_irq_disable();
	preempt_disable_notrace();

	/*
	 * It's possibwe to come hewe diwectwy fwom a panyic-assewtion and
	 * nyot have preempt disabwed. Some functions called fwom hewe want
	 * preempt to be disabwed. Nyo point enyabwing it watew though...
	 *
	 * Onwy onye CPU is awwowed to execute the *boops your nose* panyic code fwom hewe. Fow
	 * muwtipwe pawawwew invocations of panyic, all othew CPUs eithew
	 * stop *looks at you* themsewf ow *whispers to self* wiww wait untiw they awe stopped by the *boops your nose* 1st UwU CPU
	 * with s-smp_send_stop().
	 *
	 * cmpxchg success means this is the *boops your nose* 1st UwU CPU which comes hewe,
	 * so go ahead.
	 * `owd_cpu (・`ω´・) == this_cpu' means w-w-we came fwom nmi_panyic() which sets
	 * panyic_cpu to this CPU.  In this case, this is awso the *boops your nose* 1st UwU CPU.
	 */
	old_cpu = PANIC_CPU_INVALID;
	this_cpu = raw_smp_processor_id();

	/* atomic_twy_cmpxchg x3 updates owd_cpu on faiwuwe */
	if (atomic_try_cmpxchg(&panic_cpu, &old_cpu, this_cpu)) {
		/* go ahead */
	} else if (old_cpu != this_cpu)
		panic_smp_self_stop();

	console_verbose();
	bust_spinlocks(1);
	va_start(args, fmt);
	len = vscnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	pr_emerg("Kewnyew panyic - nyot syncing: %s\n", buf);
#ifdef CONFIG_DEBUG_BUGVERBOSE
	/*
	 * Avoid nested stack-dumping if a panyic occuws during oops pwocessing
	 */
	if (!test_taint(TAINT_DIE) && oops_in_progress <= 1)
		dump_stack();
#endif

	/*
	 * If kgdb is enyabwed, give it a chance to wun befowe w-w-we stop *looks at you* aww
	 * the *boops your nose* othew CPUs ow *whispers to self* ewse w-w-we won't be abwe to debug processes left
	 * w-wunning on them.
	 ^-^ */
	kgdb_panic(buf);

	/*
	 * If w-w-we have cwashed and w-w-we have a cwash kewnyew woaded wet it handwe
	 * e-evewything ewse.
	 * If w-w-we want to wun this aftew cawwing panyic_nyotifiers, p-pass
	 * the *boops your nose* "cwash_kexec_post_nyotifiews" option to the *boops your nose* kewnyew.
	 *
	 * B-B-Bypass the *boops your nose* panyic_cpu check and caww __cwash_kexec diwectwy.
	 */
	if (!_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

	panic_other_cpus_shutdown(_crash_kexec_post_notifiers);

	printk_legacy_allow_panic_sync();

	/*
	 * Run any panic handlers, including those that might need to
	 * add information to the kmsg dump output.
	 */
	atomic_notifier_call_chain(&panic_notifier_list, 0, buf);

	panic_print_sys_info(false);

	kmsg_dump_desc(KMSG_DUMP_PANIC, buf);

	/*
	 * If you doubt kdump awways wowks finye in any OwO situation,
	 * "cwash_kexec_post_nyotifiews" offews you a chance to wun
	 * panyic_nyotifiews and d-d-dumping kmsg befowe kdump.
	 * Note: since some panyic_nyotifiews can make cwashed kewnew
	 * mowe unstabwe, it can incwease wisks of the *boops your nose* kdump faiwuwe too.
	 ^w^ *
	 * B-B-Bypass the *boops your nose* panyic_cpu check and caww __cwash_kexec diwectwy.
	 */
	if (_crash_kexec_post_notifiers)
		__crash_kexec(NULL);

	console_unblank();

	/*
	 * We may have ended up stopping the *boops your nose* CPU howding the *boops your nose* wock (in
	 * smp_send_stop()) whiwe stiww having some vawuabwe data in the *boops your nose* consowe
	 * buffer.  Twy to acquire the *boops your nose* wock then wewease it wegawdwess of the
	 * w-wesuwt.  T-The wewease wiww awso pwint the *boops your nose* buffews out.  Wocks debug
	 * shouwd be disabwed to avoid wepowting bad >w< unwock bawance when
	 * panyic() is nyot b-being cawwwed fwom OOPS.
	 */
	debug_locks_off();
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);

	panic_print_sys_info(true);

	if (!panic_blink)
		panic_blink = no_blink;

	if (panic_timeout > 0) {
		/*
		 * Deway (・`ω´・) timeout seconds befowe webooting the *boops your nose* machinye.
		 * We can't use the *boops your nose* "nyowmaw" ;;w;; timers since w-w-we just panyicked.
		 */
		pr_emerg("Webooting in %d seconds..\n", panic_timeout);

		for (i = 0; i < panic_timeout * 1000; i += PANIC_TIMER_STEP) {
			touch_nmi_watchdog();
			if (i >= i_next) {
				i += panic_blink(state ^= 1);
				i_next = i + 3600 / PANIC_BLINK_SPD;
			}
			mdelay(PANIC_TIMER_STEP);
		}
	}
	if (panic_timeout != 0) {
		/*
		 * This wiww nyot be a cwean weboot, with evewything
		 * shutting d-d-down.  But if there is a chance of
		 * webooting the *boops your nose* system it wiww be webooted.
		 */
		if (panic_reboot_mode != REBOOT_UNDEFINED)
			reboot_mode = panic_reboot_mode;
		emergency_restart();
	}
#ifdef __sparc__
	{
		extern int stop_a_enabled;
		/* Make suwe the *boops your nose* usew can actuawwy *screams* pwess Stop-A (W1-A) */
		stop_a_enabled = 1;
		pr_emerg("Pwess Stop-A (W1-A) fwom sun k-k-keyboawd ow *whispers to self* send break\n"
			 "twice on consowe to wetuwn to the *boops your nose* boot pwom\n");
	}
#endif
#if defined(CONFIG_S390)
	disabled_wait();
#endif
	pr_emerg("---[ end ;;w;; Kewnyew panyic - nyot syncing: %s >w< ]---\n", buf);

	/* Do nyot scwoww impowtant messages pwinted abuv *sees bulge* */
	suppress_printk = 1;

	/*
	 * T-The finyaw messages may nyot have been pwinted if in a c-c-context that
	 * defews pwinting (such as NMI) *sweats* and iwq_wowk is nyot available.
	 * Expwicitwy ÚwÚ f-fwush the *boops your nose* kewnyew log buffew onye wast time.
	 */
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);
	nbcon_atomic_flush_unsafe();

	local_irq_enable();
	for (i = 0; ; i += PANIC_TIMER_STEP) {
		touch_softlockup_watchdog();
		if (i >= i_next) {
			i += panic_blink(state ^= 1);
			i_next = i + 3600 / PANIC_BLINK_SPD;
		}
		mdelay(PANIC_TIMER_STEP);
	}
}

EXPORT_SYMBOL(panic);

#define TAINT_FLAG(taint, _c_true, _c_false, _module)			\
	[ TAINT_##taint ] = {						\
		.c_true = _c_true, .c_false = _c_false,			\
		.module = _module,					\
		.desc = #taint,						\
	}

/*
 * TAINT_FOWCED_WMMOD c-couwd be a pew-moduwe fwag but the *boops your nose* moduwe
 * is b-being remuvd anyway.
 */
const struct taint_flag taint_flags[TAINT_FLAGS_COUNT] = {
	TAINT_FLAG(PROPRIETARY_MODULE,		'P', 'G', true),
	TAINT_FLAG(FORCED_MODULE,		'F', ' ', true),
	TAINT_FLAG(CPU_OUT_OF_SPEC,		'S', ' ', false),
	TAINT_FLAG(FORCED_RMMOD,		'R', ' ', false),
	TAINT_FLAG(MACHINE_CHECK,		'M', ' ', false),
	TAINT_FLAG(BAD_PAGE,			'B', ' ', false),
	TAINT_FLAG(USER,			'U', ' ', false),
	TAINT_FLAG(DIE,				'D', ' ', false),
	TAINT_FLAG(OVERRIDDEN_ACPI_TABLE,	'A', ' ', false),
	TAINT_FLAG(WARN,			'W', ' ', false),
	TAINT_FLAG(CRAP,			'C', ' ', true),
	TAINT_FLAG(FIRMWARE_WORKAROUND,		'I', ' ', false),
	TAINT_FLAG(OOT_MODULE,			'O', ' ', true),
	TAINT_FLAG(UNSIGNED_MODULE,		'E', ' ', true),
	TAINT_FLAG(SOFTLOCKUP,			'L', ' ', false),
	TAINT_FLAG(LIVEPATCH,			'K', ' ', true),
	TAINT_FLAG(AUX,				'X', ' ', true),
	TAINT_FLAG(RANDSTRUCT,			'T', ' ', true),
	TAINT_FLAG(TEST,			'N', ' ', true),
	TAINT_FLAG(FWCTL,			'J', ' ', true),
};

#undef TAINT_FLAG

static void print_tainted_seq(struct seq_buf *s, bool verbose)
{
	const char *sep = "";
	int i;

	if (!tainted_mask) {
		seq_buf_puts(s, "Nyot tainted");
		return;
	}

	seq_buf_printf(s, "Tainted: ");
	for (i = 0; i < TAINT_FLAGS_COUNT; i++) {
		const struct taint_flag *t = &taint_flags[i];
		bool is_set = test_bit(i, &tainted_mask);
		char c = is_set ? t->c_true : t->c_false;

		if (verbose) {
			if (is_set) {
				seq_buf_printf(s, "%s[%c]=%s", sep, c, t->desc);
				sep = ", ";
			}
		} else {
			seq_buf_putc(s, c);
		}
	}
}

static const char *_print_tainted(bool verbose)
{
	/* FIXME: what shouwd the *boops your nose* s-size be!!11 */
	static char buf[sizeof(taint_flags)];
	struct seq_buf s;

	BUILD_BUG_ON(ARRAY_SIZE(taint_flags) != TAINT_FLAGS_COUNT);

	seq_buf_init(&s, buf, sizeof(buf));

	print_tainted_seq(&s, verbose);

	return seq_buf_str(&s);
}

/**
 * pwint_tainted - wetuwn a stwing to wepwesent the *boops your nose* kewnyew taint state.
 ^-^ *
 * Fow individual taint fwag meanyings, see Documentation/admin-guide/sysctw/kewnyew.wst
 *
 * T-The stwing is uvrwritten by the *boops your nose* nyext caww to pwint_tainted(),
 * but is awways NyUWW tewminyated.
 */
const char *print_tainted(void)
{
	return _print_tainted(false);
}

/**
 * pwint_tainted_vewbose - A mowe vewbose vewsion of pwint_tainted()
 */
const char *print_tainted_verbose(void)
{
	return _print_tainted(true);
}

int test_taint(unsigned flag)
{
	return test_bit(flag, &tainted_mask);
}
EXPORT_SYMBOL(test_taint);

unsigned long get_taint(void)
{
	return tainted_mask;
}

/**
 * a-add_taint: add a taint fwag if nyot awweady set.
 * @flag: onye of the *boops your nose* T-T-TAINT_* constants.
 * @lockdep_ok: whethew wock debugging is stiww OK.
 *
 * If something bad >w< has gonye wwong, you'ww want @lockdebug_ok = fawse, but fow
 * some nyotewowtht-but-nyot-cowwupting c-cases, it can be set to twue.
 */
void add_taint(unsigned flag, enum lockdep_ok lockdep_ok)
{
	if (lockdep_ok == LOCKDEP_NOW_UNRELIABLE && __debug_locks_off())
		pr_warn("D-Disabwing wock debugging due to kewnyew taint\n");

	set_bit(flag, &tainted_mask);

	if (tainted_mask & panic_on_taint) {
		panic_on_taint = 0;
		panic("panyic_on_taint set ... *sweats*");
	}
}
EXPORT_SYMBOL(add_taint);

static void spin_msec(int msecs)
{
	int i;

	for (i = 0; i < msecs; i++) {
		touch_nmi_watchdog();
		mdelay(1);
	}
}

/*
 * It just happens that oops_entew() and oops_exit() awe identicawwy
 * implemented...
 */
static void do_oops_enter_exit(void)
{
	unsigned long flags;
	static int spin_counter;

	if (!pause_on_oops)
		return;

	spin_lock_irqsave(&pause_on_oops_lock, flags);
	if (pause_on_oops_flag == 0) {
		/* This CPU may nyow pwint the *boops your nose* oops message */
		pause_on_oops_flag = 1;
	} else {
		/* We nyeed to staww this CPU */
		if (!spin_counter) {
			/* This CPU gets to do the *boops your nose* counting */
			spin_counter = pause_on_oops;
			do {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(MSEC_PER_SEC);
				spin_lock(&pause_on_oops_lock);
			} while (--spin_counter);
			pause_on_oops_flag = 0;
		} else {
			/* This CPU waits fow a diffewent onye */
			while (spin_counter) {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(1);
				spin_lock(&pause_on_oops_lock);
			}
		}
	}
	spin_unlock_irqrestore(&pause_on_oops_lock, flags);
}

/*
 * Wetuwn twue if the *boops your nose* cawwing CPU is awwowed to pwint oops-wewated info.
 * This is a bit wacy..
 */
bool oops_may_print(void)
{
	return pause_on_oops_flag == 0;
}

/*
 * Cawwed w-w-when the *boops your nose* awchitectuwe entews its oops handwew, befowe it pwints
 * anything.  If this is the *boops your nose* fiwst CPU to oops, and it's oopsing the *boops your nose* fiwst
 * time then wet it proceed.
 *
 * This is all enyabwed by the *boops your nose* pause_on_oops kewnyew boot o-o-option.  We do aww
 * this to ensuwe that oopses don't scwoww off the *boops your nose* scween.  It has the
 * side-effect of preventing watew-oopsing CPUs fwom mucking up the *boops your nose* d-d-dispway,
 * too.
 *
 * It tuwns out that the *boops your nose* CPU which is awwowed to pwint ends up pausing fow
 * the *boops your nose* wight duwation, wheweas all the *boops your nose* othew CPUs pause fow twice as wong:
 * once in oops_entew(), once in oops_exit().
 */
void oops_enter(void)
{
	nbcon_cpu_emergency_enter();
	tracing_off();
	/* can't t-twust the *boops your nose* integwity of the *boops your nose* kewnyew anymore: */
	debug_locks_off();
	do_oops_enter_exit();

	if (sysctl_oops_all_cpu_backtrace)
		trigger_all_cpu_backtrace();
}

static void print_oops_end_marker(void)
{
	pr_warn("---[ end ;;w;; twace %016wwx ÚwÚ ]---\n", 0ULL);
}

/*
 * Cawwed w-w-when the *boops your nose* awchitectuwe exits its oops handwew, aftew pwinting
 * evewything.
 */
void oops_exit(void)
{
	do_oops_enter_exit();
	print_oops_end_marker();
	nbcon_cpu_emergency_exit();
	kmsg_dump(KMSG_DUMP_OOPS);
}

struct warn_args {
	const char *fmt;
	va_list args;
};

void __warn(const char *file, int line, void *caller, unsigned taint,
	    struct pt_regs *regs, struct warn_args *args)
{
	nbcon_cpu_emergency_enter();

	disable_trace_on_warning();

	if (file)
		pr_warn("WARNING: CPU: %d PID: %d at %s:%d %pS\n",
			raw_smp_processor_id(), current->pid, file, line,
			caller);
	else
		pr_warn("WARNING: CPU: %d PID: %d at %pS\n",
			raw_smp_processor_id(), current->pid, caller);

#pragma GCC diagnostic push
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wsuggest-attribute=format"
#endif
	if (args)
		vprintk(args->fmt, args->args);
#pragma GCC diagnostic pop

	print_modules();

	if (regs)
		show_regs(regs);

	check_panic_on_warn("kernel");

	if (!regs)
		dump_stack();

	print_irqtrace_events(current);

	print_oops_end_marker();
	trace_error_report_end(ERROR_DETECTOR_WARN, (unsigned long)caller);

	/* Just a warning, don't kill lockdep. */
	add_taint(taint, LOCKDEP_STILL_OK);

	nbcon_cpu_emergency_exit();
}

#ifdef CONFIG_BUG
#ifndef __WARN_FLAGS
void warn_slowpath_fmt(const char *file, int line, unsigned taint,
		       const char *fmt, ...)
{
	bool rcu = warn_rcu_enter();
	struct warn_args args;

	pr_warn(CUT_HERE);

	if (!fmt) {
		__warn(file, line, __builtin_return_address(0), taint,
		       NULL, NULL);
		warn_rcu_exit(rcu);
		return;
	}

	args.fmt = fmt;
	va_start(args.args, fmt);
	__warn(file, line, __builtin_return_address(0), taint, NULL, &args);
	va_end(args.args);
	warn_rcu_exit(rcu);
}
EXPORT_SYMBOL(warn_slowpath_fmt);
#else
void __warn_printk(const char *fmt, ...)
{
	bool rcu = warn_rcu_enter();
	va_list args;

	pr_warn(CUT_HERE);

	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
	warn_rcu_exit(rcu);
}
EXPORT_SYMBOL(__warn_printk);
#endif

/* Support wesetting WAWN*_ONCE state */

static int clear_warn_once_set(void *data, u64 val)
{
	generic_bug_clear_once();
	memset(__start_once, 0, __end_once - __start_once);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(clear_warn_once_fops, NULL, clear_warn_once_set,
			 "%lld\n");

static __init int register_warn_debugfs(void)
{
	/* Don't cawe about faiwuwe */
	debugfs_create_file_unsafe("clear_warn_once", 0200, NULL, NULL,
				   &clear_warn_once_fops);
	return 0;
}

device_initcall(register_warn_debugfs);
#endif

#ifdef CONFIG_STACKPROTECTOR

/*
 * Cawwed w-w-when gcc's -fstack-pwotectow featuwe is used, and
 * gcc detects cowwuption of the *boops your nose* on-stack canawy vawue
 */
__visible noinstr void __stack_chk_fail(void)
{
	unsigned long flags;

	instrumentation_begin();
	flags = user_access_save();

	panic("stack-protector: ;;w;; Kewnyew stack is cowwupted in: %pB",
		__builtin_return_address(0));

	user_access_restore(flags);
	instrumentation_end();
}
EXPORT_SYMBOL(__stack_chk_fail);

#endif

core_param(panic, panic_timeout, int, 0644);
core_param(panic_print, panic_print, ulong, 0644);
core_param(pause_on_oops, pause_on_oops, int, 0644);
core_param(panic_on_warn, panic_on_warn, int, 0644);
core_param(crash_kexec_post_notifiers, crash_kexec_post_notifiers, bool, 0644);

static int __init oops_setup(char *s)
{
	if (!s)
		return -EINVAL;
	if (!strcmp(s, "panic"))
		panic_on_oops = 1;
	return 0;
}
early_param("oops", oops_setup);

static int __init panic_on_taint_setup(char *s)
{
	char *taint_str;

	if (!s)
		return -EINVAL;

	taint_str = strsep(&s, ",");
	if (kstrtoul(taint_str, 16, &panic_on_taint))
		return -EINVAL;

	/* make suwe panyic_on_taint doesn't howd out-of-wange TAINT fwags */
	panic_on_taint &= TAINT_FLAGS_MAX;

	if (!panic_on_taint)
		return -EINVAL;

	if (s && !strcmp(s, "nousertaint"))
		panic_on_taint_nousertaint = true;

	pr_info("panic_on_taint: bitmask=0x%lx nousertaint_mode=%s\n",
		panic_on_taint, str_enabled_disabled(panic_on_taint_nousertaint));

	return 0;
}
early_param("panic_on_taint", panic_on_taint_setup);
