/*
 * WPA Supplicant - wired Ethernet driver interface
 * Copyright (c) 2005-2007, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2004, Gunter Burchardt <tira@isx.de>
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
#include <sys/ioctl.h>
#include <net/if.h>
#ifdef __linux__
#include <netpacket/packet.h>
#include <net/if_arp.h>
#include <net/if.h>
#endif /* __linux__ */
#ifdef __FreeBSD__
#include <net/if_dl.h>
#endif /* __FreeBSD__ */

#include "common.h"
#include "driver.h"

#ifdef HOSTAPD
#include "eloop.h"
#include "../../hostapd/hostapd_defs.h"
#include "../../hostapd/sta_info.h"
#endif /* HOSTAPD */

static const u8 pae_group_addr[ETH_ALEN] =
{ 0x01, 0x80, 0xc2, 0x00, 0x00, 0x03 };


struct wpa_driver_wired_data {
#ifdef HOSTAPD
	struct hostapd_data *hapd;
	char iface[IFNAMSIZ + 1];

	int sock; /* raw packet socket for driver access */
	int dhcp_sock; /* socket for dhcp packets */
	int use_pae_group_addr;
#else /* HOSTAPD */
	void *ctx;
	int pf_sock;
	char ifname[IFNAMSIZ + 1];
	int membership, multi, iff_allmulti, iff_up;
#endif /* HOSTAPD */
};


#ifdef HOSTAPD

/* TODO: detecting new devices should eventually be changed from using DHCP
 * snooping to trigger on any packet from a new layer 2 MAC address, e.g.,
 * based on ebtables, etc. */

struct dhcp_message {
	u_int8_t op;
	u_int8_t htype;
	u_int8_t hlen;
	u_int8_t hops;
	u_int32_t xid;
	u_int16_t secs;
	u_int16_t flags;
	u_int32_t ciaddr;
	u_int32_t yiaddr;
	u_int32_t siaddr;
	u_int32_t giaddr;
	u_int8_t chaddr[16];
	u_int8_t sname[64];
	u_int8_t file[128];
	u_int32_t cookie;
	u_int8_t options[308]; /* 312 - cookie */
};


static void wired_possible_new_sta(struct hostapd_data *hapd, u8 *addr)
{
	struct sta_info *sta;

	sta = ap_get_sta(hapd, addr);
	if (sta)
		return;

	wpa_printf(MSG_DEBUG, "Data frame from unknown STA " MACSTR
		   " - adding a new STA", MAC2STR(addr));
	sta = ap_sta_add(hapd, addr);
	if (sta) {
		hostapd_new_assoc_sta(hapd, sta, 0);
	} else {
		wpa_printf(MSG_DEBUG, "Failed to add STA entry for " MACSTR,
			   MAC2STR(addr));
	}
}


static void handle_data(struct hostapd_data *hapd, unsigned char *buf,
			size_t len)
{
	struct ieee8023_hdr *hdr;
	u8 *pos, *sa;
	size_t left;

	/* must contain at least ieee8023_hdr 6 byte source, 6 byte dest,
	 * 2 byte ethertype */
	if (len < 14) {
		wpa_printf(MSG_MSGDUMP, "handle_data: too short (%lu)",
			   (unsigned long) len);
		return;
	}

	hdr = (struct ieee8023_hdr *) buf;

	switch (ntohs(hdr->ethertype)) {
		case ETH_P_PAE:
			wpa_printf(MSG_MSGDUMP, "Received EAPOL packet");
			sa = hdr->src;
			wired_possible_new_sta(hapd, sa);

			pos = (u8 *) (hdr + 1);
			left = len - sizeof(*hdr);

			hostapd_eapol_receive(hapd, sa, pos, left);
		break;

	default:
		wpa_printf(MSG_DEBUG, "Unknown ethertype 0x%04x in data frame",
			   ntohs(hdr->ethertype));
		break;
	}
}


