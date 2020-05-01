/*
 * DPP functionality shared between hostapd and wpa_supplicant
 * Copyright (c) 2017, Qualcomm Atheros, Inc.
 * Copyright (c) 2018-2020, The Linux Foundation
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"
#include <fcntl.h>
#include <openssl/opensslv.h>
#include <openssl/err.h>

#include "utils/common.h"
#include "utils/base64.h"
#include "utils/json.h"
#include "utils/ip_addr.h"
#include "utils/eloop.h"
#include "common/ieee802_11_common.h"
#include "common/wpa_ctrl.h"
#include "common/gas.h"
#include "crypto/crypto.h"
#include "crypto/random.h"
#include "crypto/aes.h"
#include "crypto/aes_siv.h"
#include "tls/asn1.h"
#include "drivers/driver.h"
#include "dpp.h"
#include "dpp_i.h"


static const char * dpp_netrole_str(enum dpp_netrole netrole);

#ifdef CONFIG_TESTING_OPTIONS
#ifdef CONFIG_DPP2
int dpp_version_override = 2;
#else
int dpp_version_override = 1;
#endif
enum dpp_test_behavior dpp_test = DPP_TEST_DISABLED;
u8 dpp_protocol_key_override[600];
size_t dpp_protocol_key_override_len = 0;
u8 dpp_nonce_override[DPP_MAX_NONCE_LEN];
size_t dpp_nonce_override_len = 0;
#endif /* CONFIG_TESTING_OPTIONS */

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
	(defined(LIBRESSL_VERSION_NUMBER) && \
	 LIBRESSL_VERSION_NUMBER < 0x20700000L)
/* Compatibility wrappers for older versions. */

#ifdef CONFIG_DPP2
static EC_KEY * EVP_PKEY_get0_EC_KEY(EVP_PKEY *pkey)
{
	if (pkey->type != EVP_PKEY_EC)
		return NULL;
	return pkey->pkey.ec;
}
#endif /* CONFIG_DPP2 */

#endif


struct dpp_connection {
	struct dl_list list;
	struct dpp_controller *ctrl;
	struct dpp_relay_controller *relay;
	struct dpp_global *global;
	struct dpp_authentication *auth;
	int sock;
	u8 mac_addr[ETH_ALEN];
	unsigned int freq;
	u8 msg_len[4];
	size_t msg_len_octets;
	struct wpabuf *msg;
	struct wpabuf *msg_out;
	size_t msg_out_pos;
	unsigned int read_eloop:1;
	unsigned int write_eloop:1;
	unsigned int on_tcp_tx_complete_gas_done:1;
	unsigned int on_tcp_tx_complete_remove:1;
	unsigned int on_tcp_tx_complete_auth_ok:1;
};

/* Remote Controller */
struct dpp_relay_controller {
	struct dl_list list;
	struct dpp_global *global;
	u8 pkhash[SHA256_MAC_LEN];
	struct hostapd_ip_addr ipaddr;
	void *cb_ctx;
	void (*tx)(void *ctx, const u8 *addr, unsigned int freq, const u8 *msg,
		   size_t len);
	void (*gas_resp_tx)(void *ctx, const u8 *addr, u8 dialog_token,
			    int prot, struct wpabuf *buf);
	struct dl_list conn; /* struct dpp_connection */
};

/* Local Controller */
struct dpp_controller {
	struct dpp_global *global;
	u8 allowed_roles;
	int qr_mutual;
	int sock;
	struct dl_list conn; /* struct dpp_connection */
	char *configurator_params;
};


static void dpp_auth_fail(struct dpp_authentication *auth, const char *txt)
{
	wpa_msg(auth->msg_ctx, MSG_INFO, DPP_EVENT_FAIL "%s", txt);
}


struct wpabuf * dpp_alloc_msg(enum dpp_public_action_frame_type type,
			      size_t len)
{
	struct wpabuf *msg;

	msg = wpabuf_alloc(8 + len);
	if (!msg)
		return NULL;
	wpabuf_put_u8(msg, WLAN_ACTION_PUBLIC);
	wpabuf_put_u8(msg, WLAN_PA_VENDOR_SPECIFIC);
	wpabuf_put_be24(msg, OUI_WFA);
	wpabuf_put_u8(msg, DPP_OUI_TYPE);
	wpabuf_put_u8(msg, 1); /* Crypto Suite */
	wpabuf_put_u8(msg, type);
	return msg;
}


const u8 * dpp_get_attr(const u8 *buf, size_t len, u16 req_id, u16 *ret_len)
{
	u16 id, alen;
	const u8 *pos = buf, *end = buf + len;

	while (end - pos >= 4) {
		id = WPA_GET_LE16(pos);
		pos += 2;
		alen = WPA_GET_LE16(pos);
		pos += 2;
		if (alen > end - pos)
			return NULL;
		if (id == req_id) {
			*ret_len = alen;
			return pos;
		}
		pos += alen;
	}

	return NULL;
}


static const u8 * dpp_get_attr_next(const u8 *prev, const u8 *buf, size_t len,
				    u16 req_id, u16 *ret_len)
{
	u16 id, alen;
	const u8 *pos, *end = buf + len;

	if (!prev)
		pos = buf;
	else
		pos = prev + WPA_GET_LE16(prev - 2);
	while (end - pos >= 4) {
		id = WPA_GET_LE16(pos);
		pos += 2;
		alen = WPA_GET_LE16(pos);
		pos += 2;
		if (alen > end - pos)
			return NULL;
		if (id == req_id) {
			*ret_len = alen;
			return pos;
		}
		pos += alen;
	}

	return NULL;
}


int dpp_check_attrs(const u8 *buf, size_t len)
{
	const u8 *pos, *end;
	int wrapped_data = 0;

	pos = buf;
	end = buf + len;
	while (end - pos >= 4) {
		u16 id, alen;

		id = WPA_GET_LE16(pos);
		pos += 2;
		alen = WPA_GET_LE16(pos);
		pos += 2;
		wpa_printf(MSG_MSGDUMP, "DPP: Attribute ID %04x len %u",
			   id, alen);
		if (alen > end - pos) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Truncated message - not enough room for the attribute - dropped");
			return -1;
		}
		if (wrapped_data) {
			wpa_printf(MSG_DEBUG,
				   "DPP: An unexpected attribute included after the Wrapped Data attribute");
			return -1;
		}
		if (id == DPP_ATTR_WRAPPED_DATA)
			wrapped_data = 1;
		pos += alen;
	}

	if (end != pos) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Unexpected octets (%d) after the last attribute",
			   (int) (end - pos));
		return -1;
	}

	return 0;
}


void dpp_bootstrap_info_free(struct dpp_bootstrap_info *info)
{
	if (!info)
		return;
	os_free(info->uri);
	os_free(info->info);
	os_free(info->chan);
	os_free(info->pk);
	EVP_PKEY_free(info->pubkey);
	str_clear_free(info->configurator_params);
	os_free(info);
}


const char * dpp_bootstrap_type_txt(enum dpp_bootstrap_type type)
{
	switch (type) {
	case DPP_BOOTSTRAP_QR_CODE:
		return "QRCODE";
	case DPP_BOOTSTRAP_PKEX:
		return "PKEX";
	case DPP_BOOTSTRAP_NFC_URI:
		return "NFC-URI";
	}
	return "??";
}


static int dpp_uri_valid_info(const char *info)
{
	while (*info) {
		unsigned char val = *info++;

		if (val < 0x20 || val > 0x7e || val == 0x3b)
			return 0;
	}

	return 1;
}


static int dpp_clone_uri(struct dpp_bootstrap_info *bi, const char *uri)
{
	bi->uri = os_strdup(uri);
	return bi->uri ? 0 : -1;
}


int dpp_parse_uri_chan_list(struct dpp_bootstrap_info *bi,
			    const char *chan_list)
{
	const char *pos = chan_list, *pos2;
	int opclass = -1, channel, freq;

	while (pos && *pos && *pos != ';') {
		pos2 = pos;
		while (*pos2 >= '0' && *pos2 <= '9')
			pos2++;
		if (*pos2 == '/') {
			opclass = atoi(pos);
			pos = pos2 + 1;
		}
		if (opclass <= 0)
			goto fail;
		channel = atoi(pos);
		if (channel <= 0)
			goto fail;
		while (*pos >= '0' && *pos <= '9')
			pos++;
		freq = ieee80211_chan_to_freq(NULL, opclass, channel);
		wpa_printf(MSG_DEBUG,
			   "DPP: URI channel-list: opclass=%d channel=%d ==> freq=%d",
			   opclass, channel, freq);
		if (freq < 0) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Ignore unknown URI channel-list channel (opclass=%d channel=%d)",
				   opclass, channel);
		} else if (bi->num_freq == DPP_BOOTSTRAP_MAX_FREQ) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Too many channels in URI channel-list - ignore list");
			bi->num_freq = 0;
			break;
		} else {
			bi->freq[bi->num_freq++] = freq;
		}

		if (*pos == ';' || *pos == '\0')
			break;
		if (*pos != ',')
			goto fail;
		pos++;
	}

	return 0;
fail:
	wpa_printf(MSG_DEBUG, "DPP: Invalid URI channel-list");
	return -1;
}


int dpp_parse_uri_mac(struct dpp_bootstrap_info *bi, const char *mac)
{
	if (!mac)
		return 0;

	if (hwaddr_aton2(mac, bi->mac_addr) < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Invalid URI mac");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "DPP: URI mac: " MACSTR, MAC2STR(bi->mac_addr));

	return 0;
}


int dpp_parse_uri_info(struct dpp_bootstrap_info *bi, const char *info)
{
	const char *end;

	if (!info)
		return 0;

	end = os_strchr(info, ';');
	if (!end)
		end = info + os_strlen(info);
	bi->info = os_malloc(end - info + 1);
	if (!bi->info)
		return -1;
	os_memcpy(bi->info, info, end - info);
	bi->info[end - info] = '\0';
	wpa_printf(MSG_DEBUG, "DPP: URI(information): %s", bi->info);
	if (!dpp_uri_valid_info(bi->info)) {
		wpa_printf(MSG_DEBUG, "DPP: Invalid URI information payload");
		return -1;
	}

	return 0;
}


int dpp_parse_uri_version(struct dpp_bootstrap_info *bi, const char *version)
{
#ifdef CONFIG_DPP2
	if (!version || DPP_VERSION < 2)
		return 0;

	if (*version == '1')
		bi->version = 1;
	else if (*version == '2')
		bi->version = 2;
	else
		wpa_printf(MSG_DEBUG, "DPP: Unknown URI version");

	wpa_printf(MSG_DEBUG, "DPP: URI version: %d", bi->version);
#endif /* CONFIG_DPP2 */

	return 0;
}


static int dpp_parse_uri_pk(struct dpp_bootstrap_info *bi, const char *info)
{
	u8 *data;
	size_t data_len;
	int res;
	const char *end;

	end = os_strchr(info, ';');
	if (!end)
		return -1;

	data = base64_decode(info, end - info, &data_len);
	if (!data) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Invalid base64 encoding on URI public-key");
		return -1;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Base64 decoded URI public-key",
		    data, data_len);

	res = dpp_get_subject_public_key(bi, data, data_len);
	os_free(data);
	return res;
}


static struct dpp_bootstrap_info * dpp_parse_uri(const char *uri)
{
	const char *pos = uri;
	const char *end;
	const char *chan_list = NULL, *mac = NULL, *info = NULL, *pk = NULL;
	const char *version = NULL;
	struct dpp_bootstrap_info *bi;

	wpa_hexdump_ascii(MSG_DEBUG, "DPP: URI", uri, os_strlen(uri));

	if (os_strncmp(pos, "DPP:", 4) != 0) {
		wpa_printf(MSG_INFO, "DPP: Not a DPP URI");
		return NULL;
	}
	pos += 4;

	for (;;) {
		end = os_strchr(pos, ';');
		if (!end)
			break;

		if (end == pos) {
			/* Handle terminating ";;" and ignore unexpected ";"
			 * for parsing robustness. */
			pos++;
			continue;
		}

		if (pos[0] == 'C' && pos[1] == ':' && !chan_list)
			chan_list = pos + 2;
		else if (pos[0] == 'M' && pos[1] == ':' && !mac)
			mac = pos + 2;
		else if (pos[0] == 'I' && pos[1] == ':' && !info)
			info = pos + 2;
		else if (pos[0] == 'K' && pos[1] == ':' && !pk)
			pk = pos + 2;
		else if (pos[0] == 'V' && pos[1] == ':' && !version)
			version = pos + 2;
		else
			wpa_hexdump_ascii(MSG_DEBUG,
					  "DPP: Ignore unrecognized URI parameter",
					  pos, end - pos);
		pos = end + 1;
	}

	if (!pk) {
		wpa_printf(MSG_INFO, "DPP: URI missing public-key");
		return NULL;
	}

	bi = os_zalloc(sizeof(*bi));
	if (!bi)
		return NULL;

	if (dpp_clone_uri(bi, uri) < 0 ||
	    dpp_parse_uri_chan_list(bi, chan_list) < 0 ||
	    dpp_parse_uri_mac(bi, mac) < 0 ||
	    dpp_parse_uri_info(bi, info) < 0 ||
	    dpp_parse_uri_version(bi, version) < 0 ||
	    dpp_parse_uri_pk(bi, pk) < 0) {
		dpp_bootstrap_info_free(bi);
		bi = NULL;
	}

	return bi;
}


void dpp_build_attr_status(struct wpabuf *msg, enum dpp_status_error status)
{
	wpa_printf(MSG_DEBUG, "DPP: Status %d", status);
	wpabuf_put_le16(msg, DPP_ATTR_STATUS);
	wpabuf_put_le16(msg, 1);
	wpabuf_put_u8(msg, status);
}


static void dpp_build_attr_r_bootstrap_key_hash(struct wpabuf *msg,
						const u8 *hash)
{
	if (hash) {
		wpa_printf(MSG_DEBUG, "DPP: R-Bootstrap Key Hash");
		wpabuf_put_le16(msg, DPP_ATTR_R_BOOTSTRAP_KEY_HASH);
		wpabuf_put_le16(msg, SHA256_MAC_LEN);
		wpabuf_put_data(msg, hash, SHA256_MAC_LEN);
	}
}


static void dpp_build_attr_i_bootstrap_key_hash(struct wpabuf *msg,
						const u8 *hash)
{
	if (hash) {
		wpa_printf(MSG_DEBUG, "DPP: I-Bootstrap Key Hash");
		wpabuf_put_le16(msg, DPP_ATTR_I_BOOTSTRAP_KEY_HASH);
		wpabuf_put_le16(msg, SHA256_MAC_LEN);
		wpabuf_put_data(msg, hash, SHA256_MAC_LEN);
	}
}


static struct wpabuf * dpp_auth_build_req(struct dpp_authentication *auth,
					  const struct wpabuf *pi,
					  size_t nonce_len,
					  const u8 *r_pubkey_hash,
					  const u8 *i_pubkey_hash,
					  unsigned int neg_freq)
{
	struct wpabuf *msg;
	u8 clear[4 + DPP_MAX_NONCE_LEN + 4 + 1];
	u8 wrapped_data[4 + DPP_MAX_NONCE_LEN + 4 + 1 + AES_BLOCK_SIZE];
	u8 *pos;
	const u8 *addr[2];
	size_t len[2], siv_len, attr_len;
	u8 *attr_start, *attr_end;

	/* Build DPP Authentication Request frame attributes */
	attr_len = 2 * (4 + SHA256_MAC_LEN) + 4 + (pi ? wpabuf_len(pi) : 0) +
		4 + sizeof(wrapped_data);
	if (neg_freq > 0)
		attr_len += 4 + 2;
#ifdef CONFIG_DPP2
	attr_len += 5;
#endif /* CONFIG_DPP2 */
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_AUTH_REQ)
		attr_len += 5;
#endif /* CONFIG_TESTING_OPTIONS */
	msg = dpp_alloc_msg(DPP_PA_AUTHENTICATION_REQ, attr_len);
	if (!msg)
		return NULL;

	attr_start = wpabuf_put(msg, 0);

	/* Responder Bootstrapping Key Hash */
	dpp_build_attr_r_bootstrap_key_hash(msg, r_pubkey_hash);

	/* Initiator Bootstrapping Key Hash */
	dpp_build_attr_i_bootstrap_key_hash(msg, i_pubkey_hash);

	/* Initiator Protocol Key */
	if (pi) {
		wpabuf_put_le16(msg, DPP_ATTR_I_PROTOCOL_KEY);
		wpabuf_put_le16(msg, wpabuf_len(pi));
		wpabuf_put_buf(msg, pi);
	}

	/* Channel */
	if (neg_freq > 0) {
		u8 op_class, channel;

		if (ieee80211_freq_to_channel_ext(neg_freq, 0, 0, &op_class,
						  &channel) ==
		    NUM_HOSTAPD_MODES) {
			wpa_printf(MSG_INFO,
				   "DPP: Unsupported negotiation frequency request: %d",
				   neg_freq);
			wpabuf_free(msg);
			return NULL;
		}
		wpabuf_put_le16(msg, DPP_ATTR_CHANNEL);
		wpabuf_put_le16(msg, 2);
		wpabuf_put_u8(msg, op_class);
		wpabuf_put_u8(msg, channel);
	}

#ifdef CONFIG_DPP2
	/* Protocol Version */
	if (DPP_VERSION > 1) {
		wpabuf_put_le16(msg, DPP_ATTR_PROTOCOL_VERSION);
		wpabuf_put_le16(msg, 1);
		wpabuf_put_u8(msg, DPP_VERSION);
	}
#endif /* CONFIG_DPP2 */

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_WRAPPED_DATA_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no Wrapped Data");
		goto skip_wrapped_data;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* Wrapped data ({I-nonce, I-capabilities}k1) */
	pos = clear;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_I_NONCE_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-nonce");
		goto skip_i_nonce;
	}
	if (dpp_test == DPP_TEST_INVALID_I_NONCE_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - invalid I-nonce");
		WPA_PUT_LE16(pos, DPP_ATTR_I_NONCE);
		pos += 2;
		WPA_PUT_LE16(pos, nonce_len - 1);
		pos += 2;
		os_memcpy(pos, auth->i_nonce, nonce_len - 1);
		pos += nonce_len - 1;
		goto skip_i_nonce;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* I-nonce */
	WPA_PUT_LE16(pos, DPP_ATTR_I_NONCE);
	pos += 2;
	WPA_PUT_LE16(pos, nonce_len);
	pos += 2;
	os_memcpy(pos, auth->i_nonce, nonce_len);
	pos += nonce_len;

#ifdef CONFIG_TESTING_OPTIONS
skip_i_nonce:
	if (dpp_test == DPP_TEST_NO_I_CAPAB_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-capab");
		goto skip_i_capab;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* I-capabilities */
	WPA_PUT_LE16(pos, DPP_ATTR_I_CAPABILITIES);
	pos += 2;
	WPA_PUT_LE16(pos, 1);
	pos += 2;
	auth->i_capab = auth->allowed_roles;
	*pos++ = auth->i_capab;
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_ZERO_I_CAPAB) {
		wpa_printf(MSG_INFO, "DPP: TESTING - zero I-capabilities");
		pos[-1] = 0;
	}
skip_i_capab:
#endif /* CONFIG_TESTING_OPTIONS */

	attr_end = wpabuf_put(msg, 0);

	/* OUI, OUI type, Crypto Suite, DPP frame type */
	addr[0] = wpabuf_head_u8(msg) + 2;
	len[0] = 3 + 1 + 1 + 1;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);

	/* Attributes before Wrapped Data */
	addr[1] = attr_start;
	len[1] = attr_end - attr_start;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);

	siv_len = pos - clear;
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext", clear, siv_len);
	if (aes_siv_encrypt(auth->k1, auth->curve->hash_len, clear, siv_len,
			    2, addr, len, wrapped_data) < 0) {
		wpabuf_free(msg);
		return NULL;
	}
	siv_len += AES_BLOCK_SIZE;
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, siv_len);

	wpabuf_put_le16(msg, DPP_ATTR_WRAPPED_DATA);
	wpabuf_put_le16(msg, siv_len);
	wpabuf_put_data(msg, wrapped_data, siv_len);

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - attr after Wrapped Data");
		dpp_build_attr_status(msg, DPP_STATUS_OK);
	}
skip_wrapped_data:
#endif /* CONFIG_TESTING_OPTIONS */

	wpa_hexdump_buf(MSG_DEBUG,
			"DPP: Authentication Request frame attributes", msg);

	return msg;
}


static struct wpabuf * dpp_auth_build_resp(struct dpp_authentication *auth,
					   enum dpp_status_error status,
					   const struct wpabuf *pr,
					   size_t nonce_len,
					   const u8 *r_pubkey_hash,
					   const u8 *i_pubkey_hash,
					   const u8 *r_nonce, const u8 *i_nonce,
					   const u8 *wrapped_r_auth,
					   size_t wrapped_r_auth_len,
					   const u8 *siv_key)
{
	struct wpabuf *msg;
#define DPP_AUTH_RESP_CLEAR_LEN 2 * (4 + DPP_MAX_NONCE_LEN) + 4 + 1 + \
		4 + 4 + DPP_MAX_HASH_LEN + AES_BLOCK_SIZE
	u8 clear[DPP_AUTH_RESP_CLEAR_LEN];
	u8 wrapped_data[DPP_AUTH_RESP_CLEAR_LEN + AES_BLOCK_SIZE];
	const u8 *addr[2];
	size_t len[2], siv_len, attr_len;
	u8 *attr_start, *attr_end, *pos;

	auth->waiting_auth_conf = 1;
	auth->auth_resp_tries = 0;

	/* Build DPP Authentication Response frame attributes */
	attr_len = 4 + 1 + 2 * (4 + SHA256_MAC_LEN) +
		4 + (pr ? wpabuf_len(pr) : 0) + 4 + sizeof(wrapped_data);
#ifdef CONFIG_DPP2
	attr_len += 5;
#endif /* CONFIG_DPP2 */
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_AUTH_RESP)
		attr_len += 5;
#endif /* CONFIG_TESTING_OPTIONS */
	msg = dpp_alloc_msg(DPP_PA_AUTHENTICATION_RESP, attr_len);
	if (!msg)
		return NULL;

	attr_start = wpabuf_put(msg, 0);

	/* DPP Status */
	if (status != 255)
		dpp_build_attr_status(msg, status);

	/* Responder Bootstrapping Key Hash */
	dpp_build_attr_r_bootstrap_key_hash(msg, r_pubkey_hash);

	/* Initiator Bootstrapping Key Hash (mutual authentication) */
	dpp_build_attr_i_bootstrap_key_hash(msg, i_pubkey_hash);

	/* Responder Protocol Key */
	if (pr) {
		wpabuf_put_le16(msg, DPP_ATTR_R_PROTOCOL_KEY);
		wpabuf_put_le16(msg, wpabuf_len(pr));
		wpabuf_put_buf(msg, pr);
	}

#ifdef CONFIG_DPP2
	/* Protocol Version */
	if (auth->peer_version >= 2) {
		wpabuf_put_le16(msg, DPP_ATTR_PROTOCOL_VERSION);
		wpabuf_put_le16(msg, 1);
		wpabuf_put_u8(msg, DPP_VERSION);
	}
#endif /* CONFIG_DPP2 */

	attr_end = wpabuf_put(msg, 0);

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_WRAPPED_DATA_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no Wrapped Data");
		goto skip_wrapped_data;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* Wrapped data ({R-nonce, I-nonce, R-capabilities, {R-auth}ke}k2) */
	pos = clear;

	if (r_nonce) {
		/* R-nonce */
		WPA_PUT_LE16(pos, DPP_ATTR_R_NONCE);
		pos += 2;
		WPA_PUT_LE16(pos, nonce_len);
		pos += 2;
		os_memcpy(pos, r_nonce, nonce_len);
		pos += nonce_len;
	}

	if (i_nonce) {
		/* I-nonce */
		WPA_PUT_LE16(pos, DPP_ATTR_I_NONCE);
		pos += 2;
		WPA_PUT_LE16(pos, nonce_len);
		pos += 2;
		os_memcpy(pos, i_nonce, nonce_len);
#ifdef CONFIG_TESTING_OPTIONS
		if (dpp_test == DPP_TEST_I_NONCE_MISMATCH_AUTH_RESP) {
			wpa_printf(MSG_INFO, "DPP: TESTING - I-nonce mismatch");
			pos[nonce_len / 2] ^= 0x01;
		}
#endif /* CONFIG_TESTING_OPTIONS */
		pos += nonce_len;
	}

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_R_CAPAB_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no R-capab");
		goto skip_r_capab;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* R-capabilities */
	WPA_PUT_LE16(pos, DPP_ATTR_R_CAPABILITIES);
	pos += 2;
	WPA_PUT_LE16(pos, 1);
	pos += 2;
	auth->r_capab = auth->configurator ? DPP_CAPAB_CONFIGURATOR :
		DPP_CAPAB_ENROLLEE;
	*pos++ = auth->r_capab;
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_ZERO_R_CAPAB) {
		wpa_printf(MSG_INFO, "DPP: TESTING - zero R-capabilities");
		pos[-1] = 0;
	} else if (dpp_test == DPP_TEST_INCOMPATIBLE_R_CAPAB_AUTH_RESP) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - incompatible R-capabilities");
		if ((auth->i_capab & DPP_CAPAB_ROLE_MASK) ==
		    (DPP_CAPAB_CONFIGURATOR | DPP_CAPAB_ENROLLEE))
			pos[-1] = 0;
		else
			pos[-1] = auth->configurator ? DPP_CAPAB_ENROLLEE :
				DPP_CAPAB_CONFIGURATOR;
	}
skip_r_capab:
#endif /* CONFIG_TESTING_OPTIONS */

	if (wrapped_r_auth) {
		/* {R-auth}ke */
		WPA_PUT_LE16(pos, DPP_ATTR_WRAPPED_DATA);
		pos += 2;
		WPA_PUT_LE16(pos, wrapped_r_auth_len);
		pos += 2;
		os_memcpy(pos, wrapped_r_auth, wrapped_r_auth_len);
		pos += wrapped_r_auth_len;
	}

	/* OUI, OUI type, Crypto Suite, DPP frame type */
	addr[0] = wpabuf_head_u8(msg) + 2;
	len[0] = 3 + 1 + 1 + 1;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);

	/* Attributes before Wrapped Data */
	addr[1] = attr_start;
	len[1] = attr_end - attr_start;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);

	siv_len = pos - clear;
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext", clear, siv_len);
	if (aes_siv_encrypt(siv_key, auth->curve->hash_len, clear, siv_len,
			    2, addr, len, wrapped_data) < 0) {
		wpabuf_free(msg);
		return NULL;
	}
	siv_len += AES_BLOCK_SIZE;
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, siv_len);

	wpabuf_put_le16(msg, DPP_ATTR_WRAPPED_DATA);
	wpabuf_put_le16(msg, siv_len);
	wpabuf_put_data(msg, wrapped_data, siv_len);

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - attr after Wrapped Data");
		dpp_build_attr_status(msg, DPP_STATUS_OK);
	}
skip_wrapped_data:
#endif /* CONFIG_TESTING_OPTIONS */

	wpa_hexdump_buf(MSG_DEBUG,
			"DPP: Authentication Response frame attributes", msg);
	return msg;
}


static int dpp_channel_ok_init(struct hostapd_hw_modes *own_modes,
			       u16 num_modes, unsigned int freq)
{
	u16 m;
	int c, flag;

	if (!own_modes || !num_modes)
		return 1;

	for (m = 0; m < num_modes; m++) {
		for (c = 0; c < own_modes[m].num_channels; c++) {
			if ((unsigned int) own_modes[m].channels[c].freq !=
			    freq)
				continue;
			flag = own_modes[m].channels[c].flag;
			if (!(flag & (HOSTAPD_CHAN_DISABLED |
				      HOSTAPD_CHAN_NO_IR |
				      HOSTAPD_CHAN_RADAR)))
				return 1;
		}
	}

	wpa_printf(MSG_DEBUG, "DPP: Peer channel %u MHz not supported", freq);
	return 0;
}


static int freq_included(const unsigned int freqs[], unsigned int num,
			 unsigned int freq)
{
	while (num > 0) {
		if (freqs[--num] == freq)
			return 1;
	}
	return 0;
}


static void freq_to_start(unsigned int freqs[], unsigned int num,
			  unsigned int freq)
{
	unsigned int i;

	for (i = 0; i < num; i++) {
		if (freqs[i] == freq)
			break;
	}
	if (i == 0 || i >= num)
		return;
	os_memmove(&freqs[1], &freqs[0], i * sizeof(freqs[0]));
	freqs[0] = freq;
}


static int dpp_channel_intersect(struct dpp_authentication *auth,
				 struct hostapd_hw_modes *own_modes,
				 u16 num_modes)
{
	struct dpp_bootstrap_info *peer_bi = auth->peer_bi;
	unsigned int i, freq;

	for (i = 0; i < peer_bi->num_freq; i++) {
		freq = peer_bi->freq[i];
		if (freq_included(auth->freq, auth->num_freq, freq))
			continue;
		if (dpp_channel_ok_init(own_modes, num_modes, freq))
			auth->freq[auth->num_freq++] = freq;
	}
	if (!auth->num_freq) {
		wpa_printf(MSG_INFO,
			   "DPP: No available channels for initiating DPP Authentication");
		return -1;
	}
	auth->curr_freq = auth->freq[0];
	return 0;
}


static int dpp_channel_local_list(struct dpp_authentication *auth,
				  struct hostapd_hw_modes *own_modes,
				  u16 num_modes)
{
	u16 m;
	int c, flag;
	unsigned int freq;

	auth->num_freq = 0;

	if (!own_modes || !num_modes) {
		auth->freq[0] = 2412;
		auth->freq[1] = 2437;
		auth->freq[2] = 2462;
		auth->num_freq = 3;
		return 0;
	}

	for (m = 0; m < num_modes; m++) {
		for (c = 0; c < own_modes[m].num_channels; c++) {
			freq = own_modes[m].channels[c].freq;
			flag = own_modes[m].channels[c].flag;
			if (flag & (HOSTAPD_CHAN_DISABLED |
				    HOSTAPD_CHAN_NO_IR |
				    HOSTAPD_CHAN_RADAR))
				continue;
			if (freq_included(auth->freq, auth->num_freq, freq))
				continue;
			auth->freq[auth->num_freq++] = freq;
			if (auth->num_freq == DPP_BOOTSTRAP_MAX_FREQ) {
				m = num_modes;
				break;
			}
		}
	}

	return auth->num_freq == 0 ? -1 : 0;
}


static int dpp_prepare_channel_list(struct dpp_authentication *auth,
				    unsigned int neg_freq,
				    struct hostapd_hw_modes *own_modes,
				    u16 num_modes)
{
	int res;
	char freqs[DPP_BOOTSTRAP_MAX_FREQ * 6 + 10], *pos, *end;
	unsigned int i;

	if (!own_modes) {
		if (!neg_freq)
			return -1;
		auth->num_freq = 1;
		auth->freq[0] = neg_freq;
		return 0;
	}

	if (auth->peer_bi->num_freq > 0)
		res = dpp_channel_intersect(auth, own_modes, num_modes);
	else
		res = dpp_channel_local_list(auth, own_modes, num_modes);
	if (res < 0)
		return res;

	/* Prioritize 2.4 GHz channels 6, 1, 11 (in this order) to hit the most
	 * likely channels first. */
	freq_to_start(auth->freq, auth->num_freq, 2462);
	freq_to_start(auth->freq, auth->num_freq, 2412);
	freq_to_start(auth->freq, auth->num_freq, 2437);

	auth->freq_idx = 0;
	auth->curr_freq = auth->freq[0];

	pos = freqs;
	end = pos + sizeof(freqs);
	for (i = 0; i < auth->num_freq; i++) {
		res = os_snprintf(pos, end - pos, " %u", auth->freq[i]);
		if (os_snprintf_error(end - pos, res))
			break;
		pos += res;
	}
	*pos = '\0';
	wpa_printf(MSG_DEBUG, "DPP: Possible frequencies for initiating:%s",
		   freqs);

	return 0;
}


static int dpp_gen_uri(struct dpp_bootstrap_info *bi)
{
	char macstr[ETH_ALEN * 2 + 10];
	size_t len;

	len = 4; /* "DPP:" */
	if (bi->chan)
		len += 3 + os_strlen(bi->chan); /* C:...; */
	if (is_zero_ether_addr(bi->mac_addr))
		macstr[0] = '\0';
	else
		os_snprintf(macstr, sizeof(macstr), "M:" COMPACT_MACSTR ";",
			    MAC2STR(bi->mac_addr));
	len += os_strlen(macstr); /* M:...; */
	if (bi->info)
		len += 3 + os_strlen(bi->info); /* I:...; */
#ifdef CONFIG_DPP2
	len += 4; /* V:2; */
#endif /* CONFIG_DPP2 */
	len += 4 + os_strlen(bi->pk); /* K:...;; */

	os_free(bi->uri);
	bi->uri = os_malloc(len + 1);
	if (!bi->uri)
		return -1;
	os_snprintf(bi->uri, len + 1, "DPP:%s%s%s%s%s%s%s%sK:%s;;",
		    bi->chan ? "C:" : "", bi->chan ? bi->chan : "",
		    bi->chan ? ";" : "",
		    macstr,
		    bi->info ? "I:" : "", bi->info ? bi->info : "",
		    bi->info ? ";" : "",
		    DPP_VERSION == 2 ? "V:2;" : "",
		    bi->pk);
	return 0;
}


static int dpp_autogen_bootstrap_key(struct dpp_authentication *auth)
{
	struct dpp_bootstrap_info *bi;

	if (auth->own_bi)
		return 0; /* already generated */

	bi = os_zalloc(sizeof(*bi));
	if (!bi)
		return -1;
	bi->type = DPP_BOOTSTRAP_QR_CODE;
	if (dpp_keygen(bi, auth->peer_bi->curve->name, NULL, 0) < 0 ||
	    dpp_gen_uri(bi) < 0)
		goto fail;
	wpa_printf(MSG_DEBUG,
		   "DPP: Auto-generated own bootstrapping key info: URI %s",
		   bi->uri);

	auth->tmp_own_bi = auth->own_bi = bi;

	return 0;
fail:
	dpp_bootstrap_info_free(bi);
	return -1;
}


struct dpp_authentication *
dpp_alloc_auth(struct dpp_global *dpp, void *msg_ctx)
{
	struct dpp_authentication *auth;

	auth = os_zalloc(sizeof(*auth));
	if (!auth)
		return NULL;
	auth->global = dpp;
	auth->msg_ctx = msg_ctx;
	auth->conf_resp_status = 255;
	return auth;
}


struct dpp_authentication * dpp_auth_init(struct dpp_global *dpp, void *msg_ctx,
					  struct dpp_bootstrap_info *peer_bi,
					  struct dpp_bootstrap_info *own_bi,
					  u8 dpp_allowed_roles,
					  unsigned int neg_freq,
					  struct hostapd_hw_modes *own_modes,
					  u16 num_modes)
{
	struct dpp_authentication *auth;
	size_t nonce_len;
	size_t secret_len;
	struct wpabuf *pi = NULL;
	const u8 *r_pubkey_hash, *i_pubkey_hash;
#ifdef CONFIG_TESTING_OPTIONS
	u8 test_hash[SHA256_MAC_LEN];
#endif /* CONFIG_TESTING_OPTIONS */

	auth = dpp_alloc_auth(dpp, msg_ctx);
	if (!auth)
		return NULL;
	if (peer_bi->configurator_params &&
	    dpp_set_configurator(auth, peer_bi->configurator_params) < 0)
		goto fail;
	auth->initiator = 1;
	auth->waiting_auth_resp = 1;
	auth->allowed_roles = dpp_allowed_roles;
	auth->configurator = !!(dpp_allowed_roles & DPP_CAPAB_CONFIGURATOR);
	auth->peer_bi = peer_bi;
	auth->own_bi = own_bi;
	auth->curve = peer_bi->curve;

	if (dpp_autogen_bootstrap_key(auth) < 0 ||
	    dpp_prepare_channel_list(auth, neg_freq, own_modes, num_modes) < 0)
		goto fail;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_nonce_override_len > 0) {
		wpa_printf(MSG_INFO, "DPP: TESTING - override I-nonce");
		nonce_len = dpp_nonce_override_len;
		os_memcpy(auth->i_nonce, dpp_nonce_override, nonce_len);
	} else {
		nonce_len = auth->curve->nonce_len;
		if (random_get_bytes(auth->i_nonce, nonce_len)) {
			wpa_printf(MSG_ERROR,
				   "DPP: Failed to generate I-nonce");
			goto fail;
		}
	}
