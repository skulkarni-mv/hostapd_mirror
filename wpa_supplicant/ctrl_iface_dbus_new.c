/*
 * WPA Supplicant / dbus-based control interface
 * Copyright (c) 2006, Dan Williams <dcbw@redhat.com> and Red Hat, Inc.
 * Copyright (c) 2009, Witold Sowa <witold.sowa@gmail.com>
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
#include "config.h"
#include "wpa_supplicant_i.h"
#include "drivers/driver.h"
#include "wps/wps.h"
#include "ctrl_iface_dbus_new_helpers.h"
#include "dbus_dict_helpers.h"
#include "ctrl_iface_dbus_new.h"
#include "ctrl_iface_dbus_new_handlers.h"

/**
 * wpas_dbus_set_path - Assign a dbus path to an interface
 * @wpa_s: wpa_supplicant interface structure
 * @path: dbus path to set on the interface
 * Returns: 0 on success, -1 on error
 */
static int wpas_dbus_set_path(struct wpa_supplicant *wpa_s,
			      const char *path)
{
	u32 len = os_strlen(path);
	if (len >= WPAS_DBUS_OBJECT_PATH_MAX)
		return -1;
	if (wpa_s->dbus_new_path)
		return -1;
	wpa_s->dbus_new_path = os_strdup(path);
	return 0;
}


/**
 * wpas_dbus_signal_interface - Send a interface related event signal
 * @wpa_s: %wpa_supplicant network interface data
 * @sig_name: signal name - InterfaceAdded or InterfaceRemoved
 *
 * Notify listeners about event related with interface
 */
static void wpas_dbus_signal_interface(struct wpa_supplicant *wpa_s,
				       const char *sig_name)
{
	struct ctrl_iface_dbus_new_priv *iface;
	DBusMessage *_signal;
	const char *path;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (path == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_interface[dbus]: "
			   "Interface doesn't have a dbus path. "
			   "Can't send signal.");
		return;
	}
	_signal = dbus_message_new_signal(WPAS_DBUS_NEW_PATH,
					  WPAS_DBUS_NEW_INTERFACE, sig_name);
	if (_signal == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_interface[dbus]: "
			   "enough memory to send scan results signal.");
		return;
	}

	if (dbus_message_append_args(_signal, DBUS_TYPE_OBJECT_PATH, &path,
				     DBUS_TYPE_INVALID)) {
		dbus_connection_send(iface->con, _signal, NULL);
	} else {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_interface[dbus]: "
			   "not enough memory to construct signal.");
	}
	dbus_message_unref(_signal);
}


/**
 * wpas_dbus_signal_interface_created - Send a interface created signal
 * @wpa_s: %wpa_supplicant network interface data
 *
 * Notify listeners about creating new interface
 */
static void wpas_dbus_signal_interface_created(struct wpa_supplicant *wpa_s)
{
	wpas_dbus_signal_interface(wpa_s, "InterfaceCreated");
}


/**
 * wpas_dbus_signal_interface_removed - Send a interface removed signal
 * @wpa_s: %wpa_supplicant network interface data
 *
 * Notify listeners about removing interface
 */
static void wpas_dbus_signal_interface_removed(struct wpa_supplicant *wpa_s)
{
	wpas_dbus_signal_interface(wpa_s, "InterfaceRemoved");

}


/**
 * wpas_dbus_signal_scan_done - send scan done signal
 * @wpa_s: %wpa_supplicant network interface data
 * @success: indicates if scanning succeed or failed
 *
 * Notify listeners about finishing a scan
 */
static void wpas_dbus_signal_scan_done(struct wpa_supplicant *wpa_s,
				       int success)
{
	struct ctrl_iface_dbus_new_priv *iface;
	DBusMessage *_signal;
	const char *path;
	dbus_bool_t succ;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (path == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_scan_done[dbus]: "
			   "Interface doesn't have a dbus path. "
			   "Can't send signal.");
		return;
	}
	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_INTERFACE,
					  "ScanDone");
	if (_signal == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_scan_done[dbus]: "
			   "enough memory to send signal.");
		return;
	}

	succ = success ? TRUE : FALSE;
	if (dbus_message_append_args(_signal, DBUS_TYPE_BOOLEAN, &succ,
				     DBUS_TYPE_INVALID)) {
		dbus_connection_send(iface->con, _signal, NULL);
	} else {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_scan_done[dbus]: "
			   "not enough memory to construct signal.");
	}
	dbus_message_unref(_signal);
}


/**
 * wpas_dbus_signal_blob - Send a BSS related event signal
 * @wpa_s: %wpa_supplicant network interface data
 * @bss_obj_path: BSS object path
 * @sig_name: signal name - BSSAdded or BSSRemoved
 *
 * Notify listeners about event related with BSS
 */
static void wpas_dbus_signal_bss(struct wpa_supplicant *wpa_s,
				 const char *bss_obj_path,
				 const char *sig_name)
{
	struct ctrl_iface_dbus_new_priv *iface;
	DBusMessage *_signal;
	const char *path;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (path == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_bss[dbus]: "
			   "Interface doesn't have a dbus path. "
			   "Can't send signal.");
		return;
	}
	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_INTERFACE,
					  sig_name);
	if (_signal == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_bss[dbus]: "
			   "enough memory to send signal.");
		return;
	}

	if (dbus_message_append_args(_signal, DBUS_TYPE_OBJECT_PATH,
				     &bss_obj_path, DBUS_TYPE_INVALID)) {
		dbus_connection_send(iface->con, _signal, NULL);
	} else {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_bss[dbus]: "
			   "not enough memory to construct signal.");
	}
	dbus_message_unref(_signal);
}


/**
 * wpas_dbus_signal_bss_added - Send a BSS added signal
 * @wpa_s: %wpa_supplicant network interface data
 * @bss_obj_path: new BSS object path
 *
 * Notify listeners about adding new BSS
 */
static void wpas_dbus_signal_bss_added(struct wpa_supplicant *wpa_s,
				       const char *bss_obj_path)
{
	wpas_dbus_signal_bss(wpa_s, bss_obj_path, "BSSAdded");
}


/**
 * wpas_dbus_signal_bss_removed - Send a BSS removed signal
 * @wpa_s: %wpa_supplicant network interface data
 * @bss_obj_path: BSS object path
 *
 * Notify listeners about removing BSS
 */
static void wpas_dbus_signal_bss_removed(struct wpa_supplicant *wpa_s,
					 const char *bss_obj_path)
{
	wpas_dbus_signal_bss(wpa_s, bss_obj_path, "BSSRemoved");
}


/**
 * wpas_dbus_signal_blob - Send a blob related event signal
 * @wpa_s: %wpa_supplicant network interface data
 * @name: blob name
 * @sig_name: signal name - BlobAdded or BlobRemoved
 *
 * Notify listeners about event related with blob
 */
