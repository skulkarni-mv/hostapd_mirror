/*
 * Simultaneous authentication of equals
 * Copyright (c) 2012, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"

#include "common.h"
#include "crypto/crypto.h"
#include "crypto/sha256.h"
#include "crypto/random.h"
#include "ieee802_11_defs.h"
#include "sae.h"


static const u8 group19_prime[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static const u8 group19_order[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
	0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51
};


static int val_zero_or_one(const u8 *val, size_t len)
{
	size_t i;

	for (i = 0; i < len - 1; i++) {
		if (val[i])
			return 0;
	}

	return val[len - 1] <= 1;
}


static int val_zero(const u8 *val, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		if (val[i])
			return 0;
	}
	return 1;
}


static int sae_get_rand(u8 *val)
{
	int iter = 0;

	do {
		if (random_get_bytes(val, sizeof(group19_prime)) < 0)
			return -1;
		if (iter++ > 100)
			return -1;
	} while (os_memcmp(val, group19_order, sizeof(group19_prime)) >= 0 ||
		 val_zero_or_one(val, sizeof(group19_prime)));

	return 0;
}


static void sae_pwd_seed_key(const u8 *addr1, const u8 *addr2, u8 *key)
{
	wpa_printf(MSG_DEBUG, "SAE: PWE derivation - addr1=" MACSTR
		   " addr2=" MACSTR, MAC2STR(addr1), MAC2STR(addr2));
	if (os_memcmp(addr1, addr2, ETH_ALEN) > 0) {
		os_memcpy(key, addr1, ETH_ALEN);
		os_memcpy(key + ETH_ALEN, addr2, ETH_ALEN);
	} else {
		os_memcpy(key, addr2, ETH_ALEN);
		os_memcpy(key + ETH_ALEN, addr1, ETH_ALEN);
	}
}


static int sae_test_pwd_seed(struct crypto_ec *ecc, const u8 *pwd_seed,
			     struct crypto_ec_point *pwe, u8 *pwe_bin)
{
	u8 pwd_value[32];
	struct crypto_bignum *x;
	int y_bit;

	wpa_hexdump_key(MSG_DEBUG, "SAE: pwd-seed", pwd_seed, 32);

	/* pwd-value = KDF-z(pwd-seed, "SAE Hunting and Pecking", p) */
	sha256_prf(pwd_seed, 32, "SAE Hunting and Pecking",
		   group19_prime, sizeof(group19_prime),
		   pwd_value, sizeof(pwd_value));
	wpa_hexdump_key(MSG_DEBUG, "SAE: pwd-value",
			pwd_value, sizeof(pwd_value));

	if (os_memcmp(pwd_value, group19_prime, sizeof(group19_prime)) >= 0)
		return 0;

	y_bit = pwd_seed[SHA256_MAC_LEN - 1] & 0x01;

	x = crypto_bignum_init_set(pwd_value, sizeof(pwd_value));
	if (x == NULL)
		return -1;
	if (crypto_ec_point_solve_y_coord(ecc, pwe, x, y_bit) < 0) {
		crypto_bignum_deinit(x, 0);
		wpa_printf(MSG_DEBUG, "SAE: No solution found");
		return 0;
	}
	crypto_bignum_deinit(x, 0);

	wpa_printf(MSG_DEBUG, "SAE: PWE found");

	if (crypto_ec_point_to_bin(ecc, pwe, pwe_bin, pwe_bin + 32) < 0)
		return -1;

	wpa_hexdump_key(MSG_DEBUG, "SAE: PWE x", pwe_bin, 32);
	wpa_hexdump_key(MSG_DEBUG, "SAE: PWE y", pwe_bin + 32, 32);
	return 1;
}