#else /* CONFIG_TESTING_OPTIONS */
	nonce_len = auth->curve->nonce_len;
	if (random_get_bytes(auth->i_nonce, nonce_len)) {
		wpa_printf(MSG_ERROR, "DPP: Failed to generate I-nonce");
		goto fail;
	}
#endif /* CONFIG_TESTING_OPTIONS */
	wpa_hexdump(MSG_DEBUG, "DPP: I-nonce", auth->i_nonce, nonce_len);

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_protocol_key_override_len) {
		const struct dpp_curve_params *tmp_curve;

		wpa_printf(MSG_INFO,
			   "DPP: TESTING - override protocol key");
		auth->own_protocol_key = dpp_set_keypair(
			&tmp_curve, dpp_protocol_key_override,
			dpp_protocol_key_override_len);
	} else {
		auth->own_protocol_key = dpp_gen_keypair(auth->curve);
	}
#else /* CONFIG_TESTING_OPTIONS */
	auth->own_protocol_key = dpp_gen_keypair(auth->curve);
#endif /* CONFIG_TESTING_OPTIONS */
	if (!auth->own_protocol_key)
		goto fail;

	pi = dpp_get_pubkey_point(auth->own_protocol_key, 0);
	if (!pi)
		goto fail;

	/* ECDH: M = pI * BR */
	if (dpp_ecdh(auth->own_protocol_key, auth->peer_bi->pubkey,
		     auth->Mx, &secret_len) < 0)
		goto fail;
	auth->secret_len = secret_len;

	wpa_hexdump_key(MSG_DEBUG, "DPP: ECDH shared secret (M.x)",
			auth->Mx, auth->secret_len);
	auth->Mx_len = auth->secret_len;

	if (dpp_derive_k1(auth->Mx, auth->secret_len, auth->k1,
			  auth->curve->hash_len) < 0)
		goto fail;

	r_pubkey_hash = auth->peer_bi->pubkey_hash;
	i_pubkey_hash = auth->own_bi->pubkey_hash;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_R_BOOTSTRAP_KEY_HASH_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no R-Bootstrap Key Hash");
		r_pubkey_hash = NULL;
	} else if (dpp_test == DPP_TEST_INVALID_R_BOOTSTRAP_KEY_HASH_AUTH_REQ) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - invalid R-Bootstrap Key Hash");
		os_memcpy(test_hash, r_pubkey_hash, SHA256_MAC_LEN);
		test_hash[SHA256_MAC_LEN - 1] ^= 0x01;
		r_pubkey_hash = test_hash;
	} else if (dpp_test == DPP_TEST_NO_I_BOOTSTRAP_KEY_HASH_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-Bootstrap Key Hash");
		i_pubkey_hash = NULL;
	} else if (dpp_test == DPP_TEST_INVALID_I_BOOTSTRAP_KEY_HASH_AUTH_REQ) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - invalid I-Bootstrap Key Hash");
		os_memcpy(test_hash, i_pubkey_hash, SHA256_MAC_LEN);
		test_hash[SHA256_MAC_LEN - 1] ^= 0x01;
		i_pubkey_hash = test_hash;
	} else if (dpp_test == DPP_TEST_NO_I_PROTO_KEY_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-Proto Key");
		wpabuf_free(pi);
		pi = NULL;
	} else if (dpp_test == DPP_TEST_INVALID_I_PROTO_KEY_AUTH_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - invalid I-Proto Key");
		wpabuf_free(pi);
		pi = wpabuf_alloc(2 * auth->curve->prime_len);
		if (!pi || dpp_test_gen_invalid_key(pi, auth->curve) < 0)
			goto fail;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	if (neg_freq && auth->num_freq == 1 && auth->freq[0] == neg_freq)
		neg_freq = 0;
	auth->req_msg = dpp_auth_build_req(auth, pi, nonce_len, r_pubkey_hash,
					   i_pubkey_hash, neg_freq);
	if (!auth->req_msg)
		goto fail;

out:
	wpabuf_free(pi);
	return auth;
fail:
	dpp_auth_deinit(auth);
	auth = NULL;
	goto out;
}


static struct wpabuf * dpp_build_conf_req_attr(struct dpp_authentication *auth,
					       const char *json)
{
	size_t nonce_len;
	size_t json_len, clear_len;
	struct wpabuf *clear = NULL, *msg = NULL;
	u8 *wrapped;
	size_t attr_len;

	wpa_printf(MSG_DEBUG, "DPP: Build configuration request");

	nonce_len = auth->curve->nonce_len;
	if (random_get_bytes(auth->e_nonce, nonce_len)) {
		wpa_printf(MSG_ERROR, "DPP: Failed to generate E-nonce");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: E-nonce", auth->e_nonce, nonce_len);
	json_len = os_strlen(json);
	wpa_hexdump_ascii(MSG_DEBUG, "DPP: configRequest JSON", json, json_len);

	/* { E-nonce, configAttrib }ke */
	clear_len = 4 + nonce_len + 4 + json_len;
	clear = wpabuf_alloc(clear_len);
	attr_len = 4 + clear_len + AES_BLOCK_SIZE;
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_CONF_REQ)
		attr_len += 5;
#endif /* CONFIG_TESTING_OPTIONS */
	msg = wpabuf_alloc(attr_len);
	if (!clear || !msg)
		goto fail;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_E_NONCE_CONF_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no E-nonce");
		goto skip_e_nonce;
	}
	if (dpp_test == DPP_TEST_INVALID_E_NONCE_CONF_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - invalid E-nonce");
		wpabuf_put_le16(clear, DPP_ATTR_ENROLLEE_NONCE);
		wpabuf_put_le16(clear, nonce_len - 1);
		wpabuf_put_data(clear, auth->e_nonce, nonce_len - 1);
		goto skip_e_nonce;
	}
	if (dpp_test == DPP_TEST_NO_WRAPPED_DATA_CONF_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no Wrapped Data");
		goto skip_wrapped_data;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* E-nonce */
	wpabuf_put_le16(clear, DPP_ATTR_ENROLLEE_NONCE);
	wpabuf_put_le16(clear, nonce_len);
	wpabuf_put_data(clear, auth->e_nonce, nonce_len);

#ifdef CONFIG_TESTING_OPTIONS
skip_e_nonce:
	if (dpp_test == DPP_TEST_NO_CONFIG_ATTR_OBJ_CONF_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no configAttrib");
		goto skip_conf_attr_obj;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* configAttrib */
	wpabuf_put_le16(clear, DPP_ATTR_CONFIG_ATTR_OBJ);
	wpabuf_put_le16(clear, json_len);
	wpabuf_put_data(clear, json, json_len);

#ifdef CONFIG_TESTING_OPTIONS
skip_conf_attr_obj:
#endif /* CONFIG_TESTING_OPTIONS */

	wpabuf_put_le16(msg, DPP_ATTR_WRAPPED_DATA);
	wpabuf_put_le16(msg, wpabuf_len(clear) + AES_BLOCK_SIZE);
	wrapped = wpabuf_put(msg, wpabuf_len(clear) + AES_BLOCK_SIZE);

	/* No AES-SIV AD */
	wpa_hexdump_buf(MSG_DEBUG, "DPP: AES-SIV cleartext", clear);
	if (aes_siv_encrypt(auth->ke, auth->curve->hash_len,
			    wpabuf_head(clear), wpabuf_len(clear),
			    0, NULL, NULL, wrapped) < 0)
		goto fail;
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped, wpabuf_len(clear) + AES_BLOCK_SIZE);

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_CONF_REQ) {
		wpa_printf(MSG_INFO, "DPP: TESTING - attr after Wrapped Data");
		dpp_build_attr_status(msg, DPP_STATUS_OK);
	}
skip_wrapped_data:
#endif /* CONFIG_TESTING_OPTIONS */

	wpa_hexdump_buf(MSG_DEBUG,
			"DPP: Configuration Request frame attributes", msg);
	wpabuf_free(clear);
	return msg;

fail:
	wpabuf_free(clear);
	wpabuf_free(msg);
	return NULL;
}


static void dpp_write_adv_proto(struct wpabuf *buf)
{
	/* Advertisement Protocol IE */
	wpabuf_put_u8(buf, WLAN_EID_ADV_PROTO);
	wpabuf_put_u8(buf, 8); /* Length */
	wpabuf_put_u8(buf, 0x7f);
	wpabuf_put_u8(buf, WLAN_EID_VENDOR_SPECIFIC);
	wpabuf_put_u8(buf, 5);
	wpabuf_put_be24(buf, OUI_WFA);
	wpabuf_put_u8(buf, DPP_OUI_TYPE);
	wpabuf_put_u8(buf, 0x01);
}


static void dpp_write_gas_query(struct wpabuf *buf, struct wpabuf *query)
{
	/* GAS Query */
	wpabuf_put_le16(buf, wpabuf_len(query));
	wpabuf_put_buf(buf, query);
}


struct wpabuf * dpp_build_conf_req(struct dpp_authentication *auth,
				   const char *json)
{
	struct wpabuf *buf, *conf_req;

	conf_req = dpp_build_conf_req_attr(auth, json);
	if (!conf_req) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No configuration request data available");
		return NULL;
	}

	buf = gas_build_initial_req(0, 10 + 2 + wpabuf_len(conf_req));
	if (!buf) {
		wpabuf_free(conf_req);
		return NULL;
	}

	dpp_write_adv_proto(buf);
	dpp_write_gas_query(buf, conf_req);
	wpabuf_free(conf_req);
	wpa_hexdump_buf(MSG_MSGDUMP, "DPP: GAS Config Request", buf);

	return buf;
}


struct wpabuf * dpp_build_conf_req_helper(struct dpp_authentication *auth,
					  const char *name,
					  enum dpp_netrole netrole,
					  const char *mud_url, int *opclasses)
{
	size_t len, name_len;
	const char *tech = "infra";
	const char *dpp_name;
	struct wpabuf *buf, *json;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_INVALID_CONFIG_ATTR_OBJ_CONF_REQ) {
		static const char *bogus_tech = "knfra";

		wpa_printf(MSG_INFO, "DPP: TESTING - invalid Config Attr");
		tech = bogus_tech;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	dpp_name = name ? name : "Test";
	name_len = os_strlen(dpp_name);

	len = 100 + name_len * 6 + 1 + int_array_len(opclasses) * 4;
	if (mud_url && mud_url[0])
		len += 10 + os_strlen(mud_url);
	json = wpabuf_alloc(len);
	if (!json)
		return NULL;

	json_start_object(json, NULL);
	if (json_add_string_escape(json, "name", dpp_name, name_len) < 0) {
		wpabuf_free(json);
		return NULL;
	}
	json_value_sep(json);
	json_add_string(json, "wi-fi_tech", tech);
	json_value_sep(json);
	json_add_string(json, "netRole", dpp_netrole_str(netrole));
	if (mud_url && mud_url[0]) {
		json_value_sep(json);
		json_add_string(json, "mudurl", mud_url);
	}
	if (opclasses) {
		int i;

		json_value_sep(json);
		json_start_array(json, "bandSupport");
		for (i = 0; opclasses[i]; i++)
			wpabuf_printf(json, "%s%u", i ? "," : "", opclasses[i]);
		json_end_array(json);
	}
	json_end_object(json);

	buf = dpp_build_conf_req(auth, wpabuf_head(json));
	wpabuf_free(json);

	return buf;
}


static void dpp_auth_success(struct dpp_authentication *auth)
{
	wpa_printf(MSG_DEBUG,
		   "DPP: Authentication success - clear temporary keys");
	os_memset(auth->Mx, 0, sizeof(auth->Mx));
	auth->Mx_len = 0;
	os_memset(auth->Nx, 0, sizeof(auth->Nx));
	auth->Nx_len = 0;
	os_memset(auth->Lx, 0, sizeof(auth->Lx));
	auth->Lx_len = 0;
	os_memset(auth->k1, 0, sizeof(auth->k1));
	os_memset(auth->k2, 0, sizeof(auth->k2));

	auth->auth_success = 1;
}


static int dpp_auth_build_resp_ok(struct dpp_authentication *auth)
{
	size_t nonce_len;
	size_t secret_len;
	struct wpabuf *msg, *pr = NULL;
	u8 r_auth[4 + DPP_MAX_HASH_LEN];
	u8 wrapped_r_auth[4 + DPP_MAX_HASH_LEN + AES_BLOCK_SIZE], *w_r_auth;
	size_t wrapped_r_auth_len;
	int ret = -1;
	const u8 *r_pubkey_hash, *i_pubkey_hash, *r_nonce, *i_nonce;
	enum dpp_status_error status = DPP_STATUS_OK;
#ifdef CONFIG_TESTING_OPTIONS
	u8 test_hash[SHA256_MAC_LEN];
#endif /* CONFIG_TESTING_OPTIONS */

	wpa_printf(MSG_DEBUG, "DPP: Build Authentication Response");
	if (!auth->own_bi)
		return -1;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_nonce_override_len > 0) {
		wpa_printf(MSG_INFO, "DPP: TESTING - override R-nonce");
		nonce_len = dpp_nonce_override_len;
		os_memcpy(auth->r_nonce, dpp_nonce_override, nonce_len);
	} else {
		nonce_len = auth->curve->nonce_len;
		if (random_get_bytes(auth->r_nonce, nonce_len)) {
			wpa_printf(MSG_ERROR,
				   "DPP: Failed to generate R-nonce");
			goto fail;
		}
	}
#else /* CONFIG_TESTING_OPTIONS */
	nonce_len = auth->curve->nonce_len;
	if (random_get_bytes(auth->r_nonce, nonce_len)) {
		wpa_printf(MSG_ERROR, "DPP: Failed to generate R-nonce");
		goto fail;
	}
#endif /* CONFIG_TESTING_OPTIONS */
	wpa_hexdump(MSG_DEBUG, "DPP: R-nonce", auth->r_nonce, nonce_len);

	EVP_PKEY_free(auth->own_protocol_key);
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_protocol_key_override_len) {
		const struct dpp_curve_params *tmp_curve;

		wpa_printf(MSG_INFO,
			   "DPP: TESTING - override protocol key");
		auth->own_protocol_key = dpp_set_keypair(
			&tmp_curve, dpp_protocol_key_override,
			dpp_protocol_key_override_len);
	} else {
		auth->own_protocol_key = dpp_gen_keypair(auth->curve);
	}
#else /* CONFIG_TESTING_OPTIONS */
	auth->own_protocol_key = dpp_gen_keypair(auth->curve);
#endif /* CONFIG_TESTING_OPTIONS */
	if (!auth->own_protocol_key)
		goto fail;

	pr = dpp_get_pubkey_point(auth->own_protocol_key, 0);
	if (!pr)
		goto fail;

	/* ECDH: N = pR * PI */
	if (dpp_ecdh(auth->own_protocol_key, auth->peer_protocol_key,
		     auth->Nx, &secret_len) < 0)
		goto fail;

	wpa_hexdump_key(MSG_DEBUG, "DPP: ECDH shared secret (N.x)",
			auth->Nx, auth->secret_len);
	auth->Nx_len = auth->secret_len;

	if (dpp_derive_k2(auth->Nx, auth->secret_len, auth->k2,
			  auth->curve->hash_len) < 0)
		goto fail;

	if (auth->own_bi && auth->peer_bi) {
		/* Mutual authentication */
		if (dpp_auth_derive_l_responder(auth) < 0)
			goto fail;
	}

	if (dpp_derive_bk_ke(auth) < 0)
		goto fail;

	/* R-auth = H(I-nonce | R-nonce | PI.x | PR.x | [BI.x |] BR.x | 0) */
	WPA_PUT_LE16(r_auth, DPP_ATTR_R_AUTH_TAG);
	WPA_PUT_LE16(&r_auth[2], auth->curve->hash_len);
	if (dpp_gen_r_auth(auth, r_auth + 4) < 0)
		goto fail;
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_R_AUTH_MISMATCH_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - R-auth mismatch");
		r_auth[4 + auth->curve->hash_len / 2] ^= 0x01;
	}
#endif /* CONFIG_TESTING_OPTIONS */
	if (aes_siv_encrypt(auth->ke, auth->curve->hash_len,
			    r_auth, 4 + auth->curve->hash_len,
			    0, NULL, NULL, wrapped_r_auth) < 0)
		goto fail;
	wrapped_r_auth_len = 4 + auth->curve->hash_len + AES_BLOCK_SIZE;
	wpa_hexdump(MSG_DEBUG, "DPP: {R-auth}ke",
		    wrapped_r_auth, wrapped_r_auth_len);
	w_r_auth = wrapped_r_auth;

	r_pubkey_hash = auth->own_bi->pubkey_hash;
	if (auth->peer_bi)
		i_pubkey_hash = auth->peer_bi->pubkey_hash;
	else
		i_pubkey_hash = NULL;

	i_nonce = auth->i_nonce;
	r_nonce = auth->r_nonce;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_R_BOOTSTRAP_KEY_HASH_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no R-Bootstrap Key Hash");
		r_pubkey_hash = NULL;
	} else if (dpp_test ==
		   DPP_TEST_INVALID_R_BOOTSTRAP_KEY_HASH_AUTH_RESP) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - invalid R-Bootstrap Key Hash");
		os_memcpy(test_hash, r_pubkey_hash, SHA256_MAC_LEN);
		test_hash[SHA256_MAC_LEN - 1] ^= 0x01;
		r_pubkey_hash = test_hash;
	} else if (dpp_test == DPP_TEST_NO_I_BOOTSTRAP_KEY_HASH_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-Bootstrap Key Hash");
		i_pubkey_hash = NULL;
	} else if (dpp_test ==
		   DPP_TEST_INVALID_I_BOOTSTRAP_KEY_HASH_AUTH_RESP) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - invalid I-Bootstrap Key Hash");
		if (i_pubkey_hash)
			os_memcpy(test_hash, i_pubkey_hash, SHA256_MAC_LEN);
		else
			os_memset(test_hash, 0, SHA256_MAC_LEN);
		test_hash[SHA256_MAC_LEN - 1] ^= 0x01;
		i_pubkey_hash = test_hash;
	} else if (dpp_test == DPP_TEST_NO_R_PROTO_KEY_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no R-Proto Key");
		wpabuf_free(pr);
		pr = NULL;
	} else if (dpp_test == DPP_TEST_INVALID_R_PROTO_KEY_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - invalid R-Proto Key");
		wpabuf_free(pr);
		pr = wpabuf_alloc(2 * auth->curve->prime_len);
		if (!pr || dpp_test_gen_invalid_key(pr, auth->curve) < 0)
			goto fail;
	} else if (dpp_test == DPP_TEST_NO_R_AUTH_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no R-Auth");
		w_r_auth = NULL;
		wrapped_r_auth_len = 0;
	} else if (dpp_test == DPP_TEST_NO_STATUS_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no Status");
		status = 255;
	} else if (dpp_test == DPP_TEST_INVALID_STATUS_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - invalid Status");
		status = 254;
	} else if (dpp_test == DPP_TEST_NO_R_NONCE_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no R-nonce");
		r_nonce = NULL;
	} else if (dpp_test == DPP_TEST_NO_I_NONCE_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-nonce");
		i_nonce = NULL;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	msg = dpp_auth_build_resp(auth, status, pr, nonce_len,
				  r_pubkey_hash, i_pubkey_hash,
				  r_nonce, i_nonce,
				  w_r_auth, wrapped_r_auth_len,
				  auth->k2);
	if (!msg)
		goto fail;
	wpabuf_free(auth->resp_msg);
	auth->resp_msg = msg;
	ret = 0;
fail:
	wpabuf_free(pr);
	return ret;
}


static int dpp_auth_build_resp_status(struct dpp_authentication *auth,
				      enum dpp_status_error status)
{
	struct wpabuf *msg;
	const u8 *r_pubkey_hash, *i_pubkey_hash, *i_nonce;
#ifdef CONFIG_TESTING_OPTIONS
	u8 test_hash[SHA256_MAC_LEN];
#endif /* CONFIG_TESTING_OPTIONS */

	if (!auth->own_bi)
		return -1;
	wpa_printf(MSG_DEBUG, "DPP: Build Authentication Response");

	r_pubkey_hash = auth->own_bi->pubkey_hash;
	if (auth->peer_bi)
		i_pubkey_hash = auth->peer_bi->pubkey_hash;
	else
		i_pubkey_hash = NULL;

	i_nonce = auth->i_nonce;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_R_BOOTSTRAP_KEY_HASH_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no R-Bootstrap Key Hash");
		r_pubkey_hash = NULL;
	} else if (dpp_test ==
		   DPP_TEST_INVALID_R_BOOTSTRAP_KEY_HASH_AUTH_RESP) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - invalid R-Bootstrap Key Hash");
		os_memcpy(test_hash, r_pubkey_hash, SHA256_MAC_LEN);
		test_hash[SHA256_MAC_LEN - 1] ^= 0x01;
		r_pubkey_hash = test_hash;
	} else if (dpp_test == DPP_TEST_NO_I_BOOTSTRAP_KEY_HASH_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-Bootstrap Key Hash");
		i_pubkey_hash = NULL;
	} else if (dpp_test ==
		   DPP_TEST_INVALID_I_BOOTSTRAP_KEY_HASH_AUTH_RESP) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - invalid I-Bootstrap Key Hash");
		if (i_pubkey_hash)
			os_memcpy(test_hash, i_pubkey_hash, SHA256_MAC_LEN);
		else
			os_memset(test_hash, 0, SHA256_MAC_LEN);
		test_hash[SHA256_MAC_LEN - 1] ^= 0x01;
		i_pubkey_hash = test_hash;
	} else if (dpp_test == DPP_TEST_NO_STATUS_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no Status");
		status = 255;
	} else if (dpp_test == DPP_TEST_NO_I_NONCE_AUTH_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-nonce");
		i_nonce = NULL;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	msg = dpp_auth_build_resp(auth, status, NULL, auth->curve->nonce_len,
				  r_pubkey_hash, i_pubkey_hash,
				  NULL, i_nonce, NULL, 0, auth->k1);
	if (!msg)
		return -1;
	wpabuf_free(auth->resp_msg);
	auth->resp_msg = msg;
	return 0;
}


struct dpp_authentication *
dpp_auth_req_rx(struct dpp_global *dpp, void *msg_ctx, u8 dpp_allowed_roles,
		int qr_mutual, struct dpp_bootstrap_info *peer_bi,
		struct dpp_bootstrap_info *own_bi,
		unsigned int freq, const u8 *hdr, const u8 *attr_start,
		size_t attr_len)
{
	EVP_PKEY *pi = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	size_t secret_len;
	const u8 *addr[2];
	size_t len[2];
	u8 *unwrapped = NULL;
	size_t unwrapped_len = 0;
	const u8 *wrapped_data, *i_proto, *i_nonce, *i_capab, *i_bootstrap,
		*channel;
	u16 wrapped_data_len, i_proto_len, i_nonce_len, i_capab_len,
		i_bootstrap_len, channel_len;
	struct dpp_authentication *auth = NULL;
#ifdef CONFIG_DPP2
	const u8 *version;
	u16 version_len;
#endif /* CONFIG_DPP2 */

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_STOP_AT_AUTH_REQ) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - stop at Authentication Request");
		return NULL;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	wrapped_data = dpp_get_attr(attr_start, attr_len, DPP_ATTR_WRAPPED_DATA,
				    &wrapped_data_len);
	if (!wrapped_data || wrapped_data_len < AES_BLOCK_SIZE) {
		wpa_msg(msg_ctx, MSG_INFO, DPP_EVENT_FAIL
			"Missing or invalid required Wrapped Data attribute");
		return NULL;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: Wrapped Data",
		    wrapped_data, wrapped_data_len);
	attr_len = wrapped_data - 4 - attr_start;

	auth = dpp_alloc_auth(dpp, msg_ctx);
	if (!auth)
		goto fail;
	if (peer_bi && peer_bi->configurator_params &&
	    dpp_set_configurator(auth, peer_bi->configurator_params) < 0)
		goto fail;
	auth->peer_bi = peer_bi;
	auth->own_bi = own_bi;
	auth->curve = own_bi->curve;
	auth->curr_freq = freq;

	auth->peer_version = 1; /* default to the first version */
#ifdef CONFIG_DPP2
	version = dpp_get_attr(attr_start, attr_len, DPP_ATTR_PROTOCOL_VERSION,
			       &version_len);
	if (version && DPP_VERSION > 1) {
		if (version_len < 1 || version[0] == 0) {
			dpp_auth_fail(auth,
				      "Invalid Protocol Version attribute");
			goto fail;
		}
		auth->peer_version = version[0];
		wpa_printf(MSG_DEBUG, "DPP: Peer protocol version %u",
			   auth->peer_version);
	}
#endif /* CONFIG_DPP2 */

	channel = dpp_get_attr(attr_start, attr_len, DPP_ATTR_CHANNEL,
			       &channel_len);
	if (channel) {
		int neg_freq;

		if (channel_len < 2) {
			dpp_auth_fail(auth, "Too short Channel attribute");
			goto fail;
		}

		neg_freq = ieee80211_chan_to_freq(NULL, channel[0], channel[1]);
		wpa_printf(MSG_DEBUG,
			   "DPP: Initiator requested different channel for negotiation: op_class=%u channel=%u --> freq=%d",
			   channel[0], channel[1], neg_freq);
		if (neg_freq < 0) {
			dpp_auth_fail(auth,
				      "Unsupported Channel attribute value");
			goto fail;
		}

		if (auth->curr_freq != (unsigned int) neg_freq) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Changing negotiation channel from %u MHz to %u MHz",
				   freq, neg_freq);
			auth->curr_freq = neg_freq;
		}
	}

	i_proto = dpp_get_attr(attr_start, attr_len, DPP_ATTR_I_PROTOCOL_KEY,
			       &i_proto_len);
	if (!i_proto) {
		dpp_auth_fail(auth,
			      "Missing required Initiator Protocol Key attribute");
		goto fail;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: Initiator Protocol Key",
		    i_proto, i_proto_len);

	/* M = bR * PI */
	pi = dpp_set_pubkey_point(own_bi->pubkey, i_proto, i_proto_len);
	if (!pi) {
		dpp_auth_fail(auth, "Invalid Initiator Protocol Key");
		goto fail;
	}
	dpp_debug_print_key("Peer (Initiator) Protocol Key", pi);

	if (dpp_ecdh(own_bi->pubkey, pi, auth->Mx, &secret_len) < 0)
		goto fail;
	auth->secret_len = secret_len;

	wpa_hexdump_key(MSG_DEBUG, "DPP: ECDH shared secret (M.x)",
			auth->Mx, auth->secret_len);
	auth->Mx_len = auth->secret_len;

	if (dpp_derive_k1(auth->Mx, auth->secret_len, auth->k1,
			  auth->curve->hash_len) < 0)
		goto fail;

	addr[0] = hdr;
	len[0] = DPP_HDR_LEN;
	addr[1] = attr_start;
	len[1] = attr_len;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped)
		goto fail;
	if (aes_siv_decrypt(auth->k1, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    2, addr, len, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	i_nonce = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_I_NONCE,
			       &i_nonce_len);
	if (!i_nonce || i_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth, "Missing or invalid I-nonce");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: I-nonce", i_nonce, i_nonce_len);
	os_memcpy(auth->i_nonce, i_nonce, i_nonce_len);

	i_capab = dpp_get_attr(unwrapped, unwrapped_len,
			       DPP_ATTR_I_CAPABILITIES,
			       &i_capab_len);
	if (!i_capab || i_capab_len < 1) {
		dpp_auth_fail(auth, "Missing or invalid I-capabilities");
		goto fail;
	}
	auth->i_capab = i_capab[0];
	wpa_printf(MSG_DEBUG, "DPP: I-capabilities: 0x%02x", auth->i_capab);

	bin_clear_free(unwrapped, unwrapped_len);
	unwrapped = NULL;

	switch (auth->i_capab & DPP_CAPAB_ROLE_MASK) {
	case DPP_CAPAB_ENROLLEE:
		if (!(dpp_allowed_roles & DPP_CAPAB_CONFIGURATOR)) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Local policy does not allow Configurator role");
			goto not_compatible;
		}
		wpa_printf(MSG_DEBUG, "DPP: Acting as Configurator");
		auth->configurator = 1;
		break;
	case DPP_CAPAB_CONFIGURATOR:
		if (!(dpp_allowed_roles & DPP_CAPAB_ENROLLEE)) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Local policy does not allow Enrollee role");
			goto not_compatible;
		}
		wpa_printf(MSG_DEBUG, "DPP: Acting as Enrollee");
		auth->configurator = 0;
		break;
	case DPP_CAPAB_CONFIGURATOR | DPP_CAPAB_ENROLLEE:
		if (dpp_allowed_roles & DPP_CAPAB_ENROLLEE) {
			wpa_printf(MSG_DEBUG, "DPP: Acting as Enrollee");
			auth->configurator = 0;
		} else if (dpp_allowed_roles & DPP_CAPAB_CONFIGURATOR) {
			wpa_printf(MSG_DEBUG, "DPP: Acting as Configurator");
			auth->configurator = 1;
		} else {
			wpa_printf(MSG_DEBUG,
				   "DPP: Local policy does not allow Configurator/Enrollee role");
			goto not_compatible;
		}
		break;
	default:
		wpa_printf(MSG_DEBUG, "DPP: Unexpected role in I-capabilities");
		wpa_msg(auth->msg_ctx, MSG_INFO,
			DPP_EVENT_FAIL "Invalid role in I-capabilities 0x%02x",
			auth->i_capab & DPP_CAPAB_ROLE_MASK);
		goto fail;
	}

	auth->peer_protocol_key = pi;
	pi = NULL;
	if (qr_mutual && !peer_bi && own_bi->type == DPP_BOOTSTRAP_QR_CODE) {
		char hex[SHA256_MAC_LEN * 2 + 1];

		wpa_printf(MSG_DEBUG,
			   "DPP: Mutual authentication required with QR Codes, but peer info is not yet available - request more time");
		if (dpp_auth_build_resp_status(auth,
					       DPP_STATUS_RESPONSE_PENDING) < 0)
			goto fail;
		i_bootstrap = dpp_get_attr(attr_start, attr_len,
					   DPP_ATTR_I_BOOTSTRAP_KEY_HASH,
					   &i_bootstrap_len);
		if (i_bootstrap && i_bootstrap_len == SHA256_MAC_LEN) {
			auth->response_pending = 1;
			os_memcpy(auth->waiting_pubkey_hash,
				  i_bootstrap, i_bootstrap_len);
			wpa_snprintf_hex(hex, sizeof(hex), i_bootstrap,
					 i_bootstrap_len);
		} else {
			hex[0] = '\0';
		}

		wpa_msg(auth->msg_ctx, MSG_INFO, DPP_EVENT_SCAN_PEER_QR_CODE
			"%s", hex);
		return auth;
	}
	if (dpp_auth_build_resp_ok(auth) < 0)
		goto fail;

	return auth;

not_compatible:
	wpa_msg(auth->msg_ctx, MSG_INFO, DPP_EVENT_NOT_COMPATIBLE
		"i-capab=0x%02x", auth->i_capab);
	if (dpp_allowed_roles & DPP_CAPAB_CONFIGURATOR)
		auth->configurator = 1;
	else
		auth->configurator = 0;
	auth->peer_protocol_key = pi;
	pi = NULL;
	if (dpp_auth_build_resp_status(auth, DPP_STATUS_NOT_COMPATIBLE) < 0)
		goto fail;

	auth->remove_on_tx_status = 1;
	return auth;
fail:
	bin_clear_free(unwrapped, unwrapped_len);
	EVP_PKEY_free(pi);
	EVP_PKEY_CTX_free(ctx);
	dpp_auth_deinit(auth);
	return NULL;
}


int dpp_notify_new_qr_code(struct dpp_authentication *auth,
			   struct dpp_bootstrap_info *peer_bi)
{
	if (!auth || !auth->response_pending ||
	    os_memcmp(auth->waiting_pubkey_hash, peer_bi->pubkey_hash,
		      SHA256_MAC_LEN) != 0)
		return 0;

	wpa_printf(MSG_DEBUG,
		   "DPP: New scanned QR Code has matching public key that was needed to continue DPP Authentication exchange with "
		   MACSTR, MAC2STR(auth->peer_mac_addr));
	auth->peer_bi = peer_bi;

	if (dpp_auth_build_resp_ok(auth) < 0)
		return -1;

	return 1;
}


static struct wpabuf * dpp_auth_build_conf(struct dpp_authentication *auth,
					   enum dpp_status_error status)
{
	struct wpabuf *msg;
	u8 i_auth[4 + DPP_MAX_HASH_LEN];
	size_t i_auth_len;
	u8 r_nonce[4 + DPP_MAX_NONCE_LEN];
	size_t r_nonce_len;
	const u8 *addr[2];
	size_t len[2], attr_len;
	u8 *wrapped_i_auth;
	u8 *wrapped_r_nonce;
	u8 *attr_start, *attr_end;
	const u8 *r_pubkey_hash, *i_pubkey_hash;
#ifdef CONFIG_TESTING_OPTIONS
	u8 test_hash[SHA256_MAC_LEN];
#endif /* CONFIG_TESTING_OPTIONS */

	wpa_printf(MSG_DEBUG, "DPP: Build Authentication Confirmation");

	i_auth_len = 4 + auth->curve->hash_len;
	r_nonce_len = 4 + auth->curve->nonce_len;
	/* Build DPP Authentication Confirmation frame attributes */
	attr_len = 4 + 1 + 2 * (4 + SHA256_MAC_LEN) +
		4 + i_auth_len + r_nonce_len + AES_BLOCK_SIZE;
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_AUTH_CONF)
		attr_len += 5;
#endif /* CONFIG_TESTING_OPTIONS */
	msg = dpp_alloc_msg(DPP_PA_AUTHENTICATION_CONF, attr_len);
	if (!msg)
		goto fail;

	attr_start = wpabuf_put(msg, 0);

	r_pubkey_hash = auth->peer_bi->pubkey_hash;
	if (auth->own_bi)
		i_pubkey_hash = auth->own_bi->pubkey_hash;
	else
		i_pubkey_hash = NULL;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_STATUS_AUTH_CONF) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no Status");
		goto skip_status;
	} else if (dpp_test == DPP_TEST_INVALID_STATUS_AUTH_CONF) {
		wpa_printf(MSG_INFO, "DPP: TESTING - invalid Status");
		status = 254;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* DPP Status */
	dpp_build_attr_status(msg, status);

#ifdef CONFIG_TESTING_OPTIONS
skip_status:
	if (dpp_test == DPP_TEST_NO_R_BOOTSTRAP_KEY_HASH_AUTH_CONF) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no R-Bootstrap Key Hash");
		r_pubkey_hash = NULL;
	} else if (dpp_test ==
		   DPP_TEST_INVALID_R_BOOTSTRAP_KEY_HASH_AUTH_CONF) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - invalid R-Bootstrap Key Hash");
		os_memcpy(test_hash, r_pubkey_hash, SHA256_MAC_LEN);
		test_hash[SHA256_MAC_LEN - 1] ^= 0x01;
		r_pubkey_hash = test_hash;
	} else if (dpp_test == DPP_TEST_NO_I_BOOTSTRAP_KEY_HASH_AUTH_CONF) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no I-Bootstrap Key Hash");
		i_pubkey_hash = NULL;
	} else if (dpp_test ==
		   DPP_TEST_INVALID_I_BOOTSTRAP_KEY_HASH_AUTH_CONF) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - invalid I-Bootstrap Key Hash");
		if (i_pubkey_hash)
			os_memcpy(test_hash, i_pubkey_hash, SHA256_MAC_LEN);
		else
			os_memset(test_hash, 0, SHA256_MAC_LEN);
		test_hash[SHA256_MAC_LEN - 1] ^= 0x01;
		i_pubkey_hash = test_hash;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* Responder Bootstrapping Key Hash */
	dpp_build_attr_r_bootstrap_key_hash(msg, r_pubkey_hash);

	/* Initiator Bootstrapping Key Hash (mutual authentication) */
	dpp_build_attr_i_bootstrap_key_hash(msg, i_pubkey_hash);

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_WRAPPED_DATA_AUTH_CONF)
		goto skip_wrapped_data;
	if (dpp_test == DPP_TEST_NO_I_AUTH_AUTH_CONF)
		i_auth_len = 0;
