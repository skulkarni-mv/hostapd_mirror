/*
 * wpa_supplicant / WPS integration
 * Copyright (c) 2008, Jouni Malinen <j@w1.fi>
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

#include "includes.h"

#include "common.h"
#include "ieee802_11_defs.h"
#include "wpa_common.h"
#include "config.h"
#include "eap_peer/eap.h"
#include "wpa_supplicant_i.h"
#include "eloop.h"
#include "wpa_ctrl.h"
#include "eap_common/eap_wsc_common.h"
#include "wps/wps.h"
#include "wps/wps_defs.h"
#include "wps_supplicant.h"


static void wpas_wps_timeout(void *eloop_ctx, void *timeout_ctx);


int wpas_wps_eapol_cb(struct wpa_supplicant *wpa_s)
{
	eloop_cancel_timeout(wpas_wps_timeout, wpa_s, NULL);

	if (wpa_s->key_mgmt == WPA_KEY_MGMT_WPS && wpa_s->current_ssid &&
	    !(wpa_s->current_ssid->key_mgmt & WPA_KEY_MGMT_WPS)) {
		wpa_printf(MSG_DEBUG, "WPS: Network configuration replaced - "
			   "try to associate with the received credential");
		wpa_supplicant_deauthenticate(wpa_s,
					      WLAN_REASON_DEAUTH_LEAVING);
		wpa_s->reassociate = 1;
		wpa_supplicant_req_scan(wpa_s, 0, 0);
		return 1;
	}

	return 0;
}


static int wpa_supplicant_wps_cred(void *ctx,
				   const struct wps_credential *cred)
{
	struct wpa_supplicant *wpa_s = ctx;
	struct wpa_ssid *ssid = wpa_s->current_ssid;

	wpa_msg(wpa_s, MSG_INFO, "WPS: New credential received");

	if (ssid && (ssid->key_mgmt & WPA_KEY_MGMT_WPS)) {
		wpa_printf(MSG_DEBUG, "WPS: Replace WPS network block based "
			   "on the received credential");
		os_free(ssid->eap.identity);
		ssid->eap.identity = NULL;
		ssid->eap.identity_len = 0;
		os_free(ssid->eap.phase1);
		ssid->eap.phase1 = NULL;
		os_free(ssid->eap.eap_methods);
		ssid->eap.eap_methods = NULL;
	} else {
		wpa_printf(MSG_DEBUG, "WPS: Create a new network based on the "
			   "received credential");
		ssid = wpa_config_add_network(wpa_s->conf);
		if (ssid == NULL)
			return -1;
	}

	wpa_config_set_network_defaults(ssid);

	os_free(ssid->ssid);
	ssid->ssid = os_malloc(cred->ssid_len);
	if (ssid->ssid) {
		os_memcpy(ssid->ssid, cred->ssid, cred->ssid_len);
		ssid->ssid_len = cred->ssid_len;
	}

	switch (cred->encr_type) {
	case WPS_ENCR_NONE:
		ssid->pairwise_cipher = ssid->group_cipher = WPA_CIPHER_NONE;
		break;
	case WPS_ENCR_WEP:
		ssid->pairwise_cipher = ssid->group_cipher =
			WPA_CIPHER_WEP40 | WPA_CIPHER_WEP104;
		if (cred->key_len > 0 && cred->key_len <= MAX_WEP_KEY_LEN &&
		    cred->key_idx < NUM_WEP_KEYS) {
			os_memcpy(ssid->wep_key[cred->key_idx], cred->key,
				  cred->key_len);
			ssid->wep_key_len[cred->key_idx] = cred->key_len;
			ssid->wep_tx_keyidx = cred->key_idx;
		}
		break;
	case WPS_ENCR_TKIP:
		ssid->pairwise_cipher = WPA_CIPHER_TKIP;
		ssid->group_cipher = WPA_CIPHER_TKIP;
		break;
	case WPS_ENCR_AES:
		ssid->pairwise_cipher = WPA_CIPHER_CCMP;
		ssid->group_cipher = WPA_CIPHER_CCMP | WPA_CIPHER_TKIP;
		break;
	}

	switch (cred->auth_type) {
	case WPS_AUTH_OPEN:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_NONE;
		ssid->proto = 0;
		break;
	case WPS_AUTH_SHARED:
		ssid->auth_alg = WPA_AUTH_ALG_SHARED;
		ssid->key_mgmt = WPA_KEY_MGMT_NONE;
		ssid->proto = 0;
		break;
	case WPS_AUTH_WPAPSK:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_PSK;
		ssid->proto = WPA_PROTO_WPA;
		break;
	case WPS_AUTH_WPA:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X;
		ssid->proto = WPA_PROTO_WPA;
		break;
	case WPS_AUTH_WPA2:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X;
		ssid->proto = WPA_PROTO_RSN;
		break;
	case WPS_AUTH_WPA2PSK:
		ssid->auth_alg = WPA_AUTH_ALG_OPEN;
		ssid->key_mgmt = WPA_KEY_MGMT_PSK;
		ssid->proto = WPA_PROTO_RSN;
		break;
	}

	if (ssid->key_mgmt == WPA_KEY_MGMT_PSK) {
		if (cred->key_len == 2 * PMK_LEN) {
			if (hexstr2bin((const char *) cred->key, ssid->psk,
				       PMK_LEN)) {
				wpa_printf(MSG_ERROR, "WPS: Invalid Network "
					   "Key");
				return -1;
			}
			ssid->psk_set = 1;
		} else if (cred->key_len >= 8 && cred->key_len < 2 * PMK_LEN) {
			os_free(ssid->passphrase);
			ssid->passphrase = os_malloc(cred->key_len + 1);
			if (ssid->passphrase == NULL)
				return -1;
			os_memcpy(ssid->passphrase, cred->key, cred->key_len);
			ssid->passphrase[cred->key_len] = '\0';
			wpa_config_update_psk(ssid);
		} else {
			wpa_printf(MSG_ERROR, "WPS: Invalid Network Key "
				   "length %lu",
				   (unsigned long) cred->key_len);
			return -1;
		}
	}

#ifndef CONFIG_NO_CONFIG_WRITE
	if (wpa_s->conf->update_config &&
	    wpa_config_write(wpa_s->confname, wpa_s->conf)) {
		wpa_printf(MSG_DEBUG, "WPS: Failed to update configuration");
		return -1;
	}
#endif /* CONFIG_NO_CONFIG_WRITE */

	return 0;
}


