// SPDX-License-Identifier: GPL-2.0
/*
 * The Wonder provider in the patched Peach driver tags received frames with a radiotap
 * header that only carries the channel. Quick Share Extension's AWDL stack drops
 * management frames without TSFT, so it never syncs to Apple peers. This module adds
 * TSFT and an antenna signal to that header before wonder_rx_handler() parses it.
 *
 * When something polls get_mac_tsf, each reading is sampled together with the
 * monotonic clock and the receive time is extrapolated from the latest sample.
 * Quick Share Extension never polls it, so otherwise the monotonic clock itself
 * (in microseconds) serves as the receive timestamp.
 */
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/seqlock.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <net/ieee80211_radiotap.h>

#include "xga_wonder.h"

static int signal_dbm = -50;
module_param(signal_dbm, int, 0644);
MODULE_PARM_DESC(signal_dbm, "Antenna signal reported for received frames (dBm)");

static unsigned long max_sample_age_ms = 2000;
module_param(max_sample_age_ms, ulong, 0644);
MODULE_PARM_DESC(max_sample_age_ms, "Ignore TSF samples older than this");

static unsigned long rx_frames, patched_frames, tsf_samples;
module_param(rx_frames, ulong, 0444);
module_param(patched_frames, ulong, 0444);
module_param(tsf_samples, ulong, 0444);

static DEFINE_SEQLOCK(tsf_lock);
static u64 tsf_us;
static u64 tsf_ns;
static bool tsf_valid;

struct tsf_call {
	u32 *out;
	u64 t0;
};

static int get_tsf_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct tsf_call *call = (struct tsf_call *)ri->data;

	call->out = (u32 *)regs->regs[1];
	call->t0 = ktime_get_ns();
	return 0;
}

static int get_tsf_return(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct tsf_call *call = (struct tsf_call *)ri->data;
	u64 t1 = ktime_get_ns();
	unsigned long flags;

	if (regs_return_value(regs) != 0 || !call->out)
		return 0;
	write_seqlock_irqsave(&tsf_lock, flags);
	tsf_us = READ_ONCE(*call->out);
	tsf_ns = call->t0 + (t1 - call->t0) / 2;
	tsf_valid = true;
	tsf_samples++;
	write_sequnlock_irqrestore(&tsf_lock, flags);
	return 0;
}

static struct kretprobe get_tsf_probe = {
	.kp.symbol_name = "wondertap_get_mac_tsf",
	.entry_handler = get_tsf_entry,
	.handler = get_tsf_return,
	.data_size = sizeof(struct tsf_call),
	.maxactive = 8,
};

static void estimate_tsf(u64 *tsft)
{
	u64 us, ns, now;
	unsigned int seq;
	bool valid;

	do {
		seq = read_seqbegin(&tsf_lock);
		valid = tsf_valid;
		us = tsf_us;
		ns = tsf_ns;
	} while (read_seqretry(&tsf_lock, seq));

	now = ktime_get_ns();
	if (!valid || now < ns || now - ns > max_sample_age_ms * NSEC_PER_MSEC)
		*tsft = div_u64(now, NSEC_PER_USEC);
	else
		*tsft = us + div_u64(now - ns, NSEC_PER_USEC);
}

#define RT_BIT(f) BIT(IEEE80211_RADIOTAP_##f)
#define RT_HANDLED (RT_BIT(FLAGS) | RT_BIT(RATE) | RT_BIT(CHANNEL))

static void add_tsft(struct sk_buff *skb)
{
	struct ieee80211_radiotap_header *hdr;
	u8 new_hdr[32], flags = 0, rate = 0;
	u16 freq = 0, chan_flags = 0;
	u32 present;
	unsigned int len, off = sizeof(*hdr), pos;
	int delta;
	u64 tsft;

	rx_frames++;
	if (skb->len < sizeof(*hdr) || skb_shared(skb))
		return;
	hdr = (struct ieee80211_radiotap_header *)skb->data;
	len = get_unaligned_le16(&hdr->it_len);
	present = get_unaligned_le32(&hdr->it_present);
	/* Only rewrite the simple headers the provider builds; anything else passes through. */
	if (hdr->it_version || len < sizeof(*hdr) || len > skb->len || (present & ~RT_HANDLED))
		return;

	if (present & RT_BIT(FLAGS))
		flags = skb->data[off++];
	if (present & RT_BIT(RATE))
		rate = skb->data[off++];
	if (present & RT_BIT(CHANNEL)) {
		off = ALIGN(off, 2);
		if (off + 4 > len)
			return;
		freq = get_unaligned_le16(skb->data + off);
		chan_flags = get_unaligned_le16(skb->data + off + 2);
		off += 4;
	}
	if (off > len)
		return;
	estimate_tsf(&tsft);

	memset(new_hdr, 0, sizeof(new_hdr));
	pos = sizeof(*hdr);
	put_unaligned_le64(tsft, new_hdr + pos);
	pos += 8;
	if (present & RT_BIT(FLAGS))
		new_hdr[pos++] = flags;
	if (present & RT_BIT(RATE))
		new_hdr[pos++] = rate;
	if (present & RT_BIT(CHANNEL)) {
		pos = ALIGN(pos, 2);
		put_unaligned_le16(freq, new_hdr + pos);
		put_unaligned_le16(chan_flags, new_hdr + pos + 2);
		pos += 4;
	}
	new_hdr[pos++] = (u8)(s8)signal_dbm;

	put_unaligned_le16(pos, new_hdr + 2);
	put_unaligned_le32(present | RT_BIT(TSFT) | RT_BIT(DBM_ANTSIGNAL), new_hdr + 4);

	delta = pos - len;
	if (delta <= 0 || skb_cow_head(skb, delta))
		return;
	skb_push(skb, delta);
	memcpy(skb->data, new_hdr, pos);
	skb_reset_mac_header(skb);
	patched_frames++;
}