#endif /* CONFIG_TESTING_OPTIONS */

	attr_end = wpabuf_put(msg, 0);

	/* OUI, OUI type, Crypto Suite, DPP frame type */
	addr[0] = wpabuf_head_u8(msg) + 2;
	len[0] = 3 + 1 + 1 + 1;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);

	/* Attributes before Wrapped Data */
	addr[1] = attr_start;
	len[1] = attr_end - attr_start;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);

	if (status == DPP_STATUS_OK) {
		/* I-auth wrapped with ke */
		wpabuf_put_le16(msg, DPP_ATTR_WRAPPED_DATA);
		wpabuf_put_le16(msg, i_auth_len + AES_BLOCK_SIZE);
		wrapped_i_auth = wpabuf_put(msg, i_auth_len + AES_BLOCK_SIZE);

#ifdef CONFIG_TESTING_OPTIONS
		if (dpp_test == DPP_TEST_NO_I_AUTH_AUTH_CONF)
			goto skip_i_auth;
#endif /* CONFIG_TESTING_OPTIONS */

		/* I-auth = H(R-nonce | I-nonce | PR.x | PI.x | BR.x | [BI.x |]
		 *	      1) */
		WPA_PUT_LE16(i_auth, DPP_ATTR_I_AUTH_TAG);
		WPA_PUT_LE16(&i_auth[2], auth->curve->hash_len);
		if (dpp_gen_i_auth(auth, i_auth + 4) < 0)
			goto fail;

#ifdef CONFIG_TESTING_OPTIONS
		if (dpp_test == DPP_TEST_I_AUTH_MISMATCH_AUTH_CONF) {
			wpa_printf(MSG_INFO, "DPP: TESTING - I-auth mismatch");
			i_auth[4 + auth->curve->hash_len / 2] ^= 0x01;
		}
skip_i_auth:
#endif /* CONFIG_TESTING_OPTIONS */
		if (aes_siv_encrypt(auth->ke, auth->curve->hash_len,
				    i_auth, i_auth_len,
				    2, addr, len, wrapped_i_auth) < 0)
			goto fail;
		wpa_hexdump(MSG_DEBUG, "DPP: {I-auth}ke",
			    wrapped_i_auth, i_auth_len + AES_BLOCK_SIZE);
	} else {
		/* R-nonce wrapped with k2 */
		wpabuf_put_le16(msg, DPP_ATTR_WRAPPED_DATA);
		wpabuf_put_le16(msg, r_nonce_len + AES_BLOCK_SIZE);
		wrapped_r_nonce = wpabuf_put(msg, r_nonce_len + AES_BLOCK_SIZE);

		WPA_PUT_LE16(r_nonce, DPP_ATTR_R_NONCE);
		WPA_PUT_LE16(&r_nonce[2], auth->curve->nonce_len);
		os_memcpy(r_nonce + 4, auth->r_nonce, auth->curve->nonce_len);

		if (aes_siv_encrypt(auth->k2, auth->curve->hash_len,
				    r_nonce, r_nonce_len,
				    2, addr, len, wrapped_r_nonce) < 0)
			goto fail;
		wpa_hexdump(MSG_DEBUG, "DPP: {R-nonce}k2",
			    wrapped_r_nonce, r_nonce_len + AES_BLOCK_SIZE);
	}

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_AUTH_CONF) {
		wpa_printf(MSG_INFO, "DPP: TESTING - attr after Wrapped Data");
		dpp_build_attr_status(msg, DPP_STATUS_OK);
	}
skip_wrapped_data:
#endif /* CONFIG_TESTING_OPTIONS */

	wpa_hexdump_buf(MSG_DEBUG,
			"DPP: Authentication Confirmation frame attributes",
			msg);
	if (status == DPP_STATUS_OK)
		dpp_auth_success(auth);

	return msg;

fail:
	wpabuf_free(msg);
	return NULL;
}


static void
dpp_auth_resp_rx_status(struct dpp_authentication *auth, const u8 *hdr,
			const u8 *attr_start, size_t attr_len,
			const u8 *wrapped_data, u16 wrapped_data_len,
			enum dpp_status_error status)
{
	const u8 *addr[2];
	size_t len[2];
	u8 *unwrapped = NULL;
	size_t unwrapped_len = 0;
	const u8 *i_nonce, *r_capab;
	u16 i_nonce_len, r_capab_len;

	if (status == DPP_STATUS_NOT_COMPATIBLE) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Responder reported incompatible roles");
	} else if (status == DPP_STATUS_RESPONSE_PENDING) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Responder reported more time needed");
	} else {
		wpa_printf(MSG_DEBUG,
			   "DPP: Responder reported failure (status %d)",
			   status);
		dpp_auth_fail(auth, "Responder reported failure");
		return;
	}

	addr[0] = hdr;
	len[0] = DPP_HDR_LEN;
	addr[1] = attr_start;
	len[1] = attr_len;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped)
		goto fail;
	if (aes_siv_decrypt(auth->k1, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    2, addr, len, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	i_nonce = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_I_NONCE,
			       &i_nonce_len);
	if (!i_nonce || i_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth, "Missing or invalid I-nonce");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: I-nonce", i_nonce, i_nonce_len);
	if (os_memcmp(auth->i_nonce, i_nonce, i_nonce_len) != 0) {
		dpp_auth_fail(auth, "I-nonce mismatch");
		goto fail;
	}

	r_capab = dpp_get_attr(unwrapped, unwrapped_len,
			       DPP_ATTR_R_CAPABILITIES,
			       &r_capab_len);
	if (!r_capab || r_capab_len < 1) {
		dpp_auth_fail(auth, "Missing or invalid R-capabilities");
		goto fail;
	}
	auth->r_capab = r_capab[0];
	wpa_printf(MSG_DEBUG, "DPP: R-capabilities: 0x%02x", auth->r_capab);
	if (status == DPP_STATUS_NOT_COMPATIBLE) {
		wpa_msg(auth->msg_ctx, MSG_INFO, DPP_EVENT_NOT_COMPATIBLE
			"r-capab=0x%02x", auth->r_capab);
	} else if (status == DPP_STATUS_RESPONSE_PENDING) {
		u8 role = auth->r_capab & DPP_CAPAB_ROLE_MASK;

		if ((auth->configurator && role != DPP_CAPAB_ENROLLEE) ||
		    (!auth->configurator && role != DPP_CAPAB_CONFIGURATOR)) {
			wpa_msg(auth->msg_ctx, MSG_INFO,
				DPP_EVENT_FAIL "Unexpected role in R-capabilities 0x%02x",
				role);
		} else {
			wpa_printf(MSG_DEBUG,
				   "DPP: Continue waiting for full DPP Authentication Response");
			wpa_msg(auth->msg_ctx, MSG_INFO,
				DPP_EVENT_RESPONSE_PENDING "%s",
				auth->tmp_own_bi ? auth->tmp_own_bi->uri : "");
		}
	}
fail:
	bin_clear_free(unwrapped, unwrapped_len);
}


struct wpabuf *
dpp_auth_resp_rx(struct dpp_authentication *auth, const u8 *hdr,
		 const u8 *attr_start, size_t attr_len)
{
	EVP_PKEY *pr;
	size_t secret_len;
	const u8 *addr[2];
	size_t len[2];
	u8 *unwrapped = NULL, *unwrapped2 = NULL;
	size_t unwrapped_len = 0, unwrapped2_len = 0;
	const u8 *r_bootstrap, *i_bootstrap, *wrapped_data, *status, *r_proto,
		*r_nonce, *i_nonce, *r_capab, *wrapped2, *r_auth;
	u16 r_bootstrap_len, i_bootstrap_len, wrapped_data_len, status_len,
		r_proto_len, r_nonce_len, i_nonce_len, r_capab_len,
		wrapped2_len, r_auth_len;
	u8 r_auth2[DPP_MAX_HASH_LEN];
	u8 role;
#ifdef CONFIG_DPP2
	const u8 *version;
	u16 version_len;
#endif /* CONFIG_DPP2 */

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_STOP_AT_AUTH_RESP) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - stop at Authentication Response");
		return NULL;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	if (!auth->initiator || !auth->peer_bi) {
		dpp_auth_fail(auth, "Unexpected Authentication Response");
		return NULL;
	}

	auth->waiting_auth_resp = 0;

	wrapped_data = dpp_get_attr(attr_start, attr_len, DPP_ATTR_WRAPPED_DATA,
				    &wrapped_data_len);
	if (!wrapped_data || wrapped_data_len < AES_BLOCK_SIZE) {
		dpp_auth_fail(auth,
			      "Missing or invalid required Wrapped Data attribute");
		return NULL;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Wrapped data",
		    wrapped_data, wrapped_data_len);

	attr_len = wrapped_data - 4 - attr_start;

	r_bootstrap = dpp_get_attr(attr_start, attr_len,
				   DPP_ATTR_R_BOOTSTRAP_KEY_HASH,
				   &r_bootstrap_len);
	if (!r_bootstrap || r_bootstrap_len != SHA256_MAC_LEN) {
		dpp_auth_fail(auth,
			      "Missing or invalid required Responder Bootstrapping Key Hash attribute");
		return NULL;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Responder Bootstrapping Key Hash",
		    r_bootstrap, r_bootstrap_len);
	if (os_memcmp(r_bootstrap, auth->peer_bi->pubkey_hash,
		      SHA256_MAC_LEN) != 0) {
		dpp_auth_fail(auth,
			      "Unexpected Responder Bootstrapping Key Hash value");
		wpa_hexdump(MSG_DEBUG,
			    "DPP: Expected Responder Bootstrapping Key Hash",
			    auth->peer_bi->pubkey_hash, SHA256_MAC_LEN);
		return NULL;
	}

	i_bootstrap = dpp_get_attr(attr_start, attr_len,
				   DPP_ATTR_I_BOOTSTRAP_KEY_HASH,
				   &i_bootstrap_len);
	if (i_bootstrap) {
		if (i_bootstrap_len != SHA256_MAC_LEN) {
			dpp_auth_fail(auth,
				      "Invalid Initiator Bootstrapping Key Hash attribute");
			return NULL;
		}
		wpa_hexdump(MSG_MSGDUMP,
			    "DPP: Initiator Bootstrapping Key Hash",
			    i_bootstrap, i_bootstrap_len);
		if (!auth->own_bi ||
		    os_memcmp(i_bootstrap, auth->own_bi->pubkey_hash,
			      SHA256_MAC_LEN) != 0) {
			dpp_auth_fail(auth,
				      "Initiator Bootstrapping Key Hash attribute did not match");
			return NULL;
		}
	} else if (auth->own_bi && auth->own_bi->type == DPP_BOOTSTRAP_PKEX) {
		/* PKEX bootstrapping mandates use of mutual authentication */
		dpp_auth_fail(auth,
			      "Missing Initiator Bootstrapping Key Hash attribute");
		return NULL;
	} else if (auth->own_bi &&
		   auth->own_bi->type == DPP_BOOTSTRAP_NFC_URI &&
		   auth->own_bi->nfc_negotiated) {
		/* NFC negotiated connection handover bootstrapping mandates
		 * use of mutual authentication */
		dpp_auth_fail(auth,
			      "Missing Initiator Bootstrapping Key Hash attribute");
		return NULL;
	}

	auth->peer_version = 1; /* default to the first version */
#ifdef CONFIG_DPP2
	version = dpp_get_attr(attr_start, attr_len, DPP_ATTR_PROTOCOL_VERSION,
			       &version_len);
	if (version && DPP_VERSION > 1) {
		if (version_len < 1 || version[0] == 0) {
			dpp_auth_fail(auth,
				      "Invalid Protocol Version attribute");
			return NULL;
		}
		auth->peer_version = version[0];
		wpa_printf(MSG_DEBUG, "DPP: Peer protocol version %u",
			   auth->peer_version);
	}
#endif /* CONFIG_DPP2 */

	status = dpp_get_attr(attr_start, attr_len, DPP_ATTR_STATUS,
			      &status_len);
	if (!status || status_len < 1) {
		dpp_auth_fail(auth,
			      "Missing or invalid required DPP Status attribute");
		return NULL;
	}
	wpa_printf(MSG_DEBUG, "DPP: Status %u", status[0]);
	auth->auth_resp_status = status[0];
	if (status[0] != DPP_STATUS_OK) {
		dpp_auth_resp_rx_status(auth, hdr, attr_start,
					attr_len, wrapped_data,
					wrapped_data_len, status[0]);
		return NULL;
	}

	if (!i_bootstrap && auth->own_bi) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Responder decided not to use mutual authentication");
		auth->own_bi = NULL;
	}

	wpa_msg(auth->msg_ctx, MSG_INFO, DPP_EVENT_AUTH_DIRECTION "mutual=%d",
		auth->own_bi != NULL);

	r_proto = dpp_get_attr(attr_start, attr_len, DPP_ATTR_R_PROTOCOL_KEY,
			       &r_proto_len);
	if (!r_proto) {
		dpp_auth_fail(auth,
			      "Missing required Responder Protocol Key attribute");
		return NULL;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: Responder Protocol Key",
		    r_proto, r_proto_len);

	/* N = pI * PR */
	pr = dpp_set_pubkey_point(auth->own_protocol_key, r_proto, r_proto_len);
	if (!pr) {
		dpp_auth_fail(auth, "Invalid Responder Protocol Key");
		return NULL;
	}
	dpp_debug_print_key("Peer (Responder) Protocol Key", pr);

	if (dpp_ecdh(auth->own_protocol_key, pr, auth->Nx, &secret_len) < 0) {
		dpp_auth_fail(auth, "Failed to derive ECDH shared secret");
		goto fail;
	}
	EVP_PKEY_free(auth->peer_protocol_key);
	auth->peer_protocol_key = pr;
	pr = NULL;

	wpa_hexdump_key(MSG_DEBUG, "DPP: ECDH shared secret (N.x)",
			auth->Nx, auth->secret_len);
	auth->Nx_len = auth->secret_len;

	if (dpp_derive_k2(auth->Nx, auth->secret_len, auth->k2,
			  auth->curve->hash_len) < 0)
		goto fail;

	addr[0] = hdr;
	len[0] = DPP_HDR_LEN;
	addr[1] = attr_start;
	len[1] = attr_len;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped)
		goto fail;
	if (aes_siv_decrypt(auth->k2, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    2, addr, len, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	r_nonce = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_R_NONCE,
			       &r_nonce_len);
	if (!r_nonce || r_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth, "DPP: Missing or invalid R-nonce");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: R-nonce", r_nonce, r_nonce_len);
	os_memcpy(auth->r_nonce, r_nonce, r_nonce_len);

	i_nonce = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_I_NONCE,
			       &i_nonce_len);
	if (!i_nonce || i_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth, "Missing or invalid I-nonce");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: I-nonce", i_nonce, i_nonce_len);
	if (os_memcmp(auth->i_nonce, i_nonce, i_nonce_len) != 0) {
		dpp_auth_fail(auth, "I-nonce mismatch");
		goto fail;
	}

	if (auth->own_bi) {
		/* Mutual authentication */
		if (dpp_auth_derive_l_initiator(auth) < 0)
			goto fail;
	}

	r_capab = dpp_get_attr(unwrapped, unwrapped_len,
			       DPP_ATTR_R_CAPABILITIES,
			       &r_capab_len);
	if (!r_capab || r_capab_len < 1) {
		dpp_auth_fail(auth, "Missing or invalid R-capabilities");
		goto fail;
	}
	auth->r_capab = r_capab[0];
	wpa_printf(MSG_DEBUG, "DPP: R-capabilities: 0x%02x", auth->r_capab);
	role = auth->r_capab & DPP_CAPAB_ROLE_MASK;
	if ((auth->allowed_roles ==
	     (DPP_CAPAB_CONFIGURATOR | DPP_CAPAB_ENROLLEE)) &&
	    (role == DPP_CAPAB_CONFIGURATOR || role == DPP_CAPAB_ENROLLEE)) {
		/* Peer selected its role, so move from "either role" to the
		 * role that is compatible with peer's selection. */
		auth->configurator = role == DPP_CAPAB_ENROLLEE;
		wpa_printf(MSG_DEBUG, "DPP: Acting as %s",
			   auth->configurator ? "Configurator" : "Enrollee");
	} else if ((auth->configurator && role != DPP_CAPAB_ENROLLEE) ||
		   (!auth->configurator && role != DPP_CAPAB_CONFIGURATOR)) {
		wpa_printf(MSG_DEBUG, "DPP: Incompatible role selection");
		wpa_msg(auth->msg_ctx, MSG_INFO, DPP_EVENT_FAIL
			"Unexpected role in R-capabilities 0x%02x",
			role);
		if (role != DPP_CAPAB_ENROLLEE &&
		    role != DPP_CAPAB_CONFIGURATOR)
			goto fail;
		bin_clear_free(unwrapped, unwrapped_len);
		auth->remove_on_tx_status = 1;
		return dpp_auth_build_conf(auth, DPP_STATUS_NOT_COMPATIBLE);
	}

	wrapped2 = dpp_get_attr(unwrapped, unwrapped_len,
				DPP_ATTR_WRAPPED_DATA, &wrapped2_len);
	if (!wrapped2 || wrapped2_len < AES_BLOCK_SIZE) {
		dpp_auth_fail(auth,
			      "Missing or invalid Secondary Wrapped Data");
		goto fail;
	}

	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped2, wrapped2_len);

	if (dpp_derive_bk_ke(auth) < 0)
		goto fail;

	unwrapped2_len = wrapped2_len - AES_BLOCK_SIZE;
	unwrapped2 = os_malloc(unwrapped2_len);
	if (!unwrapped2)
		goto fail;
	if (aes_siv_decrypt(auth->ke, auth->curve->hash_len,
			    wrapped2, wrapped2_len,
			    0, NULL, NULL, unwrapped2) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped2, unwrapped2_len);

	if (dpp_check_attrs(unwrapped2, unwrapped2_len) < 0) {
		dpp_auth_fail(auth,
			      "Invalid attribute in secondary unwrapped data");
		goto fail;
	}

	r_auth = dpp_get_attr(unwrapped2, unwrapped2_len, DPP_ATTR_R_AUTH_TAG,
			       &r_auth_len);
	if (!r_auth || r_auth_len != auth->curve->hash_len) {
		dpp_auth_fail(auth,
			      "Missing or invalid Responder Authenticating Tag");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Received Responder Authenticating Tag",
		    r_auth, r_auth_len);
	/* R-auth' = H(I-nonce | R-nonce | PI.x | PR.x | [BI.x |] BR.x | 0) */
	if (dpp_gen_r_auth(auth, r_auth2) < 0)
		goto fail;
	wpa_hexdump(MSG_DEBUG, "DPP: Calculated Responder Authenticating Tag",
		    r_auth2, r_auth_len);
	if (os_memcmp(r_auth, r_auth2, r_auth_len) != 0) {
		dpp_auth_fail(auth, "Mismatching Responder Authenticating Tag");
		bin_clear_free(unwrapped, unwrapped_len);
		bin_clear_free(unwrapped2, unwrapped2_len);
		auth->remove_on_tx_status = 1;
		return dpp_auth_build_conf(auth, DPP_STATUS_AUTH_FAILURE);
	}

	bin_clear_free(unwrapped, unwrapped_len);
	bin_clear_free(unwrapped2, unwrapped2_len);

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AUTH_RESP_IN_PLACE_OF_CONF) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - Authentication Response in place of Confirm");
		if (dpp_auth_build_resp_ok(auth) < 0)
			return NULL;
		return wpabuf_dup(auth->resp_msg);
	}
#endif /* CONFIG_TESTING_OPTIONS */

	return dpp_auth_build_conf(auth, DPP_STATUS_OK);

fail:
	bin_clear_free(unwrapped, unwrapped_len);
	bin_clear_free(unwrapped2, unwrapped2_len);
	EVP_PKEY_free(pr);
	return NULL;
}


static int dpp_auth_conf_rx_failure(struct dpp_authentication *auth,
				    const u8 *hdr,
				    const u8 *attr_start, size_t attr_len,
				    const u8 *wrapped_data,
				    u16 wrapped_data_len,
				    enum dpp_status_error status)
{
	const u8 *addr[2];
	size_t len[2];
	u8 *unwrapped = NULL;
	size_t unwrapped_len = 0;
	const u8 *r_nonce;
	u16 r_nonce_len;

	/* Authentication Confirm failure cases are expected to include
	 * {R-nonce}k2 in the Wrapped Data attribute. */

	addr[0] = hdr;
	len[0] = DPP_HDR_LEN;
	addr[1] = attr_start;
	len[1] = attr_len;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped) {
		dpp_auth_fail(auth, "Authentication failed");
		goto fail;
	}
	if (aes_siv_decrypt(auth->k2, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    2, addr, len, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	r_nonce = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_R_NONCE,
			       &r_nonce_len);
	if (!r_nonce || r_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth, "DPP: Missing or invalid R-nonce");
		goto fail;
	}
	if (os_memcmp(r_nonce, auth->r_nonce, r_nonce_len) != 0) {
		wpa_hexdump(MSG_DEBUG, "DPP: Received R-nonce",
			    r_nonce, r_nonce_len);
		wpa_hexdump(MSG_DEBUG, "DPP: Expected R-nonce",
			    auth->r_nonce, r_nonce_len);
		dpp_auth_fail(auth, "R-nonce mismatch");
		goto fail;
	}

	if (status == DPP_STATUS_NOT_COMPATIBLE)
		dpp_auth_fail(auth, "Peer reported incompatible R-capab role");
	else if (status == DPP_STATUS_AUTH_FAILURE)
		dpp_auth_fail(auth, "Peer reported authentication failure)");

fail:
	bin_clear_free(unwrapped, unwrapped_len);
	return -1;
}


int dpp_auth_conf_rx(struct dpp_authentication *auth, const u8 *hdr,
		     const u8 *attr_start, size_t attr_len)
{
	const u8 *r_bootstrap, *i_bootstrap, *wrapped_data, *status, *i_auth;
	u16 r_bootstrap_len, i_bootstrap_len, wrapped_data_len, status_len,
		i_auth_len;
	const u8 *addr[2];
	size_t len[2];
	u8 *unwrapped = NULL;
	size_t unwrapped_len = 0;
	u8 i_auth2[DPP_MAX_HASH_LEN];

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_STOP_AT_AUTH_CONF) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - stop at Authentication Confirm");
		return -1;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	if (auth->initiator || !auth->own_bi || !auth->waiting_auth_conf) {
		wpa_printf(MSG_DEBUG,
			   "DPP: initiator=%d own_bi=%d waiting_auth_conf=%d",
			   auth->initiator, !!auth->own_bi,
			   auth->waiting_auth_conf);
		dpp_auth_fail(auth, "Unexpected Authentication Confirm");
		return -1;
	}

	auth->waiting_auth_conf = 0;

	wrapped_data = dpp_get_attr(attr_start, attr_len, DPP_ATTR_WRAPPED_DATA,
				    &wrapped_data_len);
	if (!wrapped_data || wrapped_data_len < AES_BLOCK_SIZE) {
		dpp_auth_fail(auth,
			      "Missing or invalid required Wrapped Data attribute");
		return -1;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Wrapped data",
		    wrapped_data, wrapped_data_len);

	attr_len = wrapped_data - 4 - attr_start;

	r_bootstrap = dpp_get_attr(attr_start, attr_len,
				   DPP_ATTR_R_BOOTSTRAP_KEY_HASH,
				   &r_bootstrap_len);
	if (!r_bootstrap || r_bootstrap_len != SHA256_MAC_LEN) {
		dpp_auth_fail(auth,
			      "Missing or invalid required Responder Bootstrapping Key Hash attribute");
		return -1;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Responder Bootstrapping Key Hash",
		    r_bootstrap, r_bootstrap_len);
	if (os_memcmp(r_bootstrap, auth->own_bi->pubkey_hash,
		      SHA256_MAC_LEN) != 0) {
		wpa_hexdump(MSG_DEBUG,
			    "DPP: Expected Responder Bootstrapping Key Hash",
			    auth->peer_bi->pubkey_hash, SHA256_MAC_LEN);
		dpp_auth_fail(auth,
			      "Responder Bootstrapping Key Hash mismatch");
		return -1;
	}

	i_bootstrap = dpp_get_attr(attr_start, attr_len,
				   DPP_ATTR_I_BOOTSTRAP_KEY_HASH,
				   &i_bootstrap_len);
	if (i_bootstrap) {
		if (i_bootstrap_len != SHA256_MAC_LEN) {
			dpp_auth_fail(auth,
				      "Invalid Initiator Bootstrapping Key Hash attribute");
			return -1;
		}
		wpa_hexdump(MSG_MSGDUMP,
			    "DPP: Initiator Bootstrapping Key Hash",
			    i_bootstrap, i_bootstrap_len);
		if (!auth->peer_bi ||
		    os_memcmp(i_bootstrap, auth->peer_bi->pubkey_hash,
			      SHA256_MAC_LEN) != 0) {
			dpp_auth_fail(auth,
				      "Initiator Bootstrapping Key Hash mismatch");
			return -1;
		}
	} else if (auth->peer_bi) {
		/* Mutual authentication and peer did not include its
		 * Bootstrapping Key Hash attribute. */
		dpp_auth_fail(auth,
			      "Missing Initiator Bootstrapping Key Hash attribute");
		return -1;
	}

	status = dpp_get_attr(attr_start, attr_len, DPP_ATTR_STATUS,
			      &status_len);
	if (!status || status_len < 1) {
		dpp_auth_fail(auth,
			      "Missing or invalid required DPP Status attribute");
		return -1;
	}
	wpa_printf(MSG_DEBUG, "DPP: Status %u", status[0]);
	if (status[0] == DPP_STATUS_NOT_COMPATIBLE ||
	    status[0] == DPP_STATUS_AUTH_FAILURE)
		return dpp_auth_conf_rx_failure(auth, hdr, attr_start,
						attr_len, wrapped_data,
						wrapped_data_len, status[0]);

	if (status[0] != DPP_STATUS_OK) {
		dpp_auth_fail(auth, "Authentication failed");
		return -1;
	}

	addr[0] = hdr;
	len[0] = DPP_HDR_LEN;
	addr[1] = attr_start;
	len[1] = attr_len;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped)
		return -1;
	if (aes_siv_decrypt(auth->ke, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    2, addr, len, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	i_auth = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_I_AUTH_TAG,
			      &i_auth_len);
	if (!i_auth || i_auth_len != auth->curve->hash_len) {
		dpp_auth_fail(auth,
			      "Missing or invalid Initiator Authenticating Tag");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Received Initiator Authenticating Tag",
		    i_auth, i_auth_len);
	/* I-auth' = H(R-nonce | I-nonce | PR.x | PI.x | BR.x | [BI.x |] 1) */
	if (dpp_gen_i_auth(auth, i_auth2) < 0)
		goto fail;
	wpa_hexdump(MSG_DEBUG, "DPP: Calculated Initiator Authenticating Tag",
		    i_auth2, i_auth_len);
	if (os_memcmp(i_auth, i_auth2, i_auth_len) != 0) {
		dpp_auth_fail(auth, "Mismatching Initiator Authenticating Tag");
		goto fail;
	}

	bin_clear_free(unwrapped, unwrapped_len);
	dpp_auth_success(auth);
	return 0;
fail:
	bin_clear_free(unwrapped, unwrapped_len);
	return -1;
}


static int bin_str_eq(const char *val, size_t len, const char *cmp)
{
	return os_strlen(cmp) == len && os_memcmp(val, cmp, len) == 0;
}


struct dpp_configuration * dpp_configuration_alloc(const char *type)
{
	struct dpp_configuration *conf;
	const char *end;
	size_t len;

	conf = os_zalloc(sizeof(*conf));
	if (!conf)
		goto fail;

	end = os_strchr(type, ' ');
	if (end)
		len = end - type;
	else
		len = os_strlen(type);

	if (bin_str_eq(type, len, "psk"))
		conf->akm = DPP_AKM_PSK;
	else if (bin_str_eq(type, len, "sae"))
		conf->akm = DPP_AKM_SAE;
	else if (bin_str_eq(type, len, "psk-sae") ||
		 bin_str_eq(type, len, "psk+sae"))
		conf->akm = DPP_AKM_PSK_SAE;
	else if (bin_str_eq(type, len, "sae-dpp") ||
		 bin_str_eq(type, len, "dpp+sae"))
		conf->akm = DPP_AKM_SAE_DPP;
	else if (bin_str_eq(type, len, "psk-sae-dpp") ||
		 bin_str_eq(type, len, "dpp+psk+sae"))
		conf->akm = DPP_AKM_PSK_SAE_DPP;
	else if (bin_str_eq(type, len, "dpp"))
		conf->akm = DPP_AKM_DPP;
	else
		goto fail;

	return conf;
fail:
	dpp_configuration_free(conf);
	return NULL;
}


int dpp_akm_psk(enum dpp_akm akm)
{
	return akm == DPP_AKM_PSK || akm == DPP_AKM_PSK_SAE ||
		akm == DPP_AKM_PSK_SAE_DPP;
}


int dpp_akm_sae(enum dpp_akm akm)
{
	return akm == DPP_AKM_SAE || akm == DPP_AKM_PSK_SAE ||
		akm == DPP_AKM_SAE_DPP || akm == DPP_AKM_PSK_SAE_DPP;
}


int dpp_akm_legacy(enum dpp_akm akm)
{
	return akm == DPP_AKM_PSK || akm == DPP_AKM_PSK_SAE ||
		akm == DPP_AKM_SAE;
}


int dpp_akm_dpp(enum dpp_akm akm)
{
	return akm == DPP_AKM_DPP || akm == DPP_AKM_SAE_DPP ||
		akm == DPP_AKM_PSK_SAE_DPP;
}


int dpp_akm_ver2(enum dpp_akm akm)
{
	return akm == DPP_AKM_SAE_DPP || akm == DPP_AKM_PSK_SAE_DPP;
}


int dpp_configuration_valid(const struct dpp_configuration *conf)
{
	if (conf->ssid_len == 0)
		return 0;
	if (dpp_akm_psk(conf->akm) && !conf->passphrase && !conf->psk_set)
		return 0;
	if (dpp_akm_sae(conf->akm) && !conf->passphrase)
		return 0;
	return 1;
}


void dpp_configuration_free(struct dpp_configuration *conf)
{
	if (!conf)
		return;
	str_clear_free(conf->passphrase);
	os_free(conf->group_id);
	bin_clear_free(conf, sizeof(*conf));
}


static int dpp_configuration_parse_helper(struct dpp_authentication *auth,
					  const char *cmd, int idx)
{
	const char *pos, *end;
	struct dpp_configuration *conf_sta = NULL, *conf_ap = NULL;
	struct dpp_configuration *conf = NULL;

	pos = os_strstr(cmd, " conf=sta-");
	if (pos) {
		conf_sta = dpp_configuration_alloc(pos + 10);
		if (!conf_sta)
			goto fail;
		conf_sta->netrole = DPP_NETROLE_STA;
		conf = conf_sta;
	}

	pos = os_strstr(cmd, " conf=ap-");
	if (pos) {
		conf_ap = dpp_configuration_alloc(pos + 9);
		if (!conf_ap)
			goto fail;
		conf_ap->netrole = DPP_NETROLE_AP;
		conf = conf_ap;
	}

	pos = os_strstr(cmd, " conf=configurator");
	if (pos)
		auth->provision_configurator = 1;

	if (!conf)
		return 0;

	pos = os_strstr(cmd, " ssid=");
	if (pos) {
		pos += 6;
		end = os_strchr(pos, ' ');
		conf->ssid_len = end ? (size_t) (end - pos) : os_strlen(pos);
		conf->ssid_len /= 2;
		if (conf->ssid_len > sizeof(conf->ssid) ||
		    hexstr2bin(pos, conf->ssid, conf->ssid_len) < 0)
			goto fail;
	} else {
#ifdef CONFIG_TESTING_OPTIONS
		/* use a default SSID for legacy testing reasons */
		os_memcpy(conf->ssid, "test", 4);
		conf->ssid_len = 4;
#else /* CONFIG_TESTING_OPTIONS */
		goto fail;
#endif /* CONFIG_TESTING_OPTIONS */
	}

	pos = os_strstr(cmd, " ssid_charset=");
	if (pos) {
		if (conf_ap) {
			wpa_printf(MSG_INFO,
				   "DPP: ssid64 option (ssid_charset param) not allowed for AP enrollee");
			goto fail;
		}
		conf->ssid_charset = atoi(pos + 14);
	}

	pos = os_strstr(cmd, " pass=");
	if (pos) {
		size_t pass_len;

		pos += 6;
		end = os_strchr(pos, ' ');
		pass_len = end ? (size_t) (end - pos) : os_strlen(pos);
		pass_len /= 2;
		if (pass_len > 63 || pass_len < 8)
			goto fail;
		conf->passphrase = os_zalloc(pass_len + 1);
		if (!conf->passphrase ||
		    hexstr2bin(pos, (u8 *) conf->passphrase, pass_len) < 0)
			goto fail;
	}

	pos = os_strstr(cmd, " psk=");
	if (pos) {
		pos += 5;
		if (hexstr2bin(pos, conf->psk, PMK_LEN) < 0)
			goto fail;
		conf->psk_set = 1;
	}

	pos = os_strstr(cmd, " group_id=");
	if (pos) {
		size_t group_id_len;

		pos += 10;
		end = os_strchr(pos, ' ');
		group_id_len = end ? (size_t) (end - pos) : os_strlen(pos);
		conf->group_id = os_malloc(group_id_len + 1);
		if (!conf->group_id)
			goto fail;
		os_memcpy(conf->group_id, pos, group_id_len);
		conf->group_id[group_id_len] = '\0';
	}

	pos = os_strstr(cmd, " expiry=");
	if (pos) {
		long int val;

		pos += 8;
		val = strtol(pos, NULL, 0);
		if (val <= 0)
			goto fail;
		conf->netaccesskey_expiry = val;
	}

	if (!dpp_configuration_valid(conf))
		goto fail;

	if (idx == 0) {
		auth->conf_sta = conf_sta;
		auth->conf_ap = conf_ap;
	} else if (idx == 1) {
		auth->conf2_sta = conf_sta;
		auth->conf2_ap = conf_ap;
	} else {
		goto fail;
	}
	return 0;

fail:
	dpp_configuration_free(conf_sta);
	dpp_configuration_free(conf_ap);
	return -1;
}


static int dpp_configuration_parse(struct dpp_authentication *auth,
				   const char *cmd)
{
	const char *pos;
	char *tmp;
	size_t len;
	int res;

	pos = os_strstr(cmd, " @CONF-OBJ-SEP@ ");
	if (!pos)
		return dpp_configuration_parse_helper(auth, cmd, 0);

	len = pos - cmd;
	tmp = os_malloc(len + 1);
	if (!tmp)
		goto fail;
	os_memcpy(tmp, cmd, len);
	tmp[len] = '\0';
	res = dpp_configuration_parse_helper(auth, cmd, 0);
	str_clear_free(tmp);
	if (res)
		goto fail;
	res = dpp_configuration_parse_helper(auth, cmd + len, 1);
	if (res)
		goto fail;
	return 0;
fail:
	dpp_configuration_free(auth->conf_sta);
	dpp_configuration_free(auth->conf2_sta);
	dpp_configuration_free(auth->conf_ap);
	dpp_configuration_free(auth->conf2_ap);
	return -1;
}