static int sae_derive_pwe(struct crypto_ec *ecc, const u8 *addr1,
			  const u8 *addr2, const u8 *password,
			  size_t password_len, struct crypto_ec_point *pwe,
			  u8 *pwe_bin)
{
	u8 counter, k = 4;
	u8 addrs[2 * ETH_ALEN];
	const u8 *addr[2];
	size_t len[2];
	int found = 0;
	struct crypto_ec_point *pwe_tmp;
	u8 pwe_bin_tmp[2 * 32];

	pwe_tmp = crypto_ec_point_init(ecc);
	if (pwe_tmp == NULL)
		return -1;

	wpa_hexdump_ascii_key(MSG_DEBUG, "SAE: password",
			      password, password_len);

	/*
	 * H(salt, ikm) = HMAC-SHA256(salt, ikm)
	 * pwd-seed = H(MAX(STA-A-MAC, STA-B-MAC) || MIN(STA-A-MAC, STA-B-MAC),
	 *              password || counter)
	 */
	sae_pwd_seed_key(addr1, addr2, addrs);

	addr[0] = password;
	len[0] = password_len;
	addr[1] = &counter;
	len[1] = sizeof(counter);

	/*
	 * Continue for at least k iterations to protect against side-channel
	 * attacks that attempt to determine the number of iterations required
	 * in the loop.
	 */
	for (counter = 1; counter < k || !found; counter++) {
		u8 pwd_seed[SHA256_MAC_LEN];
		int res;

		wpa_printf(MSG_DEBUG, "SAE: counter = %u", counter);
		if (hmac_sha256_vector(addrs, sizeof(addrs), 2, addr, len,
				       pwd_seed) < 0)
			break;
		res = sae_test_pwd_seed(ecc, pwd_seed,
					found ? pwe_tmp : pwe,
					found ? pwe_bin_tmp : pwe_bin);
		if (res < 0)
			break;
		if (res == 0)
			continue;
		if (found) {
			wpa_printf(MSG_DEBUG, "SAE: Ignore this PWE (one was "
				   "already selected)");
		} else {
			wpa_printf(MSG_DEBUG, "SAE: Use this PWE");
			found = 1;
		}

		if (counter > 200) {
			/* This should not happen in practice */
			wpa_printf(MSG_DEBUG, "SAE: Failed to derive PWE");
			break;
		}
	}

	crypto_ec_point_deinit(pwe_tmp, 1);

	return found ? 0 : -1;
}


static int sae_derive_commit(struct sae_data *sae, struct crypto_ec *ecc,
			     struct crypto_ec_point *pwe)
{
	struct crypto_bignum *x, *bn_rand, *bn_mask, *order;
	struct crypto_ec_point *elem;
	u8 mask[32];
	int ret = -1;

	if (sae_get_rand(sae->sae_rand) < 0 || sae_get_rand(mask) < 0)
		return -1;
	wpa_hexdump_key(MSG_DEBUG, "SAE: rand",
			sae->sae_rand, sizeof(sae->sae_rand));
	wpa_hexdump_key(MSG_DEBUG, "SAE: mask", mask, sizeof(mask));

	x = crypto_bignum_init();
	bn_rand = crypto_bignum_init_set(sae->sae_rand, 32);
	bn_mask = crypto_bignum_init_set(mask, sizeof(mask));
	order = crypto_bignum_init_set(group19_order, sizeof(group19_order));
	elem = crypto_ec_point_init(ecc);
	if (x == NULL || bn_rand == NULL || bn_mask == NULL || order == NULL ||
	    elem == NULL)
		goto fail;

	/* commit-scalar = (rand + mask) modulo r */
	crypto_bignum_add(bn_rand, bn_mask, x);
	crypto_bignum_mod(x, order, x);
	crypto_bignum_to_bin(x, sae->own_commit_scalar,
			     sizeof(sae->own_commit_scalar), 32);
	wpa_hexdump(MSG_DEBUG, "SAE: commit-scalar",
		    sae->own_commit_scalar, 32);

	/* COMMIT-ELEMENT = inverse(scalar-op(mask, PWE)) */
	if (crypto_ec_point_mul(ecc, pwe, bn_mask, elem) < 0 ||
	    crypto_ec_point_invert(ecc, elem) < 0 ||
	    crypto_ec_point_to_bin(ecc, elem, sae->own_commit_element,
				   sae->own_commit_element + 32) < 0)
		goto fail;

	wpa_hexdump(MSG_DEBUG, "SAE: commit-element x",
		    sae->own_commit_element, 32);
	wpa_hexdump(MSG_DEBUG, "SAE: commit-element y",
		    sae->own_commit_element + 32, 32);

	ret = 0;
fail:
	crypto_ec_point_deinit(elem, 0);
	crypto_bignum_deinit(order, 0);
	crypto_bignum_deinit(bn_mask, 1);
	os_memset(mask, 0, sizeof(mask));
	crypto_bignum_deinit(bn_rand, 1);
	crypto_bignum_deinit(x, 1);
	return ret;
}