static void handle_read(int sock, void *eloop_ctx, void *sock_ctx)
{
	struct hostapd_data *hapd = (struct hostapd_data *) eloop_ctx;
	int len;
	unsigned char buf[3000];

	len = recv(sock, buf, sizeof(buf), 0);
	if (len < 0) {
		perror("recv");
		return;
	}

	handle_data(hapd, buf, len);
}


static void handle_dhcp(int sock, void *eloop_ctx, void *sock_ctx)
{
	struct hostapd_data *hapd = (struct hostapd_data *) eloop_ctx;
	int len;
	unsigned char buf[3000];
	struct dhcp_message *msg;
	u8 *mac_address;

	len = recv(sock, buf, sizeof(buf), 0);
	if (len < 0) {
		perror("recv");
		return;
	}

	/* must contain at least dhcp_message->chaddr */
	if (len < 44) {
		wpa_printf(MSG_MSGDUMP, "handle_dhcp: too short (%d)", len);
		return;
	}

	msg = (struct dhcp_message *) buf;
	mac_address = (u8 *) &(msg->chaddr);

	wpa_printf(MSG_MSGDUMP, "Got DHCP broadcast packet from " MACSTR,
		   MAC2STR(mac_address));

	wired_possible_new_sta(hapd, mac_address);
}


