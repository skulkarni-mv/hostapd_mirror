/*
 * wpa_supplicant - Internal driver interface wrappers
 * Copyright (c) 2003-2009, Jouni Malinen <j@w1.fi>
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

#ifndef DRIVER_I_H
#define DRIVER_I_H

#include "drivers/driver.h"

/* driver_ops */
static inline void * wpa_drv_init(struct wpa_supplicant *wpa_s,
				  const char *ifname)
{
	if (wpa_s->driver->init2)
		return wpa_s->driver->init2(wpa_s, ifname, wpa_s->global);
	if (wpa_s->driver->init) {
		return wpa_s->driver->init(wpa_s, ifname);
	}
	return NULL;
}

static inline void wpa_drv_deinit(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->driver->deinit)
		wpa_s->driver->deinit(wpa_s->drv_priv);
}

static inline int wpa_drv_set_param(struct wpa_supplicant *wpa_s,
				    const char *param)
{
	if (wpa_s->driver->set_param)
		return wpa_s->driver->set_param(wpa_s->drv_priv, param);
	return 0;
}

static inline int wpa_drv_set_drop_unencrypted(struct wpa_supplicant *wpa_s,
					       int enabled)
{
	if (wpa_s->driver->set_drop_unencrypted) {
		return wpa_s->driver->set_drop_unencrypted(wpa_s->drv_priv,
							   enabled);
	}
	return -1;
}

static inline int wpa_drv_set_countermeasures(struct wpa_supplicant *wpa_s,
					      int enabled)
{
	if (wpa_s->driver->set_countermeasures) {
		return wpa_s->driver->set_countermeasures(wpa_s->drv_priv,
							  enabled);
	}
	return -1;
}

static inline int wpa_drv_set_auth_alg(struct wpa_supplicant *wpa_s,
				       int auth_alg)
{
	if (wpa_s->driver->set_auth_alg) {
		return wpa_s->driver->set_auth_alg(wpa_s->drv_priv,
						   auth_alg);
	}
	return -1;
}

static inline int wpa_drv_set_wpa(struct wpa_supplicant *wpa_s, int enabled)
{
	if (wpa_s->driver->set_wpa) {
		return wpa_s->driver->set_wpa(wpa_s->drv_priv, enabled);
	}
	return 0;
}

static inline int wpa_drv_set_mode(struct wpa_supplicant *wpa_s, int mode)
{
	if (wpa_s->driver->set_mode) {
		return wpa_s->driver->set_mode(wpa_s->drv_priv, mode);
	}
	return 0;
}

static inline int wpa_drv_authenticate(struct wpa_supplicant *wpa_s,
				       struct wpa_driver_auth_params *params)
{
	if (wpa_s->driver->authenticate)
		return wpa_s->driver->authenticate(wpa_s->drv_priv, params);
	return -1;
}

static inline int wpa_drv_associate(struct wpa_supplicant *wpa_s,
				    struct wpa_driver_associate_params *params)
{
	if (wpa_s->driver->associate) {
		return wpa_s->driver->associate(wpa_s->drv_priv, params);
	}
	return -1;
}

static inline int wpa_drv_scan(struct wpa_supplicant *wpa_s,
			       struct wpa_driver_scan_params *params)
{
	if (wpa_s->driver->scan2)
		return wpa_s->driver->scan2(wpa_s->drv_priv, params);
	if (wpa_s->driver->scan)
		return wpa_s->driver->scan(wpa_s->drv_priv,
					   params->ssids[0].ssid,
					   params->ssids[0].ssid_len);
	return -1;
}

static inline int wpa_drv_get_scan_results(struct wpa_supplicant *wpa_s,
					   struct wpa_scan_result *results,
					   size_t max_size)
{
	if (wpa_s->driver->get_scan_results) {
		return wpa_s->driver->get_scan_results(wpa_s->drv_priv,
						       results, max_size);
	}
	return -1;
}

static inline struct wpa_scan_results * wpa_drv_get_scan_results2(
	struct wpa_supplicant *wpa_s)
{
	if (wpa_s->driver->get_scan_results2)
		return wpa_s->driver->get_scan_results2(wpa_s->drv_priv);
	return NULL;
}

static inline int wpa_drv_get_bssid(struct wpa_supplicant *wpa_s, u8 *bssid)
{
	if (wpa_s->driver->get_bssid) {
		return wpa_s->driver->get_bssid(wpa_s->drv_priv, bssid);
	}
	return -1;
}

static inline int wpa_drv_get_ssid(struct wpa_supplicant *wpa_s, u8 *ssid)
{
	if (wpa_s->driver->get_ssid) {
		return wpa_s->driver->get_ssid(wpa_s->drv_priv, ssid);
	}
	return -1;
}

