/*
 * UPnP for WPS / internal definitions
 * Copyright (c) 2000-2003 Intel Corporation
 * Copyright (c) 2006-2007 Sony Corporation
 * Copyright (c) 2008-2009 Atheros Communications
 * Copyright (c) 2009, Jouni Malinen <j@w1.fi>
 *
 * See wps_upnp.c for more details on licensing and code history.
 */

#ifndef WPS_UPNP_I_H
#define WPS_UPNP_I_H

#include "http.h"

#define UPNP_MULTICAST_ADDRESS  "239.255.255.250" /* for UPnP multicasting */
#define UPNP_MULTICAST_PORT 1900 /* UDP port to monitor for UPnP */

/* min subscribe time per UPnP standard */
#define UPNP_SUBSCRIBE_SEC_MIN 1800
/* subscribe time we use */
#define UPNP_SUBSCRIBE_SEC (UPNP_SUBSCRIBE_SEC_MIN + 1)

/* "filenames" used in URLs that we service via our "web server": */
#define UPNP_WPS_DEVICE_XML_FILE "wps_device.xml"
#define UPNP_WPS_SCPD_XML_FILE   "wps_scpd.xml"
#define UPNP_WPS_DEVICE_CONTROL_FILE "wps_control"
#define UPNP_WPS_DEVICE_EVENT_FILE "wps_event"

#define MULTICAST_MAX_READ 1600 /* max bytes we'll read for UPD request */


struct subscription;
struct upnp_wps_device_sm;
struct wps_registrar;


enum advertisement_type_enum {
	ADVERTISE_UP = 0,
	ADVERTISE_DOWN = 1,
	MSEARCH_REPLY = 2
};

/*
 * Advertisements are broadcast via UDP NOTIFYs, and are also the essence of
 * the reply to UDP M-SEARCH requests. This struct handles both cases.
 *
 * A state machine is needed because a number of variant forms must be sent in
 * separate packets and spread out in time to avoid congestion.
 */
struct advertisement_state_machine {
	/* double-linked list */
	struct advertisement_state_machine *next;
	struct advertisement_state_machine *prev;
	struct upnp_wps_device_sm *sm; /* parent */
	enum advertisement_type_enum type;
	int state;
	int nerrors;
	struct sockaddr_in client; /* for M-SEARCH replies */
};


/*
 * An address of a subscriber (who may have multiple addresses). We are
 * supposed to send (via TCP) updates to each subscriber, trying each address
 * for a subscriber until we find one that seems to work.
 */
struct subscr_addr {
	/* double linked list */
	struct subscr_addr *next;
	struct subscr_addr *prev;
	struct subscription *s; /* parent */
	char *domain_and_port; /* domain and port part of url */
	char *path; /* "filepath" part of url (from "mem") */
	struct sockaddr_in saddr; /* address for doing connect */
};


/*
 * Subscribers to our events are recorded in this struct. This includes a max
 * of one outgoing connection (sending an "event message") per subscriber. We
 * also have to age out subscribers unless they renew.
 */
struct subscription {
	/* double linked list */
	struct subscription *next;
	struct subscription *prev;
	struct upnp_wps_device_sm *sm; /* parent */
	time_t timeout_time; /* when to age out the subscription */
	unsigned next_subscriber_sequence; /* number our messages */
	/*
	 * This uuid identifies the subscription and is randomly generated by
	 * us and given to the subscriber when the subscription is accepted;
	 * and is then included with each event sent to the subscriber.
	 */
	u8 uuid[UUID_LEN];
	/* Linked list of address alternatives (rotate through on failure) */
	struct subscr_addr *addr_list;
	int n_addr; /* Number of addresses in list */
	struct wps_event_ *event_queue; /* Queued event messages. */
	int n_queue; /* How many events are queued */
	struct wps_event_ *current_event; /* non-NULL if being sent (not in q)
					   */