static void wpas_dbus_signal_blob(struct wpa_supplicant *wpa_s,
				  const char *name, const char *sig_name)
{
	struct ctrl_iface_dbus_new_priv *iface;
	DBusMessage *_signal;
	const char *path;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (path == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_blob[dbus]: "
			   "Interface doesn't have a dbus path. "
			   "Can't send signal.");
		return;
	}
	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_INTERFACE,
					  sig_name);
	if (_signal == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_blob[dbus]: "
			   "enough memory to send signal.");
		return;
	}

	if (dbus_message_append_args(_signal, DBUS_TYPE_STRING, &name,
				     DBUS_TYPE_INVALID)) {
		dbus_connection_send(iface->con, _signal, NULL);
	} else {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_blob[dbus]: "
			   "not enough memory to construct signal.");
	}
	dbus_message_unref(_signal);
}


/**
 * wpas_dbus_signal_blob_added - Send a blob added signal
 * @wpa_s: %wpa_supplicant network interface data
 * @name: blob name
 *
 * Notify listeners about adding a new blob
 */
static void wpas_dbus_signal_blob_added(struct wpa_supplicant *wpa_s,
					const char *name)
{
	wpas_dbus_signal_blob(wpa_s, name, "BlobAdded");
}


/**
 * wpas_dbus_signal_blob_removed - Send a blob removed signal
 * @wpa_s: %wpa_supplicant network interface data
 * @name: blob name
 *
 * Notify listeners about removing blob
 */
static void wpas_dbus_signal_blob_removed(struct wpa_supplicant *wpa_s,
					  const char *name)
{
	wpas_dbus_signal_blob(wpa_s, name, "BlobRemoved");
}


/**
 * wpas_dbus_signal_network - Send a network related event signal
 * @wpa_s: %wpa_supplicant network interface data
 * @id: new network id
 * @sig_name: signal name - NetworkAdded, NetworkRemoved or NetworkSelected
 *
 * Notify listeners about event related with configured network
 */
static void wpas_dbus_signal_network(struct wpa_supplicant *wpa_s,
				     int id, const char *sig_name)
{
	struct ctrl_iface_dbus_new_priv *iface;
	DBusMessage *_signal;
	const char *path;
	char *net_obj_path;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (path == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_network[dbus]: "
			   "Interface doesn't have a dbus path. "
			   "Can't send signal.");
		return;
	}

	net_obj_path = os_zalloc(WPAS_DBUS_OBJECT_PATH_MAX);
	if (net_obj_path == NULL)
		return;
	os_snprintf(net_obj_path, WPAS_DBUS_OBJECT_PATH_MAX,
		    "%s/" WPAS_DBUS_NEW_NETWORKS_PART "/%u", path, id);

	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_INTERFACE,
					  sig_name);
	if (_signal == NULL) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_network[dbus]: "
			   "enough memory to send signal.");
		os_free(net_obj_path);
		return;
	}

	if (dbus_message_append_args(_signal, DBUS_TYPE_OBJECT_PATH,
				     &net_obj_path, DBUS_TYPE_INVALID)) {
		dbus_connection_send(iface->con, _signal, NULL);
	} else {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_network[dbus]: "
			   "not enough memory to construct signal.");
	}

	os_free(net_obj_path);
	dbus_message_unref(_signal);
}


/**
 * wpas_dbus_signal_network_added - Send a network added signal
 * @wpa_s: %wpa_supplicant network interface data
 * @id: new network id
 *
 * Notify listeners about adding new network
 */
static void wpas_dbus_signal_network_added(struct wpa_supplicant *wpa_s,
					   int id)
{
	wpas_dbus_signal_network(wpa_s, id, "NetworkAdded");
}


/**
 * wpas_dbus_signal_network_removed - Send a network removed signal
 * @wpa_s: %wpa_supplicant network interface data
 * @id: network id
 *
 * Notify listeners about removing a network
 */
static void wpas_dbus_signal_network_removed(struct wpa_supplicant *wpa_s,
					     int id)
{
	wpas_dbus_signal_network(wpa_s, id, "NetworkRemoved");
}


/**
 * wpas_dbus_signal_network_selected - Send a network selected signal
 * @wpa_s: %wpa_supplicant network interface data
 * @id: network id
 *
 * Notify listeners about selecting a network
 */
static void wpas_dbus_signal_network_selected(struct wpa_supplicant *wpa_s,
					      int id)
{
	wpas_dbus_signal_network(wpa_s, id, "NetworkSelected");
}


/**
 * wpas_dbus_signal_state_changed - Send a state changed signal
 * @wpa_s: %wpa_supplicant network interface data
 * @new_state: new state wpa_supplicant is entering
 * @old_state: old state wpa_supplicant is leaving
 *
 * Notify listeners that wpa_supplicant has changed state
 */
static void wpas_dbus_signal_state_changed(struct wpa_supplicant *wpa_s,
					   wpa_states new_state,
					   wpa_states old_state)
{
	struct ctrl_iface_dbus_new_priv *iface;
	DBusMessage *_signal = NULL;
	const char *path;
	char *new_state_str, *old_state_str;
	char *tmp;

	/* Do nothing if the control interface is not turned on */
	if (wpa_s->global == NULL)
		return;
	iface = wpa_s->global->dbus_new_ctrl_iface;
	if (iface == NULL)
		return;

	/* Only send signal if state really changed */
	if (new_state == old_state)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (path == NULL) {
		perror("wpas_dbus_signal_state_changed[dbus]: "
		       "interface didn't have a dbus path");
		wpa_printf(MSG_ERROR,
		           "wpas_dbus_signal_state_changed[dbus]: "
		           "interface didn't have a dbus path; can't send "
		           "signal.");
		return;
	}
	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_INTERFACE,
					  "StateChanged");
	if (_signal == NULL) {
		perror("wpas_dbus_signal_state_changed[dbus]: "
		       "couldn't create dbus signal; likely out of memory");
		wpa_printf(MSG_ERROR,
		           "wpas_dbus_signal_state_changed[dbus]: "
		           "couldn't create dbus signal; likely out of "
		           "memory.");
		return;
	}

	new_state_str = os_strdup(wpa_supplicant_state_txt(new_state));
	old_state_str = os_strdup(wpa_supplicant_state_txt(old_state));
	if (new_state_str == NULL || old_state_str == NULL) {
		perror("wpas_dbus_signal_state_changed[dbus]: "
		       "couldn't convert state strings");
		wpa_printf(MSG_ERROR,
		           "wpas_dbus_signal_state_changed[dbus]: "
		           "couldn't convert state strings.");
		goto out;
	}

	/* make state string lowercase to fit new DBus API convention */
	tmp = new_state_str;
	while (*tmp) {
		*tmp = tolower(*tmp);
		tmp++;
	}
	tmp = old_state_str;
	while (*tmp) {
		*tmp = tolower(*tmp);
		tmp++;
	}

	if (!dbus_message_append_args(_signal,
	                              DBUS_TYPE_STRING, &new_state_str,
	                              DBUS_TYPE_STRING, &old_state_str,
	                              DBUS_TYPE_INVALID)) {
		perror("wpas_dbus_signal_state_changed[dbus]: "
		       "not enough memory to construct state change signal.");
		wpa_printf(MSG_ERROR,
		           "wpas_dbus_signal_state_changed[dbus]: "
		           "not enough memory to construct state change "
		           "signal.");
		goto out;
	}

	dbus_connection_send(iface->con, _signal, NULL);