int sae_prepare_commit(const u8 *addr1, const u8 *addr2,
		       const u8 *password, size_t password_len,
		       struct sae_data *sae)
{
	struct crypto_ec *ecc;
	struct crypto_ec_point *pwe;
	int ret = 0;

	ecc = crypto_ec_init(19);
	pwe = crypto_ec_point_init(ecc);
	if (ecc == NULL || pwe == NULL ||
	    sae_derive_pwe(ecc, addr1, addr2, password, password_len, pwe,
			   sae->pwe) < 0 ||
	    sae_derive_commit(sae, ecc, pwe) < 0)
		ret = -1;

	crypto_ec_point_deinit(pwe, 1);
	crypto_ec_deinit(ecc);

	return ret;
}


static int sae_check_peer_commit(struct sae_data *sae)
{
	/* 0 < scalar < r */
	if (val_zero(sae->peer_commit_scalar, 32) ||
	    os_memcmp(sae->peer_commit_scalar, group19_order,
		      sizeof(group19_prime)) >= 0) {
		wpa_printf(MSG_DEBUG, "SAE: Invalid peer scalar");
		return -1;
	}

	/* element x and y coordinates < p */
	if (os_memcmp(sae->peer_commit_element, group19_prime,
		      sizeof(group19_prime)) >= 0 ||
	    os_memcmp(sae->peer_commit_element + 32, group19_prime,
		      sizeof(group19_prime)) >= 0) {
		wpa_printf(MSG_DEBUG, "SAE: Invalid coordinates in peer "
			   "element");
		return -1;
	}

	return 0;
}


static int sae_derive_k(struct sae_data *sae, u8 *k)
{
	struct crypto_ec *ecc;
	struct crypto_ec_point *pwe, *peer_elem, *K;
	struct crypto_bignum *rand_bn, *peer_scalar;
	int ret = -1;

	ecc = crypto_ec_init(19);
	if (ecc == NULL)
		return -1;
	pwe = crypto_ec_point_from_bin(ecc, sae->pwe);
	peer_scalar = crypto_bignum_init_set(sae->peer_commit_scalar, 32);
	peer_elem = crypto_ec_point_from_bin(ecc, sae->peer_commit_element);
	K = crypto_ec_point_init(ecc);
	rand_bn = crypto_bignum_init_set(sae->sae_rand, 32);
	if (pwe == NULL || peer_elem == NULL || peer_scalar == NULL ||
	    K == NULL || rand_bn == NULL)
		goto fail;

	if (!crypto_ec_point_is_on_curve(ecc, peer_elem)) {
		wpa_printf(MSG_DEBUG, "SAE: Peer element is not on curve");
		goto fail;
	}

	/*
	 * K = scalar-op(rand, (elem-op(scalar-op(peer-commit-scalar, PWE),
	 *                                        PEER-COMMIT-ELEMENT)))
	 * If K is identity element (point-at-infinity), reject
	 * k = F(K) (= x coordinate)
	 */

	if (crypto_ec_point_mul(ecc, pwe, peer_scalar, K) < 0 ||
	    crypto_ec_point_add(ecc, K, peer_elem, K) < 0 ||
	    crypto_ec_point_mul(ecc, K, rand_bn, K) < 0 ||
	    crypto_ec_point_is_at_infinity(ecc, K) ||
	    crypto_ec_point_to_bin(ecc, K, k, NULL) < 0) {
		wpa_printf(MSG_DEBUG, "SAE: Failed to calculate K and k");
		goto fail;
	}

	wpa_hexdump_key(MSG_DEBUG, "SAE: k", k, 32);

	ret = 0;
fail:
	crypto_ec_point_deinit(pwe, 1);
	crypto_ec_point_deinit(peer_elem, 0);
	crypto_ec_point_deinit(K, 1);
	crypto_bignum_deinit(rand_bn, 1);
	crypto_ec_deinit(ecc);
	return ret;
}


