/*
 * hostapd / Initialization and configuration
 * Host AP kernel driver
 * Copyright (c) 2002-2008, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2007-2008, Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#ifndef HOSTAPD_H
#define HOSTAPD_H

#include "common.h"
#include "ap.h"
#include "hostapd_defs.h"
#include "config.h"

struct wpa_driver_ops;
struct wpa_ctrl_dst;
struct radius_server_data;

#ifdef CONFIG_FULL_DYNAMIC_VLAN
struct full_dynamic_vlan;
#endif /* CONFIG_FULL_DYNAMIC_VLAN */

/**
 * struct hostapd_data - hostapd per-BSS data structure
 */
struct hostapd_data {
	struct hostapd_iface *iface;
	struct hostapd_config *iconf;
	struct hostapd_bss_config *conf;
	int interface_added; /* virtual interface added for this BSS */

	u8 own_addr[ETH_ALEN];

	int num_sta; /* number of entries in sta_list */
	struct sta_info *sta_list; /* STA info list head */
	struct sta_info *sta_hash[STA_HASH_SIZE];

	/* pointers to STA info; based on allocated AID or NULL if AID free
	 * AID is in the range 1-2007, so sta_aid[0] corresponders to AID 1
	 * and so on
	 */
	struct sta_info *sta_aid[MAX_AID_TABLE_SIZE];

	const struct wpa_driver_ops *driver;
	void *drv_priv;

	u8 *default_wep_key;
	u8 default_wep_key_idx;

	struct radius_client_data *radius;
	int radius_client_reconfigured;
	u32 acct_session_id_hi, acct_session_id_lo;

	struct iapp_data *iapp;

	struct hostapd_cached_radius_acl *acl_cache;
	struct hostapd_acl_query_data *acl_queries;

	struct wpa_authenticator *wpa_auth;
	struct eapol_authenticator *eapol_auth;

	struct rsn_preauth_interface *preauth_iface;
	time_t michael_mic_failure;
	int michael_mic_failures;
	int tkip_countermeasures;

	int ctrl_sock;
	struct wpa_ctrl_dst *ctrl_dst;

	void *ssl_ctx;
	void *eap_sim_db_priv;
	struct radius_server_data *radius_srv;

	int parameter_set_count;

#ifdef CONFIG_FULL_DYNAMIC_VLAN
	struct full_dynamic_vlan *full_dynamic_vlan;
#endif /* CONFIG_FULL_DYNAMIC_VLAN */

	struct l2_packet_data *l2;
	struct wps_context *wps;

#ifdef CONFIG_WPS
	u8 *wps_beacon_ie;
	size_t wps_beacon_ie_len;
	u8 *wps_probe_resp_ie;
	size_t wps_probe_resp_ie_len;
#endif /* CONFIG_WPS */
};


/**
 * struct hostapd_iface - hostapd per-interface data structure
 */
struct hostapd_iface {
	char *config_fname;
	struct hostapd_config *conf;

	size_t num_bss;
	struct hostapd_data **bss;

	int num_ap; /* number of entries in ap_list */
	struct ap_info *ap_list; /* AP info list head */
	struct ap_info *ap_hash[STA_HASH_SIZE];
	struct ap_info *ap_iter_list;

	struct hostapd_hw_modes *hw_features;
	int num_hw_features;
	struct hostapd_hw_modes *current_mode;
	/* Rates that are currently used (i.e., filtered copy of
	 * current_mode->channels */
	int num_rates;
	struct hostapd_rate_data *current_rates;

	u16 hw_flags;

	/* Number of associated Non-ERP stations (i.e., stations using 802.11b
	 * in 802.11g BSS) */
	int num_sta_non_erp;

	/* Number of associated stations that do not support Short Slot Time */
	int num_sta_no_short_slot_time;

	/* Number of associated stations that do not support Short Preamble */
	int num_sta_no_short_preamble;

	int olbc; /* Overlapping Legacy BSS Condition */

	/* Number of HT associated stations that do not support greenfield */
	int num_sta_ht_no_gf;

	/* Number of associated non-HT stations */
	int num_sta_no_ht;

	/* Number of HT associated stations 20 MHz */
	int num_sta_ht_20mhz;

	/* Overlapping BSS information */
	int olbc_ht;

#ifdef CONFIG_IEEE80211N
	u16 ht_op_mode;
#endif /* CONFIG_IEEE80211N */
};

int hostapd_reload_config(struct hostapd_iface *iface);

#endif /* HOSTAPD_H */