static struct dpp_configurator *
dpp_configurator_get_id(struct dpp_global *dpp, unsigned int id)
{
	struct dpp_configurator *conf;

	if (!dpp)
		return NULL;

	dl_list_for_each(conf, &dpp->configurator,
			 struct dpp_configurator, list) {
		if (conf->id == id)
			return conf;
	}
	return NULL;
}


int dpp_set_configurator(struct dpp_authentication *auth, const char *cmd)
{
	const char *pos;
	char *tmp = NULL;
	int ret = -1;

	if (!cmd || auth->configurator_set)
		return 0;
	auth->configurator_set = 1;

	if (cmd[0] != ' ') {
		size_t len;

		len = os_strlen(cmd);
		tmp = os_malloc(len + 2);
		if (!tmp)
			goto fail;
		tmp[0] = ' ';
		os_memcpy(tmp + 1, cmd, len + 1);
		cmd = tmp;
	}

	wpa_printf(MSG_DEBUG, "DPP: Set configurator parameters: %s", cmd);

	pos = os_strstr(cmd, " configurator=");
	if (pos) {
		pos += 14;
		auth->conf = dpp_configurator_get_id(auth->global, atoi(pos));
		if (!auth->conf) {
			wpa_printf(MSG_INFO,
				   "DPP: Could not find the specified configurator");
			goto fail;
		}
	}

	pos = os_strstr(cmd, " conn_status=");
	if (pos) {
		pos += 13;
		auth->send_conn_status = atoi(pos);
	}

	pos = os_strstr(cmd, " akm_use_selector=");
	if (pos) {
		pos += 18;
		auth->akm_use_selector = atoi(pos);
	}

	if (dpp_configuration_parse(auth, cmd) < 0) {
		wpa_msg(auth->msg_ctx, MSG_INFO,
			"DPP: Failed to set configurator parameters");
		goto fail;
	}
	ret = 0;
fail:
	os_free(tmp);
	return ret;
}


static void dpp_free_asymmetric_key(struct dpp_asymmetric_key *key)
{
	while (key) {
		struct dpp_asymmetric_key *next = key->next;

		EVP_PKEY_free(key->csign);
		str_clear_free(key->config_template);
		str_clear_free(key->connector_template);
		os_free(key);
		key = next;
	}
}


void dpp_auth_deinit(struct dpp_authentication *auth)
{
	unsigned int i;

	if (!auth)
		return;
	dpp_configuration_free(auth->conf_ap);
	dpp_configuration_free(auth->conf2_ap);
	dpp_configuration_free(auth->conf_sta);
	dpp_configuration_free(auth->conf2_sta);
	EVP_PKEY_free(auth->own_protocol_key);
	EVP_PKEY_free(auth->peer_protocol_key);
	wpabuf_free(auth->req_msg);
	wpabuf_free(auth->resp_msg);
	wpabuf_free(auth->conf_req);
	for (i = 0; i < auth->num_conf_obj; i++) {
		struct dpp_config_obj *conf = &auth->conf_obj[i];

		os_free(conf->connector);
		wpabuf_free(conf->c_sign_key);
	}
	dpp_free_asymmetric_key(auth->conf_key_pkg);
	wpabuf_free(auth->net_access_key);
	dpp_bootstrap_info_free(auth->tmp_own_bi);
#ifdef CONFIG_TESTING_OPTIONS
	os_free(auth->config_obj_override);
	os_free(auth->discovery_override);
	os_free(auth->groups_override);
#endif /* CONFIG_TESTING_OPTIONS */
	bin_clear_free(auth, sizeof(*auth));
}


static struct wpabuf *
dpp_build_conf_start(struct dpp_authentication *auth,
		     struct dpp_configuration *conf, size_t tailroom)
{
	struct wpabuf *buf;

#ifdef CONFIG_TESTING_OPTIONS
	if (auth->discovery_override)
		tailroom += os_strlen(auth->discovery_override);
#endif /* CONFIG_TESTING_OPTIONS */

	buf = wpabuf_alloc(200 + tailroom);
	if (!buf)
		return NULL;
	json_start_object(buf, NULL);
	json_add_string(buf, "wi-fi_tech", "infra");
	json_value_sep(buf);
#ifdef CONFIG_TESTING_OPTIONS
	if (auth->discovery_override) {
		wpa_printf(MSG_DEBUG, "DPP: TESTING - discovery override: '%s'",
			   auth->discovery_override);
		wpabuf_put_str(buf, "\"discovery\":");
		wpabuf_put_str(buf, auth->discovery_override);
		json_value_sep(buf);
		return buf;
	}
#endif /* CONFIG_TESTING_OPTIONS */
	json_start_object(buf, "discovery");
	if (((!conf->ssid_charset || auth->peer_version < 2) &&
	     json_add_string_escape(buf, "ssid", conf->ssid,
				    conf->ssid_len) < 0) ||
	    ((conf->ssid_charset && auth->peer_version >= 2) &&
	     json_add_base64url(buf, "ssid64", conf->ssid,
				conf->ssid_len) < 0)) {
		wpabuf_free(buf);
		return NULL;
	}
	if (conf->ssid_charset > 0) {
		json_value_sep(buf);
		json_add_int(buf, "ssid_charset", conf->ssid_charset);
	}
	json_end_object(buf);
	json_value_sep(buf);

	return buf;
}


static int dpp_build_jwk(struct wpabuf *buf, const char *name, EVP_PKEY *key,
			 const char *kid, const struct dpp_curve_params *curve)
{
	struct wpabuf *pub;
	const u8 *pos;
	int ret = -1;

	pub = dpp_get_pubkey_point(key, 0);
	if (!pub)
		goto fail;

	json_start_object(buf, name);
	json_add_string(buf, "kty", "EC");
	json_value_sep(buf);
	json_add_string(buf, "crv", curve->jwk_crv);
	json_value_sep(buf);
	pos = wpabuf_head(pub);
	if (json_add_base64url(buf, "x", pos, curve->prime_len) < 0)
		goto fail;
	json_value_sep(buf);
	pos += curve->prime_len;
	if (json_add_base64url(buf, "y", pos, curve->prime_len) < 0)
		goto fail;
	if (kid) {
		json_value_sep(buf);
		json_add_string(buf, "kid", kid);
	}
	json_end_object(buf);
	ret = 0;
fail:
	wpabuf_free(pub);
	return ret;
}


static void dpp_build_legacy_cred_params(struct wpabuf *buf,
					 struct dpp_configuration *conf)
{
	if (conf->passphrase && os_strlen(conf->passphrase) < 64) {
		json_add_string_escape(buf, "pass", conf->passphrase,
				       os_strlen(conf->passphrase));
	} else if (conf->psk_set) {
		char psk[2 * sizeof(conf->psk) + 1];

		wpa_snprintf_hex(psk, sizeof(psk),
				 conf->psk, sizeof(conf->psk));
		json_add_string(buf, "psk_hex", psk);
		forced_memzero(psk, sizeof(psk));
	}
}


static const char * dpp_netrole_str(enum dpp_netrole netrole)
{
	switch (netrole) {
	case DPP_NETROLE_STA:
		return "sta";
	case DPP_NETROLE_AP:
		return "ap";
	case DPP_NETROLE_CONFIGURATOR:
		return "configurator";
	default:
		return "??";
	}
}


static struct wpabuf *
dpp_build_conf_obj_dpp(struct dpp_authentication *auth,
		       struct dpp_configuration *conf)
{
	struct wpabuf *buf = NULL;
	char *signed_conn = NULL;
	size_t tailroom;
	const struct dpp_curve_params *curve;
	struct wpabuf *dppcon = NULL;
	size_t extra_len = 1000;
	int incl_legacy;
	enum dpp_akm akm;
	const char *akm_str;

	if (!auth->conf) {
		wpa_printf(MSG_INFO,
			   "DPP: No configurator specified - cannot generate DPP config object");
		goto fail;
	}
	curve = auth->conf->curve;

	akm = conf->akm;
	if (dpp_akm_ver2(akm) && auth->peer_version < 2) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Convert DPP+legacy credential to DPP-only for peer that does not support version 2");
		akm = DPP_AKM_DPP;
	}

#ifdef CONFIG_TESTING_OPTIONS
	if (auth->groups_override)
		extra_len += os_strlen(auth->groups_override);
#endif /* CONFIG_TESTING_OPTIONS */

	if (conf->group_id)
		extra_len += os_strlen(conf->group_id);

	/* Connector (JSON dppCon object) */
	dppcon = wpabuf_alloc(extra_len + 2 * auth->curve->prime_len * 4 / 3);
	if (!dppcon)
		goto fail;
#ifdef CONFIG_TESTING_OPTIONS
	if (auth->groups_override) {
		wpabuf_put_u8(dppcon, '{');
		if (auth->groups_override) {
			wpa_printf(MSG_DEBUG,
				   "DPP: TESTING - groups override: '%s'",
				   auth->groups_override);
			wpabuf_put_str(dppcon, "\"groups\":");
			wpabuf_put_str(dppcon, auth->groups_override);
			json_value_sep(dppcon);
		}
		goto skip_groups;
	}
#endif /* CONFIG_TESTING_OPTIONS */
	json_start_object(dppcon, NULL);
	json_start_array(dppcon, "groups");
	json_start_object(dppcon, NULL);
	json_add_string(dppcon, "groupId",
			conf->group_id ? conf->group_id : "*");
	json_value_sep(dppcon);
	json_add_string(dppcon, "netRole", dpp_netrole_str(conf->netrole));
	json_end_object(dppcon);
	json_end_array(dppcon);
	json_value_sep(dppcon);
#ifdef CONFIG_TESTING_OPTIONS
skip_groups:
#endif /* CONFIG_TESTING_OPTIONS */
	if (dpp_build_jwk(dppcon, "netAccessKey", auth->peer_protocol_key, NULL,
			  auth->curve) < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Failed to build netAccessKey JWK");
		goto fail;
	}
	if (conf->netaccesskey_expiry) {
		struct os_tm tm;
		char expiry[30];

		if (os_gmtime(conf->netaccesskey_expiry, &tm) < 0) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Failed to generate expiry string");
			goto fail;
		}
		os_snprintf(expiry, sizeof(expiry),
			    "%04u-%02u-%02uT%02u:%02u:%02uZ",
			    tm.year, tm.month, tm.day,
			    tm.hour, tm.min, tm.sec);
		json_value_sep(dppcon);
		json_add_string(dppcon, "expiry", expiry);
	}
	json_end_object(dppcon);
	wpa_printf(MSG_DEBUG, "DPP: dppCon: %s",
		   (const char *) wpabuf_head(dppcon));

	signed_conn = dpp_sign_connector(auth->conf, dppcon);
	if (!signed_conn)
		goto fail;

	incl_legacy = dpp_akm_psk(akm) || dpp_akm_sae(akm);
	tailroom = 1000;
	tailroom += 2 * curve->prime_len * 4 / 3 + os_strlen(auth->conf->kid);
	tailroom += os_strlen(signed_conn);
	if (incl_legacy)
		tailroom += 1000;
	buf = dpp_build_conf_start(auth, conf, tailroom);
	if (!buf)
		goto fail;

	if (auth->akm_use_selector && dpp_akm_ver2(akm))
		akm_str = dpp_akm_selector_str(akm);
	else
		akm_str = dpp_akm_str(akm);
	json_start_object(buf, "cred");
	json_add_string(buf, "akm", akm_str);
	json_value_sep(buf);
	if (incl_legacy) {
		dpp_build_legacy_cred_params(buf, conf);
		json_value_sep(buf);
	}
	wpabuf_put_str(buf, "\"signedConnector\":\"");
	wpabuf_put_str(buf, signed_conn);
	wpabuf_put_str(buf, "\"");
	json_value_sep(buf);
	if (dpp_build_jwk(buf, "csign", auth->conf->csign, auth->conf->kid,
			  curve) < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Failed to build csign JWK");
		goto fail;
	}

	json_end_object(buf);
	json_end_object(buf);

	wpa_hexdump_ascii_key(MSG_DEBUG, "DPP: Configuration Object",
			      wpabuf_head(buf), wpabuf_len(buf));

out:
	os_free(signed_conn);
	wpabuf_free(dppcon);
	return buf;
fail:
	wpa_printf(MSG_DEBUG, "DPP: Failed to build configuration object");
	wpabuf_free(buf);
	buf = NULL;
	goto out;
}


static struct wpabuf *
dpp_build_conf_obj_legacy(struct dpp_authentication *auth,
			  struct dpp_configuration *conf)
{
	struct wpabuf *buf;
	const char *akm_str;

	buf = dpp_build_conf_start(auth, conf, 1000);
	if (!buf)
		return NULL;

	if (auth->akm_use_selector && dpp_akm_ver2(conf->akm))
		akm_str = dpp_akm_selector_str(conf->akm);
	else
		akm_str = dpp_akm_str(conf->akm);
	json_start_object(buf, "cred");
	json_add_string(buf, "akm", akm_str);
	json_value_sep(buf);
	dpp_build_legacy_cred_params(buf, conf);
	json_end_object(buf);
	json_end_object(buf);

	wpa_hexdump_ascii_key(MSG_DEBUG, "DPP: Configuration Object (legacy)",
			      wpabuf_head(buf), wpabuf_len(buf));

	return buf;
}


static struct wpabuf *
dpp_build_conf_obj(struct dpp_authentication *auth, enum dpp_netrole netrole,
		   int idx)
{
	struct dpp_configuration *conf = NULL;

#ifdef CONFIG_TESTING_OPTIONS
	if (auth->config_obj_override) {
		if (idx != 0)
			return NULL;
		wpa_printf(MSG_DEBUG, "DPP: Testing - Config Object override");
		return wpabuf_alloc_copy(auth->config_obj_override,
					 os_strlen(auth->config_obj_override));
	}
#endif /* CONFIG_TESTING_OPTIONS */

	if (idx == 0) {
		if (netrole == DPP_NETROLE_STA)
			conf = auth->conf_sta;
		else if (netrole == DPP_NETROLE_AP)
			conf = auth->conf_ap;
	} else if (idx == 1) {
		if (netrole == DPP_NETROLE_STA)
			conf = auth->conf2_sta;
		else if (netrole == DPP_NETROLE_AP)
			conf = auth->conf2_ap;
	}
	if (!conf) {
		if (idx == 0)
			wpa_printf(MSG_DEBUG,
				   "DPP: No configuration available for Enrollee(%s) - reject configuration request",
				   dpp_netrole_str(netrole));
		return NULL;
	}

	if (dpp_akm_dpp(conf->akm) || (auth->peer_version >= 2 && auth->conf))
		return dpp_build_conf_obj_dpp(auth, conf);
	return dpp_build_conf_obj_legacy(auth, conf);
}


#ifdef CONFIG_DPP2

static struct wpabuf * dpp_build_conf_params(void)
{
	struct wpabuf *buf;
	size_t len;
	/* TODO: proper template values */
	const char *conf_template = "{\"wi-fi_tech\":\"infra\",\"discovery\":{\"ssid\":\"test\"},\"cred\":{\"akm\":\"dpp\"}}";
	const char *connector_template = NULL;

	len = 100 + os_strlen(conf_template);
	if (connector_template)
		len += os_strlen(connector_template);
	buf = wpabuf_alloc(len);
	if (!buf)
		return NULL;

	/*
	 * DPPConfigurationParameters ::= SEQUENCE {
	 *    configurationTemplate	UTF8String,
	 *    connectorTemplate		UTF8String OPTIONAL}
	 */

	asn1_put_utf8string(buf, conf_template);
	if (connector_template)
		asn1_put_utf8string(buf, connector_template);
	return asn1_encaps(buf, ASN1_CLASS_UNIVERSAL, ASN1_TAG_SEQUENCE);
}


static struct wpabuf * dpp_build_attribute(void)
{
	struct wpabuf *conf_params, *attr;

	/*
	 * aa-DPPConfigurationParameters ATTRIBUTE ::=
	 * { TYPE DPPConfigurationParameters IDENTIFIED BY id-DPPConfigParams }
	 *
	 * Attribute ::= SEQUENCE {
	 *    type OBJECT IDENTIFIER,
	 *    values SET SIZE(1..MAX) OF Type
	 */
	conf_params = dpp_build_conf_params();
	conf_params = asn1_encaps(conf_params, ASN1_CLASS_UNIVERSAL,
				  ASN1_TAG_SET);
	if (!conf_params)
		return NULL;

	attr = wpabuf_alloc(100 + wpabuf_len(conf_params));
	if (!attr) {
		wpabuf_clear_free(conf_params);
		return NULL;
	}

	asn1_put_oid(attr, &asn1_dpp_config_params_oid);
	wpabuf_put_buf(attr, conf_params);
	wpabuf_clear_free(conf_params);

	return asn1_encaps(attr, ASN1_CLASS_UNIVERSAL, ASN1_TAG_SEQUENCE);
}


static struct wpabuf * dpp_build_key_alg(const struct dpp_curve_params *curve)
{
	const struct asn1_oid *oid;
	struct wpabuf *params, *res;

	switch (curve->ike_group) {
	case 19:
		oid = &asn1_prime256v1_oid;
		break;
	case 20:
		oid = &asn1_secp384r1_oid;
		break;
	case 21:
		oid = &asn1_secp521r1_oid;
		break;
	case 28:
		oid = &asn1_brainpoolP256r1_oid;
		break;
	case 29:
		oid = &asn1_brainpoolP384r1_oid;
		break;
	case 30:
		oid = &asn1_brainpoolP512r1_oid;
		break;
	default:
		return NULL;
	}

	params = wpabuf_alloc(20);
	if (!params)
		return NULL;
	asn1_put_oid(params, oid); /* namedCurve */

	res = asn1_build_alg_id(&asn1_ec_public_key_oid, params);
	wpabuf_free(params);
	return res;
}


static struct wpabuf * dpp_build_key_pkg(struct dpp_authentication *auth)
{
	struct wpabuf *key = NULL, *attr, *alg, *priv_key = NULL;
	EC_KEY *eckey;
	unsigned char *der = NULL;
	int der_len;

	eckey = EVP_PKEY_get0_EC_KEY(auth->conf->csign);
	if (!eckey)
		return NULL;

	EC_KEY_set_enc_flags(eckey, EC_PKEY_NO_PUBKEY);
	der_len = i2d_ECPrivateKey(eckey, &der);
	if (der_len > 0)
		priv_key = wpabuf_alloc_copy(der, der_len);
	OPENSSL_free(der);

	alg = dpp_build_key_alg(auth->conf->curve);

	/* Attributes ::= SET OF Attribute { { OneAsymmetricKeyAttributes } } */
	attr = dpp_build_attribute();
	attr = asn1_encaps(attr, ASN1_CLASS_UNIVERSAL, ASN1_TAG_SET);
	if (!priv_key || !attr || !alg)
		goto fail;

	/*
	 * OneAsymmetricKey ::= SEQUENCE {
	 *    version			Version,
	 *    privateKeyAlgorithm	PrivateKeyAlgorithmIdentifier,
	 *    privateKey		PrivateKey,
	 *    attributes		[0] Attributes OPTIONAL,
	 *    ...,
	 *    [[2: publicKey		[1] BIT STRING OPTIONAL ]],
	 *    ...
	 * }
	 */

	key = wpabuf_alloc(100 + wpabuf_len(alg) + wpabuf_len(priv_key) +
			   wpabuf_len(attr));
	if (!key)
		goto fail;

	asn1_put_integer(key, 1); /* version = v2(1) */

	/* PrivateKeyAlgorithmIdentifier */
	wpabuf_put_buf(key, alg);

	/* PrivateKey ::= OCTET STRING */
	asn1_put_octet_string(key, priv_key);

	/* [0] Attributes OPTIONAL */
	asn1_put_hdr(key, ASN1_CLASS_CONTEXT_SPECIFIC, 1, 0, wpabuf_len(attr));
	wpabuf_put_buf(key, attr);

fail:
	wpabuf_clear_free(attr);
	wpabuf_clear_free(priv_key);
	wpabuf_free(alg);

	/*
	 * DPPAsymmetricKeyPackage ::= AsymmetricKeyPackage
	 *
	 * AsymmetricKeyPackage ::= SEQUENCE SIZE (1..MAX) OF OneAsymmetricKey
	 *
	 * OneAsymmetricKey ::= SEQUENCE
	 */
	return asn1_encaps(asn1_encaps(key,
				       ASN1_CLASS_UNIVERSAL, ASN1_TAG_SEQUENCE),
			   ASN1_CLASS_UNIVERSAL, ASN1_TAG_SEQUENCE);
}


static struct wpabuf * dpp_build_pbkdf2_alg_id(const struct wpabuf *salt,
					       size_t hash_len)
{
	struct wpabuf *params = NULL, *buf = NULL, *prf = NULL;
	const struct asn1_oid *oid;

	/*
	 * PBKDF2-params ::= SEQUENCE {
	 *    salt CHOICE {
	 *       specified OCTET STRING,
	 *       otherSource AlgorithmIdentifier}
	 *    iterationCount INTEGER (1..MAX),
	 *    keyLength INTEGER (1..MAX),
	 *    prf AlgorithmIdentifier}
	 *
	 * salt is an 64 octet value, iterationCount is 1000, keyLength is based
	 * on Configurator signing key length, prf is
	 * id-hmacWithSHA{256,384,512} based on Configurator signing key.
	 */

	if (hash_len == 32)
		oid = &asn1_pbkdf2_hmac_sha256_oid;
	else if (hash_len == 48)
		oid = &asn1_pbkdf2_hmac_sha384_oid;
	else if (hash_len == 64)
		oid = &asn1_pbkdf2_hmac_sha512_oid;
	else
		goto fail;
	prf = asn1_build_alg_id(oid, NULL);
	if (!prf)
		goto fail;
	params = wpabuf_alloc(100 + wpabuf_len(salt) + wpabuf_len(prf));
	if (!params)
		goto fail;
	asn1_put_octet_string(params, salt); /* salt.specified */
	asn1_put_integer(params, 1000); /* iterationCount */
	asn1_put_integer(params, hash_len); /* keyLength */
	wpabuf_put_buf(params, prf);
	params = asn1_encaps(params, ASN1_CLASS_UNIVERSAL, ASN1_TAG_SEQUENCE);
	if (!params)
		goto fail;
	buf = asn1_build_alg_id(&asn1_pbkdf2_oid, params);
fail:
	wpabuf_free(params);
	wpabuf_free(prf);
	return buf;
}


static struct wpabuf *
dpp_build_pw_recipient_info(struct dpp_authentication *auth, size_t hash_len,
			    const struct wpabuf *cont_enc_key)
{
	struct wpabuf *pwri = NULL, *enc_key = NULL, *key_der_alg = NULL,
		*key_enc_alg = NULL, *salt;
	u8 kek[DPP_MAX_HASH_LEN];
	const u8 *key;
	size_t key_len;

	salt = wpabuf_alloc(64);
	if (!salt || os_get_random(wpabuf_put(salt, 64), 64) < 0)
		goto fail;
	wpa_hexdump_buf(MSG_DEBUG, "DPP: PBKDF2 salt", salt);

	/* TODO: For initial testing, use ke as the key. Replace this with a
	 * new key once that has been defined. */
	key = auth->ke;
	key_len = auth->curve->hash_len;
	wpa_hexdump_key(MSG_DEBUG, "DPP: PBKDF2 key", key, key_len);

	if (dpp_pbkdf2(hash_len, key, key_len, wpabuf_head(salt), 64, 1000,
		       kek, hash_len)) {
		wpa_printf(MSG_DEBUG, "DPP: PBKDF2 failed");
		goto fail;
	}
	wpa_hexdump_key(MSG_DEBUG, "DPP: key-encryption key from PBKDF2",
			kek, hash_len);

	enc_key = wpabuf_alloc(hash_len + AES_BLOCK_SIZE);
	if (!enc_key ||
	    aes_siv_encrypt(kek, hash_len, wpabuf_head(cont_enc_key),
			    wpabuf_len(cont_enc_key), 0, NULL, NULL,
			    wpabuf_put(enc_key, hash_len + AES_BLOCK_SIZE)) < 0)
		goto fail;
	wpa_hexdump_buf(MSG_DEBUG, "DPP: encryptedKey", enc_key);

	/*
	 * PasswordRecipientInfo ::= SEQUENCE {
	 *    version			CMSVersion,
	 *    keyDerivationAlgorithm [0] KeyDerivationAlgorithmIdentifier OPTIONAL,
	 *    keyEncryptionAlgorithm	KeyEncryptionAlgorithmIdentifier,
	 *    encryptedKey		EncryptedKey}
	 *
	 * version is 0, keyDerivationAlgorithm is id-PKBDF2, and the
	 * parameters contains PBKDF2-params SEQUENCE.
	 */

	key_der_alg = dpp_build_pbkdf2_alg_id(salt, hash_len);
	key_enc_alg = asn1_build_alg_id(&asn1_aes_siv_cmac_aead_256_oid, NULL);
	if (!key_der_alg || !key_enc_alg)
		goto fail;
	pwri = wpabuf_alloc(100 + wpabuf_len(key_der_alg) +
			    wpabuf_len(key_enc_alg) + wpabuf_len(enc_key));
	if (!pwri)
		goto fail;

	/* version = 0 */
	asn1_put_integer(pwri, 0);

	/* [0] KeyDerivationAlgorithmIdentifier */
	asn1_put_hdr(pwri, ASN1_CLASS_CONTEXT_SPECIFIC, 1, 0,
		     wpabuf_len(key_der_alg));
	wpabuf_put_buf(pwri, key_der_alg);

	/* KeyEncryptionAlgorithmIdentifier */
	wpabuf_put_buf(pwri, key_enc_alg);

	/* EncryptedKey ::= OCTET STRING */
	asn1_put_octet_string(pwri, enc_key);

fail:
	wpabuf_clear_free(key_der_alg);
	wpabuf_free(key_enc_alg);
	wpabuf_free(enc_key);
	wpabuf_free(salt);
	forced_memzero(kek, sizeof(kek));
	return asn1_encaps(pwri, ASN1_CLASS_UNIVERSAL, ASN1_TAG_SEQUENCE);
}


static struct wpabuf *
dpp_build_recipient_info(struct dpp_authentication *auth, size_t hash_len,
			 const struct wpabuf *cont_enc_key)
{
	struct wpabuf *pwri;

	/*
	 * RecipientInfo ::= CHOICE {
	 *    ktri		KeyTransRecipientInfo,
	 *    kari	[1]	KeyAgreeRecipientInfo,
	 *    kekri	[2]	KEKRecipientInfo,
	 *    pwri	[3]	PasswordRecipientInfo,
	 *    ori	[4]	OtherRecipientInfo}
	 *
	 * Shall always use the pwri CHOICE.
	 */

	pwri = dpp_build_pw_recipient_info(auth, hash_len, cont_enc_key);
	return asn1_encaps(pwri, ASN1_CLASS_CONTEXT_SPECIFIC, 3);
}


static struct wpabuf *
dpp_build_enc_cont_info(struct dpp_authentication *auth, size_t hash_len,
			const struct wpabuf *cont_enc_key)
{
	struct wpabuf *key_pkg, *enc_cont_info = NULL, *enc_cont = NULL,
		*enc_alg;
	const struct asn1_oid *oid;
	size_t enc_cont_len;

	/*
	 * EncryptedContentInfo ::= SEQUENCE {
	 *    contentType			ContentType,
	 *    contentEncryptionAlgorithm  ContentEncryptionAlgorithmIdentifier,
	 *    encryptedContent	[0] IMPLICIT	EncryptedContent OPTIONAL}
	 */

	if (hash_len == 32)
		oid = &asn1_aes_siv_cmac_aead_256_oid;
	else if (hash_len == 48)
		oid = &asn1_aes_siv_cmac_aead_384_oid;
	else if (hash_len == 64)
		oid = &asn1_aes_siv_cmac_aead_512_oid;
	else
		return NULL;

	key_pkg = dpp_build_key_pkg(auth);
	enc_alg = asn1_build_alg_id(oid, NULL);
	if (!key_pkg || !enc_alg)
		goto fail;

	wpa_hexdump_buf_key(MSG_MSGDUMP, "DPP: DPPAsymmetricKeyPackage",
			    key_pkg);

	enc_cont_len = wpabuf_len(key_pkg) + AES_BLOCK_SIZE;
	enc_cont = wpabuf_alloc(enc_cont_len);
	if (!enc_cont ||
	    aes_siv_encrypt(wpabuf_head(cont_enc_key), wpabuf_len(cont_enc_key),
			    wpabuf_head(key_pkg), wpabuf_len(key_pkg),
			    0, NULL, NULL,
			    wpabuf_put(enc_cont, enc_cont_len)) < 0)
		goto fail;

	enc_cont_info = wpabuf_alloc(100 + wpabuf_len(enc_alg) +
				     wpabuf_len(enc_cont));
	if (!enc_cont_info)
		goto fail;

	/* ContentType ::= OBJECT IDENTIFIER */
	asn1_put_oid(enc_cont_info, &asn1_dpp_asymmetric_key_package_oid);

	/* ContentEncryptionAlgorithmIdentifier ::= AlgorithmIdentifier */
	wpabuf_put_buf(enc_cont_info, enc_alg);

	/* encryptedContent [0] IMPLICIT EncryptedContent OPTIONAL
	 * EncryptedContent ::= OCTET STRING */
	asn1_put_hdr(enc_cont_info, ASN1_CLASS_CONTEXT_SPECIFIC, 0, 0,
		     wpabuf_len(enc_cont));
	wpabuf_put_buf(enc_cont_info, enc_cont);

fail:
	wpabuf_clear_free(key_pkg);
	wpabuf_free(enc_cont);
	wpabuf_free(enc_alg);
	return enc_cont_info;
}


static struct wpabuf * dpp_gen_random(size_t len)
{
	struct wpabuf *key;

	key = wpabuf_alloc(len);
	if (!key || os_get_random(wpabuf_put(key, len), len) < 0) {
		wpabuf_free(key);
		key = NULL;
	}
	wpa_hexdump_buf_key(MSG_DEBUG, "DPP: content-encryption key", key);
	return key;
}


static struct wpabuf * dpp_build_enveloped_data(struct dpp_authentication *auth)
{
	struct wpabuf *env = NULL;
	struct wpabuf *recipient_info = NULL, *enc_cont_info = NULL;
	struct wpabuf *cont_enc_key = NULL;
	size_t hash_len;

	if (!auth->conf) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No Configurator instance selected for the session - cannot build DPPEnvelopedData");
		return NULL;
	}

	if (!auth->provision_configurator) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Configurator provisioning not allowed");
		return NULL;
	}

	wpa_printf(MSG_DEBUG, "DPP: Building DPPEnvelopedData");

	hash_len = auth->conf->curve->hash_len;
	cont_enc_key = dpp_gen_random(hash_len);
	if (!cont_enc_key)
		goto fail;
	recipient_info = dpp_build_recipient_info(auth, hash_len, cont_enc_key);
	enc_cont_info = dpp_build_enc_cont_info(auth, hash_len, cont_enc_key);
	if (!recipient_info || !enc_cont_info)
		goto fail;

	env = wpabuf_alloc(wpabuf_len(recipient_info) +
			   wpabuf_len(enc_cont_info) +
			   100);
	if (!env)
		goto fail;

	/*
	 * DPPEnvelopedData ::= EnvelopedData
	 *
	 * EnvelopedData ::= SEQUENCE {
	 *    version			CMSVersion,
	 *    originatorInfo	[0]	IMPLICIT OriginatorInfo OPTIONAL,
	 *    recipientInfos		RecipientInfos,
	 *    encryptedContentInfo	EncryptedContentInfo,
	 *    unprotectedAttrs  [1] IMPLICIT	UnprotectedAttributes OPTIONAL}
	 *
	 * For DPP, version is 3, both originatorInfo and
	 * unprotectedAttrs are omitted, and recipientInfos contains a single
	 * RecipientInfo.
	 */

	/* EnvelopedData.version = 3 */
	asn1_put_integer(env, 3);

	/* RecipientInfos ::= SET SIZE (1..MAX) OF RecipientInfo */
	asn1_put_set(env, recipient_info);

	/* EncryptedContentInfo ::= SEQUENCE */
	asn1_put_sequence(env, enc_cont_info);

	env = asn1_encaps(env, ASN1_CLASS_UNIVERSAL, ASN1_TAG_SEQUENCE);
	wpa_hexdump_buf(MSG_MSGDUMP, "DPP: DPPEnvelopedData", env);
out:
	wpabuf_clear_free(cont_enc_key);
	wpabuf_clear_free(recipient_info);
	wpabuf_free(enc_cont_info);
	return env;
fail:
	wpabuf_free(env);
	env = NULL;
	goto out;
}

#endif /* CONFIG_DPP2 */


static struct wpabuf *
dpp_build_conf_resp(struct dpp_authentication *auth, const u8 *e_nonce,
		    u16 e_nonce_len, enum dpp_netrole netrole)
{
	struct wpabuf *conf = NULL, *conf2 = NULL, *env_data = NULL;
	size_t clear_len, attr_len;
	struct wpabuf *clear = NULL, *msg = NULL;
	u8 *wrapped;
	const u8 *addr[1];
	size_t len[1];
	enum dpp_status_error status;

	if (netrole == DPP_NETROLE_CONFIGURATOR) {
#ifdef CONFIG_DPP2
		env_data = dpp_build_enveloped_data(auth);
#endif /* CONFIG_DPP2 */
	} else {
		conf = dpp_build_conf_obj(auth, netrole, 0);
		if (conf) {
			wpa_hexdump_ascii(MSG_DEBUG,
					  "DPP: configurationObject JSON",
					  wpabuf_head(conf), wpabuf_len(conf));
			conf2 = dpp_build_conf_obj(auth, netrole, 1);
		}
	}
	status = (conf || env_data) ? DPP_STATUS_OK :
		DPP_STATUS_CONFIGURE_FAILURE;
	auth->conf_resp_status = status;

	/* { E-nonce, configurationObject[, sendConnStatus]}ke */
	clear_len = 4 + e_nonce_len;
	if (conf)
		clear_len += 4 + wpabuf_len(conf);
	if (conf2)
		clear_len += 4 + wpabuf_len(conf2);
	if (env_data)
		clear_len += 4 + wpabuf_len(env_data);
	if (auth->peer_version >= 2 && auth->send_conn_status &&
	    netrole == DPP_NETROLE_STA)
		clear_len += 4;
	clear = wpabuf_alloc(clear_len);
	attr_len = 4 + 1 + 4 + clear_len + AES_BLOCK_SIZE;
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_CONF_RESP)
		attr_len += 5;