static int wired_init_sockets(struct wpa_driver_wired_data *drv, u8 *own_addr)
{
	struct hostapd_data *hapd = drv->hapd;
	struct ifreq ifr;
	struct sockaddr_ll addr;
	struct sockaddr_in addr2;
	struct packet_mreq mreq;
	int n = 1;

	drv->sock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_PAE));
	if (drv->sock < 0) {
		perror("socket[PF_PACKET,SOCK_RAW]");
		return -1;
	}

	if (eloop_register_read_sock(drv->sock, handle_read, hapd, NULL)) {
		printf("Could not register read socket\n");
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, drv->iface, sizeof(ifr.ifr_name));
	if (ioctl(drv->sock, SIOCGIFINDEX, &ifr) != 0) {
		perror("ioctl(SIOCGIFINDEX)");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sll_family = AF_PACKET;
	addr.sll_ifindex = ifr.ifr_ifindex;
	wpa_printf(MSG_DEBUG, "Opening raw packet socket for ifindex %d",
		   addr.sll_ifindex);

	if (bind(drv->sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("bind");
		return -1;
	}

	/* filter multicast address */
	memset(&mreq, 0, sizeof(mreq));
	mreq.mr_ifindex = ifr.ifr_ifindex;
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = 6;
	memcpy(mreq.mr_address, pae_group_addr, mreq.mr_alen);

	if (setsockopt(drv->sock, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq,
		       sizeof(mreq)) < 0) {
		perror("setsockopt[SOL_SOCKET,PACKET_ADD_MEMBERSHIP]");
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, drv->iface, sizeof(ifr.ifr_name));
	if (ioctl(drv->sock, SIOCGIFHWADDR, &ifr) != 0) {
		perror("ioctl(SIOCGIFHWADDR)");
		return -1;
	}

	if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
		printf("Invalid HW-addr family 0x%04x\n",
		       ifr.ifr_hwaddr.sa_family);
		return -1;
	}
	memcpy(own_addr, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

	/* setup dhcp listen socket for sta detection */
	if ((drv->dhcp_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		perror("socket call failed for dhcp");
		return -1;
	}

	if (eloop_register_read_sock(drv->dhcp_sock, handle_dhcp, hapd, NULL))
	{
		printf("Could not register read socket\n");
		return -1;
	}

	memset(&addr2, 0, sizeof(addr2));
	addr2.sin_family = AF_INET;
	addr2.sin_port = htons(67);
	addr2.sin_addr.s_addr = INADDR_ANY;

	if (setsockopt(drv->dhcp_sock, SOL_SOCKET, SO_REUSEADDR, (char *) &n,
		       sizeof(n)) == -1) {
		perror("setsockopt[SOL_SOCKET,SO_REUSEADDR]");
		return -1;
	}
	if (setsockopt(drv->dhcp_sock, SOL_SOCKET, SO_BROADCAST, (char *) &n,
		       sizeof(n)) == -1) {
		perror("setsockopt[SOL_SOCKET,SO_BROADCAST]");
		return -1;
	}

	memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_ifrn.ifrn_name, drv->iface, IFNAMSIZ);
	if (setsockopt(drv->dhcp_sock, SOL_SOCKET, SO_BINDTODEVICE,
		       (char *) &ifr, sizeof(ifr)) < 0) {
		perror("setsockopt[SOL_SOCKET,SO_BINDTODEVICE]");
		return -1;
	}

	if (bind(drv->dhcp_sock, (struct sockaddr *) &addr2,
		 sizeof(struct sockaddr)) == -1) {
		perror("bind");
		return -1;
	}

	return 0;
}


static int wired_send_eapol(void *priv, const u8 *addr,
			    const u8 *data, size_t data_len, int encrypt,
			    const u8 *own_addr)
{
	struct wpa_driver_wired_data *drv = priv;
	struct ieee8023_hdr *hdr;
	size_t len;
	u8 *pos;
	int res;

	len = sizeof(*hdr) + data_len;
	hdr = os_zalloc(len);
	if (hdr == NULL) {
		printf("malloc() failed for wired_send_eapol(len=%lu)\n",
		       (unsigned long) len);
		return -1;
	}

	memcpy(hdr->dest, drv->use_pae_group_addr ? pae_group_addr : addr,
	       ETH_ALEN);
	memcpy(hdr->src, own_addr, ETH_ALEN);
	hdr->ethertype = htons(ETH_P_PAE);

	pos = (u8 *) (hdr + 1);
	memcpy(pos, data, data_len);

	res = send(drv->sock, (u8 *) hdr, len, 0);
	free(hdr);

	if (res < 0) {
		perror("wired_send_eapol: send");
		printf("wired_send_eapol - packet len: %lu - failed\n",
		       (unsigned long) len);
	}

	return res;
}


static void * wired_driver_hapd_init(struct hostapd_data *hapd,
				     struct wpa_init_params *params)
{
	struct wpa_driver_wired_data *drv;

	drv = os_zalloc(sizeof(struct wpa_driver_wired_data));
	if (drv == NULL) {
		printf("Could not allocate memory for wired driver data\n");
		return NULL;
	}

	drv->hapd = hapd;
	os_strlcpy(drv->iface, params->ifname, sizeof(drv->iface));
	drv->use_pae_group_addr = params->use_pae_group_addr;

	if (wired_init_sockets(drv, params->own_addr)) {
		free(drv);
		return NULL;
	}

	return drv;
}


static void wired_driver_hapd_deinit(void *priv)
{
	struct wpa_driver_wired_data *drv = priv;

	if (drv->sock >= 0)
		close(drv->sock);

	if (drv->dhcp_sock >= 0)
		close(drv->dhcp_sock);

	free(drv);
}

#else /* HOSTAPD */

static int wpa_driver_wired_get_ssid(void *priv, u8 *ssid)
{
	ssid[0] = 0;
	return 0;
}


static int wpa_driver_wired_get_bssid(void *priv, u8 *bssid)
{
	/* Report PAE group address as the "BSSID" for wired connection. */
	os_memcpy(bssid, pae_group_addr, ETH_ALEN);
	return 0;
}


static int wpa_driver_wired_get_capa(void *priv, struct wpa_driver_capa *capa)
{
	os_memset(capa, 0, sizeof(*capa));
	capa->flags = WPA_DRIVER_FLAGS_WIRED;
	return 0;
}


static int wpa_driver_wired_get_ifflags(const char *ifname, int *flags)
{
	struct ifreq ifr;
	int s;

	s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("socket");
		return -1;
	}

	os_memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);
	if (ioctl(s, SIOCGIFFLAGS, (caddr_t) &ifr) < 0) {
		perror("ioctl[SIOCGIFFLAGS]");
		close(s);
		return -1;
	}
	close(s);
	*flags = ifr.ifr_flags & 0xffff;
	return 0;
}