out:
	dbus_message_unref(_signal);
	os_free(new_state_str);
	os_free(old_state_str);
}


/**
 * wpas_dbus_signal_network_enabled_changed - Signals Enabled property changes
 * @wpa_s: %wpa_supplicant network interface data
 * @ssid: configured network which Enabled property has changed
 *
 * Sends PropertyChanged signals containing new value of Enabled property
 * for specified network
 */
static void wpas_dbus_signal_network_enabled_changed(
	struct wpa_supplicant *wpa_s, struct wpa_ssid *ssid)
{

	struct network_handler_args args = {wpa_s, ssid};

	char path[WPAS_DBUS_OBJECT_PATH_MAX];
	os_snprintf(path, WPAS_DBUS_OBJECT_PATH_MAX,
		    "%s/" WPAS_DBUS_NEW_NETWORKS_PART "/%d",
		    wpas_dbus_get_path(wpa_s), ssid->id);

	wpa_dbus_signal_property_changed(wpa_s->global->dbus_new_ctrl_iface,
					 (WPADBusPropertyAccessor)
					 wpas_dbus_getter_enabled, &args,
					 path, WPAS_DBUS_NEW_IFACE_NETWORK,
					 "Enabled");
}


#ifdef CONFIG_WPS

/**
 * wpas_dbus_signal_wps_event_success - Signals Success WPS event
 * @wpa_s: %wpa_supplicant network interface data
 *
 * Sends Event dbus signal with name "success" and empty dict as arguments
 */
static void wpas_dbus_signal_wps_event_success(struct wpa_supplicant *wpa_s)
{

	DBusMessage *_signal = NULL;
	DBusMessageIter iter, dict_iter;
	struct ctrl_iface_dbus_new_priv *iface;
	char *key = "success";
	const char *path;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (!path) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_success"
			   "[dbus]: interface has no dbus path set");
		return;
	}

	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_WPS,
					  "Event");
	if (!_signal) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_success"
			   "[dbus]: out of memory when creating a signal");
		return;
	}

	dbus_message_iter_init_append(_signal, &iter);

	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &key) ||
	    !wpa_dbus_dict_open_write(&iter, &dict_iter) ||
	    !wpa_dbus_dict_close_write(&iter, &dict_iter)) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_success"
			   "[dbus]: out of memory");
		goto out;
	}

	dbus_connection_send(iface->con, _signal, NULL);
out:
	dbus_message_unref(_signal);
}


/**
 * wpas_dbus_signal_wps_event_fail - Signals Fail WPS event
 * @wpa_s: %wpa_supplicant network interface data
 *
 * Sends Event dbus signal with name "fail" and dictionary containing
 * "msg field with fail message number (int32) as arguments
 */
static void wpas_dbus_signal_wps_event_fail(struct wpa_supplicant *wpa_s,
					    struct wps_event_fail *fail)
{

	DBusMessage *_signal = NULL;
	DBusMessageIter iter, dict_iter;
	struct ctrl_iface_dbus_new_priv *iface;
	char *key = "fail";
	const char *path;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (!path) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_fail[dbus]: "
			   "interface has no dbus path set");
		return;
	}

	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_WPS,
					  "Event");
	if (!_signal) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_fail[dbus]: "
			   "out of memory when creating a signal");
		return;
	}

	dbus_message_iter_init_append(_signal, &iter);

	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &key) ||
	    !wpa_dbus_dict_open_write(&iter, &dict_iter) ||
	    !wpa_dbus_dict_append_int32(&dict_iter, "msg", fail->msg) ||
	    !wpa_dbus_dict_close_write(&iter, &dict_iter)) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_fail[dbus]: "
			   "out of memory");
		goto out;
	}

	dbus_connection_send(iface->con, _signal, NULL);
out:
	dbus_message_unref(_signal);
}


/**
 * wpas_dbus_signal_wps_event_m2d - Signals M2D WPS event
 * @wpa_s: %wpa_supplicant network interface data
 *
 * Sends Event dbus signal with name "m2d" and dictionary containing
 * fields of wps_event_m2d structure.
 */
static void wpas_dbus_signal_wps_event_m2d(struct wpa_supplicant *wpa_s,
					   struct wps_event_m2d *m2d)
{

	DBusMessage *_signal = NULL;
	DBusMessageIter iter, dict_iter;
	struct ctrl_iface_dbus_new_priv *iface;
	char *key = "m2d";
	const char *path;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (!path) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_m2d[dbus]: "
			   "interface has no dbus path set");
		return;
	}

	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_WPS,
					  "Event");
	if (!_signal) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_m2d[dbus]: "
			   "out of memory when creating a signal");
		return;
	}

	dbus_message_iter_init_append(_signal, &iter);

	if (!(dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &key) &&
	      wpa_dbus_dict_open_write(&iter, &dict_iter) &&
	      wpa_dbus_dict_append_uint16(&dict_iter, "config_methods",
					  m2d->config_methods) &&
	      wpa_dbus_dict_append_byte_array(&dict_iter, "manufacturer",
					      (const char *) m2d->manufacturer,
					      m2d->manufacturer_len) &&
	      wpa_dbus_dict_append_byte_array(&dict_iter, "model_name",
					      (const char *) m2d->model_name,
					      m2d->model_name_len) &&
	      wpa_dbus_dict_append_byte_array(&dict_iter, "model_number",
					      (const char *) m2d->model_number,
					      m2d->model_number_len) &&
	      wpa_dbus_dict_append_byte_array(&dict_iter, "serial_number",
					      (const char *)
					      m2d->serial_number,
					      m2d->serial_number_len) &&
	      wpa_dbus_dict_append_byte_array(&dict_iter, "dev_name",
					      (const char *) m2d->dev_name,
					      m2d->dev_name_len) &&
	      wpa_dbus_dict_append_byte_array(&dict_iter, "primary_dev_type",
					      (const char *)
					      m2d->primary_dev_type, 8) &&
	      wpa_dbus_dict_append_uint16(&dict_iter, "config_error",
					  m2d->config_error) &&
	      wpa_dbus_dict_append_uint16(&dict_iter, "dev_password_id",
					  m2d->dev_password_id) &&
	      wpa_dbus_dict_close_write(&iter, &dict_iter))) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_event_m2d[dbus]: "
			   "out of memory");
		goto out;
	}

	dbus_connection_send(iface->con, _signal, NULL);