#endif /* CONFIG_TESTING_OPTIONS */
	msg = wpabuf_alloc(attr_len);
	if (!clear || !msg)
		goto fail;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_NO_E_NONCE_CONF_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no E-nonce");
		goto skip_e_nonce;
	}
	if (dpp_test == DPP_TEST_E_NONCE_MISMATCH_CONF_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - E-nonce mismatch");
		wpabuf_put_le16(clear, DPP_ATTR_ENROLLEE_NONCE);
		wpabuf_put_le16(clear, e_nonce_len);
		wpabuf_put_data(clear, e_nonce, e_nonce_len - 1);
		wpabuf_put_u8(clear, e_nonce[e_nonce_len - 1] ^ 0x01);
		goto skip_e_nonce;
	}
	if (dpp_test == DPP_TEST_NO_WRAPPED_DATA_CONF_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - no Wrapped Data");
		goto skip_wrapped_data;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* E-nonce */
	wpabuf_put_le16(clear, DPP_ATTR_ENROLLEE_NONCE);
	wpabuf_put_le16(clear, e_nonce_len);
	wpabuf_put_data(clear, e_nonce, e_nonce_len);

#ifdef CONFIG_TESTING_OPTIONS
skip_e_nonce:
	if (dpp_test == DPP_TEST_NO_CONFIG_OBJ_CONF_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - Config Object");
		goto skip_config_obj;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	if (conf) {
		wpabuf_put_le16(clear, DPP_ATTR_CONFIG_OBJ);
		wpabuf_put_le16(clear, wpabuf_len(conf));
		wpabuf_put_buf(clear, conf);
	}
	if (auth->peer_version >= 2 && conf2) {
		wpabuf_put_le16(clear, DPP_ATTR_CONFIG_OBJ);
		wpabuf_put_le16(clear, wpabuf_len(conf2));
		wpabuf_put_buf(clear, conf2);
	} else if (conf2) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Second Config Object available, but peer does not support more than one");
	}
	if (env_data) {
		wpabuf_put_le16(clear, DPP_ATTR_ENVELOPED_DATA);
		wpabuf_put_le16(clear, wpabuf_len(env_data));
		wpabuf_put_buf(clear, env_data);
	}

	if (auth->peer_version >= 2 && auth->send_conn_status &&
	    netrole == DPP_NETROLE_STA) {
		wpa_printf(MSG_DEBUG, "DPP: sendConnStatus");
		wpabuf_put_le16(clear, DPP_ATTR_SEND_CONN_STATUS);
		wpabuf_put_le16(clear, 0);
	}

#ifdef CONFIG_TESTING_OPTIONS
skip_config_obj:
	if (dpp_test == DPP_TEST_NO_STATUS_CONF_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - Status");
		goto skip_status;
	}
	if (dpp_test == DPP_TEST_INVALID_STATUS_CONF_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - invalid Status");
		status = 255;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	/* DPP Status */
	dpp_build_attr_status(msg, status);

#ifdef CONFIG_TESTING_OPTIONS
skip_status:
#endif /* CONFIG_TESTING_OPTIONS */

	addr[0] = wpabuf_head(msg);
	len[0] = wpabuf_len(msg);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD", addr[0], len[0]);

	wpabuf_put_le16(msg, DPP_ATTR_WRAPPED_DATA);
	wpabuf_put_le16(msg, wpabuf_len(clear) + AES_BLOCK_SIZE);
	wrapped = wpabuf_put(msg, wpabuf_len(clear) + AES_BLOCK_SIZE);

	wpa_hexdump_buf(MSG_DEBUG, "DPP: AES-SIV cleartext", clear);
	if (aes_siv_encrypt(auth->ke, auth->curve->hash_len,
			    wpabuf_head(clear), wpabuf_len(clear),
			    1, addr, len, wrapped) < 0)
		goto fail;
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped, wpabuf_len(clear) + AES_BLOCK_SIZE);

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_AFTER_WRAPPED_DATA_CONF_RESP) {
		wpa_printf(MSG_INFO, "DPP: TESTING - attr after Wrapped Data");
		dpp_build_attr_status(msg, DPP_STATUS_OK);
	}
skip_wrapped_data:
#endif /* CONFIG_TESTING_OPTIONS */

	wpa_hexdump_buf(MSG_DEBUG,
			"DPP: Configuration Response attributes", msg);
out:
	wpabuf_clear_free(conf);
	wpabuf_clear_free(conf2);
	wpabuf_clear_free(env_data);
	wpabuf_clear_free(clear);

	return msg;
fail:
	wpabuf_free(msg);
	msg = NULL;
	goto out;
}


struct wpabuf *
dpp_conf_req_rx(struct dpp_authentication *auth, const u8 *attr_start,
		size_t attr_len)
{
	const u8 *wrapped_data, *e_nonce, *config_attr;
	u16 wrapped_data_len, e_nonce_len, config_attr_len;
	u8 *unwrapped = NULL;
	size_t unwrapped_len = 0;
	struct wpabuf *resp = NULL;
	struct json_token *root = NULL, *token;
	enum dpp_netrole netrole;

#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_STOP_AT_CONF_REQ) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - stop at Config Request");
		return NULL;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	if (dpp_check_attrs(attr_start, attr_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in config request");
		return NULL;
	}

	wrapped_data = dpp_get_attr(attr_start, attr_len, DPP_ATTR_WRAPPED_DATA,
				    &wrapped_data_len);
	if (!wrapped_data || wrapped_data_len < AES_BLOCK_SIZE) {
		dpp_auth_fail(auth,
			      "Missing or invalid required Wrapped Data attribute");
		return NULL;
	}

	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped)
		return NULL;
	if (aes_siv_decrypt(auth->ke, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    0, NULL, NULL, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	e_nonce = dpp_get_attr(unwrapped, unwrapped_len,
			       DPP_ATTR_ENROLLEE_NONCE,
			       &e_nonce_len);
	if (!e_nonce || e_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth,
			      "Missing or invalid Enrollee Nonce attribute");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Enrollee Nonce", e_nonce, e_nonce_len);
	os_memcpy(auth->e_nonce, e_nonce, e_nonce_len);

	config_attr = dpp_get_attr(unwrapped, unwrapped_len,
				   DPP_ATTR_CONFIG_ATTR_OBJ,
				   &config_attr_len);
	if (!config_attr) {
		dpp_auth_fail(auth,
			      "Missing or invalid Config Attributes attribute");
		goto fail;
	}
	wpa_hexdump_ascii(MSG_DEBUG, "DPP: Config Attributes",
			  config_attr, config_attr_len);

	root = json_parse((const char *) config_attr, config_attr_len);
	if (!root) {
		dpp_auth_fail(auth, "Could not parse Config Attributes");
		goto fail;
	}

	token = json_get_member(root, "name");
	if (!token || token->type != JSON_STRING) {
		dpp_auth_fail(auth, "No Config Attributes - name");
		goto fail;
	}
	wpa_printf(MSG_DEBUG, "DPP: Enrollee name = '%s'", token->string);

	token = json_get_member(root, "wi-fi_tech");
	if (!token || token->type != JSON_STRING) {
		dpp_auth_fail(auth, "No Config Attributes - wi-fi_tech");
		goto fail;
	}
	wpa_printf(MSG_DEBUG, "DPP: wi-fi_tech = '%s'", token->string);
	if (os_strcmp(token->string, "infra") != 0) {
		wpa_printf(MSG_DEBUG, "DPP: Unsupported wi-fi_tech '%s'",
			   token->string);
		dpp_auth_fail(auth, "Unsupported wi-fi_tech");
		goto fail;
	}

	token = json_get_member(root, "netRole");
	if (!token || token->type != JSON_STRING) {
		dpp_auth_fail(auth, "No Config Attributes - netRole");
		goto fail;
	}
	wpa_printf(MSG_DEBUG, "DPP: netRole = '%s'", token->string);
	if (os_strcmp(token->string, "sta") == 0) {
		netrole = DPP_NETROLE_STA;
	} else if (os_strcmp(token->string, "ap") == 0) {
		netrole = DPP_NETROLE_AP;
	} else if (os_strcmp(token->string, "configurator") == 0) {
		netrole = DPP_NETROLE_CONFIGURATOR;
	} else {
		wpa_printf(MSG_DEBUG, "DPP: Unsupported netRole '%s'",
			   token->string);
		dpp_auth_fail(auth, "Unsupported netRole");
		goto fail;
	}

	token = json_get_member(root, "mudurl");
	if (token && token->type == JSON_STRING) {
		wpa_printf(MSG_DEBUG, "DPP: mudurl = '%s'", token->string);
		wpa_msg(auth->msg_ctx, MSG_INFO, DPP_EVENT_MUD_URL "%s",
			token->string);
	}

	token = json_get_member(root, "bandSupport");
	if (token && token->type == JSON_ARRAY) {
		int *opclass = NULL;
		char txt[200], *pos, *end;
		int i, res;

		wpa_printf(MSG_DEBUG, "DPP: bandSupport");
		token = token->child;
		while (token) {
			if (token->type != JSON_NUMBER) {
				wpa_printf(MSG_DEBUG,
					   "DPP: Invalid bandSupport array member type");
			} else {
				wpa_printf(MSG_DEBUG,
					   "DPP: Supported global operating class: %d",
					   token->number);
				int_array_add_unique(&opclass, token->number);
			}
			token = token->sibling;
		}

		txt[0] = '\0';
		pos = txt;
		end = txt + sizeof(txt);
		for (i = 0; opclass && opclass[i]; i++) {
			res = os_snprintf(pos, end - pos, "%s%d",
					  pos == txt ? "" : ",", opclass[i]);
			if (os_snprintf_error(end - pos, res)) {
				*pos = '\0';
				break;
			}
			pos += res;
		}
		os_free(opclass);
		wpa_msg(auth->msg_ctx, MSG_INFO, DPP_EVENT_BAND_SUPPORT "%s",
			txt);
	}

	resp = dpp_build_conf_resp(auth, e_nonce, e_nonce_len, netrole);

fail:
	json_free(root);
	os_free(unwrapped);
	return resp;
}


static int dpp_parse_cred_legacy(struct dpp_config_obj *conf,
				 struct json_token *cred)
{
	struct json_token *pass, *psk_hex;

	wpa_printf(MSG_DEBUG, "DPP: Legacy akm=psk credential");

	pass = json_get_member(cred, "pass");
	psk_hex = json_get_member(cred, "psk_hex");

	if (pass && pass->type == JSON_STRING) {
		size_t len = os_strlen(pass->string);

		wpa_hexdump_ascii_key(MSG_DEBUG, "DPP: Legacy passphrase",
				      pass->string, len);
		if (len < 8 || len > 63)
			return -1;
		os_strlcpy(conf->passphrase, pass->string,
			   sizeof(conf->passphrase));
	} else if (psk_hex && psk_hex->type == JSON_STRING) {
		if (dpp_akm_sae(conf->akm) && !dpp_akm_psk(conf->akm)) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Unexpected psk_hex with akm=sae");
			return -1;
		}
		if (os_strlen(psk_hex->string) != PMK_LEN * 2 ||
		    hexstr2bin(psk_hex->string, conf->psk, PMK_LEN) < 0) {
			wpa_printf(MSG_DEBUG, "DPP: Invalid psk_hex encoding");
			return -1;
		}
		wpa_hexdump_key(MSG_DEBUG, "DPP: Legacy PSK",
				conf->psk, PMK_LEN);
		conf->psk_set = 1;
	} else {
		wpa_printf(MSG_DEBUG, "DPP: No pass or psk_hex strings found");
		return -1;
	}

	if (dpp_akm_sae(conf->akm) && !conf->passphrase[0]) {
		wpa_printf(MSG_DEBUG, "DPP: No pass for sae found");
		return -1;
	}

	return 0;
}


static EVP_PKEY * dpp_parse_jwk(struct json_token *jwk,
				const struct dpp_curve_params **key_curve)
{
	struct json_token *token;
	const struct dpp_curve_params *curve;
	struct wpabuf *x = NULL, *y = NULL;
	EC_GROUP *group;
	EVP_PKEY *pkey = NULL;

	token = json_get_member(jwk, "kty");
	if (!token || token->type != JSON_STRING) {
		wpa_printf(MSG_DEBUG, "DPP: No kty in JWK");
		goto fail;
	}
	if (os_strcmp(token->string, "EC") != 0) {
		wpa_printf(MSG_DEBUG, "DPP: Unexpected JWK kty '%s'",
			   token->string);
		goto fail;
	}

	token = json_get_member(jwk, "crv");
	if (!token || token->type != JSON_STRING) {
		wpa_printf(MSG_DEBUG, "DPP: No crv in JWK");
		goto fail;
	}
	curve = dpp_get_curve_jwk_crv(token->string);
	if (!curve) {
		wpa_printf(MSG_DEBUG, "DPP: Unsupported JWK crv '%s'",
			   token->string);
		goto fail;
	}

	x = json_get_member_base64url(jwk, "x");
	if (!x) {
		wpa_printf(MSG_DEBUG, "DPP: No x in JWK");
		goto fail;
	}
	wpa_hexdump_buf(MSG_DEBUG, "DPP: JWK x", x);
	if (wpabuf_len(x) != curve->prime_len) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Unexpected JWK x length %u (expected %u for curve %s)",
			   (unsigned int) wpabuf_len(x),
			   (unsigned int) curve->prime_len, curve->name);
		goto fail;
	}

	y = json_get_member_base64url(jwk, "y");
	if (!y) {
		wpa_printf(MSG_DEBUG, "DPP: No y in JWK");
		goto fail;
	}
	wpa_hexdump_buf(MSG_DEBUG, "DPP: JWK y", y);
	if (wpabuf_len(y) != curve->prime_len) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Unexpected JWK y length %u (expected %u for curve %s)",
			   (unsigned int) wpabuf_len(y),
			   (unsigned int) curve->prime_len, curve->name);
		goto fail;
	}

	group = EC_GROUP_new_by_curve_name(OBJ_txt2nid(curve->name));
	if (!group) {
		wpa_printf(MSG_DEBUG, "DPP: Could not prepare group for JWK");
		goto fail;
	}

	pkey = dpp_set_pubkey_point_group(group, wpabuf_head(x), wpabuf_head(y),
					  wpabuf_len(x));
	EC_GROUP_free(group);
	*key_curve = curve;

fail:
	wpabuf_free(x);
	wpabuf_free(y);

	return pkey;
}


int dpp_key_expired(const char *timestamp, os_time_t *expiry)
{
	struct os_time now;
	unsigned int year, month, day, hour, min, sec;
	os_time_t utime;
	const char *pos;

	/* ISO 8601 date and time:
	 * <date>T<time>
	 * YYYY-MM-DDTHH:MM:SSZ
	 * YYYY-MM-DDTHH:MM:SS+03:00
	 */
	if (os_strlen(timestamp) < 19) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Too short timestamp - assume expired key");
		return 1;
	}
	if (sscanf(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u",
		   &year, &month, &day, &hour, &min, &sec) != 6) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Failed to parse expiration day - assume expired key");
		return 1;
	}

	if (os_mktime(year, month, day, hour, min, sec, &utime) < 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Invalid date/time information - assume expired key");
		return 1;
	}

	pos = timestamp + 19;
	if (*pos == 'Z' || *pos == '\0') {
		/* In UTC - no need to adjust */
	} else if (*pos == '-' || *pos == '+') {
		int items;

		/* Adjust local time to UTC */
		items = sscanf(pos + 1, "%02u:%02u", &hour, &min);
		if (items < 1) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Invalid time zone designator (%s) - assume expired key",
				   pos);
			return 1;
		}
		if (*pos == '-')
			utime += 3600 * hour;
		if (*pos == '+')
			utime -= 3600 * hour;
		if (items > 1) {
			if (*pos == '-')
				utime += 60 * min;
			if (*pos == '+')
				utime -= 60 * min;
		}
	} else {
		wpa_printf(MSG_DEBUG,
			   "DPP: Invalid time zone designator (%s) - assume expired key",
			   pos);
		return 1;
	}
	if (expiry)
		*expiry = utime;

	if (os_get_time(&now) < 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Cannot get current time - assume expired key");
		return 1;
	}

	if (now.sec > utime) {
		wpa_printf(MSG_DEBUG, "DPP: Key has expired (%lu < %lu)",
			   utime, now.sec);
		return 1;
	}

	return 0;
}


static int dpp_parse_connector(struct dpp_authentication *auth,
			       struct dpp_config_obj *conf,
			       const unsigned char *payload,
			       u16 payload_len)
{
	struct json_token *root, *groups, *netkey, *token;
	int ret = -1;
	EVP_PKEY *key = NULL;
	const struct dpp_curve_params *curve;
	unsigned int rules = 0;

	root = json_parse((const char *) payload, payload_len);
	if (!root) {
		wpa_printf(MSG_DEBUG, "DPP: JSON parsing of connector failed");
		goto fail;
	}

	groups = json_get_member(root, "groups");
	if (!groups || groups->type != JSON_ARRAY) {
		wpa_printf(MSG_DEBUG, "DPP: No groups array found");
		goto skip_groups;
	}
	for (token = groups->child; token; token = token->sibling) {
		struct json_token *id, *role;

		id = json_get_member(token, "groupId");
		if (!id || id->type != JSON_STRING) {
			wpa_printf(MSG_DEBUG, "DPP: Missing groupId string");
			goto fail;
		}

		role = json_get_member(token, "netRole");
		if (!role || role->type != JSON_STRING) {
			wpa_printf(MSG_DEBUG, "DPP: Missing netRole string");
			goto fail;
		}
		wpa_printf(MSG_DEBUG,
			   "DPP: connector group: groupId='%s' netRole='%s'",
			   id->string, role->string);
		rules++;
	}
skip_groups:

	if (!rules) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Connector includes no groups");
		goto fail;
	}

	token = json_get_member(root, "expiry");
	if (!token || token->type != JSON_STRING) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No expiry string found - connector does not expire");
	} else {
		wpa_printf(MSG_DEBUG, "DPP: expiry = %s", token->string);
		if (dpp_key_expired(token->string,
				    &auth->net_access_key_expiry)) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Connector (netAccessKey) has expired");
			goto fail;
		}
	}

	netkey = json_get_member(root, "netAccessKey");
	if (!netkey || netkey->type != JSON_OBJECT) {
		wpa_printf(MSG_DEBUG, "DPP: No netAccessKey object found");
		goto fail;
	}

	key = dpp_parse_jwk(netkey, &curve);
	if (!key)
		goto fail;
	dpp_debug_print_key("DPP: Received netAccessKey", key);

	if (EVP_PKEY_cmp(key, auth->own_protocol_key) != 1) {
		wpa_printf(MSG_DEBUG,
			   "DPP: netAccessKey in connector does not match own protocol key");
#ifdef CONFIG_TESTING_OPTIONS
		if (auth->ignore_netaccesskey_mismatch) {
			wpa_printf(MSG_DEBUG,
				   "DPP: TESTING - skip netAccessKey mismatch");
		} else {
			goto fail;
		}
#else /* CONFIG_TESTING_OPTIONS */
		goto fail;
#endif /* CONFIG_TESTING_OPTIONS */
	}

	ret = 0;
fail:
	EVP_PKEY_free(key);
	json_free(root);
	return ret;
}


static void dpp_copy_csign(struct dpp_config_obj *conf, EVP_PKEY *csign)
{
	unsigned char *der = NULL;
	int der_len;

	der_len = i2d_PUBKEY(csign, &der);
	if (der_len <= 0)
		return;
	wpabuf_free(conf->c_sign_key);
	conf->c_sign_key = wpabuf_alloc_copy(der, der_len);
	OPENSSL_free(der);
}


static void dpp_copy_netaccesskey(struct dpp_authentication *auth,
				  struct dpp_config_obj *conf)
{
	unsigned char *der = NULL;
	int der_len;
	EC_KEY *eckey;

	eckey = EVP_PKEY_get1_EC_KEY(auth->own_protocol_key);
	if (!eckey)
		return;

	der_len = i2d_ECPrivateKey(eckey, &der);
	if (der_len <= 0) {
		EC_KEY_free(eckey);
		return;
	}
	wpabuf_free(auth->net_access_key);
	auth->net_access_key = wpabuf_alloc_copy(der, der_len);
	OPENSSL_free(der);
	EC_KEY_free(eckey);
}


static int dpp_parse_cred_dpp(struct dpp_authentication *auth,
			      struct dpp_config_obj *conf,
			      struct json_token *cred)
{
	struct dpp_signed_connector_info info;
	struct json_token *token, *csign;
	int ret = -1;
	EVP_PKEY *csign_pub = NULL;
	const struct dpp_curve_params *key_curve = NULL;
	const char *signed_connector;

	os_memset(&info, 0, sizeof(info));

	if (dpp_akm_psk(conf->akm) || dpp_akm_sae(conf->akm)) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Legacy credential included in Connector credential");
		if (dpp_parse_cred_legacy(conf, cred) < 0)
			return -1;
	}

	wpa_printf(MSG_DEBUG, "DPP: Connector credential");

	csign = json_get_member(cred, "csign");
	if (!csign || csign->type != JSON_OBJECT) {
		wpa_printf(MSG_DEBUG, "DPP: No csign JWK in JSON");
		goto fail;
	}

	csign_pub = dpp_parse_jwk(csign, &key_curve);
	if (!csign_pub) {
		wpa_printf(MSG_DEBUG, "DPP: Failed to parse csign JWK");
		goto fail;
	}
	dpp_debug_print_key("DPP: Received C-sign-key", csign_pub);

	token = json_get_member(cred, "signedConnector");
	if (!token || token->type != JSON_STRING) {
		wpa_printf(MSG_DEBUG, "DPP: No signedConnector string found");
		goto fail;
	}
	wpa_hexdump_ascii(MSG_DEBUG, "DPP: signedConnector",
			  token->string, os_strlen(token->string));
	signed_connector = token->string;

	if (os_strchr(signed_connector, '"') ||
	    os_strchr(signed_connector, '\n')) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Unexpected character in signedConnector");
		goto fail;
	}

	if (dpp_process_signed_connector(&info, csign_pub,
					 signed_connector) != DPP_STATUS_OK)
		goto fail;

	if (dpp_parse_connector(auth, conf,
				info.payload, info.payload_len) < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Failed to parse connector");
		goto fail;
	}

	os_free(conf->connector);
	conf->connector = os_strdup(signed_connector);

	dpp_copy_csign(conf, csign_pub);
	if (dpp_akm_dpp(conf->akm) || auth->peer_version >= 2)
		dpp_copy_netaccesskey(auth, conf);

	ret = 0;
fail:
	EVP_PKEY_free(csign_pub);
	os_free(info.payload);
	return ret;
}


const char * dpp_akm_str(enum dpp_akm akm)
{
	switch (akm) {
	case DPP_AKM_DPP:
		return "dpp";
	case DPP_AKM_PSK:
		return "psk";
	case DPP_AKM_SAE:
		return "sae";
	case DPP_AKM_PSK_SAE:
		return "psk+sae";
	case DPP_AKM_SAE_DPP:
		return "dpp+sae";
	case DPP_AKM_PSK_SAE_DPP:
		return "dpp+psk+sae";
	default:
		return "??";
	}
}


const char * dpp_akm_selector_str(enum dpp_akm akm)
{
	switch (akm) {
	case DPP_AKM_DPP:
		return "506F9A02";
	case DPP_AKM_PSK:
		return "000FAC02+000FAC06";
	case DPP_AKM_SAE:
		return "000FAC08";
	case DPP_AKM_PSK_SAE:
		return "000FAC02+000FAC06+000FAC08";
	case DPP_AKM_SAE_DPP:
		return "506F9A02+000FAC08";
	case DPP_AKM_PSK_SAE_DPP:
		return "506F9A02+000FAC08+000FAC02+000FAC06";
	default:
		return "??";
	}
}


static enum dpp_akm dpp_akm_from_str(const char *akm)
{
	const char *pos;
	int dpp = 0, psk = 0, sae = 0;

	if (os_strcmp(akm, "psk") == 0)
		return DPP_AKM_PSK;
	if (os_strcmp(akm, "sae") == 0)
		return DPP_AKM_SAE;
	if (os_strcmp(akm, "psk+sae") == 0)
		return DPP_AKM_PSK_SAE;
	if (os_strcmp(akm, "dpp") == 0)
		return DPP_AKM_DPP;
	if (os_strcmp(akm, "dpp+sae") == 0)
		return DPP_AKM_SAE_DPP;
	if (os_strcmp(akm, "dpp+psk+sae") == 0)
		return DPP_AKM_PSK_SAE_DPP;

	pos = akm;
	while (*pos) {
		if (os_strlen(pos) < 8)
			break;
		if (os_strncasecmp(pos, "506F9A02", 8) == 0)
			dpp = 1;
		else if (os_strncasecmp(pos, "000FAC02", 8) == 0)
			psk = 1;
		else if (os_strncasecmp(pos, "000FAC06", 8) == 0)
			psk = 1;
		else if (os_strncasecmp(pos, "000FAC08", 8) == 0)
			sae = 1;
		pos += 8;
		if (*pos != '+')
			break;
		pos++;
	}

	if (dpp && psk && sae)
		return DPP_AKM_PSK_SAE_DPP;
	if (dpp && sae)
		return DPP_AKM_SAE_DPP;
	if (dpp)
		return DPP_AKM_DPP;
	if (psk && sae)
		return DPP_AKM_PSK_SAE;
	if (sae)
		return DPP_AKM_SAE;
	if (psk)
		return DPP_AKM_PSK;

	return DPP_AKM_UNKNOWN;
}


static int dpp_parse_conf_obj(struct dpp_authentication *auth,
			      const u8 *conf_obj, u16 conf_obj_len)
{
	int ret = -1;
	struct json_token *root, *token, *discovery, *cred;
	struct dpp_config_obj *conf;
	struct wpabuf *ssid64 = NULL;
	int legacy;

	root = json_parse((const char *) conf_obj, conf_obj_len);
	if (!root)
		return -1;
	if (root->type != JSON_OBJECT) {
		dpp_auth_fail(auth, "JSON root is not an object");
		goto fail;
	}

	token = json_get_member(root, "wi-fi_tech");
	if (!token || token->type != JSON_STRING) {
		dpp_auth_fail(auth, "No wi-fi_tech string value found");
		goto fail;
	}
	if (os_strcmp(token->string, "infra") != 0) {
		wpa_printf(MSG_DEBUG, "DPP: Unsupported wi-fi_tech value: '%s'",
			   token->string);
		dpp_auth_fail(auth, "Unsupported wi-fi_tech value");
		goto fail;
	}

	discovery = json_get_member(root, "discovery");
	if (!discovery || discovery->type != JSON_OBJECT) {
		dpp_auth_fail(auth, "No discovery object in JSON");
		goto fail;
	}

	ssid64 = json_get_member_base64url(discovery, "ssid64");
	if (ssid64) {
		wpa_hexdump_ascii(MSG_DEBUG, "DPP: discovery::ssid64",
				  wpabuf_head(ssid64), wpabuf_len(ssid64));
		if (wpabuf_len(ssid64) > SSID_MAX_LEN) {
			dpp_auth_fail(auth, "Too long discovery::ssid64 value");
			goto fail;
		}
	} else {
		token = json_get_member(discovery, "ssid");
		if (!token || token->type != JSON_STRING) {
			dpp_auth_fail(auth,
				      "No discovery::ssid string value found");
			goto fail;
		}
		wpa_hexdump_ascii(MSG_DEBUG, "DPP: discovery::ssid",
				  token->string, os_strlen(token->string));
		if (os_strlen(token->string) > SSID_MAX_LEN) {
			dpp_auth_fail(auth,
				      "Too long discovery::ssid string value");
			goto fail;
		}
	}

	if (auth->num_conf_obj == DPP_MAX_CONF_OBJ) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No room for this many Config Objects - ignore this one");
		ret = 0;
		goto fail;
	}
	conf = &auth->conf_obj[auth->num_conf_obj++];

	if (ssid64) {
		conf->ssid_len = wpabuf_len(ssid64);
		os_memcpy(conf->ssid, wpabuf_head(ssid64), conf->ssid_len);
	} else {
		conf->ssid_len = os_strlen(token->string);
		os_memcpy(conf->ssid, token->string, conf->ssid_len);
	}

	token = json_get_member(discovery, "ssid_charset");
	if (token && token->type == JSON_NUMBER) {
		conf->ssid_charset = token->number;
		wpa_printf(MSG_DEBUG, "DPP: ssid_charset=%d",
			   conf->ssid_charset);
	}

	cred = json_get_member(root, "cred");
	if (!cred || cred->type != JSON_OBJECT) {
		dpp_auth_fail(auth, "No cred object in JSON");
		goto fail;
	}

	token = json_get_member(cred, "akm");
	if (!token || token->type != JSON_STRING) {
		dpp_auth_fail(auth, "No cred::akm string value found");
		goto fail;
	}
	conf->akm = dpp_akm_from_str(token->string);

	legacy = dpp_akm_legacy(conf->akm);
	if (legacy && auth->peer_version >= 2) {
		struct json_token *csign, *s_conn;

		csign = json_get_member(cred, "csign");
		s_conn = json_get_member(cred, "signedConnector");
		if (csign && csign->type == JSON_OBJECT &&
		    s_conn && s_conn->type == JSON_STRING)
			legacy = 0;
	}
	if (legacy) {
		if (dpp_parse_cred_legacy(conf, cred) < 0)
			goto fail;
	} else if (dpp_akm_dpp(conf->akm) ||
		   (auth->peer_version >= 2 && dpp_akm_legacy(conf->akm))) {
		if (dpp_parse_cred_dpp(auth, conf, cred) < 0)
			goto fail;
	} else {
		wpa_printf(MSG_DEBUG, "DPP: Unsupported akm: %s",
			   token->string);
		dpp_auth_fail(auth, "Unsupported akm");
		goto fail;
	}

	wpa_printf(MSG_DEBUG, "DPP: JSON parsing completed successfully");
	ret = 0;
fail:
	wpabuf_free(ssid64);
	json_free(root);
	return ret;
}


#ifdef CONFIG_DPP2

struct dpp_enveloped_data {
	const u8 *enc_cont;
	size_t enc_cont_len;
	const u8 *enc_key;
	size_t enc_key_len;
	const u8 *salt;
	size_t pbkdf2_key_len;
	size_t prf_hash_len;
};


static int dpp_parse_recipient_infos(const u8 *pos, size_t len,
				     struct dpp_enveloped_data *data)
{
	struct asn1_hdr hdr;
	const u8 *end = pos + len;
	const u8 *next, *e_end;
	struct asn1_oid oid;
	int val;
	const u8 *params;
	size_t params_len;

	wpa_hexdump(MSG_MSGDUMP, "DPP: RecipientInfos", pos, len);

	/*
	 * RecipientInfo ::= CHOICE {
	 *    ktri		KeyTransRecipientInfo,
	 *    kari	[1]	KeyAgreeRecipientInfo,
	 *    kekri	[2]	KEKRecipientInfo,
	 *    pwri	[3]	PasswordRecipientInfo,
	 *    ori	[4]	OtherRecipientInfo}
	 *
	 * Shall always use the pwri CHOICE.
	 */

	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_CONTEXT_SPECIFIC || hdr.tag != 3) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected CHOICE [3] (pwri) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		return -1;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: PasswordRecipientInfo",
		    hdr.payload, hdr.length);
	pos = hdr.payload;
	end = pos + hdr.length;

	/*
	 * PasswordRecipientInfo ::= SEQUENCE {
	 *    version			CMSVersion,
	 *    keyDerivationAlgorithm [0] KeyDerivationAlgorithmIdentifier OPTIONAL,
	 *    keyEncryptionAlgorithm	KeyEncryptionAlgorithmIdentifier,
	 *    encryptedKey		EncryptedKey}
	 *
	 * version is 0, keyDerivationAlgorithm is id-PKBDF2, and the
	 * parameters contains PBKDF2-params SEQUENCE.
	 */

	if (asn1_get_sequence(pos, end - pos, &hdr, &end) < 0)
		return -1;
	pos = hdr.payload;

	if (asn1_get_integer(pos, end - pos, &val, &pos) < 0)
		return -1;
	if (val != 0) {
		wpa_printf(MSG_DEBUG, "DPP: pwri.version != 0");
		return -1;
	}

	wpa_hexdump(MSG_MSGDUMP, "DPP: Remaining PasswordRecipientInfo after version",
		    pos, end - pos);

	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_CONTEXT_SPECIFIC || hdr.tag != 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected keyDerivationAlgorithm [0] - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		return -1;
	}
	pos = hdr.payload;
	e_end = pos + hdr.length;

	/* KeyDerivationAlgorithmIdentifier ::= AlgorithmIdentifier */
	if (asn1_get_alg_id(pos, e_end - pos, &oid, &params, &params_len,
			    &next) < 0)
		return -1;
	if (!asn1_oid_equal(&oid, &asn1_pbkdf2_oid)) {
		char buf[80];

		asn1_oid_to_str(&oid, buf, sizeof(buf));
		wpa_printf(MSG_DEBUG,
			   "DPP: Unexpected KeyDerivationAlgorithmIdentifier %s",
			   buf);
		return -1;
	}

	/*
	 * PBKDF2-params ::= SEQUENCE {
	 *    salt CHOICE {
	 *       specified OCTET STRING,
	 *       otherSource AlgorithmIdentifier}
	 *    iterationCount INTEGER (1..MAX),
	 *    keyLength INTEGER (1..MAX),
	 *    prf AlgorithmIdentifier}
	 *
	 * salt is an 64 octet value, iterationCount is 1000, keyLength is based
	 * on Configurator signing key length, prf is
	 * id-hmacWithSHA{256,384,512} based on Configurator signing key.
	 */
	if (!params ||
	    asn1_get_sequence(params, params_len, &hdr, &e_end) < 0)
		return -1;
	pos = hdr.payload;

	if (asn1_get_next(pos, e_end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_UNIVERSAL ||
	    hdr.tag != ASN1_TAG_OCTETSTRING) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected OCTETSTRING (salt.specified) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		return -1;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: salt.specified",
		    hdr.payload, hdr.length);
	if (hdr.length != 64) {
		wpa_printf(MSG_DEBUG, "DPP: Unexpected salt length %u",
			   hdr.length);
		return -1;
	}
	data->salt = hdr.payload;
	pos = hdr.payload + hdr.length;

	if (asn1_get_integer(pos, e_end - pos, &val, &pos) < 0)
		return -1;
	if (val != 1000) {
		wpa_printf(MSG_DEBUG, "DPP: Unexpected iterationCount %d", val);
		return -1;
	}

	if (asn1_get_integer(pos, e_end - pos, &val, &pos) < 0)
		return -1;
	if (val != 32 && val != 48 && val != 64) {
		wpa_printf(MSG_DEBUG, "DPP: Unexpected keyLength %d", val);
		return -1;
	}
	data->pbkdf2_key_len = val;

	if (asn1_get_sequence(pos, e_end - pos, &hdr, NULL) < 0 ||
	    asn1_get_oid(hdr.payload, hdr.length, &oid, &pos) < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Could not parse prf");
		return -1;
	}
	if (asn1_oid_equal(&oid, &asn1_pbkdf2_hmac_sha256_oid)) {
		data->prf_hash_len = 32;
	} else if (asn1_oid_equal(&oid, &asn1_pbkdf2_hmac_sha384_oid)) {
		data->prf_hash_len = 48;
	} else if (asn1_oid_equal(&oid, &asn1_pbkdf2_hmac_sha512_oid)) {
		data->prf_hash_len = 64;
	} else {
		char buf[80];

		asn1_oid_to_str(&oid, buf, sizeof(buf));
		wpa_printf(MSG_DEBUG, "DPP: Unexpected PBKDF2-params.prf %s",
			   buf);
		return -1;
	}

	pos = next;

	/* keyEncryptionAlgorithm KeyEncryptionAlgorithmIdentifier
	 *
	 * KeyEncryptionAlgorithmIdentifier ::= AlgorithmIdentifier
	 *
	 * id-alg-AES-SIV-CMAC-aed-256, id-alg-AES-SIV-CMAC-aed-384, or
	 * id-alg-AES-SIV-CMAC-aed-512. */
	if (asn1_get_alg_id(pos, end - pos, &oid, NULL, NULL, &pos) < 0)
		return -1;
	if (!asn1_oid_equal(&oid, &asn1_aes_siv_cmac_aead_256_oid) &&
	    !asn1_oid_equal(&oid, &asn1_aes_siv_cmac_aead_384_oid) &&
	    !asn1_oid_equal(&oid, &asn1_aes_siv_cmac_aead_512_oid)) {
		char buf[80];

		asn1_oid_to_str(&oid, buf, sizeof(buf));
		wpa_printf(MSG_DEBUG,
			   "DPP: Unexpected KeyEncryptionAlgorithmIdentifier %s",
			   buf);
		return -1;
	}

	/*
	 * encryptedKey EncryptedKey
	 *
	 * EncryptedKey ::= OCTET STRING
	 */
	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_UNIVERSAL ||
	    hdr.tag != ASN1_TAG_OCTETSTRING) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected OCTETSTRING (pwri.encryptedKey) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		return -1;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: pwri.encryptedKey",
		    hdr.payload, hdr.length);
	data->enc_key = hdr.payload;
	data->enc_key_len = hdr.length;

	return 0;
}


static int dpp_parse_encrypted_content_info(const u8 *pos, const u8 *end,
					    struct dpp_enveloped_data *data)
{
	struct asn1_hdr hdr;
	struct asn1_oid oid;

	/*
	 * EncryptedContentInfo ::= SEQUENCE {
	 *    contentType			ContentType,
	 *    contentEncryptionAlgorithm  ContentEncryptionAlgorithmIdentifier,
	 *    encryptedContent	[0] IMPLICIT	EncryptedContent OPTIONAL}
	 */
	if (asn1_get_sequence(pos, end - pos, &hdr, &pos) < 0)
		return -1;
	wpa_hexdump(MSG_MSGDUMP, "DPP: EncryptedContentInfo",
		    hdr.payload, hdr.length);
	if (pos < end) {
		wpa_hexdump(MSG_DEBUG,
			    "DPP: Unexpected extra data after EncryptedContentInfo",
			    pos, end - pos);
		return -1;
	}