u8 wpas_wps_get_req_type(struct wpa_ssid *ssid)
{
	if (eap_is_wps_pbc_enrollee(&ssid->eap) ||
	    eap_is_wps_pin_enrollee(&ssid->eap))
		return WPS_REQ_ENROLLEE;
	else
		return WPS_REQ_REGISTRAR;
}


static void wpas_clear_wps(struct wpa_supplicant *wpa_s)
{
	int id;
	struct wpa_ssid *ssid;

	eloop_cancel_timeout(wpas_wps_timeout, wpa_s, NULL);

	/* Remove any existing WPS network from configuration */
	ssid = wpa_s->conf->ssid;
	while (ssid) {
		if (ssid->key_mgmt & WPA_KEY_MGMT_WPS)
			id = ssid->id;
		else
			id = -1;
		ssid = ssid->next;
		if (id >= 0)
			wpa_config_remove_network(wpa_s->conf, id);
	}
}


static void wpas_wps_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct wpa_supplicant *wpa_s = eloop_ctx;
	wpa_printf(MSG_DEBUG, "WPS: Requested operation timed out");
	wpas_clear_wps(wpa_s);
}


static struct wpa_ssid * wpas_wps_add_network(struct wpa_supplicant *wpa_s,
					      int registrar, const u8 *bssid)
{
	struct wpa_ssid *ssid;

	ssid = wpa_config_add_network(wpa_s->conf);
	if (ssid == NULL)
		return NULL;
	wpa_config_set_network_defaults(ssid);
	if (wpa_config_set(ssid, "key_mgmt", "WPS", 0) < 0 ||
	    wpa_config_set(ssid, "eap", "WSC", 0) < 0 ||
	    wpa_config_set(ssid, "identity", registrar ?
			   "\"" WSC_ID_REGISTRAR "\"" :
			   "\"" WSC_ID_ENROLLEE "\"", 0) < 0) {
		wpa_config_remove_network(wpa_s->conf, ssid->id);
		return NULL;
	}

	if (bssid) {
		size_t i;
		struct wpa_scan_res *res;

		os_memcpy(ssid->bssid, bssid, ETH_ALEN);

		/* Try to get SSID from scan results */
		if (wpa_s->scan_res == NULL &&
		    wpa_supplicant_get_scan_results(wpa_s) < 0)
			return ssid; /* Could not find any scan results */

		for (i = 0; i < wpa_s->scan_res->num; i++) {
			const u8 *ie;

			res = wpa_s->scan_res->res[i];
			if (os_memcmp(bssid, res->bssid, ETH_ALEN) != 0)
				continue;

			ie = wpa_scan_get_ie(res, WLAN_EID_SSID);
			if (ie == NULL)
				break;
			os_free(ssid->ssid);
			ssid->ssid = os_malloc(ie[1]);
			if (ssid->ssid == NULL)
				break;
			os_memcpy(ssid->ssid, ie + 2, ie[1]);
			ssid->ssid_len = ie[1];
			break;
		}
	}

	return ssid;
}