out:
	dbus_message_unref(_signal);
}


/**
 * wpas_dbus_signal_wps_cred - Signals new credentials
 * @wpa_s: %wpa_supplicant network interface data
 *
 * Sends signal with credentials in directory argument
 */
static void wpas_dbus_signal_wps_cred(struct wpa_supplicant *wpa_s,
				      const struct wps_credential *cred)
{
	DBusMessage *_signal = NULL;
	DBusMessageIter iter, dict_iter;
	struct ctrl_iface_dbus_new_priv *iface;
	const char *path;
	char *auth_type[6]; /* we have six possible authorization types */
	int at_num = 0;
	char *encr_type[4]; /* we have four possible encryption types */
	int et_num = 0;

	iface = wpa_s->global->dbus_new_ctrl_iface;

	/* Do nothing if the control interface is not turned on */
	if (iface == NULL)
		return;

	path = wpas_dbus_get_path(wpa_s);
	if (!path) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_cred[dbus]: "
			   "interface has no dbus path set");
		return;
	}

	_signal = dbus_message_new_signal(path, WPAS_DBUS_NEW_IFACE_WPS,
					  "Credentials");
	if (!_signal) {
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_wps_cred[dbus]: "
			   "out of memory when creating a signal");
		return;
	}

	dbus_message_iter_init_append(_signal, &iter);

	if (!wpa_dbus_dict_open_write(&iter, &dict_iter)) {
		perror("wpas_dbus_signal_wps_cred[dbus]: out of memory "
		       "when opening a dictionary");
		goto nomem;
	}

	if (cred->auth_type & WPS_AUTH_OPEN)
		auth_type[at_num++] = "open";
	if (cred->auth_type & WPS_AUTH_WPAPSK)
		auth_type[at_num++] = "wpa-psk";
	if (cred->auth_type & WPS_AUTH_SHARED)
		auth_type[at_num++] = "shared";
	if (cred->auth_type & WPS_AUTH_WPA)
		auth_type[at_num++] = "wpa-eap";
	if (cred->auth_type & WPS_AUTH_WPA2)
		auth_type[at_num++] = "wpa2-eap";
	if (cred->auth_type & WPS_AUTH_WPA2PSK)
		auth_type[at_num++] =
		"wpa2-psk";

	if (cred->encr_type & WPS_ENCR_NONE)
		encr_type[et_num++] = "none";
	if (cred->encr_type & WPS_ENCR_WEP)
		encr_type[et_num++] = "wep";
	if (cred->encr_type & WPS_ENCR_TKIP)
		encr_type[et_num++] = "tkip";
	if (cred->encr_type & WPS_ENCR_AES)
		encr_type[et_num++] = "aes";

	if (wpa_s->current_ssid) {
		if (!wpa_dbus_dict_append_byte_array(
			    &dict_iter, "BSSID",
			    (const char *) wpa_s->current_ssid->bssid,
			    ETH_ALEN)) {
			perror("wpas_dbus_signal_wps_cred[dbus]: out of "
			       "memory when appending bssid to dictionary");
			goto nomem;
		}
	}

	if (!(wpa_dbus_dict_append_byte_array(&dict_iter, "SSID",
					      (const char *) cred->ssid,
					      cred->ssid_len) &&
	      wpa_dbus_dict_append_string_array(&dict_iter, "AuthType",
						(const char **) auth_type,
						at_num) &&
	      wpa_dbus_dict_append_string_array(&dict_iter, "EncrType",
						(const char **) encr_type,
						et_num) &&
	      wpa_dbus_dict_append_byte_array(&dict_iter, "Key",
					      (const char *) cred->key,
					      cred->key_len) &&
	      wpa_dbus_dict_append_uint32(&dict_iter, "KeyIndex",
					  cred->key_idx))) {
		perror("wpas_dbus_signal_wps_cred[dbus]: out of memory "
		       "when appending to dictionary");
		goto nomem;
	}

	if (!wpa_dbus_dict_close_write(&iter, &dict_iter)) {
		perror("wpas_dbus_signal_wps_cred[dbus]: out of memory "
		       "when closing a dictionary");
		goto nomem;
	}

	dbus_connection_send(iface->con, _signal, NULL);

nomem:
	dbus_message_unref(_signal);
}

#endif /* CONFIG_WPS */


/**
 * wpas_dbus_signal_prop_changed - Signals change of property
 * @wpa_s: %wpa_supplicant network interface data
 * @property: indicates which property has changed
 *
 * Sends ProertyChanged signals with path, interface and arguments
 * depending on which property has changed.
 */
static void wpas_dbus_signal_prop_changed(struct wpa_supplicant *wpa_s,
					  enum wpas_dbus_prop property)
{
	WPADBusPropertyAccessor getter;
	char *iface;
	char *prop;
	void *arg;

	switch (property) {
	case WPAS_DBUS_PROP_AP_SCAN:
		getter = (WPADBusPropertyAccessor) wpas_dbus_getter_ap_scan;
		arg = wpa_s;
		iface = WPAS_DBUS_NEW_IFACE_INTERFACE;
		prop = "ApScan";
		break;
	case WPAS_DBUS_PROP_SCANNING:
		getter = (WPADBusPropertyAccessor) wpas_dbus_getter_scanning;
		arg = wpa_s;
		iface = WPAS_DBUS_NEW_IFACE_INTERFACE;
		prop = "Scanning";
		break;
	case WPAS_DBUS_PROP_CURRENT_BSS:
		getter = (WPADBusPropertyAccessor)
			wpas_dbus_getter_current_bss;
		arg = wpa_s;
		iface = WPAS_DBUS_NEW_IFACE_INTERFACE;
		prop = "CurrentBSS";
		break;
	case WPAS_DBUS_PROP_CURRENT_NETWORK:
		getter = (WPADBusPropertyAccessor)
			wpas_dbus_getter_current_network;
		arg = wpa_s;
		iface = WPAS_DBUS_NEW_IFACE_INTERFACE;
		prop = "CurrentNetwork";
		break;
	default:
		wpa_printf(MSG_ERROR, "wpas_dbus_signal_prop_changed[dbus]: "
			   "Unknown Property enum value %d", property);
		return;
	}

	wpa_dbus_signal_property_changed(wpa_s->global->dbus_new_ctrl_iface,
					 getter, arg,
					 wpas_dbus_get_path(wpa_s), iface,
					 prop);
}


