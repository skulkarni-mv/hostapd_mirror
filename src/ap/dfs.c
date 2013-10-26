/*
 * DFS - Dynamic Frequency Selection
 * Copyright (c) 2002-2013, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2013, Qualcomm Atheros, Inc.
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"

#include "utils/common.h"
#include "common/ieee802_11_defs.h"
#include "hostapd.h"
#include "ap_drv_ops.h"
#include "drivers/driver.h"
#include "dfs.h"


static int dfs_get_used_n_chans(struct hostapd_data *hapd)
{
	int n_chans = 1;

	if (hapd->iconf->ieee80211n && hapd->iconf->secondary_channel)
		n_chans = 2;

	if (hapd->iconf->ieee80211ac) {
		switch (hapd->iconf->vht_oper_chwidth) {
		case VHT_CHANWIDTH_USE_HT:
			break;
		case VHT_CHANWIDTH_80MHZ:
			n_chans = 4;
			break;
		case VHT_CHANWIDTH_160MHZ:
			n_chans = 8;
			break;
		default:
			break;
		}
	}

	return n_chans;
}


static int dfs_channel_available(struct hostapd_channel_data *chan)
{
	if (chan->flag & HOSTAPD_CHAN_DISABLED)
		return 0;
	if ((chan->flag & HOSTAPD_CHAN_RADAR) &&
	    ((chan->flag & HOSTAPD_CHAN_DFS_MASK) ==
	     HOSTAPD_CHAN_DFS_UNAVAILABLE))
		return 0;
	return 1;
}


static int dfs_is_ht40_allowed(struct hostapd_channel_data *chan)
{
	int allowed[] = { 36, 44, 52, 60, 100, 108, 116, 124, 132, 149, 157,
			  184, 192 };
	unsigned int i;

	for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
		if (chan->chan == allowed[i])
			return 1;
	}

	return 0;
}


static int dfs_find_channel(struct hostapd_data *hapd,
			    struct hostapd_channel_data **ret_chan,
			    int idx)
{
	struct hostapd_hw_modes *mode;
	struct hostapd_channel_data *chan, *next_chan;
	int i, j, channel_idx = 0, n_chans;

	mode = hapd->iface->current_mode;
	n_chans = dfs_get_used_n_chans(hapd);

	wpa_printf(MSG_DEBUG, "DFS new chan checking %d channels", n_chans);
	for (i = 0; i < mode->num_channels; i++) {
		chan = &mode->channels[i];

		/* Skip not available channels */
		if (!dfs_channel_available(chan))
			continue;

		/* Skip HT40/VHT uncompatible channels */
		if (hapd->iconf->ieee80211n &&
		    hapd->iconf->secondary_channel) {
			if (!dfs_is_ht40_allowed(chan))
				continue;

			for (j = 1; j < n_chans; j++) {
				next_chan = &mode->channels[i + j];
				if (!dfs_channel_available(next_chan))
					break;
			}
			if (j != n_chans)
				continue;

			/* Set HT40+ */
			hapd->iconf->secondary_channel = 1;
		}

		if (ret_chan && idx == channel_idx) {
			wpa_printf(MSG_DEBUG, "Selected ch. #%d", chan->chan);
			*ret_chan = chan;
			return idx;
		}
		wpa_printf(MSG_DEBUG, "Adding channel: %d", chan->chan);
		channel_idx++;
	}
	return channel_idx;
}


static void dfs_adjust_vht_center_freq(struct hostapd_data *hapd,
				       struct hostapd_channel_data *chan)
{
	if (!hapd->iconf->ieee80211ac)
		return;

	if (!chan)
		return;

	switch (hapd->iconf->vht_oper_chwidth) {
	case VHT_CHANWIDTH_USE_HT:
		hapd->iconf->vht_oper_centr_freq_seg0_idx = chan->chan + 2;
		break;
	case VHT_CHANWIDTH_80MHZ:
		hapd->iconf->vht_oper_centr_freq_seg0_idx = chan->chan + 6;
		break;
	case VHT_CHANWIDTH_160MHZ:
		hapd->iconf->vht_oper_centr_freq_seg0_idx =
						chan->chan + 14;
		break;
	default:
		wpa_printf(MSG_INFO, "DFS only VHT20/40/80/160 is supported now");
		break;
	}

	wpa_printf(MSG_DEBUG, "DFS adjusting VHT center frequency: %d",
		   hapd->iconf->vht_oper_centr_freq_seg0_idx);
}