	/* Information from SetSelectedRegistrar action */
	u8 selected_registrar;
	u16 dev_password_id;
	u16 config_methods;
	struct wps_registrar *reg;
};


/*
 * Our instance data corresponding to one WiFi network interface
 * (multiple might share the same wired network interface!).
 *
 * This is known as an opaque struct declaration to users of the WPS UPnP code.
 */
struct upnp_wps_device_sm {
	struct upnp_wps_device_ctx *ctx; /* callback table */
	struct wps_context *wps;
	void *priv;
	char *root_dir;
	char *desc_url;
	int started; /* nonzero if we are active */
	char *net_if; /* network interface we use */
	char *mac_addr_text; /* mac addr of network i.f. we use */
	u8 mac_addr[ETH_ALEN]; /* mac addr of network i.f. we use */
	char *ip_addr_text; /* IP address of network i.f. we use */
	unsigned ip_addr; /* IP address of network i.f. we use (host order) */
	int multicast_sd; /* send multicast messages over this socket */
	int ssdp_sd; /* receive discovery UPD packets on socket */
	int ssdp_sd_registered; /* nonzero if we must unregister */
	unsigned advertise_count; /* how many advertisements done */
	struct advertisement_state_machine advertisement;
	struct advertisement_state_machine *msearch_replies;
	int n_msearch_replies; /* no. of pending M-SEARCH replies */
	int web_port; /* our port that others get xml files from */
	struct http_server *web_srv;
	/* Note: subscriptions are kept in expiry order */
	struct subscription *subscriptions; /* linked list */
	int n_subscriptions; /* no of current subscriptions */
	int event_send_all_queued; /* if we are scheduled to send events soon
				    */

	char *wlanevent; /* the last WLANEvent data */

	/* FIX: maintain separate structures for each UPnP peer */
	struct upnp_wps_peer peer;
};

/* wps_upnp.c */
void format_date(struct wpabuf *buf);
struct subscription * subscription_start(struct upnp_wps_device_sm *sm,
					 const char *callback_urls);
struct subscription * subscription_renew(struct upnp_wps_device_sm *sm,
					 const u8 uuid[UUID_LEN]);
void subscription_unlink(struct subscription *s);
void subscription_destroy(struct subscription *s);
struct subscription * subscription_find(struct upnp_wps_device_sm *sm,
					const u8 uuid[UUID_LEN]);
int send_wpabuf(int fd, struct wpabuf *buf);
int get_netif_info(const char *net_if, unsigned *ip_addr, char **ip_addr_text,
		   u8 mac[ETH_ALEN], char **mac_addr_text);

/* wps_upnp_ssdp.c */
void msearchreply_state_machine_stop(struct advertisement_state_machine *a);
int advertisement_state_machine_start(struct upnp_wps_device_sm *sm);
void advertisement_state_machine_stop(struct upnp_wps_device_sm *sm,
				      int send_byebye);
void ssdp_listener_stop(struct upnp_wps_device_sm *sm);
int ssdp_listener_start(struct upnp_wps_device_sm *sm);
int ssdp_listener_open(void);
int add_ssdp_network(const char *net_if);
int ssdp_open_multicast_sock(u32 ip_addr);
int ssdp_open_multicast(struct upnp_wps_device_sm *sm);

/* wps_upnp_web.c */
int web_listener_start(struct upnp_wps_device_sm *sm);
void web_listener_stop(struct upnp_wps_device_sm *sm);

/* wps_upnp_event.c */
int event_add(struct subscription *s, const struct wpabuf *data);
void event_delete_all(struct subscription *s);
void event_send_all_later(struct upnp_wps_device_sm *sm);
void event_send_stop_all(struct upnp_wps_device_sm *sm);

/* wps_upnp_ap.c */
int upnp_er_set_selected_registrar(struct wps_registrar *reg,
				   struct subscription *s,
				   const struct wpabuf *msg);
void upnp_er_remove_notification(struct subscription *s);

#endif /* WPS_UPNP_I_H */
