/*
 * binder interface for wpa_supplicant daemon
 * Copyright (c) 2004-2016, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2004-2016, Roshan Pius <rpius@google.com>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#ifndef BINDER_MANAGER_H
#define BINDER_MANAGER_H

#include <map>
#include <string>

#include "supplicant.h"
#include "iface.h"

struct wpa_global;
struct wpa_supplicant;

namespace wpa_supplicant_binder {

/**
 * BinderManager is responsible for managing the lifetime of all
 * binder objects created by wpa_supplicant. This is a singleton
 * class which is created by the supplicant core and can be used
 * to get references to the binder objects.
 */
class BinderManager {
public:
	static const char kBinderServiceName[];

	static BinderManager * getInstance();
	static void destroyInstance();
	int registerBinderService(struct wpa_global *global);

private:
	BinderManager() = default;
	~BinderManager() = default;

	/* Singleton instance of this class. */
	static BinderManager *instance_;
	/* The main binder service object. */
	android::sp<Supplicant> supplicant_object_;
	/* Map of all the interface specific binder objects controlled by
	 * wpa_supplicant. This map is keyed in by the corresponding
	 * wpa_supplicant structure pointer. */
	std::map<const void *, android::sp<Iface>> iface_object_map_;
};

} /* namespace wpa_supplicant_binder */

#endif /* BINDER_MANAGER_H */