static void wpas_wps_reassoc(struct wpa_supplicant *wpa_s,
			     struct wpa_ssid *selected)
{
	struct wpa_ssid *ssid;

	/* Mark all other networks disabled and trigger reassociation */
	ssid = wpa_s->conf->ssid;
	while (ssid) {
		ssid->disabled = ssid != selected;
		ssid = ssid->next;
	}
	wpa_s->disconnected = 0;
	wpa_s->reassociate = 1;
	wpa_supplicant_req_scan(wpa_s, 0, 0);
}


int wpas_wps_start_pbc(struct wpa_supplicant *wpa_s, const u8 *bssid)
{
	struct wpa_ssid *ssid;
	wpas_clear_wps(wpa_s);
	ssid = wpas_wps_add_network(wpa_s, 0, bssid);
	if (ssid == NULL)
		return -1;
	wpa_config_set(ssid, "phase1", "\"pbc=1\"", 0);
	eloop_register_timeout(WPS_PBC_WALK_TIME, 0, wpas_wps_timeout,
			       wpa_s, NULL);
	wpas_wps_reassoc(wpa_s, ssid);
	return 0;
}


int wpas_wps_start_pin(struct wpa_supplicant *wpa_s, const u8 *bssid,
		       const char *pin)
{
	struct wpa_ssid *ssid;
	char val[30];
	unsigned int rpin = 0;

	wpas_clear_wps(wpa_s);
	ssid = wpas_wps_add_network(wpa_s, 0, bssid);
	if (ssid == NULL)
		return -1;
	if (pin)
		os_snprintf(val, sizeof(val), "\"pin=%s\"", pin);
	else {
		rpin = wps_generate_pin();
		os_snprintf(val, sizeof(val), "\"pin=%08d\"", rpin);
	}
	wpa_config_set(ssid, "phase1", val, 0);
	eloop_register_timeout(WPS_PBC_WALK_TIME, 0, wpas_wps_timeout,
			       wpa_s, NULL);
	wpas_wps_reassoc(wpa_s, ssid);
	return rpin;
}


int wpas_wps_start_reg(struct wpa_supplicant *wpa_s, const u8 *bssid,
		       const char *pin)
{
	struct wpa_ssid *ssid;
	char val[30];

	if (!pin)
		return -1;
	wpas_clear_wps(wpa_s);
	ssid = wpas_wps_add_network(wpa_s, 1, bssid);
	if (ssid == NULL)
		return -1;
	os_snprintf(val, sizeof(val), "\"pin=%s\"", pin);
	wpa_config_set(ssid, "phase1", val, 0);
	eloop_register_timeout(WPS_PBC_WALK_TIME, 0, wpas_wps_timeout,
			       wpa_s, NULL);
	wpas_wps_reassoc(wpa_s, ssid);
	return 0;
}