static int wpa_driver_wired_set_ifflags(const char *ifname, int flags)
{
	struct ifreq ifr;
	int s;

	s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("socket");
		return -1;
	}

	os_memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);
	ifr.ifr_flags = flags & 0xffff;
	if (ioctl(s, SIOCSIFFLAGS, (caddr_t) &ifr) < 0) {
		perror("ioctl[SIOCSIFFLAGS]");
		close(s);
		return -1;
	}
	close(s);
	return 0;
}


static int wpa_driver_wired_multi(const char *ifname, const u8 *addr, int add)
{
	struct ifreq ifr;
	int s;

	s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("socket");
		return -1;
	}

	os_memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);
#ifdef __linux__
	ifr.ifr_hwaddr.sa_family = AF_UNSPEC;
	os_memcpy(ifr.ifr_hwaddr.sa_data, addr, ETH_ALEN);
#endif /* __linux__ */
#ifdef __FreeBSD__
	{
		struct sockaddr_dl *dlp;
		dlp = (struct sockaddr_dl *) &ifr.ifr_addr;
		dlp->sdl_len = sizeof(struct sockaddr_dl);
		dlp->sdl_family = AF_LINK;
		dlp->sdl_index = 0;
		dlp->sdl_nlen = 0;
		dlp->sdl_alen = ETH_ALEN;
		dlp->sdl_slen = 0;
		os_memcpy(LLADDR(dlp), addr, ETH_ALEN);
	}
#endif /* __FreeBSD__ */
#if defined(__NetBSD__) || defined(__OpenBSD__)
	{
		struct sockaddr *sap;
		sap = (struct sockaddr *) &ifr.ifr_addr;
		sap->sa_len = sizeof(struct sockaddr);
		sap->sa_family = AF_UNSPEC;
		os_memcpy(sap->sa_data, addr, ETH_ALEN);
	}
#endif /* defined(__NetBSD__) || defined(__OpenBSD__) */

	if (ioctl(s, add ? SIOCADDMULTI : SIOCDELMULTI, (caddr_t) &ifr) < 0) {
		perror("ioctl[SIOC{ADD/DEL}MULTI]");
		close(s);
		return -1;
	}
	close(s);
	return 0;
}


static int wpa_driver_wired_membership(struct wpa_driver_wired_data *drv,
				       const u8 *addr, int add)
{
#ifdef __linux__
	struct packet_mreq mreq;

	if (drv->pf_sock == -1)
		return -1;

	os_memset(&mreq, 0, sizeof(mreq));
	mreq.mr_ifindex = if_nametoindex(drv->ifname);
	mreq.mr_type = PACKET_MR_MULTICAST;
	mreq.mr_alen = ETH_ALEN;
	os_memcpy(mreq.mr_address, addr, ETH_ALEN);

	if (setsockopt(drv->pf_sock, SOL_PACKET,
		       add ? PACKET_ADD_MEMBERSHIP : PACKET_DROP_MEMBERSHIP,
		       &mreq, sizeof(mreq)) < 0) {
		perror("setsockopt");
		return -1;
	}
	return 0;
#else /* __linux__ */
	return -1;
#endif /* __linux__ */
}


