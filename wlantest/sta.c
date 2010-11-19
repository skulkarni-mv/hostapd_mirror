/*
 * STA list
 * Copyright (c) 2010, Jouni Malinen <j@w1.fi>
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

#include "utils/includes.h"

#include "utils/common.h"
#include "common/defs.h"
#include "common/ieee802_11_common.h"
#include "wlantest.h"


struct wlantest_sta * sta_get(struct wlantest_bss *bss, const u8 *addr)
{
	struct wlantest_sta *sta;

	if (addr[0] & 0x01)
		return NULL; /* Skip group addressed frames */

	dl_list_for_each(sta, &bss->sta, struct wlantest_sta, list) {
		if (os_memcmp(sta->addr, addr, ETH_ALEN) == 0)
			return sta;
	}

	sta = os_zalloc(sizeof(*sta));
	if (sta == NULL)
		return NULL;
	sta->bss = bss;
	os_memcpy(sta->addr, addr, ETH_ALEN);
	dl_list_add(&bss->sta, &sta->list);
	wpa_printf(MSG_DEBUG, "Discovered new STA " MACSTR " in BSS " MACSTR,
		   MAC2STR(sta->addr), MAC2STR(bss->bssid));
	return sta;
}


void sta_deinit(struct wlantest_sta *sta)
{
	dl_list_del(&sta->list);
	os_free(sta->assocreq_ies);
	os_free(sta);
}


void sta_update_assoc(struct wlantest_sta *sta, struct ieee802_11_elems *elems)
{
	struct wpa_ie_data data;
	struct wlantest_bss *bss = sta->bss;

	if (elems->wpa_ie && elems->rsn_ie) {
		wpa_printf(MSG_INFO, "Both WPA IE and RSN IE included in "
			   "Association Request frame from " MACSTR,
			   MAC2STR(sta->addr));
	}

	if (elems->rsn_ie) {
		wpa_hexdump(MSG_DEBUG, "RSN IE", elems->rsn_ie - 2,
			    elems->rsn_ie_len + 2);
		os_memcpy(sta->rsnie, elems->rsn_ie - 2,
			  elems->rsn_ie_len + 2);
		if (wpa_parse_wpa_ie_rsn(sta->rsnie, 2 + sta->rsnie[1], &data)
		    < 0) {
			wpa_printf(MSG_INFO, "Failed to parse RSN IE from "
				   MACSTR, MAC2STR(sta->addr));
		}
	} else if (elems->wpa_ie) {
		wpa_hexdump(MSG_DEBUG, "WPA IE", elems->wpa_ie - 2,
			    elems->wpa_ie_len + 2);
		os_memcpy(sta->rsnie, elems->wpa_ie - 2,
			  elems->wpa_ie_len + 2);
		if (wpa_parse_wpa_ie_wpa(sta->rsnie, 2 + sta->rsnie[1], &data)
		    < 0) {
			wpa_printf(MSG_INFO, "Failed to parse WPA IE from "
				   MACSTR, MAC2STR(sta->addr));
		}
	} else
		sta->rsnie[0] = 0;

	sta->proto = data.proto;
	sta->pairwise_cipher = data.pairwise_cipher;
	sta->key_mgmt = data.key_mgmt;
	sta->rsn_capab = data.capabilities;
	if (bss->proto && (sta->proto & bss->proto) == 0) {
		wpa_printf(MSG_INFO, "Mismatch in WPA/WPA2 proto: STA "
			   MACSTR " 0x%x  BSS " MACSTR " 0x%x",
			   MAC2STR(sta->addr), sta->proto,
			   MAC2STR(bss->bssid), bss->proto);
	}
	if (bss->pairwise_cipher &&
	    (sta->pairwise_cipher & bss->pairwise_cipher) == 0) {
		wpa_printf(MSG_INFO, "Mismatch in pairwise cipher: STA "
			   MACSTR " 0x%x  BSS " MACSTR " 0x%x",
			   MAC2STR(sta->addr), sta->pairwise_cipher,
			   MAC2STR(bss->bssid), bss->pairwise_cipher);
	}
	if (sta->proto && data.group_cipher != bss->group_cipher) {
		wpa_printf(MSG_INFO, "Mismatch in group cipher: STA "
			   MACSTR " 0x%x != BSS " MACSTR " 0x%x",
			   MAC2STR(sta->addr), data.group_cipher,
			   MAC2STR(bss->bssid), bss->group_cipher);
	}
	if ((bss->rsn_capab & WPA_CAPABILITY_MFPR) &&
	    !(sta->rsn_capab & WPA_CAPABILITY_MFPC)) {
		wpa_printf(MSG_INFO, "STA " MACSTR " tries to associate "
			   "without MFP to BSS " MACSTR " that advertises "
			   "MFPR", MAC2STR(sta->addr), MAC2STR(bss->bssid));
	}

	wpa_printf(MSG_INFO, "STA " MACSTR
		   " proto=%s%s%s"
		   "pairwise=%s%s%s%s"
		   "key_mgmt=%s%s%s%s%s%s%s%s"
		   "rsn_capab=%s%s%s%s%s",
		   MAC2STR(sta->addr),
		   sta->proto == 0 ? "OPEN " : "",
		   sta->proto & WPA_PROTO_WPA ? "WPA " : "",
		   sta->proto & WPA_PROTO_RSN ? "WPA2 " : "",
		   sta->pairwise_cipher == 0 ? "N/A " : "",
		   sta->pairwise_cipher & WPA_CIPHER_NONE ? "NONE " : "",
		   sta->pairwise_cipher & WPA_CIPHER_TKIP ? "TKIP " : "",
		   sta->pairwise_cipher & WPA_CIPHER_CCMP ? "CCMP " : "",
		   sta->key_mgmt == 0 ? "N/A " : "",
		   sta->key_mgmt & WPA_KEY_MGMT_IEEE8021X ? "EAP " : "",
		   sta->key_mgmt & WPA_KEY_MGMT_PSK ? "PSK " : "",
		   sta->key_mgmt & WPA_KEY_MGMT_WPA_NONE ? "WPA-NONE " : "",
		   sta->key_mgmt & WPA_KEY_MGMT_FT_IEEE8021X ? "FT-EAP " : "",
		   sta->key_mgmt & WPA_KEY_MGMT_FT_PSK ? "FT-PSK " : "",
		   sta->key_mgmt & WPA_KEY_MGMT_IEEE8021X_SHA256 ?
		   "EAP-SHA256 " : "",
		   sta->key_mgmt & WPA_KEY_MGMT_PSK_SHA256 ?
		   "PSK-SHA256 " : "",
		   sta->rsn_capab & WPA_CAPABILITY_PREAUTH ? "PREAUTH " : "",
		   sta->rsn_capab & WPA_CAPABILITY_NO_PAIRWISE ?
		   "NO_PAIRWISE " : "",
		   sta->rsn_capab & WPA_CAPABILITY_MFPR ? "MFPR " : "",
		   sta->rsn_capab & WPA_CAPABILITY_MFPC ? "MFPC " : "",
		   sta->rsn_capab & WPA_CAPABILITY_PEERKEY_ENABLED ?
		   "PEERKEY " : "");
}
