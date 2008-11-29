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

#ifndef WPS_SUPPLICANT_H
#define WPS_SUPPLICANT_H

#ifdef CONFIG_WPS

int wpas_wps_init(struct wpa_supplicant *wpa_s);
void wpas_wps_deinit(struct wpa_supplicant *wpa_s);
int wpas_wps_eapol_cb(struct wpa_supplicant *wpa_s);
u8 wpas_wps_get_req_type(struct wpa_ssid *ssid);
int wpas_wps_start_pbc(struct wpa_supplicant *wpa_s, const u8 *bssid);
int wpas_wps_start_pin(struct wpa_supplicant *wpa_s, const u8 *bssid,
		       const char *pin);
int wpas_wps_start_reg(struct wpa_supplicant *wpa_s, const u8 *bssid,
		       const char *pin);

#else /* CONFIG_WPS */

static inline int wpas_wps_init(struct wpa_supplicant *wpa_s)
{
	return 0;
}

static inline void wpas_wps_deinit(struct wpa_supplicant *wpa_s)
{
}

static inline int wpas_wps_eapol_cb(struct wpa_supplicant *wpa_s)
{
	return 0;
}

u8 wpas_wps_get_req_type(struct wpa_ssid *ssid)
{
	return 0;
}

#endif /* CONFIG_WPS */

#endif /* WPS_SUPPLICANT_H */