	end = pos;
	pos = hdr.payload;

	/* ContentType ::= OBJECT IDENTIFIER */
	if (asn1_get_oid(pos, end - pos, &oid, &pos) < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Could not parse ContentType");
		return -1;
	}
	if (!asn1_oid_equal(&oid, &asn1_dpp_asymmetric_key_package_oid)) {
		char buf[80];

		asn1_oid_to_str(&oid, buf, sizeof(buf));
		wpa_printf(MSG_DEBUG, "DPP: Unexpected ContentType %s", buf);
		return -1;
	}

	/* ContentEncryptionAlgorithmIdentifier ::= AlgorithmIdentifier */
	if (asn1_get_alg_id(pos, end - pos, &oid, NULL, NULL, &pos) < 0)
		return -1;
	if (!asn1_oid_equal(&oid, &asn1_aes_siv_cmac_aead_256_oid) &&
	    !asn1_oid_equal(&oid, &asn1_aes_siv_cmac_aead_384_oid) &&
	    !asn1_oid_equal(&oid, &asn1_aes_siv_cmac_aead_512_oid)) {
		char buf[80];

		asn1_oid_to_str(&oid, buf, sizeof(buf));
		wpa_printf(MSG_DEBUG,
			   "DPP: Unexpected ContentEncryptionAlgorithmIdentifier %s",
			   buf);
		return -1;
	}
	/* ignore optional parameters */

	/* encryptedContent [0] IMPLICIT EncryptedContent OPTIONAL
	 * EncryptedContent ::= OCTET STRING */
	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_CONTEXT_SPECIFIC || hdr.tag != 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected [0] IMPLICIT (EncryptedContent) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		return -1;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: EncryptedContent",
		    hdr.payload, hdr.length);
	data->enc_cont = hdr.payload;
	data->enc_cont_len = hdr.length;
	return 0;
}


static int dpp_parse_enveloped_data(const u8 *env_data, size_t env_data_len,
				    struct dpp_enveloped_data *data)
{
	struct asn1_hdr hdr;
	const u8 *pos, *end;
	int val;

	os_memset(data, 0, sizeof(*data));

	/*
	 * DPPEnvelopedData ::= EnvelopedData
	 *
	 * EnvelopedData ::= SEQUENCE {
	 *    version			CMSVersion,
	 *    originatorInfo	[0]	IMPLICIT OriginatorInfo OPTIONAL,
	 *    recipientInfos		RecipientInfos,
	 *    encryptedContentInfo	EncryptedContentInfo,
	 *    unprotectedAttrs  [1] IMPLICIT	UnprotectedAttributes OPTIONAL}
	 *
	 * CMSVersion ::= INTEGER
	 *
	 * RecipientInfos ::= SET SIZE (1..MAX) OF RecipientInfo
	 *
	 * For DPP, version is 3, both originatorInfo and
	 * unprotectedAttrs are omitted, and recipientInfos contains a single
	 * RecipientInfo.
	 */
	if (asn1_get_sequence(env_data, env_data_len, &hdr, &end) < 0)
		return -1;
	pos = hdr.payload;
	if (end < env_data + env_data_len) {
		wpa_hexdump(MSG_DEBUG,
			    "DPP: Unexpected extra data after DPPEnvelopedData",
			    end, env_data + env_data_len - end);
		return -1;
	}

	if (asn1_get_integer(pos, end - pos, &val, &pos) < 0)
		return -1;
	if (val != 3) {
		wpa_printf(MSG_DEBUG, "DPP: EnvelopedData.version != 3");
		return -1;
	}

	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_UNIVERSAL || hdr.tag != ASN1_TAG_SET) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected SET (RecipientInfos) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		return -1;
	}

	if (dpp_parse_recipient_infos(hdr.payload, hdr.length, data) < 0)
		return -1;
	return dpp_parse_encrypted_content_info(hdr.payload + hdr.length, end,
						data);
}


static struct dpp_asymmetric_key *
dpp_parse_one_asymmetric_key(const u8 *buf, size_t len)
{
	struct asn1_hdr hdr;
	const u8 *pos = buf, *end = buf + len, *next;
	int val;
	const u8 *params;
	size_t params_len;
	struct asn1_oid oid;
	char txt[80];
	struct dpp_asymmetric_key *key;
	EC_KEY *eckey;

	wpa_hexdump_key(MSG_MSGDUMP, "DPP: OneAsymmetricKey", buf, len);

	key = os_zalloc(sizeof(*key));
	if (!key)
		return NULL;

	/*
	 * OneAsymmetricKey ::= SEQUENCE {
	 *    version			Version,
	 *    privateKeyAlgorithm	PrivateKeyAlgorithmIdentifier,
	 *    privateKey		PrivateKey,
	 *    attributes		[0] Attributes OPTIONAL,
	 *    ...,
	 *    [[2: publicKey		[1] BIT STRING OPTIONAL ]],
	 *    ...
	 * }
	 */
	if (asn1_get_sequence(pos, end - pos, &hdr, &end) < 0)
		goto fail;
	pos = hdr.payload;

	/* Version ::= INTEGER { v1(0), v2(1) } (v1, ..., v2) */
	if (asn1_get_integer(pos, end - pos, &val, &pos) < 0)
		goto fail;
	if (val != 1) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Unsupported DPPAsymmetricKeyPackage version %d",
			   val);
		goto fail;
	}

	/* PrivateKeyAlgorithmIdentifier ::= AlgorithmIdentifier */
	if (asn1_get_alg_id(pos, end - pos, &oid, &params, &params_len,
			    &pos) < 0)
		goto fail;
	if (!asn1_oid_equal(&oid, &asn1_ec_public_key_oid)) {
		asn1_oid_to_str(&oid, txt, sizeof(txt));
		wpa_printf(MSG_DEBUG,
			   "DPP: Unsupported PrivateKeyAlgorithmIdentifier %s",
			   txt);
		goto fail;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: PrivateKeyAlgorithmIdentifier params",
		    params, params_len);
	/*
	 * ECParameters ::= CHOICE {
	 *    namedCurve	OBJECT IDENTIFIER
	 *    -- implicitCurve	NULL
	 *    -- specifiedCurve	SpecifiedECDomain}
	 */
	if (!params || asn1_get_oid(params, params_len, &oid, &next) < 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Could not parse ECParameters.namedCurve");
		goto fail;
	}
	asn1_oid_to_str(&oid, txt, sizeof(txt));
	wpa_printf(MSG_MSGDUMP, "DPP: namedCurve %s", txt);
	/* Assume the curve is identified within ECPrivateKey, so that this
	 * separate indication is not really needed. */

	/*
	 * PrivateKey ::= OCTET STRING
	 *    (Contains DER encoding of ECPrivateKey)
	 */
	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_UNIVERSAL ||
	    hdr.tag != ASN1_TAG_OCTETSTRING) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected OCTETSTRING (PrivateKey) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		goto fail;
	}
	wpa_hexdump_key(MSG_MSGDUMP, "DPP: PrivateKey",
			hdr.payload, hdr.length);
	pos = hdr.payload + hdr.length;
	eckey = d2i_ECPrivateKey(NULL, &hdr.payload, hdr.length);
	if (!eckey) {
		wpa_printf(MSG_INFO,
			   "DPP: OpenSSL: d2i_ECPrivateKey() failed: %s",
			   ERR_error_string(ERR_get_error(), NULL));
		goto fail;
	}
	key->csign = EVP_PKEY_new();
	if (!key->csign || EVP_PKEY_assign_EC_KEY(key->csign, eckey) != 1) {
		EC_KEY_free(eckey);
		goto fail;
	}
	if (wpa_debug_show_keys)
		dpp_debug_print_key("DPP: Received c-sign-key", key->csign);

	/*
	 * Attributes ::= SET OF Attribute { { OneAsymmetricKeyAttributes } }
	 *
	 * Exactly one instance of type Attribute in OneAsymmetricKey.
	 */
	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_CONTEXT_SPECIFIC || hdr.tag != 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected [0] Attributes - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		goto fail;
	}
	wpa_hexdump_key(MSG_MSGDUMP, "DPP: Attributes",
			hdr.payload, hdr.length);
	if (hdr.payload + hdr.length < end) {
		wpa_hexdump_key(MSG_MSGDUMP,
				"DPP: Ignore additional data at the end of OneAsymmetricKey",
				hdr.payload + hdr.length,
				end - (hdr.payload + hdr.length));
	}
	pos = hdr.payload;
	end = hdr.payload + hdr.length;

	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_UNIVERSAL || hdr.tag != ASN1_TAG_SET) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected SET (Attributes) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		goto fail;
	}
	if (hdr.payload + hdr.length < end) {
		wpa_hexdump_key(MSG_MSGDUMP,
				"DPP: Ignore additional data at the end of OneAsymmetricKey (after SET)",
				hdr.payload + hdr.length,
				end - (hdr.payload + hdr.length));
	}
	pos = hdr.payload;
	end = hdr.payload + hdr.length;

	/*
	 * OneAsymmetricKeyAttributes ATTRIBUTE ::= {
	 *    aa-DPPConfigurationParameters,
	 *    ... -- For local profiles
	 * }
	 *
	 * aa-DPPConfigurationParameters ATTRIBUTE ::=
	 * { TYPE DPPConfigurationParameters IDENTIFIED BY id-DPPConfigParams }
	 *
	 * Attribute ::= SEQUENCE {
	 *    type OBJECT IDENTIFIER,
	 *    values SET SIZE(1..MAX) OF Type
	 *
	 * Exactly one instance of ATTRIBUTE in attrValues.
	 */
	if (asn1_get_sequence(pos, end - pos, &hdr, &pos) < 0)
		goto fail;
	if (pos < end) {
		wpa_hexdump_key(MSG_MSGDUMP,
				"DPP: Ignore additional data at the end of ATTRIBUTE",
				pos, end - pos);
	}
	end = pos;
	pos = hdr.payload;

	if (asn1_get_oid(pos, end - pos, &oid, &pos) < 0)
		goto fail;
	if (!asn1_oid_equal(&oid, &asn1_dpp_config_params_oid)) {
		asn1_oid_to_str(&oid, txt, sizeof(txt));
		wpa_printf(MSG_DEBUG,
			   "DPP: Unexpected Attribute identifier %s", txt);
		goto fail;
	}

	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_UNIVERSAL || hdr.tag != ASN1_TAG_SET) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected SET (Attribute) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		goto fail;
	}
	pos = hdr.payload;
	end = hdr.payload + hdr.length;

	/*
	 * DPPConfigurationParameters ::= SEQUENCE {
	 *    configurationTemplate	UTF8String,
	 *    connectorTemplate		UTF8String OPTIONAL}
	 */

	wpa_hexdump_key(MSG_MSGDUMP, "DPP: DPPConfigurationParameters",
			pos, end - pos);
	if (asn1_get_sequence(pos, end - pos, &hdr, &pos) < 0)
		goto fail;
	if (pos < end) {
		wpa_hexdump_key(MSG_MSGDUMP,
				"DPP: Ignore additional data after DPPConfigurationParameters",
				pos, end - pos);
	}
	end = pos;
	pos = hdr.payload;

	if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
	    hdr.class != ASN1_CLASS_UNIVERSAL ||
	    hdr.tag != ASN1_TAG_UTF8STRING) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Expected UTF8STRING (configurationTemplate) - found class %d tag 0x%x",
			   hdr.class, hdr.tag);
		goto fail;
	}
	wpa_hexdump_ascii_key(MSG_MSGDUMP, "DPP: configurationTemplate",
			      hdr.payload, hdr.length);
	key->config_template = os_zalloc(hdr.length + 1);
	if (!key->config_template)
		goto fail;
	os_memcpy(key->config_template, hdr.payload, hdr.length);

	pos = hdr.payload + hdr.length;

	if (pos < end) {
		if (asn1_get_next(pos, end - pos, &hdr) < 0 ||
		    hdr.class != ASN1_CLASS_UNIVERSAL ||
		    hdr.tag != ASN1_TAG_UTF8STRING) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Expected UTF8STRING (connectorTemplate) - found class %d tag 0x%x",
				   hdr.class, hdr.tag);
			goto fail;
		}
		wpa_hexdump_ascii_key(MSG_MSGDUMP, "DPP: connectorTemplate",
				      hdr.payload, hdr.length);
		key->connector_template = os_zalloc(hdr.length + 1);
		if (!key->connector_template)
			goto fail;
		os_memcpy(key->connector_template, hdr.payload, hdr.length);
	}

	return key;
fail:
	wpa_printf(MSG_DEBUG, "DPP: Failed to parse OneAsymmetricKey");
	dpp_free_asymmetric_key(key);
	return NULL;
}


static struct dpp_asymmetric_key *
dpp_parse_dpp_asymmetric_key_package(const u8 *key_pkg, size_t key_pkg_len)
{
	struct asn1_hdr hdr;
	const u8 *pos = key_pkg, *end = key_pkg + key_pkg_len;
	struct dpp_asymmetric_key *first = NULL, *last = NULL, *key;

	wpa_hexdump_key(MSG_MSGDUMP, "DPP: DPPAsymmetricKeyPackage",
			key_pkg, key_pkg_len);

	/*
	 * DPPAsymmetricKeyPackage ::= AsymmetricKeyPackage
	 *
	 * AsymmetricKeyPackage ::= SEQUENCE SIZE (1..MAX) OF OneAsymmetricKey
	 */
	while (pos < end) {
		if (asn1_get_sequence(pos, end - pos, &hdr, &pos) < 0 ||
		    !(key = dpp_parse_one_asymmetric_key(hdr.payload,
							 hdr.length))) {
			dpp_free_asymmetric_key(first);
			return NULL;
		}
		if (!last) {
			first = last = key;
		} else {
			last->next = key;
			last = key;
		}
	}

	return first;
}


static int dpp_conf_resp_env_data(struct dpp_authentication *auth,
				  const u8 *env_data, size_t env_data_len)
{
	const u8 *key;
	size_t key_len;
	u8 kek[DPP_MAX_HASH_LEN];
	u8 cont_encr_key[DPP_MAX_HASH_LEN];
	size_t cont_encr_key_len;
	int res;
	u8 *key_pkg;
	size_t key_pkg_len;
	struct dpp_enveloped_data data;
	struct dpp_asymmetric_key *keys;

	wpa_hexdump(MSG_DEBUG, "DPP: DPPEnvelopedData", env_data, env_data_len);

	if (dpp_parse_enveloped_data(env_data, env_data_len, &data) < 0)
		return -1;

	/* TODO: For initial testing, use ke as the key. Replace this with a
	 * new key once that has been defined. */
	key = auth->ke;
	key_len = auth->curve->hash_len;
	wpa_hexdump_key(MSG_DEBUG, "DPP: PBKDF2 key", key, key_len);

	if (dpp_pbkdf2(data.prf_hash_len, key, key_len, data.salt, 64, 1000,
		       kek, data.pbkdf2_key_len)) {
		wpa_printf(MSG_DEBUG, "DPP: PBKDF2 failed");
		return -1;
	}
	wpa_hexdump_key(MSG_DEBUG, "DPP: key-encryption key from PBKDF2",
			kek, data.pbkdf2_key_len);

	if (data.enc_key_len < AES_BLOCK_SIZE ||
	    data.enc_key_len > sizeof(cont_encr_key) + AES_BLOCK_SIZE) {
		wpa_printf(MSG_DEBUG, "DPP: Invalid encryptedKey length");
		return -1;
	}
	res = aes_siv_decrypt(kek, data.pbkdf2_key_len,
			      data.enc_key, data.enc_key_len,
			      0, NULL, NULL, cont_encr_key);
	forced_memzero(kek, data.pbkdf2_key_len);
	if (res < 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: AES-SIV decryption of encryptedKey failed");
		return -1;
	}
	cont_encr_key_len = data.enc_key_len - AES_BLOCK_SIZE;
	wpa_hexdump_key(MSG_DEBUG, "DPP: content-encryption key",
			cont_encr_key, cont_encr_key_len);

	if (data.enc_cont_len < AES_BLOCK_SIZE)
		return -1;
	key_pkg_len = data.enc_cont_len - AES_BLOCK_SIZE;
	key_pkg = os_malloc(key_pkg_len);
	if (!key_pkg)
		return -1;
	res = aes_siv_decrypt(cont_encr_key, cont_encr_key_len,
			      data.enc_cont, data.enc_cont_len,
			      0, NULL, NULL, key_pkg);
	forced_memzero(cont_encr_key, cont_encr_key_len);
	if (res < 0) {
		bin_clear_free(key_pkg, key_pkg_len);
		wpa_printf(MSG_DEBUG,
			   "DPP: AES-SIV decryption of encryptedContent failed");
		return -1;
	}

	keys = dpp_parse_dpp_asymmetric_key_package(key_pkg, key_pkg_len);
	bin_clear_free(key_pkg, key_pkg_len);
	dpp_free_asymmetric_key(auth->conf_key_pkg);
	auth->conf_key_pkg = keys;

	return keys != NULL;;
}

#endif /* CONFIG_DPP2 */


int dpp_conf_resp_rx(struct dpp_authentication *auth,
		     const struct wpabuf *resp)
{
	const u8 *wrapped_data, *e_nonce, *status, *conf_obj;
	u16 wrapped_data_len, e_nonce_len, status_len, conf_obj_len;
	const u8 *env_data;
	u16 env_data_len;
	const u8 *addr[1];
	size_t len[1];
	u8 *unwrapped = NULL;
	size_t unwrapped_len = 0;
	int ret = -1;

	auth->conf_resp_status = 255;

	if (dpp_check_attrs(wpabuf_head(resp), wpabuf_len(resp)) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in config response");
		return -1;
	}

	wrapped_data = dpp_get_attr(wpabuf_head(resp), wpabuf_len(resp),
				    DPP_ATTR_WRAPPED_DATA,
				    &wrapped_data_len);
	if (!wrapped_data || wrapped_data_len < AES_BLOCK_SIZE) {
		dpp_auth_fail(auth,
			      "Missing or invalid required Wrapped Data attribute");
		return -1;
	}

	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped)
		return -1;

	addr[0] = wpabuf_head(resp);
	len[0] = wrapped_data - 4 - (const u8 *) wpabuf_head(resp);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD", addr[0], len[0]);

	if (aes_siv_decrypt(auth->ke, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    1, addr, len, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	e_nonce = dpp_get_attr(unwrapped, unwrapped_len,
			       DPP_ATTR_ENROLLEE_NONCE,
			       &e_nonce_len);
	if (!e_nonce || e_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth,
			      "Missing or invalid Enrollee Nonce attribute");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Enrollee Nonce", e_nonce, e_nonce_len);
	if (os_memcmp(e_nonce, auth->e_nonce, e_nonce_len) != 0) {
		dpp_auth_fail(auth, "Enrollee Nonce mismatch");
		goto fail;
	}

	status = dpp_get_attr(wpabuf_head(resp), wpabuf_len(resp),
			      DPP_ATTR_STATUS, &status_len);
	if (!status || status_len < 1) {
		dpp_auth_fail(auth,
			      "Missing or invalid required DPP Status attribute");
		goto fail;
	}
	auth->conf_resp_status = status[0];
	wpa_printf(MSG_DEBUG, "DPP: Status %u", status[0]);
	if (status[0] != DPP_STATUS_OK) {
		dpp_auth_fail(auth, "Configurator rejected configuration");
		goto fail;
	}

	env_data = dpp_get_attr(unwrapped, unwrapped_len,
				DPP_ATTR_ENVELOPED_DATA, &env_data_len);
#ifdef CONFIG_DPP2
	if (env_data &&
	    dpp_conf_resp_env_data(auth, env_data, env_data_len) < 0)
		goto fail;
#endif /* CONFIG_DPP2 */

	conf_obj = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_CONFIG_OBJ,
				&conf_obj_len);
	if (!conf_obj && !env_data) {
		dpp_auth_fail(auth,
			      "Missing required Configuration Object attribute");
		goto fail;
	}
	while (conf_obj) {
		wpa_hexdump_ascii(MSG_DEBUG, "DPP: configurationObject JSON",
				  conf_obj, conf_obj_len);
		if (dpp_parse_conf_obj(auth, conf_obj, conf_obj_len) < 0)
			goto fail;
		conf_obj = dpp_get_attr_next(conf_obj, unwrapped, unwrapped_len,
					     DPP_ATTR_CONFIG_OBJ,
					     &conf_obj_len);
	}

#ifdef CONFIG_DPP2
	status = dpp_get_attr(unwrapped, unwrapped_len,
			      DPP_ATTR_SEND_CONN_STATUS, &status_len);
	if (status) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Configurator requested connection status result");
		auth->conn_status_requested = 1;
	}
#endif /* CONFIG_DPP2 */

	ret = 0;

fail:
	os_free(unwrapped);
	return ret;
}


#ifdef CONFIG_DPP2

enum dpp_status_error dpp_conf_result_rx(struct dpp_authentication *auth,
					 const u8 *hdr,
					 const u8 *attr_start, size_t attr_len)
{
	const u8 *wrapped_data, *status, *e_nonce;
	u16 wrapped_data_len, status_len, e_nonce_len;
	const u8 *addr[2];
	size_t len[2];
	u8 *unwrapped = NULL;
	size_t unwrapped_len = 0;
	enum dpp_status_error ret = 256;

	wrapped_data = dpp_get_attr(attr_start, attr_len, DPP_ATTR_WRAPPED_DATA,
				    &wrapped_data_len);
	if (!wrapped_data || wrapped_data_len < AES_BLOCK_SIZE) {
		dpp_auth_fail(auth,
			      "Missing or invalid required Wrapped Data attribute");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Wrapped data",
		    wrapped_data, wrapped_data_len);

	attr_len = wrapped_data - 4 - attr_start;

	addr[0] = hdr;
	len[0] = DPP_HDR_LEN;
	addr[1] = attr_start;
	len[1] = attr_len;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped)
		goto fail;
	if (aes_siv_decrypt(auth->ke, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    2, addr, len, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	e_nonce = dpp_get_attr(unwrapped, unwrapped_len,
			       DPP_ATTR_ENROLLEE_NONCE,
			       &e_nonce_len);
	if (!e_nonce || e_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth,
			      "Missing or invalid Enrollee Nonce attribute");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Enrollee Nonce", e_nonce, e_nonce_len);
	if (os_memcmp(e_nonce, auth->e_nonce, e_nonce_len) != 0) {
		dpp_auth_fail(auth, "Enrollee Nonce mismatch");
		wpa_hexdump(MSG_DEBUG, "DPP: Expected Enrollee Nonce",
			    auth->e_nonce, e_nonce_len);
		goto fail;
	}

	status = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_STATUS,
			      &status_len);
	if (!status || status_len < 1) {
		dpp_auth_fail(auth,
			      "Missing or invalid required DPP Status attribute");
		goto fail;
	}
	wpa_printf(MSG_DEBUG, "DPP: Status %u", status[0]);
	ret = status[0];

fail:
	bin_clear_free(unwrapped, unwrapped_len);
	return ret;
}


struct wpabuf * dpp_build_conf_result(struct dpp_authentication *auth,
				      enum dpp_status_error status)
{
	struct wpabuf *msg, *clear;
	size_t nonce_len, clear_len, attr_len;
	const u8 *addr[2];
	size_t len[2];
	u8 *wrapped;

	nonce_len = auth->curve->nonce_len;
	clear_len = 5 + 4 + nonce_len;
	attr_len = 4 + clear_len + AES_BLOCK_SIZE;
	clear = wpabuf_alloc(clear_len);
	msg = dpp_alloc_msg(DPP_PA_CONFIGURATION_RESULT, attr_len);
	if (!clear || !msg)
		goto fail;

	/* DPP Status */
	dpp_build_attr_status(clear, status);

	/* E-nonce */
	wpabuf_put_le16(clear, DPP_ATTR_ENROLLEE_NONCE);
	wpabuf_put_le16(clear, nonce_len);
	wpabuf_put_data(clear, auth->e_nonce, nonce_len);

	/* OUI, OUI type, Crypto Suite, DPP frame type */
	addr[0] = wpabuf_head_u8(msg) + 2;
	len[0] = 3 + 1 + 1 + 1;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);

	/* Attributes before Wrapped Data (none) */
	addr[1] = wpabuf_put(msg, 0);
	len[1] = 0;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);

	/* Wrapped Data */
	wpabuf_put_le16(msg, DPP_ATTR_WRAPPED_DATA);
	wpabuf_put_le16(msg, wpabuf_len(clear) + AES_BLOCK_SIZE);
	wrapped = wpabuf_put(msg, wpabuf_len(clear) + AES_BLOCK_SIZE);

	wpa_hexdump_buf(MSG_DEBUG, "DPP: AES-SIV cleartext", clear);
	if (aes_siv_encrypt(auth->ke, auth->curve->hash_len,
			    wpabuf_head(clear), wpabuf_len(clear),
			    2, addr, len, wrapped) < 0)
		goto fail;

	wpa_hexdump_buf(MSG_DEBUG, "DPP: Configuration Result attributes", msg);
	wpabuf_free(clear);
	return msg;
fail:
	wpabuf_free(clear);
	wpabuf_free(msg);
	return NULL;
}


static int valid_channel_list(const char *val)
{
	while (*val) {
		if (!((*val >= '0' && *val <= '9') ||
		      *val == '/' || *val == ','))
			return 0;
		val++;
	}

	return 1;
}


enum dpp_status_error dpp_conn_status_result_rx(struct dpp_authentication *auth,
						const u8 *hdr,
						const u8 *attr_start,
						size_t attr_len,
						u8 *ssid, size_t *ssid_len,
						char **channel_list)
{
	const u8 *wrapped_data, *status, *e_nonce;
	u16 wrapped_data_len, status_len, e_nonce_len;
	const u8 *addr[2];
	size_t len[2];
	u8 *unwrapped = NULL;
	size_t unwrapped_len = 0;
	enum dpp_status_error ret = 256;
	struct json_token *root = NULL, *token;
	struct wpabuf *ssid64;

	*ssid_len = 0;
	*channel_list = NULL;

	wrapped_data = dpp_get_attr(attr_start, attr_len, DPP_ATTR_WRAPPED_DATA,
				    &wrapped_data_len);
	if (!wrapped_data || wrapped_data_len < AES_BLOCK_SIZE) {
		dpp_auth_fail(auth,
			      "Missing or invalid required Wrapped Data attribute");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Wrapped data",
		    wrapped_data, wrapped_data_len);

	attr_len = wrapped_data - 4 - attr_start;

	addr[0] = hdr;
	len[0] = DPP_HDR_LEN;
	addr[1] = attr_start;
	len[1] = attr_len;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV ciphertext",
		    wrapped_data, wrapped_data_len);
	unwrapped_len = wrapped_data_len - AES_BLOCK_SIZE;
	unwrapped = os_malloc(unwrapped_len);
	if (!unwrapped)
		goto fail;
	if (aes_siv_decrypt(auth->ke, auth->curve->hash_len,
			    wrapped_data, wrapped_data_len,
			    2, addr, len, unwrapped) < 0) {
		dpp_auth_fail(auth, "AES-SIV decryption failed");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: AES-SIV cleartext",
		    unwrapped, unwrapped_len);

	if (dpp_check_attrs(unwrapped, unwrapped_len) < 0) {
		dpp_auth_fail(auth, "Invalid attribute in unwrapped data");
		goto fail;
	}

	e_nonce = dpp_get_attr(unwrapped, unwrapped_len,
			       DPP_ATTR_ENROLLEE_NONCE,
			       &e_nonce_len);
	if (!e_nonce || e_nonce_len != auth->curve->nonce_len) {
		dpp_auth_fail(auth,
			      "Missing or invalid Enrollee Nonce attribute");
		goto fail;
	}
	wpa_hexdump(MSG_DEBUG, "DPP: Enrollee Nonce", e_nonce, e_nonce_len);
	if (os_memcmp(e_nonce, auth->e_nonce, e_nonce_len) != 0) {
		dpp_auth_fail(auth, "Enrollee Nonce mismatch");
		wpa_hexdump(MSG_DEBUG, "DPP: Expected Enrollee Nonce",
			    auth->e_nonce, e_nonce_len);
		goto fail;
	}

	status = dpp_get_attr(unwrapped, unwrapped_len, DPP_ATTR_CONN_STATUS,
			      &status_len);
	if (!status) {
		dpp_auth_fail(auth,
			      "Missing required DPP Connection Status attribute");
		goto fail;
	}
	wpa_hexdump_ascii(MSG_DEBUG, "DPP: connStatus JSON",
			  status, status_len);

	root = json_parse((const char *) status, status_len);
	if (!root) {
		dpp_auth_fail(auth, "Could not parse connStatus");
		goto fail;
	}

	ssid64 = json_get_member_base64url(root, "ssid64");
	if (ssid64 && wpabuf_len(ssid64) <= SSID_MAX_LEN) {
		*ssid_len = wpabuf_len(ssid64);
		os_memcpy(ssid, wpabuf_head(ssid64), *ssid_len);
	}
	wpabuf_free(ssid64);

	token = json_get_member(root, "channelList");
	if (token && token->type == JSON_STRING &&
	    valid_channel_list(token->string))
		*channel_list = os_strdup(token->string);

	token = json_get_member(root, "result");
	if (!token || token->type != JSON_NUMBER) {
		dpp_auth_fail(auth, "No connStatus - result");
		goto fail;
	}
	wpa_printf(MSG_DEBUG, "DPP: result %d", token->number);
	ret = token->number;

fail:
	json_free(root);
	bin_clear_free(unwrapped, unwrapped_len);
	return ret;
}


struct wpabuf * dpp_build_conn_status(enum dpp_status_error result,
				      const u8 *ssid, size_t ssid_len,
				      const char *channel_list)
{
	struct wpabuf *json;

	json = wpabuf_alloc(1000);
	if (!json)
		return NULL;
	json_start_object(json, NULL);
	json_add_int(json, "result", result);
	if (ssid) {
		json_value_sep(json);
		if (json_add_base64url(json, "ssid64", ssid, ssid_len) < 0) {
			wpabuf_free(json);
			return NULL;
		}
	}
	if (channel_list) {
		json_value_sep(json);
		json_add_string(json, "channelList", channel_list);
	}
	json_end_object(json);
	wpa_hexdump_ascii(MSG_DEBUG, "DPP: connStatus JSON",
			  wpabuf_head(json), wpabuf_len(json));

	return json;
}


struct wpabuf * dpp_build_conn_status_result(struct dpp_authentication *auth,
					     enum dpp_status_error result,
					     const u8 *ssid, size_t ssid_len,
					     const char *channel_list)
{
	struct wpabuf *msg = NULL, *clear = NULL, *json;
	size_t nonce_len, clear_len, attr_len;
	const u8 *addr[2];
	size_t len[2];
	u8 *wrapped;

	json = dpp_build_conn_status(result, ssid, ssid_len, channel_list);
	if (!json)
		return NULL;

	nonce_len = auth->curve->nonce_len;
	clear_len = 5 + 4 + nonce_len + 4 + wpabuf_len(json);
	attr_len = 4 + clear_len + AES_BLOCK_SIZE;
	clear = wpabuf_alloc(clear_len);
	msg = dpp_alloc_msg(DPP_PA_CONNECTION_STATUS_RESULT, attr_len);
	if (!clear || !msg)
		goto fail;

	/* E-nonce */
	wpabuf_put_le16(clear, DPP_ATTR_ENROLLEE_NONCE);
	wpabuf_put_le16(clear, nonce_len);
	wpabuf_put_data(clear, auth->e_nonce, nonce_len);

	/* DPP Connection Status */
	wpabuf_put_le16(clear, DPP_ATTR_CONN_STATUS);
	wpabuf_put_le16(clear, wpabuf_len(json));
	wpabuf_put_buf(clear, json);

	/* OUI, OUI type, Crypto Suite, DPP frame type */
	addr[0] = wpabuf_head_u8(msg) + 2;
	len[0] = 3 + 1 + 1 + 1;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[0]", addr[0], len[0]);

	/* Attributes before Wrapped Data (none) */
	addr[1] = wpabuf_put(msg, 0);
	len[1] = 0;
	wpa_hexdump(MSG_DEBUG, "DDP: AES-SIV AD[1]", addr[1], len[1]);

	/* Wrapped Data */
	wpabuf_put_le16(msg, DPP_ATTR_WRAPPED_DATA);
	wpabuf_put_le16(msg, wpabuf_len(clear) + AES_BLOCK_SIZE);
	wrapped = wpabuf_put(msg, wpabuf_len(clear) + AES_BLOCK_SIZE);

	wpa_hexdump_buf(MSG_DEBUG, "DPP: AES-SIV cleartext", clear);
	if (aes_siv_encrypt(auth->ke, auth->curve->hash_len,
			    wpabuf_head(clear), wpabuf_len(clear),
			    2, addr, len, wrapped) < 0)
		goto fail;

	wpa_hexdump_buf(MSG_DEBUG, "DPP: Connection Status Result attributes",
			msg);
	wpabuf_free(json);
	wpabuf_free(clear);
	return msg;
fail:
	wpabuf_free(json);
	wpabuf_free(clear);
	wpabuf_free(msg);
	return NULL;
}

#endif /* CONFIG_DPP2 */


void dpp_configurator_free(struct dpp_configurator *conf)
{
	if (!conf)
		return;
	EVP_PKEY_free(conf->csign);
	os_free(conf->kid);
	os_free(conf);
}


int dpp_configurator_get_key(const struct dpp_configurator *conf, char *buf,
			     size_t buflen)
{
	EC_KEY *eckey;
	int key_len, ret = -1;
	unsigned char *key = NULL;

	if (!conf->csign)
		return -1;

	eckey = EVP_PKEY_get1_EC_KEY(conf->csign);
	if (!eckey)
		return -1;

	key_len = i2d_ECPrivateKey(eckey, &key);
	if (key_len > 0)
		ret = wpa_snprintf_hex(buf, buflen, key, key_len);

	EC_KEY_free(eckey);
	OPENSSL_free(key);
	return ret;
}


static int dpp_configurator_gen_kid(struct dpp_configurator *conf)
{
	struct wpabuf *csign_pub = NULL;
	const u8 *addr[1];
	size_t len[1];
	int res;

	csign_pub = dpp_get_pubkey_point(conf->csign, 1);
	if (!csign_pub) {
		wpa_printf(MSG_INFO, "DPP: Failed to extract C-sign-key");
		return -1;
	}

	/* kid = SHA256(ANSI X9.63 uncompressed C-sign-key) */
	addr[0] = wpabuf_head(csign_pub);
	len[0] = wpabuf_len(csign_pub);
	res = sha256_vector(1, addr, len, conf->kid_hash);
	wpabuf_free(csign_pub);
	if (res < 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Failed to derive kid for C-sign-key");
		return -1;
	}

	conf->kid = base64_url_encode(conf->kid_hash, sizeof(conf->kid_hash),
				      NULL);
	return conf->kid ? 0 : -1;
}


struct dpp_configurator *
dpp_keygen_configurator(const char *curve, const u8 *privkey,
			size_t privkey_len)
{
	struct dpp_configurator *conf;

	conf = os_zalloc(sizeof(*conf));
	if (!conf)
		return NULL;

	conf->curve = dpp_get_curve_name(curve);
	if (!conf->curve) {
		wpa_printf(MSG_INFO, "DPP: Unsupported curve: %s", curve);
		os_free(conf);
		return NULL;
	}

	if (privkey)
		conf->csign = dpp_set_keypair(&conf->curve, privkey,
					      privkey_len);
	else
		conf->csign = dpp_gen_keypair(conf->curve);
	if (!conf->csign)
		goto fail;
	conf->own = 1;

	if (dpp_configurator_gen_kid(conf) < 0)
		goto fail;
	return conf;
fail:
	dpp_configurator_free(conf);
	return NULL;
}