/* Return start channel idx we will use for mode->channels[idx] */
static int dfs_get_start_chan_idx(struct hostapd_data *hapd)
{
	struct hostapd_hw_modes *mode;
	struct hostapd_channel_data *chan;
	int channel_no = hapd->iconf->channel;
	int res = -1, i;

	/* HT40- */
	if (hapd->iconf->ieee80211n && hapd->iconf->secondary_channel == -1)
		channel_no -= 4;

	/* VHT */
	if (hapd->iconf->ieee80211ac) {
		switch (hapd->iconf->vht_oper_chwidth) {
		case VHT_CHANWIDTH_USE_HT:
			break;
		case VHT_CHANWIDTH_80MHZ:
			channel_no =
				hapd->iconf->vht_oper_centr_freq_seg0_idx - 6;
			break;
		case VHT_CHANWIDTH_160MHZ:
			channel_no =
				hapd->iconf->vht_oper_centr_freq_seg0_idx - 14;
			break;
		default:
			wpa_printf(MSG_INFO,
				   "DFS only VHT20/40/80/160 is supported now");
			channel_no = -1;
			break;
		}
	}

	/* Get idx */
	mode = hapd->iface->current_mode;
	for (i = 0; i < mode->num_channels; i++) {
		chan = &mode->channels[i];
		if (chan->chan == channel_no) {
			res = i;
			break;
		}
	}

	if (res == -1)
		wpa_printf(MSG_DEBUG, "DFS chan_idx seems wrong: -1");

	return res;
}


/* At least one channel have radar flag */
static int dfs_check_chans_radar(struct hostapd_data *hapd, int start_chan_idx,
				 int n_chans)
{
	struct hostapd_channel_data *channel;
	struct hostapd_hw_modes *mode;
	int i, res = 0;

	mode = hapd->iface->current_mode;

	for (i = 0; i < n_chans; i++) {
		channel = &mode->channels[start_chan_idx + i];
		if (channel->flag & HOSTAPD_CHAN_RADAR)
			res++;
	}

	return res;
}


/* All channels available */
static int dfs_check_chans_available(struct hostapd_data *hapd,
				     int start_chan_idx, int n_chans)
{
	struct hostapd_channel_data *channel;
	struct hostapd_hw_modes *mode;
	int i;

	mode = hapd->iface->current_mode;

	for(i = 0; i < n_chans; i++) {
		channel = &mode->channels[start_chan_idx + i];
		if ((channel->flag & HOSTAPD_CHAN_DFS_MASK) !=
		    HOSTAPD_CHAN_DFS_AVAILABLE)
			break;
	}

	return i == n_chans;
}


/* At least one channel unavailable */
static int dfs_check_chans_unavailable(struct hostapd_data *hapd,
				       int start_chan_idx,
				       int n_chans)
{
	struct hostapd_channel_data *channel;
	struct hostapd_hw_modes *mode;
	int i, res = 0;

	mode = hapd->iface->current_mode;

	for(i = 0; i < n_chans; i++) {
		channel = &mode->channels[start_chan_idx + i];
		if (channel->flag & HOSTAPD_CHAN_DISABLED)
			res++;
		if ((channel->flag & HOSTAPD_CHAN_DFS_MASK) ==
		    HOSTAPD_CHAN_DFS_UNAVAILABLE)
			res++;
	}

	return res;
}


static struct hostapd_channel_data * dfs_get_valid_channel(
	struct hostapd_data *hapd)
{
	struct hostapd_hw_modes *mode;
	struct hostapd_channel_data *chan = NULL;
	int channel_idx, new_channel_idx;
	u32 _rand;

	wpa_printf(MSG_DEBUG, "DFS: Selecting random channel");

	if (hapd->iface->current_mode == NULL)
		return NULL;

	mode = hapd->iface->current_mode;
	if (mode->mode != HOSTAPD_MODE_IEEE80211A)
		return NULL;

	/* get random available channel */
	channel_idx = dfs_find_channel(hapd, NULL, 0);
	if (channel_idx > 0) {
		os_get_random((u8 *) &_rand, sizeof(_rand));
		new_channel_idx = _rand % channel_idx;
		dfs_find_channel(hapd, &chan, new_channel_idx);
	}

	/* VHT */
	dfs_adjust_vht_center_freq(hapd, chan);

	return chan;
}


static int set_dfs_state_freq(struct hostapd_data *hapd, int freq, u32 state)
{
	struct hostapd_hw_modes *mode;
	struct hostapd_channel_data *chan = NULL;
	int i;

	mode = hapd->iface->current_mode;
	if (mode == NULL)
		return 0;

	wpa_printf(MSG_DEBUG, "set_dfs_state 0x%X for %d MHz", state, freq);
	for (i = 0; i < hapd->iface->current_mode->num_channels; i++) {
		chan = &hapd->iface->current_mode->channels[i];
		if (chan->freq == freq) {
			if (chan->flag & HOSTAPD_CHAN_RADAR) {
				chan->flag &= ~HOSTAPD_CHAN_DFS_MASK;
				chan->flag |= state;
				return 1; /* Channel found */
			}
		}
	}
	wpa_printf(MSG_WARNING, "Can't set DFS state for freq %d MHz", freq);
	return 0;
}