/**
 * wpas_dbus_signal_debug_params_changed - Signals change of debug params
 * @global: wpa_global structure
 *
 * Sends ProertyChanged signals informing that debug params has changed.
 */
static void wpas_dbus_signal_debug_params_changed(struct wpa_global *global)
{

	wpa_dbus_signal_property_changed(global->dbus_new_ctrl_iface,
					 (WPADBusPropertyAccessor)
					 wpas_dbus_getter_debug_params,
					 global, WPAS_DBUS_NEW_PATH,
					 WPAS_DBUS_NEW_INTERFACE,
					 "DebugParams");
}


static void wpas_dbus_meth_reg_create_interface(
	struct wpa_global *global, struct wpa_dbus_object_desc *obj_desc)
{
	struct wpa_dbus_argument margs[] = {
		{ "args", "a{sv}", ARG_IN },
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				 "CreateInterface",
				 (WPADBusMethodHandler)
				 &wpas_dbus_handler_create_interface,
				 global, NULL, margs);
}


static void wpas_dbus_meth_reg_remove_interface(
	struct wpa_global *global, struct wpa_dbus_object_desc *obj_desc)
{
	struct wpa_dbus_argument margs[] = {
		{ "path", "o", ARG_IN },
		END_ARGS
	};
	wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				 "RemoveInterface",
				 (WPADBusMethodHandler)
				 &wpas_dbus_handler_remove_interface,
				 global, NULL, margs);
}


static void wpas_dbus_meth_reg_get_interface(
	struct wpa_global *global, struct wpa_dbus_object_desc *obj_desc)
{
	struct wpa_dbus_argument margs[] = {
		{ "ifname", "s", ARG_IN },
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				 "GetInterface",
				 (WPADBusMethodHandler)
				 &wpas_dbus_handler_get_interface,
				 global, NULL, margs);
}


static void wpas_dbus_prop_reg_debug_params(
	struct wpa_global *global, struct wpa_dbus_object_desc *obj_desc)
{
	wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				   "DebugParams", "(ibb)",
				   (WPADBusPropertyAccessor)
				   &wpas_dbus_getter_debug_params,
				   (WPADBusPropertyAccessor)
				   &wpas_dbus_setter_debug_params,
				   global, NULL, RW);
}


static void wpas_dbus_prop_reg_interfaces(
	struct wpa_global *global, struct wpa_dbus_object_desc *obj_desc)
{
	wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				   "Interfaces", "ao",
				   (WPADBusPropertyAccessor)
				   &wpas_dbus_getter_interfaces,
				   NULL, global, NULL, R);
}


static void wpas_dbus_prop_reg_eap_methods(
	struct wpa_dbus_object_desc *obj_desc)
{
	wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				   "EapMethods", "as",
				   wpas_dbus_getter_eap_methods,
				   NULL, NULL, NULL, R);
}


static void wpas_dbus_sign_reg_interface_added(
	struct wpa_global *global, struct wpa_dbus_object_desc *obj_desc)
{
	struct wpa_dbus_argument sargs[] = {
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				 "InterfaceAdded", sargs);
}


static void wpas_dbus_sign_reg_interface_removed(
	struct wpa_global *global, struct wpa_dbus_object_desc *obj_desc)
{
	struct wpa_dbus_argument sargs[] = {
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				 "InterfaceRemoved", sargs);
}


static void wpas_dbus_sign_reg_properties_changed(
	struct wpa_global *global, struct wpa_dbus_object_desc *obj_desc)
{
	struct wpa_dbus_argument sargs[] = {
		{ "properties", "a{sv}", ARG_OUT },
		END_ARGS
	};
	wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_INTERFACE,
				 "PropertiesChanged", sargs);
}


/**
 * wpas_dbus_ctrl_iface_init - Initialize dbus control interface
 * @global: Pointer to global data from wpa_supplicant_init()
 * Returns: Pointer to dbus_new_ctrl_iface date or %NULL on failure
 *
 * Initialize the dbus control interface for wpa_supplicantand and start
 * receiving commands from external programs over the bus.
 */
static struct ctrl_iface_dbus_new_priv * wpas_dbus_ctrl_iface_init(
	struct wpa_global *global)
{
	struct ctrl_iface_dbus_new_priv *ctrl_iface;
	struct wpa_dbus_object_desc *obj_desc;

	obj_desc = os_zalloc(sizeof(struct wpa_dbus_object_desc));
	if (!obj_desc) {
		wpa_printf(MSG_ERROR, "Not enough memory "
			   "to create object description");
		return NULL;
	}

	wpas_dbus_meth_reg_create_interface(global, obj_desc);
	wpas_dbus_meth_reg_remove_interface(global, obj_desc);
	wpas_dbus_meth_reg_get_interface(global, obj_desc);

	wpas_dbus_prop_reg_debug_params(global, obj_desc);
	wpas_dbus_prop_reg_interfaces(global, obj_desc);
	wpas_dbus_prop_reg_eap_methods(obj_desc);

	wpas_dbus_sign_reg_interface_added(global, obj_desc);
	wpas_dbus_sign_reg_interface_removed(global, obj_desc);
	wpas_dbus_sign_reg_properties_changed(global, obj_desc);

	ctrl_iface = wpa_dbus_ctrl_iface_init(global, WPAS_DBUS_NEW_PATH,
					      WPAS_DBUS_NEW_SERVICE,
					      obj_desc);
	if (!ctrl_iface)
		free_dbus_object_desc(obj_desc);

	return ctrl_iface;
}


/**
 * wpas_dbus_ctrl_iface_deinit - Deinitialize dbus ctrl interface for
 * wpa_supplicant
 * @iface: Pointer to dbus private data from
 * wpas_dbus_ctrl_iface_init()
 *
 * Deinitialize the dbus control interface that was initialized with
 * wpas_dbus_ctrl_iface_init().
 */
static void wpas_dbus_ctrl_iface_deinit(struct ctrl_iface_dbus_new_priv *iface)
{
	if (iface) {
		dbus_connection_unregister_object_path(iface->con,
						       WPAS_DBUS_NEW_PATH);
		wpa_dbus_ctrl_iface_deinit(iface);
	}
}


/**
 * wpas_dbus_register_network - Register a configured network with dbus
 * @wpa_s: wpa_supplicant interface structure
 * @ssid: network configuration data
 * Returns: 0 on success, -1 on failure
 *
 * Registers network representing object with dbus
 */
static int wpas_dbus_register_network(struct wpa_supplicant *wpa_s,
				      struct wpa_ssid *ssid)
{
	struct ctrl_iface_dbus_new_priv *ctrl_iface;
	struct wpa_dbus_object_desc *obj_desc;

	struct network_handler_args *arg1 = NULL;
	struct network_handler_args *arg2 = NULL;
	struct network_handler_args *arg3 = NULL;

	char *net_obj_path;