int dpp_configurator_own_config(struct dpp_authentication *auth,
				const char *curve, int ap)
{
	struct wpabuf *conf_obj;
	int ret = -1;

	if (!auth->conf) {
		wpa_printf(MSG_DEBUG, "DPP: No configurator specified");
		return -1;
	}

	auth->curve = dpp_get_curve_name(curve);
	if (!auth->curve) {
		wpa_printf(MSG_INFO, "DPP: Unsupported curve: %s", curve);
		return -1;
	}

	wpa_printf(MSG_DEBUG,
		   "DPP: Building own configuration/connector with curve %s",
		   auth->curve->name);

	auth->own_protocol_key = dpp_gen_keypair(auth->curve);
	if (!auth->own_protocol_key)
		return -1;
	dpp_copy_netaccesskey(auth, &auth->conf_obj[0]);
	auth->peer_protocol_key = auth->own_protocol_key;
	dpp_copy_csign(&auth->conf_obj[0], auth->conf->csign);

	conf_obj = dpp_build_conf_obj(auth, ap, 0);
	if (!conf_obj) {
		wpabuf_free(auth->conf_obj[0].c_sign_key);
		auth->conf_obj[0].c_sign_key = NULL;
		goto fail;
	}
	ret = dpp_parse_conf_obj(auth, wpabuf_head(conf_obj),
				 wpabuf_len(conf_obj));
fail:
	wpabuf_free(conf_obj);
	auth->peer_protocol_key = NULL;
	return ret;
}


static int dpp_compatible_netrole(const char *role1, const char *role2)
{
	return (os_strcmp(role1, "sta") == 0 && os_strcmp(role2, "ap") == 0) ||
		(os_strcmp(role1, "ap") == 0 && os_strcmp(role2, "sta") == 0);
}


static int dpp_connector_compatible_group(struct json_token *root,
					  const char *group_id,
					  const char *net_role,
					  bool reconfig)
{
	struct json_token *groups, *token;

	groups = json_get_member(root, "groups");
	if (!groups || groups->type != JSON_ARRAY)
		return 0;

	for (token = groups->child; token; token = token->sibling) {
		struct json_token *id, *role;

		id = json_get_member(token, "groupId");
		if (!id || id->type != JSON_STRING)
			continue;

		role = json_get_member(token, "netRole");
		if (!role || role->type != JSON_STRING)
			continue;

		if (os_strcmp(id->string, "*") != 0 &&
		    os_strcmp(group_id, "*") != 0 &&
		    os_strcmp(id->string, group_id) != 0)
			continue;

		if (reconfig && os_strcmp(net_role, "configurator") == 0)
			return 1;
		if (!reconfig && dpp_compatible_netrole(role->string, net_role))
			return 1;
	}

	return 0;
}


int dpp_connector_match_groups(struct json_token *own_root,
			       struct json_token *peer_root, bool reconfig)
{
	struct json_token *groups, *token;

	groups = json_get_member(peer_root, "groups");
	if (!groups || groups->type != JSON_ARRAY) {
		wpa_printf(MSG_DEBUG, "DPP: No peer groups array found");
		return 0;
	}

	for (token = groups->child; token; token = token->sibling) {
		struct json_token *id, *role;

		id = json_get_member(token, "groupId");
		if (!id || id->type != JSON_STRING) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Missing peer groupId string");
			continue;
		}

		role = json_get_member(token, "netRole");
		if (!role || role->type != JSON_STRING) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Missing peer groups::netRole string");
			continue;
		}
		wpa_printf(MSG_DEBUG,
			   "DPP: peer connector group: groupId='%s' netRole='%s'",
			   id->string, role->string);
		if (dpp_connector_compatible_group(own_root, id->string,
						   role->string, reconfig)) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Compatible group/netRole in own connector");
			return 1;
		}
	}

	return 0;
}


struct json_token * dpp_parse_own_connector(const char *own_connector)
{
	unsigned char *own_conn;
	size_t own_conn_len;
	const char *pos, *end;
	struct json_token *own_root;

	pos = os_strchr(own_connector, '.');
	if (!pos) {
		wpa_printf(MSG_DEBUG, "DPP: Own connector is missing the first dot (.)");
		return NULL;
	}
	pos++;
	end = os_strchr(pos, '.');
	if (!end) {
		wpa_printf(MSG_DEBUG, "DPP: Own connector is missing the second dot (.)");
		return NULL;
	}
	own_conn = base64_url_decode(pos, end - pos, &own_conn_len);
	if (!own_conn) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Failed to base64url decode own signedConnector JWS Payload");
		return NULL;
	}

	own_root = json_parse((const char *) own_conn, own_conn_len);
	os_free(own_conn);
	if (!own_root)
		wpa_printf(MSG_DEBUG, "DPP: Failed to parse local connector");

	return own_root;
}


enum dpp_status_error
dpp_peer_intro(struct dpp_introduction *intro, const char *own_connector,
	       const u8 *net_access_key, size_t net_access_key_len,
	       const u8 *csign_key, size_t csign_key_len,
	       const u8 *peer_connector, size_t peer_connector_len,
	       os_time_t *expiry)
{
	struct json_token *root = NULL, *netkey, *token;
	struct json_token *own_root = NULL;
	enum dpp_status_error ret = 255, res;
	EVP_PKEY *own_key = NULL, *peer_key = NULL;
	struct wpabuf *own_key_pub = NULL;
	const struct dpp_curve_params *curve, *own_curve;
	struct dpp_signed_connector_info info;
	size_t Nx_len;
	u8 Nx[DPP_MAX_SHARED_SECRET_LEN];

	os_memset(intro, 0, sizeof(*intro));
	os_memset(&info, 0, sizeof(info));
	if (expiry)
		*expiry = 0;

	own_key = dpp_set_keypair(&own_curve, net_access_key,
				  net_access_key_len);
	if (!own_key) {
		wpa_printf(MSG_ERROR, "DPP: Failed to parse own netAccessKey");
		goto fail;
	}

	own_root = dpp_parse_own_connector(own_connector);
	if (!own_root)
		goto fail;

	res = dpp_check_signed_connector(&info, csign_key, csign_key_len,
					 peer_connector, peer_connector_len);
	if (res != DPP_STATUS_OK) {
		ret = res;
		goto fail;
	}

	root = json_parse((const char *) info.payload, info.payload_len);
	if (!root) {
		wpa_printf(MSG_DEBUG, "DPP: JSON parsing of connector failed");
		ret = DPP_STATUS_INVALID_CONNECTOR;
		goto fail;
	}

	if (!dpp_connector_match_groups(own_root, root, false)) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Peer connector does not include compatible group netrole with own connector");
		ret = DPP_STATUS_NO_MATCH;
		goto fail;
	}

	token = json_get_member(root, "expiry");
	if (!token || token->type != JSON_STRING) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No expiry string found - connector does not expire");
	} else {
		wpa_printf(MSG_DEBUG, "DPP: expiry = %s", token->string);
		if (dpp_key_expired(token->string, expiry)) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Connector (netAccessKey) has expired");
			ret = DPP_STATUS_INVALID_CONNECTOR;
			goto fail;
		}
	}

	netkey = json_get_member(root, "netAccessKey");
	if (!netkey || netkey->type != JSON_OBJECT) {
		wpa_printf(MSG_DEBUG, "DPP: No netAccessKey object found");
		ret = DPP_STATUS_INVALID_CONNECTOR;
		goto fail;
	}

	peer_key = dpp_parse_jwk(netkey, &curve);
	if (!peer_key) {
		ret = DPP_STATUS_INVALID_CONNECTOR;
		goto fail;
	}
	dpp_debug_print_key("DPP: Received netAccessKey", peer_key);

	if (own_curve != curve) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Mismatching netAccessKey curves (%s != %s)",
			   own_curve->name, curve->name);
		ret = DPP_STATUS_INVALID_CONNECTOR;
		goto fail;
	}

	/* ECDH: N = nk * PK */
	if (dpp_ecdh(own_key, peer_key, Nx, &Nx_len) < 0)
		goto fail;

	wpa_hexdump_key(MSG_DEBUG, "DPP: ECDH shared secret (N.x)",
			Nx, Nx_len);

	/* PMK = HKDF(<>, "DPP PMK", N.x) */
	if (dpp_derive_pmk(Nx, Nx_len, intro->pmk, curve->hash_len) < 0) {
		wpa_printf(MSG_ERROR, "DPP: Failed to derive PMK");
		goto fail;
	}
	intro->pmk_len = curve->hash_len;

	/* PMKID = Truncate-128(H(min(NK.x, PK.x) | max(NK.x, PK.x))) */
	if (dpp_derive_pmkid(curve, own_key, peer_key, intro->pmkid) < 0) {
		wpa_printf(MSG_ERROR, "DPP: Failed to derive PMKID");
		goto fail;
	}

	ret = DPP_STATUS_OK;
fail:
	if (ret != DPP_STATUS_OK)
		os_memset(intro, 0, sizeof(*intro));
	os_memset(Nx, 0, sizeof(Nx));
	os_free(info.payload);
	EVP_PKEY_free(own_key);
	wpabuf_free(own_key_pub);
	EVP_PKEY_free(peer_key);
	json_free(root);
	json_free(own_root);
	return ret;
}


unsigned int dpp_next_id(struct dpp_global *dpp)
{
	struct dpp_bootstrap_info *bi;
	unsigned int max_id = 0;

	dl_list_for_each(bi, &dpp->bootstrap, struct dpp_bootstrap_info, list) {
		if (bi->id > max_id)
			max_id = bi->id;
	}
	return max_id + 1;
}


static int dpp_bootstrap_del(struct dpp_global *dpp, unsigned int id)
{
	struct dpp_bootstrap_info *bi, *tmp;
	int found = 0;

	if (!dpp)
		return -1;

	dl_list_for_each_safe(bi, tmp, &dpp->bootstrap,
			      struct dpp_bootstrap_info, list) {
		if (id && bi->id != id)
			continue;
		found = 1;
#ifdef CONFIG_DPP2
		if (dpp->remove_bi)
			dpp->remove_bi(dpp->cb_ctx, bi);
#endif /* CONFIG_DPP2 */
		dl_list_del(&bi->list);
		dpp_bootstrap_info_free(bi);
	}

	if (id == 0)
		return 0; /* flush succeeds regardless of entries found */
	return found ? 0 : -1;
}


struct dpp_bootstrap_info * dpp_add_qr_code(struct dpp_global *dpp,
					    const char *uri)
{
	struct dpp_bootstrap_info *bi;

	if (!dpp)
		return NULL;

	bi = dpp_parse_uri(uri);
	if (!bi)
		return NULL;

	bi->type = DPP_BOOTSTRAP_QR_CODE;
	bi->id = dpp_next_id(dpp);
	dl_list_add(&dpp->bootstrap, &bi->list);
	return bi;
}


struct dpp_bootstrap_info * dpp_add_nfc_uri(struct dpp_global *dpp,
					    const char *uri)
{
	struct dpp_bootstrap_info *bi;

	if (!dpp)
		return NULL;

	bi = dpp_parse_uri(uri);
	if (!bi)
		return NULL;

	bi->type = DPP_BOOTSTRAP_NFC_URI;
	bi->id = dpp_next_id(dpp);
	dl_list_add(&dpp->bootstrap, &bi->list);
	return bi;
}


int dpp_bootstrap_gen(struct dpp_global *dpp, const char *cmd)
{
	char *mac = NULL, *info = NULL, *curve = NULL;
	char *key = NULL;
	u8 *privkey = NULL;
	size_t privkey_len = 0;
	int ret = -1;
	struct dpp_bootstrap_info *bi;

	if (!dpp)
		return -1;

	bi = os_zalloc(sizeof(*bi));
	if (!bi)
		goto fail;

	if (os_strstr(cmd, "type=qrcode"))
		bi->type = DPP_BOOTSTRAP_QR_CODE;
	else if (os_strstr(cmd, "type=pkex"))
		bi->type = DPP_BOOTSTRAP_PKEX;
	else if (os_strstr(cmd, "type=nfc-uri"))
		bi->type = DPP_BOOTSTRAP_NFC_URI;
	else
		goto fail;

	bi->chan = get_param(cmd, " chan=");
	mac = get_param(cmd, " mac=");
	info = get_param(cmd, " info=");
	curve = get_param(cmd, " curve=");
	key = get_param(cmd, " key=");

	if (key) {
		privkey_len = os_strlen(key) / 2;
		privkey = os_malloc(privkey_len);
		if (!privkey ||
		    hexstr2bin(key, privkey, privkey_len) < 0)
			goto fail;
	}

	if (dpp_keygen(bi, curve, privkey, privkey_len) < 0 ||
	    dpp_parse_uri_chan_list(bi, bi->chan) < 0 ||
	    dpp_parse_uri_mac(bi, mac) < 0 ||
	    dpp_parse_uri_info(bi, info) < 0 ||
	    dpp_gen_uri(bi) < 0)
		goto fail;

	bi->id = dpp_next_id(dpp);
	dl_list_add(&dpp->bootstrap, &bi->list);
	ret = bi->id;
	bi = NULL;
fail:
	os_free(curve);
	os_free(mac);
	os_free(info);
	str_clear_free(key);
	bin_clear_free(privkey, privkey_len);
	dpp_bootstrap_info_free(bi);
	return ret;
}


struct dpp_bootstrap_info *
dpp_bootstrap_get_id(struct dpp_global *dpp, unsigned int id)
{
	struct dpp_bootstrap_info *bi;

	if (!dpp)
		return NULL;

	dl_list_for_each(bi, &dpp->bootstrap, struct dpp_bootstrap_info, list) {
		if (bi->id == id)
			return bi;
	}
	return NULL;
}


int dpp_bootstrap_remove(struct dpp_global *dpp, const char *id)
{
	unsigned int id_val;

	if (os_strcmp(id, "*") == 0) {
		id_val = 0;
	} else {
		id_val = atoi(id);
		if (id_val == 0)
			return -1;
	}

	return dpp_bootstrap_del(dpp, id_val);
}


const char * dpp_bootstrap_get_uri(struct dpp_global *dpp, unsigned int id)
{
	struct dpp_bootstrap_info *bi;

	bi = dpp_bootstrap_get_id(dpp, id);
	if (!bi)
		return NULL;
	return bi->uri;
}


int dpp_bootstrap_info(struct dpp_global *dpp, int id,
		       char *reply, int reply_size)
{
	struct dpp_bootstrap_info *bi;
	char pkhash[2 * SHA256_MAC_LEN + 1];

	bi = dpp_bootstrap_get_id(dpp, id);
	if (!bi)
		return -1;
	wpa_snprintf_hex(pkhash, sizeof(pkhash), bi->pubkey_hash,
			 SHA256_MAC_LEN);
	return os_snprintf(reply, reply_size, "type=%s\n"
			   "mac_addr=" MACSTR "\n"
			   "info=%s\n"
			   "num_freq=%u\n"
			   "use_freq=%u\n"
			   "curve=%s\n"
			   "pkhash=%s\n"
			   "version=%d\n",
			   dpp_bootstrap_type_txt(bi->type),
			   MAC2STR(bi->mac_addr),
			   bi->info ? bi->info : "",
			   bi->num_freq,
			   bi->num_freq == 1 ? bi->freq[0] : 0,
			   bi->curve->name,
			   pkhash,
			   bi->version);
}


int dpp_bootstrap_set(struct dpp_global *dpp, int id, const char *params)
{
	struct dpp_bootstrap_info *bi;

	bi = dpp_bootstrap_get_id(dpp, id);
	if (!bi)
		return -1;

	str_clear_free(bi->configurator_params);

	if (params) {
		bi->configurator_params = os_strdup(params);
		return bi->configurator_params ? 0 : -1;
	}

	bi->configurator_params = NULL;
	return 0;
}


void dpp_bootstrap_find_pair(struct dpp_global *dpp, const u8 *i_bootstrap,
			     const u8 *r_bootstrap,
			     struct dpp_bootstrap_info **own_bi,
			     struct dpp_bootstrap_info **peer_bi)
{
	struct dpp_bootstrap_info *bi;

	*own_bi = NULL;
	*peer_bi = NULL;
	if (!dpp)
		return;

	dl_list_for_each(bi, &dpp->bootstrap, struct dpp_bootstrap_info, list) {
		if (!*own_bi && bi->own &&
		    os_memcmp(bi->pubkey_hash, r_bootstrap,
			      SHA256_MAC_LEN) == 0) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Found matching own bootstrapping information");
			*own_bi = bi;
		}

		if (!*peer_bi && !bi->own &&
		    os_memcmp(bi->pubkey_hash, i_bootstrap,
			      SHA256_MAC_LEN) == 0) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Found matching peer bootstrapping information");
			*peer_bi = bi;
		}

		if (*own_bi && *peer_bi)
			break;
	}
}


#ifdef CONFIG_DPP2
struct dpp_bootstrap_info * dpp_bootstrap_find_chirp(struct dpp_global *dpp,
						     const u8 *hash)
{
	struct dpp_bootstrap_info *bi;

	if (!dpp)
		return NULL;

	dl_list_for_each(bi, &dpp->bootstrap, struct dpp_bootstrap_info, list) {
		if (!bi->own && os_memcmp(bi->pubkey_hash_chirp, hash,
					  SHA256_MAC_LEN) == 0)
			return bi;
	}

	return NULL;
}
#endif /* CONFIG_DPP2 */


static int dpp_nfc_update_bi_channel(struct dpp_bootstrap_info *own_bi,
				     struct dpp_bootstrap_info *peer_bi)
{
	unsigned int i, freq = 0;
	enum hostapd_hw_mode mode;
	u8 op_class, channel;
	char chan[20];

	if (peer_bi->num_freq == 0)
		return 0; /* no channel preference/constraint */

	for (i = 0; i < peer_bi->num_freq; i++) {
		if (own_bi->num_freq == 0 ||
		    freq_included(own_bi->freq, own_bi->num_freq,
				  peer_bi->freq[i])) {
			freq = peer_bi->freq[i];
			break;
		}
	}
	if (!freq) {
		wpa_printf(MSG_DEBUG, "DPP: No common channel found");
		return -1;
	}

	mode = ieee80211_freq_to_channel_ext(freq, 0, 0, &op_class, &channel);
	if (mode == NUM_HOSTAPD_MODES) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Could not determine operating class or channel number for %u MHz",
			   freq);
	}

	wpa_printf(MSG_DEBUG,
		   "DPP: Selected %u MHz (op_class %u channel %u) as the negotiation channel based on information from NFC negotiated handover",
		   freq, op_class, channel);
	os_snprintf(chan, sizeof(chan), "%u/%u", op_class, channel);
	os_free(own_bi->chan);
	own_bi->chan = os_strdup(chan);
	own_bi->freq[0] = freq;
	own_bi->num_freq = 1;
	os_free(peer_bi->chan);
	peer_bi->chan = os_strdup(chan);
	peer_bi->freq[0] = freq;
	peer_bi->num_freq = 1;

	return dpp_gen_uri(own_bi);
}


static int dpp_nfc_update_bi_key(struct dpp_bootstrap_info *own_bi,
				 struct dpp_bootstrap_info *peer_bi)
{
	if (peer_bi->curve == own_bi->curve)
		return 0;

	wpa_printf(MSG_DEBUG,
		   "DPP: Update own bootstrapping key to match peer curve from NFC handover");

	EVP_PKEY_free(own_bi->pubkey);
	own_bi->pubkey = NULL;

	if (dpp_keygen(own_bi, peer_bi->curve->name, NULL, 0) < 0 ||
	    dpp_gen_uri(own_bi) < 0)
		goto fail;

	return 0;
fail:
	dl_list_del(&own_bi->list);
	dpp_bootstrap_info_free(own_bi);
	return -1;
}


int dpp_nfc_update_bi(struct dpp_bootstrap_info *own_bi,
		      struct dpp_bootstrap_info *peer_bi)
{
	if (dpp_nfc_update_bi_channel(own_bi, peer_bi) < 0 ||
	    dpp_nfc_update_bi_key(own_bi, peer_bi) < 0)
		return -1;
	return 0;
}


static unsigned int dpp_next_configurator_id(struct dpp_global *dpp)
{
	struct dpp_configurator *conf;
	unsigned int max_id = 0;

	dl_list_for_each(conf, &dpp->configurator, struct dpp_configurator,
			 list) {
		if (conf->id > max_id)
			max_id = conf->id;
	}
	return max_id + 1;
}


int dpp_configurator_add(struct dpp_global *dpp, const char *cmd)
{
	char *curve = NULL;
	char *key = NULL;
	u8 *privkey = NULL;
	size_t privkey_len = 0;
	int ret = -1;
	struct dpp_configurator *conf = NULL;

	curve = get_param(cmd, " curve=");
	key = get_param(cmd, " key=");

	if (key) {
		privkey_len = os_strlen(key) / 2;
		privkey = os_malloc(privkey_len);
		if (!privkey ||
		    hexstr2bin(key, privkey, privkey_len) < 0)
			goto fail;
	}

	conf = dpp_keygen_configurator(curve, privkey, privkey_len);
	if (!conf)
		goto fail;

	conf->id = dpp_next_configurator_id(dpp);
	dl_list_add(&dpp->configurator, &conf->list);
	ret = conf->id;
	conf = NULL;
fail:
	os_free(curve);
	str_clear_free(key);
	bin_clear_free(privkey, privkey_len);
	dpp_configurator_free(conf);
	return ret;
}


static int dpp_configurator_del(struct dpp_global *dpp, unsigned int id)
{
	struct dpp_configurator *conf, *tmp;
	int found = 0;

	if (!dpp)
		return -1;

	dl_list_for_each_safe(conf, tmp, &dpp->configurator,
			      struct dpp_configurator, list) {
		if (id && conf->id != id)
			continue;
		found = 1;
		dl_list_del(&conf->list);
		dpp_configurator_free(conf);
	}

	if (id == 0)
		return 0; /* flush succeeds regardless of entries found */
	return found ? 0 : -1;
}


int dpp_configurator_remove(struct dpp_global *dpp, const char *id)
{
	unsigned int id_val;

	if (os_strcmp(id, "*") == 0) {
		id_val = 0;
	} else {
		id_val = atoi(id);
		if (id_val == 0)
			return -1;
	}

	return dpp_configurator_del(dpp, id_val);
}


int dpp_configurator_get_key_id(struct dpp_global *dpp, unsigned int id,
				char *buf, size_t buflen)
{
	struct dpp_configurator *conf;

	conf = dpp_configurator_get_id(dpp, id);
	if (!conf)
		return -1;

	return dpp_configurator_get_key(conf, buf, buflen);
}


#ifdef CONFIG_DPP2

int dpp_configurator_from_backup(struct dpp_global *dpp,
				 struct dpp_asymmetric_key *key)
{
	struct dpp_configurator *conf;
	const EC_KEY *eckey;
	const EC_GROUP *group;
	int nid;
	const struct dpp_curve_params *curve;

	if (!key->csign)
		return -1;
	eckey = EVP_PKEY_get0_EC_KEY(key->csign);
	if (!eckey)
		return -1;
	group = EC_KEY_get0_group(eckey);
	if (!group)
		return -1;
	nid = EC_GROUP_get_curve_name(group);
	curve = dpp_get_curve_nid(nid);
	if (!curve) {
		wpa_printf(MSG_INFO, "DPP: Unsupported group in c-sign-key");
		return -1;
	}

	conf = os_zalloc(sizeof(*conf));
	if (!conf)
		return -1;
	conf->curve = curve;
	conf->csign = key->csign;
	key->csign = NULL;
	conf->own = 1;
	if (dpp_configurator_gen_kid(conf) < 0) {
		dpp_configurator_free(conf);
		return -1;
	}

	conf->id = dpp_next_configurator_id(dpp);
	dl_list_add(&dpp->configurator, &conf->list);
	return conf->id;
}


struct dpp_configurator * dpp_configurator_find_kid(struct dpp_global *dpp,
						    const u8 *kid)
{
	struct dpp_configurator *conf;

	if (!dpp)
		return NULL;

	dl_list_for_each(conf, &dpp->configurator,
			 struct dpp_configurator, list) {
		if (os_memcmp(conf->kid_hash, kid, SHA256_MAC_LEN) == 0)
			return conf;
	}
	return NULL;
}


static void dpp_controller_conn_status_result_wait_timeout(void *eloop_ctx,
							   void *timeout_ctx);


static void dpp_connection_free(struct dpp_connection *conn)
{
	if (conn->sock >= 0) {
		wpa_printf(MSG_DEBUG, "DPP: Close Controller socket %d",
			   conn->sock);
		eloop_unregister_sock(conn->sock, EVENT_TYPE_READ);
		eloop_unregister_sock(conn->sock, EVENT_TYPE_WRITE);
		close(conn->sock);
	}
	eloop_cancel_timeout(dpp_controller_conn_status_result_wait_timeout,
			     conn, NULL);
	wpabuf_free(conn->msg);
	wpabuf_free(conn->msg_out);
	dpp_auth_deinit(conn->auth);
	os_free(conn);
}


static void dpp_connection_remove(struct dpp_connection *conn)
{
	dl_list_del(&conn->list);
	dpp_connection_free(conn);
}


static void dpp_tcp_init_flush(struct dpp_global *dpp)
{
	struct dpp_connection *conn, *tmp;

	dl_list_for_each_safe(conn, tmp, &dpp->tcp_init, struct dpp_connection,
			      list)
		dpp_connection_remove(conn);
}


static void dpp_relay_controller_free(struct dpp_relay_controller *ctrl)
{
	struct dpp_connection *conn, *tmp;

	dl_list_for_each_safe(conn, tmp, &ctrl->conn, struct dpp_connection,
			      list)
		dpp_connection_remove(conn);
	os_free(ctrl);
}


static void dpp_relay_flush_controllers(struct dpp_global *dpp)
{
	struct dpp_relay_controller *ctrl, *tmp;

	if (!dpp)
		return;

	dl_list_for_each_safe(ctrl, tmp, &dpp->controllers,
			      struct dpp_relay_controller, list) {
		dl_list_del(&ctrl->list);
		dpp_relay_controller_free(ctrl);
	}
}

#endif /* CONFIG_DPP2 */


struct dpp_global * dpp_global_init(struct dpp_global_config *config)
{
	struct dpp_global *dpp;

	dpp = os_zalloc(sizeof(*dpp));
	if (!dpp)
		return NULL;
	dpp->msg_ctx = config->msg_ctx;
#ifdef CONFIG_DPP2
	dpp->cb_ctx = config->cb_ctx;
	dpp->process_conf_obj = config->process_conf_obj;
	dpp->remove_bi = config->remove_bi;
#endif /* CONFIG_DPP2 */

	dl_list_init(&dpp->bootstrap);
	dl_list_init(&dpp->configurator);
#ifdef CONFIG_DPP2
	dl_list_init(&dpp->controllers);
	dl_list_init(&dpp->tcp_init);
#endif /* CONFIG_DPP2 */

	return dpp;
}


void dpp_global_clear(struct dpp_global *dpp)
{
	if (!dpp)
		return;

	dpp_bootstrap_del(dpp, 0);
	dpp_configurator_del(dpp, 0);
#ifdef CONFIG_DPP2
	dpp_tcp_init_flush(dpp);
	dpp_relay_flush_controllers(dpp);
	dpp_controller_stop(dpp);
#endif /* CONFIG_DPP2 */
}


void dpp_global_deinit(struct dpp_global *dpp)
{
	dpp_global_clear(dpp);
	os_free(dpp);
}


#ifdef CONFIG_DPP2

static void dpp_controller_rx(int sd, void *eloop_ctx, void *sock_ctx);
static void dpp_conn_tx_ready(int sock, void *eloop_ctx, void *sock_ctx);
static void dpp_controller_auth_success(struct dpp_connection *conn,
					int initiator);


int dpp_relay_add_controller(struct dpp_global *dpp,
			     struct dpp_relay_config *config)
{
	struct dpp_relay_controller *ctrl;

	if (!dpp)
		return -1;

	ctrl = os_zalloc(sizeof(*ctrl));
	if (!ctrl)
		return -1;
	dl_list_init(&ctrl->conn);
	ctrl->global = dpp;
	os_memcpy(&ctrl->ipaddr, config->ipaddr, sizeof(*config->ipaddr));
	os_memcpy(ctrl->pkhash, config->pkhash, SHA256_MAC_LEN);
	ctrl->cb_ctx = config->cb_ctx;
	ctrl->tx = config->tx;
	ctrl->gas_resp_tx = config->gas_resp_tx;
	dl_list_add(&dpp->controllers, &ctrl->list);
	return 0;
}


static struct dpp_relay_controller *
dpp_relay_controller_get(struct dpp_global *dpp, const u8 *pkhash)
{
	struct dpp_relay_controller *ctrl;

	if (!dpp)
		return NULL;

	dl_list_for_each(ctrl, &dpp->controllers, struct dpp_relay_controller,
			 list) {
		if (os_memcmp(pkhash, ctrl->pkhash, SHA256_MAC_LEN) == 0)
			return ctrl;
	}

	return NULL;
}


static void dpp_controller_gas_done(struct dpp_connection *conn)
{
	struct dpp_authentication *auth = conn->auth;

	if (auth->peer_version >= 2 &&
	    auth->conf_resp_status == DPP_STATUS_OK) {
		wpa_printf(MSG_DEBUG, "DPP: Wait for Configuration Result");
		auth->waiting_conf_result = 1;
		return;
	}

	wpa_msg(conn->ctrl->global->msg_ctx, MSG_INFO, DPP_EVENT_CONF_SENT);
	dpp_connection_remove(conn);
}


static int dpp_tcp_send(struct dpp_connection *conn)
{
	int res;

	if (!conn->msg_out) {
		eloop_unregister_sock(conn->sock, EVENT_TYPE_WRITE);
		conn->write_eloop = 0;
		return -1;
	}
	res = send(conn->sock,
		   wpabuf_head_u8(conn->msg_out) + conn->msg_out_pos,
		   wpabuf_len(conn->msg_out) - conn->msg_out_pos, 0);
	if (res < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Failed to send buffer: %s",
			   strerror(errno));
		dpp_connection_remove(conn);
		return -1;
	}

	conn->msg_out_pos += res;
	if (wpabuf_len(conn->msg_out) > conn->msg_out_pos) {
		wpa_printf(MSG_DEBUG,
			   "DPP: %u/%u bytes of message sent to Controller",
			   (unsigned int) conn->msg_out_pos,
			   (unsigned int) wpabuf_len(conn->msg_out));
		if (!conn->write_eloop &&
		    eloop_register_sock(conn->sock, EVENT_TYPE_WRITE,
					dpp_conn_tx_ready, conn, NULL) == 0)
			conn->write_eloop = 1;
		return 1;
	}

	wpa_printf(MSG_DEBUG, "DPP: Full message sent over TCP");
	wpabuf_free(conn->msg_out);
	conn->msg_out = NULL;
	conn->msg_out_pos = 0;
	eloop_unregister_sock(conn->sock, EVENT_TYPE_WRITE);
	conn->write_eloop = 0;
	if (!conn->read_eloop &&
	    eloop_register_sock(conn->sock, EVENT_TYPE_READ,
				dpp_controller_rx, conn, NULL) == 0)
		conn->read_eloop = 1;
	if (conn->on_tcp_tx_complete_remove) {
		dpp_connection_remove(conn);
	} else if (conn->ctrl && conn->on_tcp_tx_complete_gas_done &&
		   conn->auth) {
		dpp_controller_gas_done(conn);
	} else if (conn->on_tcp_tx_complete_auth_ok) {
		conn->on_tcp_tx_complete_auth_ok = 0;
		dpp_controller_auth_success(conn, 1);
	}

	return 0;
}


static int dpp_tcp_send_msg(struct dpp_connection *conn,
			    const struct wpabuf *msg)
{
	wpabuf_free(conn->msg_out);
	conn->msg_out_pos = 0;
	conn->msg_out = wpabuf_alloc(4 + wpabuf_len(msg) - 1);
	if (!conn->msg_out)
		return -1;
	wpabuf_put_be32(conn->msg_out, wpabuf_len(msg) - 1);
	wpabuf_put_data(conn->msg_out, wpabuf_head_u8(msg) + 1,
			wpabuf_len(msg) - 1);

	if (dpp_tcp_send(conn) == 1) {
		if (!conn->write_eloop) {
			if (eloop_register_sock(conn->sock, EVENT_TYPE_WRITE,
						dpp_conn_tx_ready,
						conn, NULL) < 0)
				return -1;
			conn->write_eloop = 1;
		}
	}

	return 0;
}


static void dpp_controller_start_gas_client(struct dpp_connection *conn)
{
	struct dpp_authentication *auth = conn->auth;
	struct wpabuf *buf;
	int netrole_ap = 0; /* TODO: make this configurable */

	buf = dpp_build_conf_req_helper(auth, "Test", netrole_ap, NULL, NULL);
	if (!buf) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No configuration request data available");
		return;
	}

	dpp_tcp_send_msg(conn, buf);
	wpabuf_free(buf);
}


static void dpp_controller_auth_success(struct dpp_connection *conn,
					int initiator)
{
	struct dpp_authentication *auth = conn->auth;

	if (!auth)
		return;

	wpa_printf(MSG_DEBUG, "DPP: Authentication succeeded");
	wpa_msg(conn->global->msg_ctx, MSG_INFO,
		DPP_EVENT_AUTH_SUCCESS "init=%d", initiator);
#ifdef CONFIG_TESTING_OPTIONS
	if (dpp_test == DPP_TEST_STOP_AT_AUTH_CONF) {
		wpa_printf(MSG_INFO,
			   "DPP: TESTING - stop at Authentication Confirm");
		if (auth->configurator) {
			/* Prevent GAS response */
			auth->auth_success = 0;
		}
		return;
	}
#endif /* CONFIG_TESTING_OPTIONS */

	if (!auth->configurator)
		dpp_controller_start_gas_client(conn);
}


static void dpp_conn_tx_ready(int sock, void *eloop_ctx, void *sock_ctx)
{
	struct dpp_connection *conn = eloop_ctx;

	wpa_printf(MSG_DEBUG, "DPP: TCP socket %d ready for TX", sock);
	dpp_tcp_send(conn);
}


static int dpp_ipaddr_to_sockaddr(struct sockaddr *addr, socklen_t *addrlen,
				  const struct hostapd_ip_addr *ipaddr,
				  int port)
{
	struct sockaddr_in *dst;
#ifdef CONFIG_IPV6
	struct sockaddr_in6 *dst6;
#endif /* CONFIG_IPV6 */

	switch (ipaddr->af) {
	case AF_INET:
		dst = (struct sockaddr_in *) addr;
		os_memset(dst, 0, sizeof(*dst));
		dst->sin_family = AF_INET;
		dst->sin_addr.s_addr = ipaddr->u.v4.s_addr;
		dst->sin_port = htons(port);
		*addrlen = sizeof(*dst);
		break;
#ifdef CONFIG_IPV6
	case AF_INET6:
		dst6 = (struct sockaddr_in6 *) addr;
		os_memset(dst6, 0, sizeof(*dst6));
		dst6->sin6_family = AF_INET6;
		os_memcpy(&dst6->sin6_addr, &ipaddr->u.v6,
			  sizeof(struct in6_addr));
		dst6->sin6_port = htons(port);
		*addrlen = sizeof(*dst6);
		break;
#endif /* CONFIG_IPV6 */
	default:
		return -1;
	}

	return 0;
}