static int sae_derive_keys(struct sae_data *sae, const u8 *k)
{
	u8 null_key[32], val[32];
	u8 keyseed[SHA256_MAC_LEN];
	u8 keys[32 + 32];
	struct crypto_bignum *order, *own_scalar, *peer_scalar, *tmp;
	int ret = -1;

	order = crypto_bignum_init_set(group19_order, sizeof(group19_order));
	own_scalar = crypto_bignum_init_set(sae->own_commit_scalar, 32);
	peer_scalar = crypto_bignum_init_set(sae->peer_commit_scalar, 32);
	tmp = crypto_bignum_init();
	if (order == NULL || own_scalar == NULL || peer_scalar == NULL ||
	    tmp == NULL)
		goto fail;

	/* keyseed = H(<0>32, k)
	 * KCK || PMK = KDF-512(keyseed, "SAE KCK and PMK",
	 *                      (commit-scalar + peer-commit-scalar) modulo r)
	 * PMKID = L((commit-scalar + peer-commit-scalar) modulo r, 0, 128)
	 */

	os_memset(null_key, 0, sizeof(null_key));
	hmac_sha256(null_key, sizeof(null_key), k, 32, keyseed);
	wpa_hexdump_key(MSG_DEBUG, "SAE: keyseed", keyseed, sizeof(keyseed));

	crypto_bignum_add(own_scalar, peer_scalar, tmp);
	crypto_bignum_mod(tmp, order, tmp);
	crypto_bignum_to_bin(tmp, val, sizeof(val), sizeof(group19_prime));
	wpa_hexdump(MSG_DEBUG, "SAE: PMKID", val, 16);
	sha256_prf(keyseed, sizeof(keyseed), "SAE KCK and PMK",
		   val, sizeof(val), keys, sizeof(keys));
	os_memcpy(sae->kck, keys, 32);
	os_memcpy(sae->pmk, keys + 32, 32);
	wpa_hexdump_key(MSG_DEBUG, "SAE: KCK", sae->kck, 32);
	wpa_hexdump_key(MSG_DEBUG, "SAE: PMK", sae->pmk, 32);

	ret = 0;
fail:
	crypto_bignum_deinit(tmp, 0);
	crypto_bignum_deinit(peer_scalar, 0);
	crypto_bignum_deinit(own_scalar, 0);
	crypto_bignum_deinit(order, 0);
	return ret;
}


int sae_process_commit(struct sae_data *sae)
{
	u8 k[32];
	if (sae_check_peer_commit(sae) < 0 ||
	    sae_derive_k(sae, k) < 0 ||
	    sae_derive_keys(sae, k) < 0)
		return -1;
	return 0;
}


void sae_write_commit(struct sae_data *sae, struct wpabuf *buf,
		      const struct wpabuf *token)
{
	wpabuf_put_le16(buf, 19); /* Finite Cyclic Group */
	if (token)
		wpabuf_put_buf(buf, token);
	wpabuf_put_data(buf, sae->own_commit_scalar, 32);
	wpabuf_put_data(buf, sae->own_commit_element, 2 * 32);
}


u16 sae_parse_commit(struct sae_data *sae, const u8 *data, size_t len,
		     const u8 **token, size_t *token_len)
{
	const u8 *pos = data, *end = data + len;
	size_t val_len;

	wpa_hexdump(MSG_DEBUG, "SAE: Commit fields", data, len);
	if (token)
		*token = NULL;
	if (token_len)
		*token_len = 0;

	/* Check Finite Cyclic Group */
	if (pos + 2 > end)
		return WLAN_STATUS_UNSPECIFIED_FAILURE;
	if (WPA_GET_LE16(pos) != 19) {
		wpa_printf(MSG_DEBUG, "SAE: Unsupported Finite Cyclic Group %u",
			   WPA_GET_LE16(pos));
		return WLAN_STATUS_FINITE_CYCLIC_GROUP_NOT_SUPPORTED;
	}
	pos += 2;
	val_len = 32;

	if (pos + 3 * val_len < end) {
		size_t tlen = end - (pos + 3 * val_len);
		wpa_hexdump(MSG_DEBUG, "SAE: Anti-Clogging Token", pos, tlen);
		if (token)
			*token = pos;
		if (token_len)
			*token_len = tlen;
		pos += tlen;
	}

	if (pos + val_len > end) {
		wpa_printf(MSG_DEBUG, "SAE: Not enough data for scalar");
		return WLAN_STATUS_UNSPECIFIED_FAILURE;
	}

	/*
	 * IEEE Std 802.11-2012, 11.3.8.6.1: If there is a protocol instance for
	 * the peer and it is in Authenticated state, the new Commit Message
	 * shall be dropped if the peer-scalar is identical to the one used in
	 * the existing protocol instance.
	 */
	if (sae->state == SAE_ACCEPTED &&
	    os_memcmp(sae->peer_commit_scalar, pos, val_len) == 0) {
		wpa_printf(MSG_DEBUG, "SAE: Do not accept re-use of previous "
			   "peer-commit-scalar");
		return WLAN_STATUS_UNSPECIFIED_FAILURE;
	}

	os_memcpy(sae->peer_commit_scalar, pos, val_len);
	wpa_hexdump(MSG_DEBUG, "SAE: Peer commit-scalar",
		    sae->peer_commit_scalar, val_len);
	pos += val_len;

	if (pos + 2 * val_len > end) {
		wpa_printf(MSG_DEBUG, "SAE: Not enough data for "
			   "commit-element");
		return WLAN_STATUS_UNSPECIFIED_FAILURE;
	}
	os_memcpy(sae->peer_commit_element, pos, 2 * val_len);
	wpa_hexdump(MSG_DEBUG, "SAE: Peer commit-element(x)",
		    sae->peer_commit_element, val_len);
	wpa_hexdump(MSG_DEBUG, "SAE: Peer commit-element(y)",
		    sae->peer_commit_element + val_len, val_len);

	return WLAN_STATUS_SUCCESS;
}