static int rx_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct sk_buff **pskb = (struct sk_buff **)regs->regs[0];

	if (pskb && *pskb)
		add_tsft(*pskb);
	return 0;
}

static struct kprobe rx_probe = {
	.symbol_name = "wonder_rx_handler",
	.pre_handler = rx_handler_pre,
};

/*
 * wonder.ko is reloaded after every Wonder session to rearm the provider, which
 * removes probes placed in its text. Re-place them whenever it comes back.
 */
static DEFINE_MUTEX(probe_lock);
static bool probes_on;

static void probes_off(void)
{
	if (!probes_on)
		return;
	unregister_kprobe(&rx_probe);
	unregister_kretprobe(&get_tsf_probe);
	probes_on = false;
	xga_rearm_probe(false);
}

static int probes_up(void)
{
	unsigned long flags;
	int ret;

	write_seqlock_irqsave(&tsf_lock, flags);
	tsf_valid = false;
	write_sequnlock_irqrestore(&tsf_lock, flags);

	memset(&get_tsf_probe.kp, 0, sizeof(get_tsf_probe.kp));
	get_tsf_probe.kp.symbol_name = "wondertap_get_mac_tsf";
	memset(&rx_probe, 0, sizeof(rx_probe));
	rx_probe.symbol_name = "wonder_rx_handler";
	rx_probe.pre_handler = rx_handler_pre;

	ret = register_kretprobe(&get_tsf_probe);
	if (ret) {
		pr_err("xga_wonder: cannot probe wondertap_get_mac_tsf: %d\n", ret);
		return ret;
	}
	ret = register_kprobe(&rx_probe);
	if (ret) {
		pr_err("xga_wonder: cannot probe wonder_rx_handler: %d\n", ret);
		unregister_kretprobe(&get_tsf_probe);
		return ret;
	}
	probes_on = true;
	xga_rearm_probe(true);
	pr_info("xga_wonder: probes placed on wonder (signal %d dBm)\n", signal_dbm);
	return 0;
}

static bool wonder_live = true;

static void rearm_work_fn(struct work_struct *work)
{
	mutex_lock(&probe_lock);
	probes_off();
	if (READ_ONCE(wonder_live))
		probes_up();
	mutex_unlock(&probe_lock);
}
static DECLARE_WORK(rearm_work, rearm_work_fn);

static int wonder_notify(struct notifier_block *nb, unsigned long state, void *data)
{
	struct module *mod = data;

	if (strcmp(mod->name, "wonder"))
		return NOTIFY_DONE;
	if (state == MODULE_STATE_LIVE)
		WRITE_ONCE(wonder_live, true);
	else if (state == MODULE_STATE_GOING)
		WRITE_ONCE(wonder_live, false);
	else
		return NOTIFY_DONE;
	schedule_work(&rearm_work);
	return NOTIFY_DONE;
}

static struct notifier_block wonder_nb = {
	.notifier_call = wonder_notify,
};

static int __init xga_wonder_init(void)
{
	int ret = register_module_notifier(&wonder_nb);

	if (ret)
		return ret;
	schedule_work(&rearm_work);
	return 0;
}

static void __exit xga_wonder_exit(void)
{
	unregister_module_notifier(&wonder_nb);
	cancel_work_sync(&rearm_work);
	mutex_lock(&probe_lock);
	probes_off();
	mutex_unlock(&probe_lock);
	xga_rearm_exit();
}

module_init(xga_wonder_init);
module_exit(xga_wonder_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Wonder fixes for Quick Share Extension AWDL: RX TSFT and in-place session rearm");