int wpas_wps_init(struct wpa_supplicant *wpa_s)
{
	struct wps_context *wps;

	wps = os_zalloc(sizeof(*wps));
	if (wps == NULL)
		return -1;

	wps->cred_cb = wpa_supplicant_wps_cred;
	wps->cb_ctx = wpa_s;

	wps->dev.device_name = wpa_s->conf->device_name;
	wps->dev.manufacturer = wpa_s->conf->manufacturer;
	wps->dev.model_name = wpa_s->conf->model_name;
	wps->dev.model_number = wpa_s->conf->model_number;
	wps->dev.serial_number = wpa_s->conf->serial_number;
	if (wpa_s->conf->device_type) {
		char *pos;
		u8 oui[4];
		/* <categ>-<OUI>-<subcateg> */
		wps->dev.categ = atoi(wpa_s->conf->device_type);
		pos = os_strchr(wpa_s->conf->device_type, '-');
		if (pos == NULL) {
			wpa_printf(MSG_ERROR, "WPS: Invalid device_type");
			os_free(wps);
			return -1;
		}
		pos++;
		if (hexstr2bin(pos, oui, 4)) {
			wpa_printf(MSG_ERROR, "WPS: Invalid device_type OUI");
			os_free(wps);
			return -1;
		}
		wps->dev.oui = WPA_GET_BE32(oui);
		pos = os_strchr(pos, '-');
		if (pos == NULL) {
			wpa_printf(MSG_ERROR, "WPS: Invalid device_type");
			os_free(wps);
			return -1;
		}
		pos++;
		wps->dev.sub_categ = atoi(pos);
	}
	wps->dev.os_version = WPA_GET_BE32(wpa_s->conf->os_version);
	wps->dev.rf_bands = WPS_RF_24GHZ | WPS_RF_50GHZ; /* TODO: config */
	os_memcpy(wps->dev.mac_addr, wpa_s->own_addr, ETH_ALEN);
	os_memcpy(wps->uuid, wpa_s->conf->uuid, 16);

	wpa_s->wps = wps;

	return 0;
}


void wpas_wps_deinit(struct wpa_supplicant *wpa_s)
{
	eloop_cancel_timeout(wpas_wps_timeout, wpa_s, NULL);

	if (wpa_s->wps == NULL)
		return;

	os_free(wpa_s->wps->network_key);
	os_free(wpa_s->wps);
	wpa_s->wps = NULL;
}


int wpas_wps_ssid_bss_match(struct wpa_ssid *ssid, struct wpa_scan_res *bss)
{
	struct wpabuf *wps_ie;

	if (!(ssid->key_mgmt & WPA_KEY_MGMT_WPS))
		return -1;

	wps_ie = wpa_scan_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
	if (eap_is_wps_pbc_enrollee(&ssid->eap)) {
		if (!wps_ie) {
			wpa_printf(MSG_DEBUG, "   skip - non-WPS AP");
			return 0;
		}

		if (!wps_is_selected_pbc_registrar(wps_ie)) {
			wpa_printf(MSG_DEBUG, "   skip - WPS AP "
				   "without active PBC Registrar");
			wpabuf_free(wps_ie);
			return 0;
		}

		/* TODO: overlap detection */
		wpa_printf(MSG_DEBUG, "   selected based on WPS IE "
			   "(Active PBC)");
		wpabuf_free(wps_ie);
		return 1;
	}

	if (eap_is_wps_pin_enrollee(&ssid->eap)) {
		if (!wps_ie) {
			wpa_printf(MSG_DEBUG, "   skip - non-WPS AP");
			return 0;
		}

		if (!wps_is_selected_pin_registrar(wps_ie)) {
			wpa_printf(MSG_DEBUG, "   skip - WPS AP "
				   "without active PIN Registrar");
			wpabuf_free(wps_ie);
			return 0;
		}
		wpa_printf(MSG_DEBUG, "   selected based on WPS IE "
			   "(Active PIN)");
		wpabuf_free(wps_ie);
		return 1;
	}

	if (wps_ie) {
		wpa_printf(MSG_DEBUG, "   selected based on WPS IE");
		wpabuf_free(wps_ie);
		return 1;
	}

	return -1;
}