static inline int wpa_drv_set_key(struct wpa_supplicant *wpa_s, wpa_alg alg,
				   const u8 *addr, int key_idx, int set_tx,
				   const u8 *seq, size_t seq_len,
				   const u8 *key, size_t key_len)
{
	if (wpa_s->driver->set_key) {
		wpa_s->keys_cleared = 0;
		return wpa_s->driver->set_key(wpa_s->drv_priv, alg, addr,
					      key_idx, set_tx, seq, seq_len,
					      key, key_len);
	}
	return -1;
}

static inline int wpa_drv_deauthenticate(struct wpa_supplicant *wpa_s,
					 const u8 *addr, int reason_code)
{
	if (wpa_s->driver->deauthenticate) {
		return wpa_s->driver->deauthenticate(wpa_s->drv_priv, addr,
						     reason_code);
	}
	return -1;
}

static inline int wpa_drv_disassociate(struct wpa_supplicant *wpa_s,
				       const u8 *addr, int reason_code)
{
	if (wpa_s->driver->disassociate) {
		return wpa_s->driver->disassociate(wpa_s->drv_priv, addr,
						   reason_code);
	}
	return -1;
}

static inline int wpa_drv_add_pmkid(struct wpa_supplicant *wpa_s,
				    const u8 *bssid, const u8 *pmkid)
{
	if (wpa_s->driver->add_pmkid) {
		return wpa_s->driver->add_pmkid(wpa_s->drv_priv, bssid, pmkid);
	}
	return -1;
}

static inline int wpa_drv_remove_pmkid(struct wpa_supplicant *wpa_s,
				       const u8 *bssid, const u8 *pmkid)
{
	if (wpa_s->driver->remove_pmkid) {
		return wpa_s->driver->remove_pmkid(wpa_s->drv_priv, bssid,
						   pmkid);
	}
	return -1;
}

static inline int wpa_drv_flush_pmkid(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->driver->flush_pmkid) {
		return wpa_s->driver->flush_pmkid(wpa_s->drv_priv);
	}
	return -1;
}

static inline int wpa_drv_get_capa(struct wpa_supplicant *wpa_s,
				   struct wpa_driver_capa *capa)
{
	if (wpa_s->driver->get_capa) {
		return wpa_s->driver->get_capa(wpa_s->drv_priv, capa);
	}
	return -1;
}

static inline void wpa_drv_poll(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->driver->poll) {
		wpa_s->driver->poll(wpa_s->drv_priv);
	}
}

static inline const char * wpa_drv_get_ifname(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->driver->get_ifname) {
		return wpa_s->driver->get_ifname(wpa_s->drv_priv);
	}
	return NULL;
}

static inline const u8 * wpa_drv_get_mac_addr(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->driver->get_mac_addr) {
		return wpa_s->driver->get_mac_addr(wpa_s->drv_priv);
	}
	return NULL;
}

static inline int wpa_drv_send_eapol(struct wpa_supplicant *wpa_s,
				     const u8 *dst, u16 proto,
				     const u8 *data, size_t data_len)
{
	if (wpa_s->driver->send_eapol)
		return wpa_s->driver->send_eapol(wpa_s->drv_priv, dst, proto,
						 data, data_len);
	return -1;
}

static inline int wpa_drv_set_operstate(struct wpa_supplicant *wpa_s,
					int state)
{
	if (wpa_s->driver->set_operstate)
		return wpa_s->driver->set_operstate(wpa_s->drv_priv, state);
	return 0;
}

static inline int wpa_drv_mlme_setprotection(struct wpa_supplicant *wpa_s,
					     const u8 *addr, int protect_type,
					     int key_type)
{
	if (wpa_s->driver->mlme_setprotection)
		return wpa_s->driver->mlme_setprotection(wpa_s->drv_priv, addr,
							 protect_type,
							 key_type);
	return 0;
}

static inline struct hostapd_hw_modes *
wpa_drv_get_hw_feature_data(struct wpa_supplicant *wpa_s, u16 *num_modes,
			    u16 *flags)
{
	if (wpa_s->driver->get_hw_feature_data)
		return wpa_s->driver->get_hw_feature_data(wpa_s->drv_priv,
							  num_modes, flags);
	return NULL;
}

static inline int wpa_drv_set_channel(struct wpa_supplicant *wpa_s,
				      hostapd_hw_mode phymode, int chan,
				      int freq)
{
	if (wpa_s->driver->set_channel)
		return wpa_s->driver->set_channel(wpa_s->drv_priv, phymode,
						  chan, freq);
	return -1;
}

static inline int wpa_drv_set_ssid(struct wpa_supplicant *wpa_s,
				   const u8 *ssid, size_t ssid_len)
{
	if (wpa_s->driver->set_ssid) {
		return wpa_s->driver->set_ssid(wpa_s->drv_priv, ssid,
					       ssid_len);
	}
	return -1;
}

static inline int wpa_drv_set_bssid(struct wpa_supplicant *wpa_s,
				    const u8 *bssid)
{
	if (wpa_s->driver->set_bssid) {
		return wpa_s->driver->set_bssid(wpa_s->drv_priv, bssid);
	}
	return -1;
}