	struct wpa_dbus_argument sargs[] = {
		{ "properties", "a{sv}", ARG_OUT },
		END_ARGS
	};

	/* Do nothing if the control interface is not turned on */
	if (wpa_s == NULL || wpa_s->global == NULL)
		return 0;
	ctrl_iface = wpa_s->global->dbus_new_ctrl_iface;
	if (ctrl_iface == NULL)
		return 0;

	net_obj_path = os_zalloc(WPAS_DBUS_OBJECT_PATH_MAX);
	if (net_obj_path == NULL)
		return -1;
	os_snprintf(net_obj_path, WPAS_DBUS_OBJECT_PATH_MAX,
		    "%s/" WPAS_DBUS_NEW_NETWORKS_PART "/%u",
		    wpas_dbus_get_path(wpa_s), ssid->id);

	obj_desc = os_zalloc(sizeof(struct wpa_dbus_object_desc));
	if (!obj_desc) {
		wpa_printf(MSG_ERROR, "Not enough memory "
			   "to create object description");
		goto err;
	}

	/* allocate memory for handlers arguments */
	arg1 =	os_zalloc(sizeof(struct network_handler_args));
	if (!arg1) {
		wpa_printf(MSG_ERROR, "Not enough memory "
			   "to create arguments for method");
		goto err;
	}
	arg2 =	os_zalloc(sizeof(struct network_handler_args));
	if (!arg2) {
		wpa_printf(MSG_ERROR, "Not enough memory "
			   "to create arguments for method");
		goto err;
	}

	arg1->wpa_s = wpa_s;
	arg1->ssid = ssid;
	arg2->wpa_s = wpa_s;
	arg2->ssid = ssid;

	/* Enabled property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_NETWORK,
				       "Enabled", "b",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_enabled,
				       (WPADBusPropertyAccessor)
				       wpas_dbus_setter_enabled,
				       arg1, free, RW)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Enabled",
			   WPAS_DBUS_NEW_IFACE_NETWORK);
	}

	/* Properties property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_NETWORK,
				       "Properties", "a{sv}",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_network_properties,
				       (WPADBusPropertyAccessor)
				       wpas_dbus_setter_network_properties,
				       arg2, free, RW)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Properties",
			   WPAS_DBUS_NEW_IFACE_NETWORK);
	}

	/* PropertiesChanged signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_NETWORK,
				     "PropertiesChanged", sargs)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "PropertiesChanged",
			   WPAS_DBUS_NEW_IFACE_NETWORK);
	}


	if (wpa_dbus_register_object_per_iface(ctrl_iface, net_obj_path,
					       wpa_s->ifname, obj_desc))
		goto err;

	wpas_dbus_signal_network_added(wpa_s, ssid->id);

	os_free(net_obj_path);
	return 0;

err:
	os_free(net_obj_path);
	os_free(obj_desc);
	os_free(arg1);
	os_free(arg2);
	os_free(arg3);
	return -1;
}


/**
 * wpas_dbus_unregister_network - Unregister a configured network from dbus
 * @wpa_s: wpa_supplicant interface structure
 * @nid: network id
 * Returns: 0 on success, -1 on failure
 *
 * Unregisters network representing object from dbus
 */
static int wpas_dbus_unregister_network(struct wpa_supplicant *wpa_s, int nid)
{
	struct ctrl_iface_dbus_new_priv *ctrl_iface;
	char *net_obj_path;
	int ret;

	/* Do nothing if the control interface is not turned on */
	if (wpa_s == NULL || wpa_s->global == NULL)
		return 0;
	ctrl_iface = wpa_s->global->dbus_new_ctrl_iface;
	if (ctrl_iface == NULL)
		return 0;

	net_obj_path = os_zalloc(WPAS_DBUS_OBJECT_PATH_MAX);
	if (net_obj_path == NULL)
		return -1;
	os_snprintf(net_obj_path, WPAS_DBUS_OBJECT_PATH_MAX,
		    "%s/" WPAS_DBUS_NEW_NETWORKS_PART "/%u",
		    wpas_dbus_get_path(wpa_s), nid);

	ret = wpa_dbus_unregister_object_per_iface(ctrl_iface, net_obj_path);

	if (!ret)
		wpas_dbus_signal_network_removed(wpa_s, nid);

	os_free(net_obj_path);
	return ret;
}


/**
 * wpas_dbus_unregister_bss - Unregister a scanned BSS from dbus
 * @wpa_s: wpa_supplicant interface structure
 * @bssid: scanned network bssid
 * Returns: 0 on success, -1 on failure
 *
 * Unregisters BSS representing object from dbus
 */
static int wpas_dbus_unregister_bss(struct wpa_supplicant *wpa_s,
				    u8 bssid[ETH_ALEN])
{
	struct ctrl_iface_dbus_new_priv *ctrl_iface;
	char *bss_obj_path;

	/* Do nothing if the control interface is not turned on */
	if (wpa_s == NULL || wpa_s->global == NULL)
		return 0;
	ctrl_iface = wpa_s->global->dbus_new_ctrl_iface;
	if (ctrl_iface == NULL)
		return 0;

	bss_obj_path = os_zalloc(WPAS_DBUS_OBJECT_PATH_MAX);
	if (bss_obj_path == NULL)
		return -1;

	os_snprintf(bss_obj_path, WPAS_DBUS_OBJECT_PATH_MAX,
		    "%s/" WPAS_DBUS_NEW_BSSIDS_PART "/" WPAS_DBUS_BSSID_FORMAT,
		    wpas_dbus_get_path(wpa_s), MAC2STR(bssid));

	if (wpa_dbus_unregister_object_per_iface(ctrl_iface, bss_obj_path)) {
		wpa_printf(MSG_ERROR,
			   "Cannot unregister BSSID dbus object %s.",
			   bss_obj_path);
		os_free(bss_obj_path);
		return -1;
	}

	wpas_dbus_signal_bss_removed(wpa_s, bss_obj_path);

	os_free(bss_obj_path);
	return 0;
}


/**
 * wpas_dbus_register_bss - Register a scanned BSS with dbus
 * @wpa_s: wpa_supplicant interface structure
 * @bssid: scanned network bssid
 * Returns: 0 on success, -1 on failure
 *
 * Registers BSS representing object with dbus
 */
static int wpas_dbus_register_bss(struct wpa_supplicant *wpa_s,
				  u8 bssid[ETH_ALEN])
{
	struct ctrl_iface_dbus_new_priv *ctrl_iface;
	struct wpa_dbus_object_desc *obj_desc;
	char *bss_obj_path;

	struct bss_handler_args *arg = NULL;

	/* Do nothing if the control interface is not turned on */
	if (wpa_s == NULL || wpa_s->global == NULL)
		return 0;
	ctrl_iface = wpa_s->global->dbus_new_ctrl_iface;
	if (ctrl_iface == NULL)
		return 0;

	bss_obj_path = os_zalloc(WPAS_DBUS_OBJECT_PATH_MAX);
	if (bss_obj_path == NULL)
		return -1;

	os_snprintf(bss_obj_path, WPAS_DBUS_OBJECT_PATH_MAX,
		    "%s/" WPAS_DBUS_NEW_BSSIDS_PART "/" WPAS_DBUS_BSSID_FORMAT,
		    wpas_dbus_get_path(wpa_s), MAC2STR(bssid));

	obj_desc = os_zalloc(sizeof(struct wpa_dbus_object_desc));
	if (!obj_desc) {
		wpa_printf(MSG_ERROR, "Not enough memory "
			   "to create object description");
		goto err;
	}

	arg = os_zalloc(sizeof(struct bss_handler_args));
	if (!arg) {
		wpa_printf(MSG_ERROR, "Not enough memory "
			   "to create arguments for handler");
		goto err;
	}
	arg->wpa_s = wpa_s;
	os_memcpy(arg->bssid, bssid, ETH_ALEN);

	/* Properties property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_BSSID,
				       "Properties", "a{sv}",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_bss_properties, NULL,
				       arg, free, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Properties",
			   WPAS_DBUS_NEW_IFACE_BSSID);
	}

	if (wpa_dbus_register_object_per_iface(ctrl_iface, bss_obj_path,
					       wpa_s->ifname, obj_desc)) {
		wpa_printf(MSG_ERROR,
			   "Cannot register BSSID dbus object %s.",
			   bss_obj_path);
		goto err;
	}

	wpas_dbus_signal_bss_added(wpa_s, bss_obj_path);

	os_free(bss_obj_path);
	return 0;

err:
	os_free(bss_obj_path);
	os_free(obj_desc);
	os_free(arg);
	return -1;
}


static int wpas_dbus_register_interface(struct wpa_supplicant *wpa_s)
{

	struct wpa_dbus_object_desc *obj_desc = NULL;
	char *path;
	struct ctrl_iface_dbus_new_priv *ctrl_iface =
		wpa_s->global->dbus_new_ctrl_iface;
	int next;

	struct wpa_dbus_argument args1[] = {
		{ "args", "a{sv}", ARG_IN },
		END_ARGS
	};
	struct wpa_dbus_argument args3[] = {
		{ "args", "a{sv}", ARG_IN },
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument args4[] = {
		{ "path", "o", ARG_IN },
		END_ARGS
	};
	struct wpa_dbus_argument args5[] = {
		{ "path", "o", ARG_IN },
		END_ARGS
	};
	struct wpa_dbus_argument args6[] = {
		{ "name", "s", ARG_IN },
		{ "data", "ay", ARG_IN },
		END_ARGS
	};
	struct wpa_dbus_argument args7[] = {
		{ "name", "s", ARG_IN },
		{ "data", "ay", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument args8[] = {
		{ "name", "s", ARG_IN },
		END_ARGS
	};
	struct wpa_dbus_argument sargs1[] = {
		{ "success", "b", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs2[] = {
		{ "newState", "s", ARG_OUT },
		{ "oldState", "s", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs3[] = {
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs4[] = {
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs5[] = {
		{ "name", "s", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs6[] = {
		{ "name", "s", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs7[] = {
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs8[] = {
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs9[] = {
		{ "path", "o", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs10[] = {
		{ "properties", "a{sv}", ARG_OUT },
		END_ARGS
	};

#ifdef CONFIG_WPS
	struct wpa_dbus_argument args9[] = {
		{ "args", "a{sv}", ARG_IN },
		{ "output", "a{sv}", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs11[] = {
		{ "name", "s", ARG_OUT },
		{ "args", "a{sv}", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs12[] = {
		{ "credentials", "a{sv}", ARG_OUT },
		END_ARGS
	};
	struct wpa_dbus_argument sargs13[] = {
		{ "properties", "a{sv}", ARG_OUT },
		END_ARGS
	};
#endif /* CONFIG_WPS */
	/* Do nothing if the control interface is not turned on */
	if (ctrl_iface == NULL)
		return 0;

	/* Create and set the interface's object path */
	path = os_zalloc(WPAS_DBUS_OBJECT_PATH_MAX);
	if (path == NULL)
		return -1;
	next = wpa_dbus_next_objid(ctrl_iface);
	os_snprintf(path, WPAS_DBUS_OBJECT_PATH_MAX,
		    WPAS_DBUS_NEW_PATH_INTERFACES "/%u",
		    next);
	if (wpas_dbus_set_path(wpa_s, path)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to set dbus path for interface %s",
			   wpa_s->ifname);
		goto err;
	}