int wpas_wps_ssid_wildcard_ok(struct wpa_ssid *ssid,
			      struct wpa_scan_res *bss)
{
	struct wpabuf *wps_ie = NULL;
	int ret = 0;

	if (eap_is_wps_pbc_enrollee(&ssid->eap)) {
		wps_ie = wpa_scan_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
		if (wps_ie && wps_is_selected_pbc_registrar(wps_ie)) {
			/* allow wildcard SSID for WPS PBC */
			ret = 1;
		}
	} else if (eap_is_wps_pin_enrollee(&ssid->eap)) {
		wps_ie = wpa_scan_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
		if (wps_ie && wps_is_selected_pin_registrar(wps_ie)) {
			/* allow wildcard SSID for WPS PIN */
			ret = 1;
		}
	}

	wpabuf_free(wps_ie);

	return ret;
}


int wpas_wps_scan_pbc_overlap(struct wpa_supplicant *wpa_s,
			      struct wpa_scan_res *selected,
			      struct wpa_ssid *ssid)
{
	const u8 *sel_uuid, *uuid;
	size_t i;
	struct wpabuf *wps_ie;
	int ret = 0;

	if (!eap_is_wps_pbc_enrollee(&ssid->eap))
		return 0;

	/* Make sure that only one AP is in active PBC mode */
	wps_ie = wpa_scan_get_vendor_ie_multi(selected, WPS_IE_VENDOR_TYPE);
	if (wps_ie)
		sel_uuid = wps_get_uuid_e(wps_ie);
	else
		sel_uuid = NULL;
	if (!sel_uuid) {
		wpa_printf(MSG_DEBUG, "WPS: UUID-E not available for PBC "
			   "overlap detection");
		wpabuf_free(wps_ie);
		return 1;
	}

	for (i = 0; i < wpa_s->scan_res->num; i++) {
		struct wpa_scan_res *bss = wpa_s->scan_res->res[i];
		struct wpabuf *ie;
		if (bss == selected)
			continue;
		ie = wpa_scan_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
		if (!ie)
			continue;
		if (!wps_is_selected_pbc_registrar(ie)) {
			wpabuf_free(ie);
			continue;
		}
		uuid = wps_get_uuid_e(ie);
		if (uuid == NULL) {
			wpa_printf(MSG_DEBUG, "WPS: UUID-E not available for "
				   "PBC overlap detection (other BSS)");
			ret = 1;
			wpabuf_free(ie);
			break;
		}
		if (os_memcmp(sel_uuid, uuid, 16) != 0) {
			ret = 1; /* PBC overlap */
			wpabuf_free(ie);
			break;
		}

		/* TODO: verify that this is reasonable dual-band situation */

		wpabuf_free(ie);
	}

	wpabuf_free(wps_ie);

	return ret;
}


void wpas_wps_notify_scan_results(struct wpa_supplicant *wpa_s)
{
	size_t i;

	if (wpa_s->disconnected || wpa_s->wpa_state >= WPA_ASSOCIATED)
		return;

	for (i = 0; i < wpa_s->scan_res->num; i++) {
		struct wpa_scan_res *bss = wpa_s->scan_res->res[i];
		struct wpabuf *ie;
		ie = wpa_scan_get_vendor_ie_multi(bss, WPS_IE_VENDOR_TYPE);
		if (!ie)
			continue;
		if (wps_is_selected_pbc_registrar(ie))
			wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_AP_AVAILABLE_PBC);
		else if (wps_is_selected_pin_registrar(ie))
			wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_AP_AVAILABLE_PIN);
		else
			wpa_msg(wpa_s, MSG_INFO, WPS_EVENT_AP_AVAILABLE);
		wpabuf_free(ie);
		break;
	}
}