static inline int wpa_drv_set_country(struct wpa_supplicant *wpa_s,
				      const char *alpha2)
{
	if (wpa_s->driver->set_country)
		return wpa_s->driver->set_country(wpa_s->drv_priv, alpha2);
	return 0;
}

static inline int wpa_drv_send_mlme(struct wpa_supplicant *wpa_s,
				    const u8 *data, size_t data_len)
{
	if (wpa_s->driver->send_mlme)
		return wpa_s->driver->send_mlme(wpa_s->drv_priv,
						data, data_len);
	return -1;
}

static inline int wpa_drv_mlme_add_sta(struct wpa_supplicant *wpa_s,
				       const u8 *addr, const u8 *supp_rates,
				       size_t supp_rates_len)
{
	if (wpa_s->driver->mlme_add_sta)
		return wpa_s->driver->mlme_add_sta(wpa_s->drv_priv, addr,
						   supp_rates, supp_rates_len);
	return -1;
}

static inline int wpa_drv_mlme_remove_sta(struct wpa_supplicant *wpa_s,
					  const u8 *addr)
{
	if (wpa_s->driver->mlme_remove_sta)
		return wpa_s->driver->mlme_remove_sta(wpa_s->drv_priv, addr);
	return -1;
}

static inline int wpa_drv_update_ft_ies(struct wpa_supplicant *wpa_s,
					const u8 *md,
					const u8 *ies, size_t ies_len)
{
	if (wpa_s->driver->update_ft_ies)
		return wpa_s->driver->update_ft_ies(wpa_s->drv_priv, md,
						    ies, ies_len);
	return -1;
}

static inline int wpa_drv_send_ft_action(struct wpa_supplicant *wpa_s,
					 u8 action, const u8 *target_ap,
					 const u8 *ies, size_t ies_len)
{
	if (wpa_s->driver->send_ft_action)
		return wpa_s->driver->send_ft_action(wpa_s->drv_priv, action,
						     target_ap, ies, ies_len);
	return -1;
}

static inline int wpa_drv_set_probe_req_ie(struct wpa_supplicant *wpa_s,
					   const u8 *ies, size_t ies_len)
{
	if (wpa_s->driver->set_probe_req_ie)
		return wpa_s->driver->set_probe_req_ie(wpa_s->drv_priv, ies,
						       ies_len);
	return -1;
}

static inline int wpa_drv_set_beacon(struct wpa_supplicant *wpa_s,
				     const u8 *head, size_t head_len,
				     const u8 *tail, size_t tail_len,
				     int dtim_period)
{
	if (wpa_s->driver->set_beacon)
		return wpa_s->driver->set_beacon(wpa_s->drv_priv, head,
						 head_len, tail, tail_len,
						 dtim_period);
	return -1;
}

static inline int wpa_drv_set_beacon_int(struct wpa_supplicant *wpa_s,
					 int value)
{
	if (wpa_s->driver->set_beacon_int)
		return wpa_s->driver->set_beacon_int(wpa_s->drv_priv, value);
	return -1;
}

static inline int wpa_drv_sta_add(struct wpa_supplicant *wpa_s,
				  struct hostapd_sta_add_params *params)
{
	if (wpa_s->driver->sta_add)
		return wpa_s->driver->sta_add(wpa_s->ifname, wpa_s->drv_priv,
					      params);
	return -1;
}

static inline int wpa_drv_sta_remove(struct wpa_supplicant *wpa_s,
				     const u8 *addr)
{
	if (wpa_s->driver->sta_remove)
		return wpa_s->driver->sta_remove(wpa_s->drv_priv, addr);
	return -1;
}

static inline int wpa_drv_hapd_send_eapol(struct wpa_supplicant *wpa_s,
					  const u8 *addr, const u8 *data,
					  size_t data_len, int encrypt,
					  const u8 *own_addr)
{
	if (wpa_s->driver->hapd_send_eapol)
		return wpa_s->driver->hapd_send_eapol(wpa_s->drv_priv, addr,
						      data, data_len, encrypt,
						      own_addr);
	return -1;
}

static inline int wpa_drv_sta_set_flags(struct wpa_supplicant *wpa_s,
					const u8 *addr, int total_flags,
					int flags_or, int flags_and)
{
	if (wpa_s->driver->sta_set_flags)
		return wpa_s->driver->sta_set_flags(wpa_s->drv_priv, addr,
						    total_flags, flags_or,
						    flags_and);
	return -1;
}

static inline int wpa_drv_set_supp_port(struct wpa_supplicant *wpa_s,
					int authorized)
{
	if (wpa_s->driver->set_supp_port) {
		return wpa_s->driver->set_supp_port(wpa_s->drv_priv,
						    authorized);
	}
	return 0;
}

#endif /* DRIVER_I_H */