	obj_desc = os_zalloc(sizeof(struct wpa_dbus_object_desc));
	if (!obj_desc) {
		wpa_printf(MSG_ERROR, "Not enough memory "
			   "to create object description");
		goto err;
	}

	/* Scan method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "Scan",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_scan,
				     wpa_s, NULL, args1)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "Scan",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* Disconnect method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "Disconnect",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_disconnect,
				     wpa_s, NULL, NULL)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "Disconnect",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* AddNetwork method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "AddNetwork",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_add_network,
				     wpa_s, NULL, args3)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "AddNetwork",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* RemoveNetwork method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "RemoveNetwork",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_remove_network,
				     wpa_s, NULL, args4)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "RemoveNetwork",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* SelectNetwork method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "SelectNetwork",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_select_network,
				     wpa_s, NULL, args5)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "SelectNetwork",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* AddBlob method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "AddBlob",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_add_blob,
				     wpa_s, NULL, args6)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "AddBlob",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* GetBlob method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "GetBlob",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_get_blob,
				     wpa_s, NULL, args7)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "GetBlob",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* RemoveBlob method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "RemoveBlob",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_remove_blob,
				     wpa_s, NULL, args8)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "RemoveBlob",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* Capabilities property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "Capabilities", "a{sv}",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_capabilities, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Capabilities",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* State property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "State", "s",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_state, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "State",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* Scanning property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "Scanning", "b",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_scanning, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Scanning",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* ApScan property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "ApScan", "u",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_ap_scan,
				       (WPADBusPropertyAccessor)
				       wpas_dbus_setter_ap_scan,
				       wpa_s, NULL, RW)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "ApScan",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* Ifname property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "Ifname", "s",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_ifname, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Ifname",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* Driver property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "Driver", "s",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_driver, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Driver",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* BridgeIfname property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "BridgeIfname", "s",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_bridge_ifname, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "BridgeIfname",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* CurrentBSS property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "CurrentBSS", "o",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_current_bss, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "CurrentBSS",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* CurrentNetwork property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "CurrentNetwork", "o",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_current_network, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "CurrentNetwork",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* Blobs property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "Blobs", "a{say}",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_blobs, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Blobs",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* BSSs property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "BSSs", "ao",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_bsss, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "BSSs",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* Networks property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				       "Networks", "ao",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_networks, NULL,
				       wpa_s, NULL, R)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "Networks",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* ScanDone signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "ScanDone", sargs1)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "ScanDone",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* StateChanged signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "StateChanged", sargs2)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "StateChanged",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* BSSAdded signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "BSSAdded", sargs3)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "BSSAdded",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* BSSRemoved signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "BSSRemoved", sargs4)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "BSSRemoved",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* BlobAdded signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "BlobAdded", sargs5)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "BlobAdded",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* BlobRemoved signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "BlobRemoved", sargs6)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "BlobRemoved",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* NetworkAdded signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "NetworkAdded", sargs7)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "NetworkAdded",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* NetworkRemoved signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "NetworkRemoved", sargs8)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "NetworkRemoved",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* NetworkSelected signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "NetworkSelected", sargs9)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "NetworkSelected",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

	/* PropertiesChanged signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_INTERFACE,
				     "PropertiesChanged", sargs10)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "PropertiesChanged",
			   WPAS_DBUS_NEW_IFACE_INTERFACE);
	}

#ifdef CONFIG_WPS
	/* Start method */
	if (wpa_dbus_method_register(obj_desc, WPAS_DBUS_NEW_IFACE_WPS,
				     "Start",
				     (WPADBusMethodHandler)
				     &wpas_dbus_handler_wps_start,
				     wpa_s, NULL, args9)) {
		wpa_printf(MSG_DEBUG,
			   "Failed to register dbus method %s"
			   "in interface %s", "Start",
			   WPAS_DBUS_NEW_IFACE_WPS);
	}