static struct dpp_connection *
dpp_relay_new_conn(struct dpp_relay_controller *ctrl, const u8 *src,
		   unsigned int freq)
{
	struct dpp_connection *conn;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	char txt[100];

	if (dl_list_len(&ctrl->conn) >= 15) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Too many ongoing Relay connections to the Controller - cannot start a new one");
		return NULL;
	}

	if (dpp_ipaddr_to_sockaddr((struct sockaddr *) &addr, &addrlen,
				   &ctrl->ipaddr, DPP_TCP_PORT) < 0)
		return NULL;

	conn = os_zalloc(sizeof(*conn));
	if (!conn)
		return NULL;

	conn->global = ctrl->global;
	conn->relay = ctrl;
	os_memcpy(conn->mac_addr, src, ETH_ALEN);
	conn->freq = freq;

	conn->sock = socket(AF_INET, SOCK_STREAM, 0);
	if (conn->sock < 0)
		goto fail;
	wpa_printf(MSG_DEBUG, "DPP: TCP relay socket %d connection to %s",
		   conn->sock, hostapd_ip_txt(&ctrl->ipaddr, txt, sizeof(txt)));

	if (fcntl(conn->sock, F_SETFL, O_NONBLOCK) != 0) {
		wpa_printf(MSG_DEBUG, "DPP: fnctl(O_NONBLOCK) failed: %s",
			   strerror(errno));
		goto fail;
	}

	if (connect(conn->sock, (struct sockaddr *) &addr, addrlen) < 0) {
		if (errno != EINPROGRESS) {
			wpa_printf(MSG_DEBUG, "DPP: Failed to connect: %s",
				   strerror(errno));
			goto fail;
		}

		/*
		 * Continue connecting in the background; eloop will call us
		 * once the connection is ready (or failed).
		 */
	}

	if (eloop_register_sock(conn->sock, EVENT_TYPE_WRITE,
				dpp_conn_tx_ready, conn, NULL) < 0)
		goto fail;
	conn->write_eloop = 1;

	/* TODO: eloop timeout to clear a connection if it does not complete
	 * properly */

	dl_list_add(&ctrl->conn, &conn->list);
	return conn;
fail:
	dpp_connection_free(conn);
	return NULL;
}


static struct wpabuf * dpp_tcp_encaps(const u8 *hdr, const u8 *buf, size_t len)
{
	struct wpabuf *msg;

	msg = wpabuf_alloc(4 + 1 + DPP_HDR_LEN + len);
	if (!msg)
		return NULL;
	wpabuf_put_be32(msg, 1 + DPP_HDR_LEN + len);
	wpabuf_put_u8(msg, WLAN_PA_VENDOR_SPECIFIC);
	wpabuf_put_data(msg, hdr, DPP_HDR_LEN);
	wpabuf_put_data(msg, buf, len);
	wpa_hexdump_buf(MSG_MSGDUMP, "DPP: Outgoing TCP message", msg);
	return msg;
}


static int dpp_relay_tx(struct dpp_connection *conn, const u8 *hdr,
			const u8 *buf, size_t len)
{
	u8 type = hdr[DPP_HDR_LEN - 1];

	wpa_printf(MSG_DEBUG,
		   "DPP: Continue already established Relay/Controller connection for this session");
	wpabuf_free(conn->msg_out);
	conn->msg_out_pos = 0;
	conn->msg_out = dpp_tcp_encaps(hdr, buf, len);
	if (!conn->msg_out) {
		dpp_connection_remove(conn);
		return -1;
	}

	/* TODO: for proto ver 1, need to do remove connection based on GAS Resp
	 * TX status */
	if (type == DPP_PA_CONFIGURATION_RESULT)
		conn->on_tcp_tx_complete_remove = 1;
	dpp_tcp_send(conn);
	return 0;
}


int dpp_relay_rx_action(struct dpp_global *dpp, const u8 *src, const u8 *hdr,
			const u8 *buf, size_t len, unsigned int freq,
			const u8 *i_bootstrap, const u8 *r_bootstrap)
{
	struct dpp_relay_controller *ctrl;
	struct dpp_connection *conn;
	u8 type = hdr[DPP_HDR_LEN - 1];

	/* Check if there is an already started session for this peer and if so,
	 * continue that session (send this over TCP) and return 0.
	 */
	if (type != DPP_PA_PEER_DISCOVERY_REQ &&
	    type != DPP_PA_PEER_DISCOVERY_RESP &&
	    type != DPP_PA_PRESENCE_ANNOUNCEMENT) {
		dl_list_for_each(ctrl, &dpp->controllers,
				 struct dpp_relay_controller, list) {
			dl_list_for_each(conn, &ctrl->conn,
					 struct dpp_connection, list) {
				if (os_memcmp(src, conn->mac_addr,
					      ETH_ALEN) == 0)
					return dpp_relay_tx(conn, hdr, buf, len);
			}
		}
	}

	if (!r_bootstrap)
		return -1;

	if (type == DPP_PA_PRESENCE_ANNOUNCEMENT) {
		/* TODO: Could send this to all configured Controllers. For now,
		 * only the first Controller is supported. */
		ctrl = dl_list_first(&dpp->controllers,
				     struct dpp_relay_controller, list);
	} else {
		ctrl = dpp_relay_controller_get(dpp, r_bootstrap);
	}
	if (!ctrl)
		return -1;

	wpa_printf(MSG_DEBUG,
		   "DPP: Authentication Request for a configured Controller");
	conn = dpp_relay_new_conn(ctrl, src, freq);
	if (!conn)
		return -1;

	conn->msg_out = dpp_tcp_encaps(hdr, buf, len);
	if (!conn->msg_out) {
		dpp_connection_remove(conn);
		return -1;
	}
	/* Message will be sent in dpp_conn_tx_ready() */

	return 0;
}


int dpp_relay_rx_gas_req(struct dpp_global *dpp, const u8 *src, const u8 *data,
			 size_t data_len)
{
	struct dpp_relay_controller *ctrl;
	struct dpp_connection *conn, *found = NULL;
	struct wpabuf *msg;

	/* Check if there is a successfully completed authentication for this
	 * and if so, continue that session (send this over TCP) and return 0.
	 */
	dl_list_for_each(ctrl, &dpp->controllers,
			 struct dpp_relay_controller, list) {
		if (found)
			break;
		dl_list_for_each(conn, &ctrl->conn,
				 struct dpp_connection, list) {
			if (os_memcmp(src, conn->mac_addr,
				      ETH_ALEN) == 0) {
				found = conn;
				break;
			}
		}
	}

	if (!found)
		return -1;

	msg = wpabuf_alloc(4 + 1 + data_len);
	if (!msg)
		return -1;
	wpabuf_put_be32(msg, 1 + data_len);
	wpabuf_put_u8(msg, WLAN_PA_GAS_INITIAL_REQ);
	wpabuf_put_data(msg, data, data_len);
	wpa_hexdump_buf(MSG_MSGDUMP, "DPP: Outgoing TCP message", msg);

	wpabuf_free(conn->msg_out);
	conn->msg_out_pos = 0;
	conn->msg_out = msg;
	dpp_tcp_send(conn);
	return 0;
}


static void dpp_controller_free(struct dpp_controller *ctrl)
{
	struct dpp_connection *conn, *tmp;

	if (!ctrl)
		return;

	dl_list_for_each_safe(conn, tmp, &ctrl->conn, struct dpp_connection,
			      list)
		dpp_connection_remove(conn);

	if (ctrl->sock >= 0) {
		close(ctrl->sock);
		eloop_unregister_sock(ctrl->sock, EVENT_TYPE_READ);
	}
	os_free(ctrl->configurator_params);
	os_free(ctrl);
}


static int dpp_controller_rx_auth_req(struct dpp_connection *conn,
				      const u8 *hdr, const u8 *buf, size_t len)
{
	const u8 *r_bootstrap, *i_bootstrap;
	u16 r_bootstrap_len, i_bootstrap_len;
	struct dpp_bootstrap_info *own_bi = NULL, *peer_bi = NULL;

	if (!conn->ctrl)
		return 0;

	wpa_printf(MSG_DEBUG, "DPP: Authentication Request");

	r_bootstrap = dpp_get_attr(buf, len, DPP_ATTR_R_BOOTSTRAP_KEY_HASH,
				   &r_bootstrap_len);
	if (!r_bootstrap || r_bootstrap_len != SHA256_MAC_LEN) {
		wpa_printf(MSG_INFO,
			   "Missing or invalid required Responder Bootstrapping Key Hash attribute");
		return -1;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: Responder Bootstrapping Key Hash",
		    r_bootstrap, r_bootstrap_len);

	i_bootstrap = dpp_get_attr(buf, len, DPP_ATTR_I_BOOTSTRAP_KEY_HASH,
				   &i_bootstrap_len);
	if (!i_bootstrap || i_bootstrap_len != SHA256_MAC_LEN) {
		wpa_printf(MSG_INFO,
			   "Missing or invalid required Initiator Bootstrapping Key Hash attribute");
		return -1;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: Initiator Bootstrapping Key Hash",
		    i_bootstrap, i_bootstrap_len);

	/* Try to find own and peer bootstrapping key matches based on the
	 * received hash values */
	dpp_bootstrap_find_pair(conn->ctrl->global, i_bootstrap, r_bootstrap,
				&own_bi, &peer_bi);
	if (!own_bi) {
		wpa_printf(MSG_INFO,
			"No matching own bootstrapping key found - ignore message");
		return -1;
	}

	if (conn->auth) {
		wpa_printf(MSG_INFO,
			   "Already in DPP authentication exchange - ignore new one");
		return 0;
	}

	conn->auth = dpp_auth_req_rx(conn->ctrl->global,
				     conn->ctrl->global->msg_ctx,
				     conn->ctrl->allowed_roles,
				     conn->ctrl->qr_mutual,
				     peer_bi, own_bi, -1, hdr, buf, len);
	if (!conn->auth) {
		wpa_printf(MSG_DEBUG, "DPP: No response generated");
		return -1;
	}

	if (dpp_set_configurator(conn->auth,
				 conn->ctrl->configurator_params) < 0) {
		dpp_connection_remove(conn);
		return -1;
	}

	return dpp_tcp_send_msg(conn, conn->auth->resp_msg);
}


static int dpp_controller_rx_auth_resp(struct dpp_connection *conn,
				       const u8 *hdr, const u8 *buf, size_t len)
{
	struct dpp_authentication *auth = conn->auth;
	struct wpabuf *msg;
	int res;

	if (!auth)
		return -1;

	wpa_printf(MSG_DEBUG, "DPP: Authentication Response");

	msg = dpp_auth_resp_rx(auth, hdr, buf, len);
	if (!msg) {
		if (auth->auth_resp_status == DPP_STATUS_RESPONSE_PENDING) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Start wait for full response");
			return -1;
		}
		wpa_printf(MSG_DEBUG, "DPP: No confirm generated");
		dpp_connection_remove(conn);
		return -1;
	}

	conn->on_tcp_tx_complete_auth_ok = 1;
	res = dpp_tcp_send_msg(conn, msg);
	wpabuf_free(msg);
	return res;
}


static int dpp_controller_rx_auth_conf(struct dpp_connection *conn,
				       const u8 *hdr, const u8 *buf, size_t len)
{
	struct dpp_authentication *auth = conn->auth;

	wpa_printf(MSG_DEBUG, "DPP: Authentication Confirmation");

	if (!auth) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No DPP Authentication in progress - drop");
		return -1;
	}

	if (dpp_auth_conf_rx(auth, hdr, buf, len) < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Authentication failed");
		return -1;
	}

	dpp_controller_auth_success(conn, 0);
	return 0;
}


static void dpp_controller_conn_status_result_wait_timeout(void *eloop_ctx,
							   void *timeout_ctx)
{
	struct dpp_connection *conn = eloop_ctx;

	if (!conn->auth->waiting_conf_result)
		return;

	wpa_printf(MSG_DEBUG,
		   "DPP: Timeout while waiting for Connection Status Result");
	wpa_msg(conn->ctrl->global->msg_ctx, MSG_INFO,
		DPP_EVENT_CONN_STATUS_RESULT "timeout");
	dpp_connection_remove(conn);
}


static int dpp_controller_rx_conf_result(struct dpp_connection *conn,
					 const u8 *hdr, const u8 *buf,
					 size_t len)
{
	struct dpp_authentication *auth = conn->auth;
	enum dpp_status_error status;

	if (!conn->ctrl)
		return 0;

	wpa_printf(MSG_DEBUG, "DPP: Configuration Result");

	if (!auth || !auth->waiting_conf_result) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No DPP Configuration waiting for result - drop");
		return -1;
	}

	status = dpp_conf_result_rx(auth, hdr, buf, len);
	if (status == DPP_STATUS_OK && auth->send_conn_status) {
		wpa_msg(conn->ctrl->global->msg_ctx, MSG_INFO,
			DPP_EVENT_CONF_SENT "wait_conn_status=1");
		wpa_printf(MSG_DEBUG, "DPP: Wait for Connection Status Result");
		eloop_cancel_timeout(
			dpp_controller_conn_status_result_wait_timeout,
			conn, NULL);
		eloop_register_timeout(
			16, 0, dpp_controller_conn_status_result_wait_timeout,
			conn, NULL);
		return 0;
	}
	if (status == DPP_STATUS_OK)
		wpa_msg(conn->ctrl->global->msg_ctx, MSG_INFO,
			DPP_EVENT_CONF_SENT);
	else
		wpa_msg(conn->ctrl->global->msg_ctx, MSG_INFO,
			DPP_EVENT_CONF_FAILED);
	return -1; /* to remove the completed connection */
}


static int dpp_controller_rx_conn_status_result(struct dpp_connection *conn,
						const u8 *hdr, const u8 *buf,
						size_t len)
{
	struct dpp_authentication *auth = conn->auth;
	enum dpp_status_error status;
	u8 ssid[SSID_MAX_LEN];
	size_t ssid_len = 0;
	char *channel_list = NULL;

	if (!conn->ctrl)
		return 0;

	wpa_printf(MSG_DEBUG, "DPP: Connection Status Result");

	if (!auth || !auth->waiting_conn_status_result) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No DPP Configuration waiting for connection status result - drop");
		return -1;
	}

	status = dpp_conn_status_result_rx(auth, hdr, buf, len,
					   ssid, &ssid_len, &channel_list);
	wpa_msg(conn->ctrl->global->msg_ctx, MSG_INFO,
		DPP_EVENT_CONN_STATUS_RESULT
		"result=%d ssid=%s channel_list=%s",
		status, wpa_ssid_txt(ssid, ssid_len),
		channel_list ? channel_list : "N/A");
	os_free(channel_list);
	return -1; /* to remove the completed connection */
}


static int dpp_controller_rx_presence_announcement(struct dpp_connection *conn,
						   const u8 *hdr, const u8 *buf,
						   size_t len)
{
	const u8 *r_bootstrap;
	u16 r_bootstrap_len;
	struct dpp_bootstrap_info *peer_bi;
	struct dpp_authentication *auth;
	struct dpp_global *dpp = conn->ctrl->global;

	if (conn->auth) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Ignore Presence Announcement during ongoing Authentication");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "DPP: Presence Announcement");

	r_bootstrap = dpp_get_attr(buf, len, DPP_ATTR_R_BOOTSTRAP_KEY_HASH,
				   &r_bootstrap_len);
	if (!r_bootstrap || r_bootstrap_len != SHA256_MAC_LEN) {
		wpa_msg(dpp->msg_ctx, MSG_INFO, DPP_EVENT_FAIL
			"Missing or invalid required Responder Bootstrapping Key Hash attribute");
		return -1;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: Responder Bootstrapping Key Hash",
		    r_bootstrap, r_bootstrap_len);
	peer_bi = dpp_bootstrap_find_chirp(dpp, r_bootstrap);
	if (!peer_bi) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No matching bootstrapping information found");
		return -1;
	}

	auth = dpp_auth_init(dpp, dpp->msg_ctx, peer_bi, NULL,
			     DPP_CAPAB_CONFIGURATOR, -1, NULL, 0);
	if (!auth)
		return -1;
	if (dpp_set_configurator(conn->auth,
				 conn->ctrl->configurator_params) < 0) {
		dpp_auth_deinit(auth);
		dpp_connection_remove(conn);
		return -1;
	}

	conn->auth = auth;
	return dpp_tcp_send_msg(conn, conn->auth->req_msg);
}


static int dpp_controller_rx_reconfig_announcement(struct dpp_connection *conn,
						   const u8 *hdr, const u8 *buf,
						   size_t len)
{
	const u8 *csign_hash;
	u16 csign_hash_len;
	struct dpp_configurator *conf;
	struct dpp_global *dpp = conn->ctrl->global;

	if (conn->auth) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Ignore Reconfig Announcement during ongoing Authentication");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "DPP: Reconfig Announcement");

	csign_hash = dpp_get_attr(buf, len, DPP_ATTR_C_SIGN_KEY_HASH,
				  &csign_hash_len);
	if (!csign_hash || csign_hash_len != SHA256_MAC_LEN) {
		wpa_msg(dpp->msg_ctx, MSG_INFO, DPP_EVENT_FAIL
			"Missing or invalid required Configurator C-sign key Hash attribute");
		return -1;
	}
	wpa_hexdump(MSG_MSGDUMP, "DPP: Configurator C-sign key Hash (kid)",
		    csign_hash, csign_hash_len);
	conf = dpp_configurator_find_kid(dpp, csign_hash);
	if (!conf) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No matching Configurator information found");
		return -1;
	}

	/* TODO: Initiate Reconfig Authentication */
	return -1;
}


static int dpp_controller_rx_action(struct dpp_connection *conn, const u8 *msg,
				    size_t len)
{
	const u8 *pos, *end;
	u8 type;

	wpa_printf(MSG_DEBUG, "DPP: Received DPP Action frame over TCP");
	pos = msg;
	end = msg + len;

	if (end - pos < DPP_HDR_LEN ||
	    WPA_GET_BE24(pos) != OUI_WFA ||
	    pos[3] != DPP_OUI_TYPE) {
		wpa_printf(MSG_DEBUG, "DPP: Unrecognized header");
		return -1;
	}

	if (pos[4] != 1) {
		wpa_printf(MSG_DEBUG, "DPP: Unsupported Crypto Suite %u",
			   pos[4]);
		return -1;
	}
	type = pos[5];
	wpa_printf(MSG_DEBUG, "DPP: Received message type %u", type);
	pos += DPP_HDR_LEN;

	wpa_hexdump(MSG_MSGDUMP, "DPP: Received message attributes",
		    pos, end - pos);
	if (dpp_check_attrs(pos, end - pos) < 0)
		return -1;

	if (conn->relay) {
		wpa_printf(MSG_DEBUG, "DPP: Relay - send over WLAN");
		conn->relay->tx(conn->relay->cb_ctx, conn->mac_addr,
				conn->freq, msg, len);
		return 0;
	}

	switch (type) {
	case DPP_PA_AUTHENTICATION_REQ:
		return dpp_controller_rx_auth_req(conn, msg, pos, end - pos);
	case DPP_PA_AUTHENTICATION_RESP:
		return dpp_controller_rx_auth_resp(conn, msg, pos, end - pos);
	case DPP_PA_AUTHENTICATION_CONF:
		return dpp_controller_rx_auth_conf(conn, msg, pos, end - pos);
	case DPP_PA_CONFIGURATION_RESULT:
		return dpp_controller_rx_conf_result(conn, msg, pos, end - pos);
	case DPP_PA_CONNECTION_STATUS_RESULT:
		return dpp_controller_rx_conn_status_result(conn, msg, pos,
							    end - pos);
	case DPP_PA_PRESENCE_ANNOUNCEMENT:
		return dpp_controller_rx_presence_announcement(conn, msg, pos,
							       end - pos);
	case DPP_PA_RECONFIG_ANNOUNCEMENT:
		return dpp_controller_rx_reconfig_announcement(conn, msg, pos,
							       end - pos);
	default:
		/* TODO: missing messages types */
		wpa_printf(MSG_DEBUG,
			   "DPP: Unsupported frame subtype %d", type);
		return -1;
	}
}


static int dpp_controller_rx_gas_req(struct dpp_connection *conn, const u8 *msg,
				     size_t len)
{
	const u8 *pos, *end, *next;
	u8 dialog_token;
	const u8 *adv_proto;
	u16 slen;
	struct wpabuf *resp, *buf;
	struct dpp_authentication *auth = conn->auth;

	if (len < 1 + 2)
		return -1;

	wpa_printf(MSG_DEBUG,
		   "DPP: Received DPP Configuration Request over TCP");

	if (!conn->ctrl || !auth || !auth->auth_success) {
		wpa_printf(MSG_DEBUG, "DPP: No matching exchange in progress");
		return -1;
	}

	pos = msg;
	end = msg + len;

	dialog_token = *pos++;
	adv_proto = pos++;
	slen = *pos++;
	if (*adv_proto != WLAN_EID_ADV_PROTO ||
	    slen > end - pos || slen < 2)
		return -1;

	next = pos + slen;
	pos++; /* skip QueryRespLenLimit and PAME-BI */

	if (slen != 8 || *pos != WLAN_EID_VENDOR_SPECIFIC ||
	    pos[1] != 5 || WPA_GET_BE24(&pos[2]) != OUI_WFA ||
	    pos[5] != DPP_OUI_TYPE || pos[6] != 0x01)
		return -1;

	pos = next;
	/* Query Request */
	if (end - pos < 2)
		return -1;
	slen = WPA_GET_LE16(pos);
	pos += 2;
	if (slen > end - pos)
		return -1;

	resp = dpp_conf_req_rx(auth, pos, slen);
	if (!resp)
		return -1;

	buf = wpabuf_alloc(4 + 18 + wpabuf_len(resp));
	if (!buf) {
		wpabuf_free(resp);
		return -1;
	}

	wpabuf_put_be32(buf, 18 + wpabuf_len(resp));

	wpabuf_put_u8(buf, WLAN_PA_GAS_INITIAL_RESP);
	wpabuf_put_u8(buf, dialog_token);
	wpabuf_put_le16(buf, WLAN_STATUS_SUCCESS);
	wpabuf_put_le16(buf, 0); /* GAS Comeback Delay */

	dpp_write_adv_proto(buf);
	dpp_write_gas_query(buf, resp);
	wpabuf_free(resp);

	/* Send Config Response over TCP; GAS fragmentation is taken care of by
	 * the Relay */
	wpa_hexdump_buf(MSG_MSGDUMP, "DPP: Outgoing TCP message", buf);
	wpabuf_free(conn->msg_out);
	conn->msg_out_pos = 0;
	conn->msg_out = buf;
	conn->on_tcp_tx_complete_gas_done = 1;
	dpp_tcp_send(conn);
	return 0;
}


static int dpp_tcp_rx_gas_resp(struct dpp_connection *conn, struct wpabuf *resp)
{
	struct dpp_authentication *auth = conn->auth;
	int res;
	struct wpabuf *msg;
	enum dpp_status_error status;

	wpa_printf(MSG_DEBUG,
		   "DPP: Configuration Response for local stack from TCP");

	res = dpp_conf_resp_rx(auth, resp);
	wpabuf_free(resp);
	if (res < 0) {
		wpa_printf(MSG_DEBUG, "DPP: Configuration attempt failed");
		return -1;
	}

	if (conn->global->process_conf_obj)
		res = conn->global->process_conf_obj(conn->global->cb_ctx,
						     auth);
	else
		res = 0;

	if (auth->peer_version < 2 || auth->conf_resp_status != DPP_STATUS_OK)
		return -1;

	wpa_printf(MSG_DEBUG, "DPP: Send DPP Configuration Result");
	status = res < 0 ? DPP_STATUS_CONFIG_REJECTED : DPP_STATUS_OK;
	msg = dpp_build_conf_result(auth, status);
	if (!msg)
		return -1;

	conn->on_tcp_tx_complete_remove = 1;
	res = dpp_tcp_send_msg(conn, msg);
	wpabuf_free(msg);

	/* This exchange will be terminated in the TX status handler */

	return res;
}


static int dpp_rx_gas_resp(struct dpp_connection *conn, const u8 *msg,
			   size_t len)
{
	struct wpabuf *buf;
	u8 dialog_token;
	const u8 *pos, *end, *next, *adv_proto;
	u16 status, slen;

	if (len < 5 + 2)
		return -1;

	wpa_printf(MSG_DEBUG,
		   "DPP: Received DPP Configuration Response over TCP");

	pos = msg;
	end = msg + len;

	dialog_token = *pos++;
	status = WPA_GET_LE16(pos);
	if (status != WLAN_STATUS_SUCCESS) {
		wpa_printf(MSG_DEBUG, "DPP: Unexpected Status Code %u", status);
		return -1;
	}
	pos += 2;
	pos += 2; /* ignore GAS Comeback Delay */

	adv_proto = pos++;
	slen = *pos++;
	if (*adv_proto != WLAN_EID_ADV_PROTO ||
	    slen > end - pos || slen < 2)
		return -1;

	next = pos + slen;
	pos++; /* skip QueryRespLenLimit and PAME-BI */

	if (slen != 8 || *pos != WLAN_EID_VENDOR_SPECIFIC ||
	    pos[1] != 5 || WPA_GET_BE24(&pos[2]) != OUI_WFA ||
	    pos[5] != DPP_OUI_TYPE || pos[6] != 0x01)
		return -1;

	pos = next;
	/* Query Response */
	if (end - pos < 2)
		return -1;
	slen = WPA_GET_LE16(pos);
	pos += 2;
	if (slen > end - pos)
		return -1;

	buf = wpabuf_alloc(slen);
	if (!buf)
		return -1;
	wpabuf_put_data(buf, pos, slen);

	if (!conn->relay && !conn->ctrl)
		return dpp_tcp_rx_gas_resp(conn, buf);

	if (!conn->relay) {
		wpa_printf(MSG_DEBUG, "DPP: No matching exchange in progress");
		wpabuf_free(buf);
		return -1;
	}
	wpa_printf(MSG_DEBUG, "DPP: Relay - send over WLAN");
	conn->relay->gas_resp_tx(conn->relay->cb_ctx, conn->mac_addr,
				 dialog_token, 0, buf);

	return 0;
}


static void dpp_controller_rx(int sd, void *eloop_ctx, void *sock_ctx)
{
	struct dpp_connection *conn = eloop_ctx;
	int res;
	const u8 *pos;

	wpa_printf(MSG_DEBUG, "DPP: TCP data available for reading (sock %d)",
		   sd);

	if (conn->msg_len_octets < 4) {
		u32 msglen;

		res = recv(sd, &conn->msg_len[conn->msg_len_octets],
			   4 - conn->msg_len_octets, 0);
		if (res < 0) {
			wpa_printf(MSG_DEBUG, "DPP: recv failed: %s",
				   strerror(errno));
			dpp_connection_remove(conn);
			return;
		}
		if (res == 0) {
			wpa_printf(MSG_DEBUG,
				   "DPP: No more data available over TCP");
			dpp_connection_remove(conn);
			return;
		}
		wpa_printf(MSG_DEBUG,
			   "DPP: Received %d/%d octet(s) of message length field",
			   res, (int) (4 - conn->msg_len_octets));
		conn->msg_len_octets += res;

		if (conn->msg_len_octets < 4) {
			wpa_printf(MSG_DEBUG,
				   "DPP: Need %d more octets of message length field",
				   (int) (4 - conn->msg_len_octets));
			return;
		}

		msglen = WPA_GET_BE32(conn->msg_len);
		wpa_printf(MSG_DEBUG, "DPP: Message length: %u", msglen);
		if (msglen > 65535) {
			wpa_printf(MSG_INFO, "DPP: Unexpectedly long message");
			dpp_connection_remove(conn);
			return;
		}

		wpabuf_free(conn->msg);
		conn->msg = wpabuf_alloc(msglen);
	}

	if (!conn->msg) {
		wpa_printf(MSG_DEBUG,
			   "DPP: No buffer available for receiving the message");
		dpp_connection_remove(conn);
		return;
	}

	wpa_printf(MSG_DEBUG, "DPP: Need %u more octets of message payload",
		   (unsigned int) wpabuf_tailroom(conn->msg));

	res = recv(sd, wpabuf_put(conn->msg, 0), wpabuf_tailroom(conn->msg), 0);
	if (res < 0) {
		wpa_printf(MSG_DEBUG, "DPP: recv failed: %s", strerror(errno));
		dpp_connection_remove(conn);
		return;
	}
	if (res == 0) {
		wpa_printf(MSG_DEBUG, "DPP: No more data available over TCP");
		dpp_connection_remove(conn);
		return;
	}
	wpa_printf(MSG_DEBUG, "DPP: Received %d octets", res);
	wpabuf_put(conn->msg, res);

	if (wpabuf_tailroom(conn->msg) > 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Need %u more octets of message payload",
			   (unsigned int) wpabuf_tailroom(conn->msg));
		return;
	}

	conn->msg_len_octets = 0;
	wpa_hexdump_buf(MSG_DEBUG, "DPP: Received TCP message", conn->msg);
	if (wpabuf_len(conn->msg) < 1) {
		dpp_connection_remove(conn);
		return;
	}

	pos = wpabuf_head(conn->msg);
	switch (*pos) {
	case WLAN_PA_VENDOR_SPECIFIC:
		if (dpp_controller_rx_action(conn, pos + 1,
					     wpabuf_len(conn->msg) - 1) < 0)
			dpp_connection_remove(conn);
		break;
	case WLAN_PA_GAS_INITIAL_REQ:
		if (dpp_controller_rx_gas_req(conn, pos + 1,
					      wpabuf_len(conn->msg) - 1) < 0)
			dpp_connection_remove(conn);
		break;
	case WLAN_PA_GAS_INITIAL_RESP:
		if (dpp_rx_gas_resp(conn, pos + 1,
				    wpabuf_len(conn->msg) - 1) < 0)
			dpp_connection_remove(conn);
		break;
	default:
		wpa_printf(MSG_DEBUG, "DPP: Ignore unsupported message type %u",
			   *pos);
		break;
	}
}


static void dpp_controller_tcp_cb(int sd, void *eloop_ctx, void *sock_ctx)
{
	struct dpp_controller *ctrl = eloop_ctx;
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int fd;
	struct dpp_connection *conn;

	wpa_printf(MSG_DEBUG, "DPP: New TCP connection");

	fd = accept(ctrl->sock, (struct sockaddr *) &addr, &addr_len);
	if (fd < 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: Failed to accept new connection: %s",
			   strerror(errno));
		return;
	}
	wpa_printf(MSG_DEBUG, "DPP: Connection from %s:%d",
		   inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	conn = os_zalloc(sizeof(*conn));
	if (!conn)
		goto fail;

	conn->global = ctrl->global;
	conn->ctrl = ctrl;
	conn->sock = fd;

	if (fcntl(conn->sock, F_SETFL, O_NONBLOCK) != 0) {
		wpa_printf(MSG_DEBUG, "DPP: fnctl(O_NONBLOCK) failed: %s",
			   strerror(errno));
		goto fail;
	}

	if (eloop_register_sock(conn->sock, EVENT_TYPE_READ,
				dpp_controller_rx, conn, NULL) < 0)
		goto fail;
	conn->read_eloop = 1;

	/* TODO: eloop timeout to expire connections that do not complete in
	 * reasonable time */
	dl_list_add(&ctrl->conn, &conn->list);
	return;

fail:
	close(fd);
	os_free(conn);
}


int dpp_tcp_init(struct dpp_global *dpp, struct dpp_authentication *auth,
		 const struct hostapd_ip_addr *addr, int port)
{
	struct dpp_connection *conn;
	struct sockaddr_storage saddr;
	socklen_t addrlen;
	const u8 *hdr, *pos, *end;
	char txt[100];

	wpa_printf(MSG_DEBUG, "DPP: Initialize TCP connection to %s port %d",
		   hostapd_ip_txt(addr, txt, sizeof(txt)), port);
	if (dpp_ipaddr_to_sockaddr((struct sockaddr *) &saddr, &addrlen,
				   addr, port) < 0) {
		dpp_auth_deinit(auth);
		return -1;
	}

	conn = os_zalloc(sizeof(*conn));
	if (!conn) {
		dpp_auth_deinit(auth);
		return -1;
	}

	conn->global = dpp;
	conn->auth = auth;
	conn->sock = socket(AF_INET, SOCK_STREAM, 0);
	if (conn->sock < 0)
		goto fail;

	if (fcntl(conn->sock, F_SETFL, O_NONBLOCK) != 0) {
		wpa_printf(MSG_DEBUG, "DPP: fnctl(O_NONBLOCK) failed: %s",
			   strerror(errno));
		goto fail;
	}

	if (connect(conn->sock, (struct sockaddr *) &saddr, addrlen) < 0) {
		if (errno != EINPROGRESS) {
			wpa_printf(MSG_DEBUG, "DPP: Failed to connect: %s",
				   strerror(errno));
			goto fail;
		}

		/*
		 * Continue connecting in the background; eloop will call us
		 * once the connection is ready (or failed).
		 */
	}

	if (eloop_register_sock(conn->sock, EVENT_TYPE_WRITE,
				dpp_conn_tx_ready, conn, NULL) < 0)
		goto fail;
	conn->write_eloop = 1;

	hdr = wpabuf_head(auth->req_msg);
	end = hdr + wpabuf_len(auth->req_msg);
	hdr += 2; /* skip Category and Actiom */
	pos = hdr + DPP_HDR_LEN;
	conn->msg_out = dpp_tcp_encaps(hdr, pos, end - pos);
	if (!conn->msg_out)
		goto fail;
	/* Message will be sent in dpp_conn_tx_ready() */

	/* TODO: eloop timeout to clear a connection if it does not complete
	 * properly */
	dl_list_add(&dpp->tcp_init, &conn->list);
	return 0;
fail:
	dpp_connection_free(conn);
	return -1;
}


int dpp_controller_start(struct dpp_global *dpp,
			 struct dpp_controller_config *config)
{
	struct dpp_controller *ctrl;
	int on = 1;
	struct sockaddr_in sin;
	int port;

	if (!dpp || dpp->controller)
		return -1;

	ctrl = os_zalloc(sizeof(*ctrl));
	if (!ctrl)
		return -1;
	ctrl->global = dpp;
	if (config->configurator_params)
		ctrl->configurator_params =
			os_strdup(config->configurator_params);
	dl_list_init(&ctrl->conn);
	/* TODO: configure these somehow */
	ctrl->allowed_roles = DPP_CAPAB_ENROLLEE | DPP_CAPAB_CONFIGURATOR;
	ctrl->qr_mutual = 0;

	ctrl->sock = socket(AF_INET, SOCK_STREAM, 0);
	if (ctrl->sock < 0)
		goto fail;

	if (setsockopt(ctrl->sock, SOL_SOCKET, SO_REUSEADDR,
		       &on, sizeof(on)) < 0) {
		wpa_printf(MSG_DEBUG,
			   "DPP: setsockopt(SO_REUSEADDR) failed: %s",
			   strerror(errno));
		/* try to continue anyway */
	}

	if (fcntl(ctrl->sock, F_SETFL, O_NONBLOCK) < 0) {
		wpa_printf(MSG_INFO, "DPP: fnctl(O_NONBLOCK) failed: %s",
			   strerror(errno));
		goto fail;
	}

	/* TODO: IPv6 */
	os_memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;
	port = config->tcp_port ? config->tcp_port : DPP_TCP_PORT;
	sin.sin_port = htons(port);
	if (bind(ctrl->sock, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		wpa_printf(MSG_INFO,
			   "DPP: Failed to bind Controller TCP port: %s",
			   strerror(errno));
		goto fail;
	}
	if (listen(ctrl->sock, 10 /* max backlog */) < 0 ||
	    fcntl(ctrl->sock, F_SETFL, O_NONBLOCK) < 0 ||
	    eloop_register_sock(ctrl->sock, EVENT_TYPE_READ,
				dpp_controller_tcp_cb, ctrl, NULL))
		goto fail;

	dpp->controller = ctrl;
	wpa_printf(MSG_DEBUG, "DPP: Controller started on TCP port %d", port);
	return 0;
fail:
	dpp_controller_free(ctrl);
	return -1;
}


void dpp_controller_stop(struct dpp_global *dpp)
{
	if (dpp) {
		dpp_controller_free(dpp->controller);
		dpp->controller = NULL;
	}
}


struct wpabuf * dpp_build_presence_announcement(struct dpp_bootstrap_info *bi)
{
	struct wpabuf *msg;

	wpa_printf(MSG_DEBUG, "DPP: Build Presence Announcement frame");

	msg = dpp_alloc_msg(DPP_PA_PRESENCE_ANNOUNCEMENT, 4 + SHA256_MAC_LEN);
	if (!msg)
		return NULL;

	/* Responder Bootstrapping Key Hash */
	dpp_build_attr_r_bootstrap_key_hash(msg, bi->pubkey_hash_chirp);
	wpa_hexdump_buf(MSG_DEBUG,
			"DPP: Presence Announcement frame attributes", msg);
	return msg;
}

#endif /* CONFIG_DPP2 */