static int set_dfs_state(struct hostapd_data *hapd, int freq, int ht_enabled,
			 int chan_offset, int chan_width, int cf1,
			 int cf2, u32 state)
{
	int n_chans = 1, i;
	struct hostapd_hw_modes *mode;
	int frequency = freq;
	int ret = 0;

	mode = hapd->iface->current_mode;
	if (mode == NULL)
		return 0;

	if (mode->mode != HOSTAPD_MODE_IEEE80211A) {
		wpa_printf(MSG_WARNING, "current_mode != IEEE80211A");
		return 0;
	}

	/* Seems cf1 and chan_width is enough here */
	switch (chan_width) {
	case CHAN_WIDTH_20_NOHT:
	case CHAN_WIDTH_20:
		n_chans = 1;
		frequency = cf1;
		break;
	case CHAN_WIDTH_40:
		n_chans = 2;
		frequency = cf1 - 10;
		break;
	case CHAN_WIDTH_80:
		n_chans = 4;
		frequency = cf1 - 30;
		break;
	case CHAN_WIDTH_160:
		n_chans = 8;
		frequency = cf1 - 70;
		break;
	default:
		wpa_printf(MSG_INFO, "DFS chan_width %d not supported",
			   chan_width);
		break;
	}

	wpa_printf(MSG_DEBUG, "DFS freq: %dMHz, n_chans: %d", frequency,
		   n_chans);
	for (i = 0; i < n_chans; i++) {
		ret += set_dfs_state_freq(hapd, frequency, state);
		frequency = frequency + 20;
	}

	return ret;
}


static int dfs_are_channels_overlapped(struct hostapd_data *hapd, int freq,
				       int chan_width, int cf1, int cf2)
{
	int start_chan_idx;
	struct hostapd_hw_modes *mode;
	struct hostapd_channel_data *chan;
	int n_chans, i, j, frequency = freq, radar_n_chans = 1;
	u8 radar_chan;
	int res = 0;

	if (hapd->iface->freq == freq)
		res++;

	/* Our configuration */
	mode = hapd->iface->current_mode;
	start_chan_idx = dfs_get_start_chan_idx(hapd);
	n_chans = dfs_get_used_n_chans(hapd);

	/* Reported via radar event */
	switch (chan_width) {
	case CHAN_WIDTH_20_NOHT:
	case CHAN_WIDTH_20:
		radar_n_chans = 1;
		frequency = cf1;
		break;
	case CHAN_WIDTH_40:
		radar_n_chans = 2;
		frequency = cf1 - 10;
		break;
	case CHAN_WIDTH_80:
		radar_n_chans = 4;
		frequency = cf1 - 30;
		break;
	case CHAN_WIDTH_160:
		radar_n_chans = 8;
		frequency = cf1 - 70;
		break;
	default:
		wpa_printf(MSG_INFO, "DFS chan_width %d not supported",
			   chan_width);
		break;
	}

	ieee80211_freq_to_chan(frequency, &radar_chan);

	for (i = 0; i < n_chans; i++) {
		chan = &mode->channels[start_chan_idx + i];
		for (j = 0; j < radar_n_chans; j++) {
			wpa_printf(MSG_DEBUG, "checking our: %d, radar: %d",
				   chan->chan, radar_chan + j * 4);
			if (chan->chan == radar_chan + j * 4)
				res++;
		}
	}

	wpa_printf(MSG_DEBUG, "overlapped: %d", res);

	return res;
}


/*
 * Main DFS handler
 * 1 - continue channel/ap setup
 * 0 - channel/ap setup will be continued after CAC
 * -1 - hit critical error
 */
