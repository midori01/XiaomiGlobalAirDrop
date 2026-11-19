// SPDX-License-Identifier: GPL-2.0
/*
 * The Wonder provider admits one session per WLAN start. When a session ends it leaves
 * three things behind that only a full WLAN stop clears:
 *  - wt_vdev_trial_claimed, which rejects the next passthrough vdev (-EINVAL);
 *  - the "used" flag of the per-WMA Wonder control, which makes binding peer events
 *    for the next session fail (-EBUSY);
 *  - possibly a closed peer owner, which fails the next owner lookup (-ESHUTDOWN).
 * Reloading WLAN takes about 4 s and drops Wi-Fi, and Quick Share stops retrying
 * before it is back. Once the passthrough TX pool is unmapped (the session is gone),
 * clear the three the same way the driver does on WLAN stop. Closing the owners
 * destroys the old peer engine, which unbinds its events from the control; only then
 * can the control be reopened.
 */
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "xga_wonder.h"

static bool fast_rearm = true;
module_param(fast_rearm, bool, 0644);
MODULE_PARM_DESC(fast_rearm, "Rearm Wonder admission in place when a session ends");

static unsigned long rearms;
module_param(rearms, ulong, 0444);

/* Wonder control (wt_controls entry), one per WMA handle. */
#define CTL_LOCK	0x50
#define CTL_CLOSED	0xd8
#define CTL_SINK	0xe0
#define CTL_USED	0xe8
/* Peer owner (wt_peer_owners entry): its WMA handle. */
#define OWNER_WMA	0x20

#define REARM_DELAY_MS	300
#define REARM_RETRIES	10

static struct mutex *trial_lock, *controls_lock, *owners_lock;
static u8 *trial_claimed;
static void **trial_psoc;
static unsigned long *transport_epoch;
static struct list_head *controls, *owners;
static void (*owners_close)(void *wma);
static int retries;

static void *lookup(const char *name)
{
	typedef unsigned long (*lookup_fn)(const char *);
	static lookup_fn fn;

	if (!fn) {
		struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

		if (register_kprobe(&kp))
			return NULL;
		fn = (lookup_fn)kp.addr;
		unregister_kprobe(&kp);
	}
	return (void *)fn(name);
}

/* The driver is reloaded as a whole by the rollback path, so resolve on every use. */
static bool resolve(void)
{
	trial_lock = lookup("wt_vdev_trial_lock");
	trial_claimed = lookup("wt_vdev_trial_claimed");
	trial_psoc = lookup("wt_vdev_trial_psoc");
	transport_epoch = lookup("wt_vdev_transport_epoch");
	controls_lock = lookup("wt_controls_lock");
	controls = lookup("wt_controls");
	owners_lock = lookup("wt_peer_owners_lock");
	owners = lookup("wt_peer_owners");
	owners_close = lookup("wma_wonder_peer_owners_close");
	return trial_lock && trial_claimed && trial_psoc && transport_epoch &&
	       controls_lock && controls && owners_lock && owners && owners_close;
}

static __nocfi void close_owners(void *wma)
{
	owners_close(wma);
}

/* Returns false while a control is still bound to the old session's events. */
static bool reopen_controls(void)
{
	struct list_head *pos;
	bool idle = true;

	mutex_lock(controls_lock);
	for (pos = controls->next; pos != controls; pos = pos->next) {
		u8 *ctl = (u8 *)pos;
		spinlock_t *lock = (spinlock_t *)(ctl + CTL_LOCK);

		spin_lock_bh(lock);
		if (ctl[CTL_CLOSED] & 1) {
			/* WMA is closing; a WLAN restart will rebuild it. */
		} else if (*(void **)(ctl + CTL_SINK)) {
			idle = false;
		} else {
			ctl[CTL_USED] = 0;
		}
		spin_unlock_bh(lock);
	}
	mutex_unlock(controls_lock);
	return idle;
}

static void rearm_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(rearm_work, rearm_fn);

static void rearm_fn(struct work_struct *work)
{
	void *wma = NULL;

	if (!READ_ONCE(fast_rearm) || !resolve())
		return;

	mutex_lock(trial_lock);
	if (!*trial_claimed) {
		mutex_unlock(trial_lock);
		return;
	}
	mutex_unlock(trial_lock);

	mutex_lock(owners_lock);
	if (owners->next != owners)
		wma = *(void **)((u8 *)owners->next + OWNER_WMA);
	mutex_unlock(owners_lock);
	if (wma)
		close_owners(wma);

	if (!reopen_controls()) {
		if (++retries <= REARM_RETRIES)
			schedule_delayed_work(&rearm_work, msecs_to_jiffies(REARM_DELAY_MS));
		else
			pr_warn("xga_wonder: Wonder control still bound; leaving the rearm to a WLAN restart\n");
		return;
	}

	mutex_lock(trial_lock);
	if (*trial_claimed) {
		*trial_psoc = NULL;
		*trial_claimed = 0;
		(*transport_epoch)++;
		rearms++;
		pr_info("xga_wonder: Wonder session rearmed in place (epoch %lu)\n", *transport_epoch);
	}
	mutex_unlock(trial_lock);
}

static int unmap_pre(struct kprobe *p, struct pt_regs *regs)
{
	retries = 0;
	mod_delayed_work(system_wq, &rearm_work, msecs_to_jiffies(REARM_DELAY_MS));
	return 0;
}

static struct kprobe unmap_probe = {
	.symbol_name = "hdd_passthrough_unmap_tx_pool",
	.pre_handler = unmap_pre,
};
static bool unmap_on;

/* Called from the wonder.ko notifier: the provider driver is loaded before wonder.ko. */
void xga_rearm_probe(bool up)
{
	if (unmap_on) {
		unregister_kprobe(&unmap_probe);
		unmap_on = false;
	}
	cancel_delayed_work_sync(&rearm_work);
	if (!up)
		return;
	memset(&unmap_probe, 0, sizeof(unmap_probe));
	unmap_probe.symbol_name = "hdd_passthrough_unmap_tx_pool";
	unmap_probe.pre_handler = unmap_pre;
	if (register_kprobe(&unmap_probe))
		pr_warn("xga_wonder: cannot probe hdd_passthrough_unmap_tx_pool; no in-place rearm\n");
	else
		unmap_on = true;
}

void xga_rearm_exit(void)
{
	xga_rearm_probe(false);
}