void sae_write_confirm(struct sae_data *sae, struct wpabuf *buf)
{
	const u8 *sc;
	const u8 *addr[5];
	size_t len[5];

	/* Send-Confirm */
	sc = wpabuf_put(buf, 0);
	wpabuf_put_le16(buf, sae->send_confirm);
	sae->send_confirm++;

	/* Confirm
	 * CN(key, X, Y, Z, ...) =
	 *    HMAC-SHA256(key, D2OS(X) || D2OS(Y) || D2OS(Z) | ...)
	 * confirm = CN(KCK, send-confirm, commit-scalar, COMMIT-ELEMENT,
	 *              peer-commit-scalar, PEER-COMMIT-ELEMENT)
	 */
	addr[0] = sc;
	len[0] = 2;
	addr[1] = sae->own_commit_scalar;
	len[1] = 32;
	addr[2] = sae->own_commit_element;
	len[2] = 2 * 32;
	addr[3] = sae->peer_commit_scalar;
	len[3] = 32;
	addr[4] = sae->peer_commit_element;
	len[4] = 2 * 32;
	hmac_sha256_vector(sae->kck, sizeof(sae->kck), 5, addr, len,
			   wpabuf_put(buf, SHA256_MAC_LEN));
}


int sae_check_confirm(struct sae_data *sae, const u8 *data, size_t len)
{
	u16 rc;
	const u8 *addr[5];
	size_t elen[5];
	u8 verifier[SHA256_MAC_LEN];

	wpa_hexdump(MSG_DEBUG, "SAE: Confirm fields", data, len);

	if (len < 2 + SHA256_MAC_LEN) {
		wpa_printf(MSG_DEBUG, "SAE: Too short confirm message");
		return -1;
	}

	rc = WPA_GET_LE16(data);
	wpa_printf(MSG_DEBUG, "SAE: peer-send-confirm %u", rc);

	/* Confirm
	 * CN(key, X, Y, Z, ...) =
	 *    HMAC-SHA256(key, D2OS(X) || D2OS(Y) || D2OS(Z) | ...)
	 * verifier = CN(KCK, peer-send-confirm, peer-commit-scalar,
	 *               PEER-COMMIT-ELEMENT, commit-scalar, COMMIT-ELEMENT)
	 */
	addr[0] = data;
	elen[0] = 2;
	addr[1] = sae->peer_commit_scalar;
	elen[1] = 32;
	addr[2] = sae->peer_commit_element;
	elen[2] = 2 * 32;
	addr[3] = sae->own_commit_scalar;
	elen[3] = 32;
	addr[4] = sae->own_commit_element;
	elen[4] = 2 * 32;
	hmac_sha256_vector(sae->kck, sizeof(sae->kck), 5, addr, elen, verifier);

	if (os_memcmp(verifier, data + 2, SHA256_MAC_LEN) != 0) {
		wpa_printf(MSG_DEBUG, "SAE: Confirm mismatch");
		wpa_hexdump(MSG_DEBUG, "SAE: Received confirm",
			    data + 2, SHA256_MAC_LEN);
		wpa_hexdump(MSG_DEBUG, "SAE: Calculated verifier",
			    verifier, SHA256_MAC_LEN);
		return -1;
	}

	return 0;
}