	/* ProcessCredentials property */
	if (wpa_dbus_property_register(obj_desc, WPAS_DBUS_NEW_IFACE_WPS,
				       "ProcessCredentials", "b",
				       (WPADBusPropertyAccessor)
				       wpas_dbus_getter_process_credentials,
				       (WPADBusPropertyAccessor)
				       wpas_dbus_setter_process_credentials,
				       wpa_s, NULL, RW)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus property %s"
			   "in interface %s", "ProcessCredentials",
			   WPAS_DBUS_NEW_IFACE_WPS);
	}

	/* Event signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_WPS,
				     "Event", sargs11)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "Event",
			   WPAS_DBUS_NEW_IFACE_WPS);
	}

	/* Credentials signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_WPS,
				     "Credentials", sargs12)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "Credentials",
			   WPAS_DBUS_NEW_IFACE_WPS);
	}

	/* PropertiesChanged signal */
	if (wpa_dbus_signal_register(obj_desc, WPAS_DBUS_NEW_IFACE_WPS,
				     "PropertiesChanged", sargs13)) {
		wpa_printf(MSG_ERROR,
			   "Failed to register dbus signal %s"
			   "in interface %s", "PropertiesChanged",
			   WPAS_DBUS_NEW_IFACE_WPS);
	}
#endif /* CONFIG_WPS */

	if (wpa_dbus_register_object_per_iface(ctrl_iface, path, wpa_s->ifname,
					       obj_desc))
		goto err;

	wpas_dbus_signal_interface_created(wpa_s);

	os_free(path);
	return 0;

err:
	os_free(obj_desc);
	os_free(path);
	return -1;
}


static int wpas_dbus_unregister_interface(struct wpa_supplicant *wpa_s)
{
	struct ctrl_iface_dbus_new_priv *ctrl_iface;
	struct wpa_ssid *ssid;
	size_t i;

	/* Do nothing if the control interface is not turned on */
	if (wpa_s == NULL || wpa_s->global == NULL)
		return 0;
	ctrl_iface = wpa_s->global->dbus_new_ctrl_iface;
	if (ctrl_iface == NULL)
		return 0;

	/* unregister all BSSs and networks from dbus */
	for (i = 0; i < wpa_s->scan_res->num; i++) {
		wpas_dbus_unregister_bss(wpa_s,
					 wpa_s->scan_res->res[i]->bssid);
	}

	ssid = wpa_s->conf->ssid;
	while (ssid) {
		wpas_dbus_unregister_network(wpa_s, ssid->id);
		ssid = ssid->next;
	}

	if (wpa_dbus_unregister_object_per_iface(ctrl_iface,
						 wpas_dbus_get_path(wpa_s)))
		return -1;

	wpas_dbus_signal_interface_removed(wpa_s);

	os_free(wpa_s->dbus_new_path);
	wpa_s->dbus_new_path = NULL;

	return 0;
}


static struct wpas_dbus_callbacks callbacks =
{
	.dbus_ctrl_init = wpas_dbus_ctrl_iface_init,
	.dbus_ctrl_deinit = wpas_dbus_ctrl_iface_deinit,

	.signal_interface_created = wpas_dbus_signal_interface_created,
	.signal_interface_removed = wpas_dbus_signal_interface_removed,

	.register_interface = wpas_dbus_register_interface,
	.unregister_interface = wpas_dbus_unregister_interface,

	.signal_scan_done = wpas_dbus_signal_scan_done,

	.signal_blob_added = wpas_dbus_signal_blob_added,
	.signal_blob_removed = wpas_dbus_signal_blob_removed,

	.signal_network_selected = wpas_dbus_signal_network_selected,

	.signal_state_changed = wpas_dbus_signal_state_changed,
	.register_network = wpas_dbus_register_network,
	.unregister_network = wpas_dbus_unregister_network,

	.signal_network_enabled_changed =
	wpas_dbus_signal_network_enabled_changed,

	.register_bss = wpas_dbus_register_bss,
	.unregister_bss = wpas_dbus_unregister_bss,

	.signal_prop_changed = wpas_dbus_signal_prop_changed,
	.signal_debug_params_changed = wpas_dbus_signal_debug_params_changed,

#ifdef CONFIG_WPS
	.signal_wps_event_success = wpas_dbus_signal_wps_event_success,
	.signal_wps_event_fail = wpas_dbus_signal_wps_event_fail,
	.signal_wps_event_m2d = wpas_dbus_signal_wps_event_m2d,
	.signal_wps_credentials = wpas_dbus_signal_wps_cred,
#endif /* CONFIG_WPS */
};


struct wpas_dbus_callbacks * wpas_dbus_get_callbacks(void)
{
	return &callbacks;
}


/**
 * wpas_dbus_get_path - Get an interface's dbus path
 * @wpa_s: %wpa_supplicant interface structure
 * Returns: Interface's dbus object path, or %NULL on error
 */
const char * wpas_dbus_get_path(struct wpa_supplicant *wpa_s)
{
	return wpa_s->dbus_new_path;
}