int hostapd_handle_dfs(struct hostapd_data *hapd)
{
	struct hostapd_channel_data *channel;
	int res, n_chans, start_chan_idx;

	do {
		/* Get start (first) channel for current configuration */
		start_chan_idx = dfs_get_start_chan_idx(hapd);
		if (start_chan_idx == -1)
			return -1;

		/* Get number of used channels, depend on width */
		n_chans = dfs_get_used_n_chans(hapd);

		/* Check if any of configured channels require DFS */
		res = dfs_check_chans_radar(hapd, start_chan_idx, n_chans);
		wpa_printf(MSG_DEBUG,
			   "DFS %d channels required radar detection",
			   res);
		if (!res)
			return 1;

		/* Check if all channels are DFS available */
		res = dfs_check_chans_available(hapd, start_chan_idx, n_chans);
		wpa_printf(MSG_DEBUG,
			   "DFS all channels available, (SKIP CAC): %s",
			   res ? "yes" : "no");
		if (res)
			return 1;

		/* Check if any of configured channels is unavailable */
		res = dfs_check_chans_unavailable(hapd, start_chan_idx,
						  n_chans);
		wpa_printf(MSG_DEBUG, "DFS %d chans unavailable - choose other channel: %s",
			   res, res ? "yes": "no");
		if (res) {
			channel = dfs_get_valid_channel(hapd);
			if (!channel) {
				wpa_printf(MSG_ERROR, "could not get valid channel");
				return -1;
			}
			hapd->iconf->channel = channel->chan;
			hapd->iface->freq = channel->freq;
		}
	} while (res);

	/* Finally start CAC */
	wpa_printf(MSG_DEBUG, "DFS start CAC on %d MHz", hapd->iface->freq);
	if (hostapd_start_dfs_cac(hapd, hapd->iconf->hw_mode,
				  hapd->iface->freq,
				  hapd->iconf->channel,
				  hapd->iconf->ieee80211n,
				  hapd->iconf->ieee80211ac,
				  hapd->iconf->secondary_channel,
				  hapd->iconf->vht_oper_chwidth,
				  hapd->iconf->vht_oper_centr_freq_seg0_idx,
				  hapd->iconf->vht_oper_centr_freq_seg1_idx)) {
		wpa_printf(MSG_DEBUG, "DFS start_dfs_cac() failed");
		return -1;
	}

	return 0;
}


int hostapd_dfs_complete_cac(struct hostapd_data *hapd, int success, int freq,
			     int ht_enabled, int chan_offset, int chan_width,
			     int cf1, int cf2)
{
	struct hostapd_channel_data *channel;
	int err = 1;

	if (success) {
		/* Complete iface/ap configuration */
		set_dfs_state(hapd, freq, ht_enabled, chan_offset,
			      chan_width, cf1, cf2,
			      HOSTAPD_CHAN_DFS_AVAILABLE);
		hostapd_setup_interface_complete(hapd->iface, 0);
	} else {
		/* Switch to new channel */
		set_dfs_state(hapd, freq, ht_enabled, chan_offset,
			      chan_width, cf1, cf2,
			      HOSTAPD_CHAN_DFS_UNAVAILABLE);
		channel = dfs_get_valid_channel(hapd);
		if (channel) {
			hapd->iconf->channel = channel->chan;
			hapd->iface->freq = channel->freq;
			err = 0;
		} else
			wpa_printf(MSG_ERROR, "No valid channel available");

		hostapd_setup_interface_complete(hapd->iface, err);
	}

	return 0;
}


static int hostapd_dfs_start_channel_switch(struct hostapd_data *hapd)
{
	struct hostapd_channel_data *channel;
	int err = 1;

	wpa_printf(MSG_DEBUG, "%s called", __func__);
	channel = dfs_get_valid_channel(hapd);
	if (channel) {
		hapd->iconf->channel = channel->chan;
		hapd->iface->freq = channel->freq;
		err = 0;
	}

	hapd->driver->stop_ap(hapd->drv_priv);

	hostapd_setup_interface_complete(hapd->iface, err);
	return 0;
}


int hostapd_dfs_radar_detected(struct hostapd_data *hapd, int freq,
			       int ht_enabled, int chan_offset, int chan_width,
			       int cf1, int cf2)
{
	int res;

	if (!hapd->iconf->ieee80211h)
		return 0;

	/* mark radar frequency as invalid */
	res = set_dfs_state(hapd, freq, ht_enabled, chan_offset,
			    chan_width, cf1, cf2,
			    HOSTAPD_CHAN_DFS_UNAVAILABLE);

	/* Skip if reported radar event not overlapped our channels */
	res = dfs_are_channels_overlapped(hapd, freq, chan_width, cf1, cf2);
	if (!res)
		return 0;

	/* we are working on non-DFS channel - skip event */
	if (res == 0)
		return 0;

	/* radar detected while operating, switch the channel. */
	res = hostapd_dfs_start_channel_switch(hapd);

	return res;
}


int hostapd_dfs_nop_finished(struct hostapd_data *hapd, int freq,
			     int ht_enabled, int chan_offset, int chan_width,
			     int cf1, int cf2)
{
	/* TODO add correct implementation here */
	set_dfs_state(hapd, freq, ht_enabled, chan_offset, chan_width, cf1, cf2,
		      HOSTAPD_CHAN_DFS_USABLE);
	return 0;
}