static void * wpa_driver_wired_init(void *ctx, const char *ifname)
{
	struct wpa_driver_wired_data *drv;
	int flags;

	drv = os_zalloc(sizeof(*drv));
	if (drv == NULL)
		return NULL;
	os_strlcpy(drv->ifname, ifname, sizeof(drv->ifname));
	drv->ctx = ctx;

#ifdef __linux__
	drv->pf_sock = socket(PF_PACKET, SOCK_DGRAM, 0);
	if (drv->pf_sock < 0)
		perror("socket(PF_PACKET)");
#else /* __linux__ */
	drv->pf_sock = -1;
#endif /* __linux__ */

	if (wpa_driver_wired_get_ifflags(ifname, &flags) == 0 &&
	    !(flags & IFF_UP) &&
	    wpa_driver_wired_set_ifflags(ifname, flags | IFF_UP) == 0) {
		drv->iff_up = 1;
	}

	if (wpa_driver_wired_membership(drv, pae_group_addr, 1) == 0) {
		wpa_printf(MSG_DEBUG, "%s: Added multicast membership with "
			   "packet socket", __func__);
		drv->membership = 1;
	} else if (wpa_driver_wired_multi(ifname, pae_group_addr, 1) == 0) {
		wpa_printf(MSG_DEBUG, "%s: Added multicast membership with "
			   "SIOCADDMULTI", __func__);
		drv->multi = 1;
	} else if (wpa_driver_wired_get_ifflags(ifname, &flags) < 0) {
		wpa_printf(MSG_INFO, "%s: Could not get interface "
			   "flags", __func__);
		os_free(drv);
		return NULL;
	} else if (flags & IFF_ALLMULTI) {
		wpa_printf(MSG_DEBUG, "%s: Interface is already configured "
			   "for multicast", __func__);
	} else if (wpa_driver_wired_set_ifflags(ifname,
						flags | IFF_ALLMULTI) < 0) {
		wpa_printf(MSG_INFO, "%s: Failed to enable allmulti",
			   __func__);
		os_free(drv);
		return NULL;
	} else {
		wpa_printf(MSG_DEBUG, "%s: Enabled allmulti mode",
			   __func__);
		drv->iff_allmulti = 1;
	}

	return drv;
}


static void wpa_driver_wired_deinit(void *priv)
{
	struct wpa_driver_wired_data *drv = priv;
	int flags;

	if (drv->membership &&
	    wpa_driver_wired_membership(drv, pae_group_addr, 0) < 0) {
		wpa_printf(MSG_DEBUG, "%s: Failed to remove PAE multicast "
			   "group (PACKET)", __func__);
	}

	if (drv->multi &&
	    wpa_driver_wired_multi(drv->ifname, pae_group_addr, 0) < 0) {
		wpa_printf(MSG_DEBUG, "%s: Failed to remove PAE multicast "
			   "group (SIOCDELMULTI)", __func__);
	}

	if (drv->iff_allmulti &&
	    (wpa_driver_wired_get_ifflags(drv->ifname, &flags) < 0 ||
	     wpa_driver_wired_set_ifflags(drv->ifname,
					  flags & ~IFF_ALLMULTI) < 0)) {
		wpa_printf(MSG_DEBUG, "%s: Failed to disable allmulti mode",
			   __func__);
	}

	if (drv->iff_up &&
	    wpa_driver_wired_get_ifflags(drv->ifname, &flags) == 0 &&
	    (flags & IFF_UP) &&
	    wpa_driver_wired_set_ifflags(drv->ifname, flags & ~IFF_UP) < 0) {
		wpa_printf(MSG_DEBUG, "%s: Failed to set the interface down",
			   __func__);
	}

	if (drv->pf_sock != -1)
		close(drv->pf_sock);

	os_free(drv);
}
#endif /* HOSTAPD */


const struct wpa_driver_ops wpa_driver_wired_ops = {
	.name = "wired",
	.desc = "Wired Ethernet driver",
#ifdef HOSTAPD
	.hapd_init = wired_driver_hapd_init,
	.hapd_deinit = wired_driver_hapd_deinit,
	.hapd_send_eapol = wired_send_eapol,
#else /* HOSTAPD */
	.get_ssid = wpa_driver_wired_get_ssid,
	.get_bssid = wpa_driver_wired_get_bssid,
	.get_capa = wpa_driver_wired_get_capa,
	.init = wpa_driver_wired_init,
	.deinit = wpa_driver_wired_deinit,
#endif /* HOSTAPD */
};
