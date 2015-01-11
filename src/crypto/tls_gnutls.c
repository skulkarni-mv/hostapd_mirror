/*
 * SSL/TLS interface functions for GnuTLS
 * Copyright (c) 2004-2011, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#ifdef PKCS12_FUNCS
#include <gnutls/pkcs12.h>
#endif /* PKCS12_FUNCS */

#include "common.h"
#include "tls.h"


static int tls_gnutls_ref_count = 0;

struct tls_global {
	/* Data for session resumption */
	void *session_data;
	size_t session_data_size;

	int server;

	int params_set;
	gnutls_certificate_credentials_t xcred;

	void (*event_cb)(void *ctx, enum tls_event ev,
			 union tls_event_data *data);
	void *cb_ctx;
	int cert_in_cb;
};

struct tls_connection {
	struct tls_global *global;
	gnutls_session_t session;
	int read_alerts, write_alerts, failed;

	u8 *pre_shared_secret;
	size_t pre_shared_secret_len;
	int established;
	int verify_peer;

	struct wpabuf *push_buf;
	struct wpabuf *pull_buf;
	const u8 *pull_buf_offset;

	int params_set;
	gnutls_certificate_credentials_t xcred;

	char *suffix_match;
};


static int tls_connection_verify_peer(gnutls_session_t session);


static void tls_log_func(int level, const char *msg)
{
	char *s, *pos;
	if (level == 6 || level == 7) {
		/* These levels seem to be mostly I/O debug and msg dumps */
		return;
	}

	s = os_strdup(msg);
	if (s == NULL)
		return;

	pos = s;
	while (*pos != '\0') {
		if (*pos == '\n') {
			*pos = '\0';
			break;
		}
		pos++;
	}
	wpa_printf(level > 3 ? MSG_MSGDUMP : MSG_DEBUG,
		   "gnutls<%d> %s", level, s);
	os_free(s);
}


void * tls_init(const struct tls_config *conf)
{
	struct tls_global *global;

	if (tls_gnutls_ref_count == 0) {
		wpa_printf(MSG_DEBUG,
			   "GnuTLS: Library version %s (runtime) - %s (build)",
			   gnutls_check_version(NULL), GNUTLS_VERSION);
	}

	global = os_zalloc(sizeof(*global));
	if (global == NULL)
		return NULL;

	if (tls_gnutls_ref_count == 0 && gnutls_global_init() < 0) {
		os_free(global);
		return NULL;
	}
	tls_gnutls_ref_count++;

	gnutls_global_set_log_function(tls_log_func);
	if (wpa_debug_show_keys)
		gnutls_global_set_log_level(11);

	if (conf) {
		global->event_cb = conf->event_cb;
		global->cb_ctx = conf->cb_ctx;
		global->cert_in_cb = conf->cert_in_cb;
	}

	return global;
}


void tls_deinit(void *ssl_ctx)
{
	struct tls_global *global = ssl_ctx;
	if (global) {
		if (global->params_set)
			gnutls_certificate_free_credentials(global->xcred);
		os_free(global->session_data);
		os_free(global);
	}

	tls_gnutls_ref_count--;
	if (tls_gnutls_ref_count == 0)
		gnutls_global_deinit();
}


int tls_get_errors(void *ssl_ctx)
{
	return 0;
}


static ssize_t tls_pull_func(gnutls_transport_ptr_t ptr, void *buf,
			     size_t len)
{
	struct tls_connection *conn = (struct tls_connection *) ptr;
	const u8 *end;
	if (conn->pull_buf == NULL) {
		errno = EWOULDBLOCK;
		return -1;
	}

	end = wpabuf_head_u8(conn->pull_buf) + wpabuf_len(conn->pull_buf);
	if ((size_t) (end - conn->pull_buf_offset) < len)
		len = end - conn->pull_buf_offset;
	os_memcpy(buf, conn->pull_buf_offset, len);
	conn->pull_buf_offset += len;
	if (conn->pull_buf_offset == end) {
		wpa_printf(MSG_DEBUG, "%s - pull_buf consumed", __func__);
		wpabuf_free(conn->pull_buf);
		conn->pull_buf = NULL;
		conn->pull_buf_offset = NULL;
	} else {
		wpa_printf(MSG_DEBUG, "%s - %lu bytes remaining in pull_buf",
			   __func__,
			   (unsigned long) (end - conn->pull_buf_offset));
	}
	return len;
}


static ssize_t tls_push_func(gnutls_transport_ptr_t ptr, const void *buf,
			     size_t len)
{
	struct tls_connection *conn = (struct tls_connection *) ptr;

	if (wpabuf_resize(&conn->push_buf, len) < 0) {
		errno = ENOMEM;
		return -1;
	}
	wpabuf_put_data(conn->push_buf, buf, len);

	return len;
}


static int tls_gnutls_init_session(struct tls_global *global,
				   struct tls_connection *conn)
{
	const char *err;
	int ret;

	ret = gnutls_init(&conn->session,
			  global->server ? GNUTLS_SERVER : GNUTLS_CLIENT);
	if (ret < 0) {
		wpa_printf(MSG_INFO, "TLS: Failed to initialize new TLS "
			   "connection: %s", gnutls_strerror(ret));
		return -1;
	}

	ret = gnutls_set_default_priority(conn->session);
	if (ret < 0)
		goto fail;

	ret = gnutls_priority_set_direct(conn->session, "NORMAL:-VERS-SSL3.0",
					 &err);
	if (ret < 0) {
		wpa_printf(MSG_ERROR, "GnuTLS: Priority string failure at "
			   "'%s'", err);
		goto fail;
	}

	gnutls_transport_set_pull_function(conn->session, tls_pull_func);
	gnutls_transport_set_push_function(conn->session, tls_push_func);
	gnutls_transport_set_ptr(conn->session, (gnutls_transport_ptr_t) conn);
	gnutls_session_set_ptr(conn->session, conn);

	return 0;

fail:
	wpa_printf(MSG_INFO, "TLS: Failed to setup new TLS connection: %s",
		   gnutls_strerror(ret));
	gnutls_deinit(conn->session);
	return -1;
}


struct tls_connection * tls_connection_init(void *ssl_ctx)
{
	struct tls_global *global = ssl_ctx;
	struct tls_connection *conn;
	int ret;

	conn = os_zalloc(sizeof(*conn));
	if (conn == NULL)
		return NULL;
	conn->global = global;

	if (tls_gnutls_init_session(global, conn)) {
		os_free(conn);
		return NULL;
	}

	if (global->params_set) {
		ret = gnutls_credentials_set(conn->session,
					     GNUTLS_CRD_CERTIFICATE,
					     global->xcred);
		if (ret < 0) {
			wpa_printf(MSG_INFO, "Failed to configure "
				   "credentials: %s", gnutls_strerror(ret));
			os_free(conn);
			return NULL;
		}
	}

	if (gnutls_certificate_allocate_credentials(&conn->xcred)) {
		os_free(conn);
		return NULL;
	}

	return conn;
}


void tls_connection_deinit(void *ssl_ctx, struct tls_connection *conn)
{
	if (conn == NULL)
		return;

	gnutls_certificate_free_credentials(conn->xcred);
	gnutls_deinit(conn->session);
	os_free(conn->pre_shared_secret);
	wpabuf_free(conn->push_buf);
	wpabuf_free(conn->pull_buf);
	os_free(conn->suffix_match);
	os_free(conn);
}


int tls_connection_established(void *ssl_ctx, struct tls_connection *conn)
{
	return conn ? conn->established : 0;
}


int tls_connection_shutdown(void *ssl_ctx, struct tls_connection *conn)
{
	struct tls_global *global = ssl_ctx;
	int ret;

	if (conn == NULL)
		return -1;

	/* Shutdown previous TLS connection without notifying the peer
	 * because the connection was already terminated in practice
	 * and "close notify" shutdown alert would confuse AS. */
	gnutls_bye(conn->session, GNUTLS_SHUT_RDWR);
	wpabuf_free(conn->push_buf);
	conn->push_buf = NULL;
	conn->established = 0;

	gnutls_deinit(conn->session);
	if (tls_gnutls_init_session(global, conn)) {
		wpa_printf(MSG_INFO, "GnuTLS: Failed to preparare new session "
			   "for session resumption use");
		return -1;
	}

	ret = gnutls_credentials_set(conn->session, GNUTLS_CRD_CERTIFICATE,
				     conn->params_set ? conn->xcred :
				     global->xcred);
	if (ret < 0) {
		wpa_printf(MSG_INFO, "GnuTLS: Failed to configure credentials "
			   "for session resumption: %s", gnutls_strerror(ret));
		return -1;
	}

	if (global->session_data) {
		ret = gnutls_session_set_data(conn->session,
					      global->session_data,
					      global->session_data_size);
		if (ret < 0) {
			wpa_printf(MSG_INFO, "GnuTLS: Failed to set session "
				   "data: %s", gnutls_strerror(ret));
			return -1;
		}
	}

	return 0;
}


int tls_connection_set_params(void *tls_ctx, struct tls_connection *conn,
			      const struct tls_connection_params *params)
{
	int ret;

	if (conn == NULL || params == NULL)
		return -1;

	if (params->subject_match) {
		wpa_printf(MSG_INFO, "GnuTLS: subject_match not supported");
		return -1;
	}

	if (params->altsubject_match) {
		wpa_printf(MSG_INFO, "GnuTLS: altsubject_match not supported");
		return -1;
	}

	os_free(conn->suffix_match);
	conn->suffix_match = NULL;
	if (params->suffix_match) {
		conn->suffix_match = os_strdup(params->suffix_match);
		if (conn->suffix_match == NULL)
			return -1;
	}

	if (params->openssl_ciphers) {
		wpa_printf(MSG_INFO, "GnuTLS: openssl_ciphers not supported");
		return -1;
	}

	/* TODO: gnutls_certificate_set_verify_flags(xcred, flags); 
	 * to force peer validation(?) */

	if (params->ca_cert) {
		wpa_printf(MSG_DEBUG, "GnuTLS: Try to parse %s in DER format",
			   params->ca_cert);
		ret = gnutls_certificate_set_x509_trust_file(
			conn->xcred, params->ca_cert, GNUTLS_X509_FMT_DER);
		if (ret < 0) {
			wpa_printf(MSG_DEBUG,
				   "GnuTLS: Failed to read CA cert '%s' in DER format (%s) - try in PEM format",
				   params->ca_cert,
				   gnutls_strerror(ret));
			ret = gnutls_certificate_set_x509_trust_file(
				conn->xcred, params->ca_cert,
				GNUTLS_X509_FMT_PEM);
			if (ret < 0) {
				wpa_printf(MSG_DEBUG,
					   "Failed to read CA cert '%s' in PEM format: %s",
					   params->ca_cert,
					   gnutls_strerror(ret));
				return -1;
			}
		}
	} else if (params->ca_cert_blob) {
		gnutls_datum_t ca;

		ca.data = (unsigned char *) params->ca_cert_blob;
		ca.size = params->ca_cert_blob_len;

		ret = gnutls_certificate_set_x509_trust_mem(
			conn->xcred, &ca, GNUTLS_X509_FMT_DER);
		if (ret < 0) {
			wpa_printf(MSG_DEBUG,
				   "Failed to parse CA cert in DER format: %s",
				   gnutls_strerror(ret));
			ret = gnutls_certificate_set_x509_trust_mem(
				conn->xcred, &ca, GNUTLS_X509_FMT_PEM);
			if (ret < 0) {
				wpa_printf(MSG_DEBUG,
					   "Failed to parse CA cert in PEM format: %s",
					   gnutls_strerror(ret));
				return -1;
			}
		}
	} else if (params->ca_path) {
		wpa_printf(MSG_INFO, "GnuTLS: ca_path not supported");
		return -1;
	}

	if (params->ca_cert || params->ca_cert_blob) {
		conn->verify_peer = 1;
		gnutls_certificate_set_verify_function(
			conn->xcred, tls_connection_verify_peer);

		if (params->flags & TLS_CONN_ALLOW_SIGN_RSA_MD5) {
			gnutls_certificate_set_verify_flags(
				conn->xcred, GNUTLS_VERIFY_ALLOW_SIGN_RSA_MD5);
		}

		if (params->flags & TLS_CONN_DISABLE_TIME_CHECKS) {
			gnutls_certificate_set_verify_flags(
				conn->xcred,
				GNUTLS_VERIFY_DISABLE_TIME_CHECKS);
		}
	}

	if (params->client_cert && params->private_key) {
#if GNUTLS_VERSION_NUMBER >= 0x03010b
		ret = gnutls_certificate_set_x509_key_file2(
			conn->xcred, params->client_cert, params->private_key,
			GNUTLS_X509_FMT_DER, params->private_key_passwd, 0);
#else
		/* private_key_passwd not (easily) supported here */
		ret = gnutls_certificate_set_x509_key_file(
			conn->xcred, params->client_cert, params->private_key,
			GNUTLS_X509_FMT_DER);
#endif
		if (ret < 0) {
			wpa_printf(MSG_DEBUG, "Failed to read client cert/key "
				   "in DER format: %s", gnutls_strerror(ret));
#if GNUTLS_VERSION_NUMBER >= 0x03010b
			ret = gnutls_certificate_set_x509_key_file2(
				conn->xcred, params->client_cert,
				params->private_key, GNUTLS_X509_FMT_PEM,
				params->private_key_passwd, 0);
#else
			ret = gnutls_certificate_set_x509_key_file(
				conn->xcred, params->client_cert,
				params->private_key, GNUTLS_X509_FMT_PEM);
#endif
			if (ret < 0) {
				wpa_printf(MSG_DEBUG, "Failed to read client "
					   "cert/key in PEM format: %s",
					   gnutls_strerror(ret));
				return ret;
			}
		}
	} else if (params->private_key) {
		int pkcs12_ok = 0;
#ifdef PKCS12_FUNCS
		/* Try to load in PKCS#12 format */
		ret = gnutls_certificate_set_x509_simple_pkcs12_file(
			conn->xcred, params->private_key, GNUTLS_X509_FMT_DER,
			params->private_key_passwd);
		if (ret != 0) {
			wpa_printf(MSG_DEBUG, "Failed to load private_key in "
				   "PKCS#12 format: %s", gnutls_strerror(ret));
			return -1;
		} else
			pkcs12_ok = 1;
#endif /* PKCS12_FUNCS */

		if (!pkcs12_ok) {
			wpa_printf(MSG_DEBUG, "GnuTLS: PKCS#12 support not "
				   "included");
			return -1;
		}
	}

	conn->params_set = 1;

	ret = gnutls_credentials_set(conn->session, GNUTLS_CRD_CERTIFICATE,
				     conn->xcred);
	if (ret < 0) {
		wpa_printf(MSG_INFO, "Failed to configure credentials: %s",
			   gnutls_strerror(ret));
	}

	return ret;
}


int tls_global_set_params(void *tls_ctx,
			  const struct tls_connection_params *params)
{
	struct tls_global *global = tls_ctx;
	int ret;

	/* Currently, global parameters are only set when running in server
	 * mode. */
	global->server = 1;

	if (global->params_set) {
		gnutls_certificate_free_credentials(global->xcred);
		global->params_set = 0;
	}

	ret = gnutls_certificate_allocate_credentials(&global->xcred);
	if (ret) {
		wpa_printf(MSG_DEBUG, "Failed to allocate global credentials "
			   "%s", gnutls_strerror(ret));
		return -1;
	}

	if (params->ca_cert) {
		ret = gnutls_certificate_set_x509_trust_file(
			global->xcred, params->ca_cert, GNUTLS_X509_FMT_DER);
		if (ret < 0) {
			wpa_printf(MSG_DEBUG, "Failed to read CA cert '%s' "
				   "in DER format: %s", params->ca_cert,
				   gnutls_strerror(ret));
			ret = gnutls_certificate_set_x509_trust_file(
				global->xcred, params->ca_cert,
				GNUTLS_X509_FMT_PEM);
			if (ret < 0) {
				wpa_printf(MSG_DEBUG, "Failed to read CA cert "
					   "'%s' in PEM format: %s",
					   params->ca_cert,
					   gnutls_strerror(ret));
				goto fail;
			}
		}

		if (params->flags & TLS_CONN_ALLOW_SIGN_RSA_MD5) {
			gnutls_certificate_set_verify_flags(
				global->xcred,
				GNUTLS_VERIFY_ALLOW_SIGN_RSA_MD5);
		}

		if (params->flags & TLS_CONN_DISABLE_TIME_CHECKS) {
			gnutls_certificate_set_verify_flags(
				global->xcred,
				GNUTLS_VERIFY_DISABLE_TIME_CHECKS);
		}
	}

	if (params->client_cert && params->private_key) {
		/* TODO: private_key_passwd? */
		ret = gnutls_certificate_set_x509_key_file(
			global->xcred, params->client_cert,
			params->private_key, GNUTLS_X509_FMT_DER);
		if (ret < 0) {
			wpa_printf(MSG_DEBUG, "Failed to read client cert/key "
				   "in DER format: %s", gnutls_strerror(ret));
			ret = gnutls_certificate_set_x509_key_file(
				global->xcred, params->client_cert,
				params->private_key, GNUTLS_X509_FMT_PEM);
			if (ret < 0) {
				wpa_printf(MSG_DEBUG, "Failed to read client "
					   "cert/key in PEM format: %s",
					   gnutls_strerror(ret));
				goto fail;
			}
		}
	} else if (params->private_key) {
		int pkcs12_ok = 0;
#ifdef PKCS12_FUNCS
		/* Try to load in PKCS#12 format */
		ret = gnutls_certificate_set_x509_simple_pkcs12_file(
			global->xcred, params->private_key,
			GNUTLS_X509_FMT_DER, params->private_key_passwd);
		if (ret != 0) {
			wpa_printf(MSG_DEBUG, "Failed to load private_key in "
				   "PKCS#12 format: %s", gnutls_strerror(ret));
			goto fail;
		} else
			pkcs12_ok = 1;
#endif /* PKCS12_FUNCS */

		if (!pkcs12_ok) {
			wpa_printf(MSG_DEBUG, "GnuTLS: PKCS#12 support not "
				   "included");
			goto fail;
		}
	}

	global->params_set = 1;

	return 0;

fail:
	gnutls_certificate_free_credentials(global->xcred);
	return -1;
}


int tls_global_set_verify(void *ssl_ctx, int check_crl)
{
	/* TODO */
	return 0;
}


int tls_connection_set_verify(void *ssl_ctx, struct tls_connection *conn,
			      int verify_peer)
{
	if (conn == NULL || conn->session == NULL)
		return -1;

	conn->verify_peer = verify_peer;
	gnutls_certificate_server_set_request(conn->session,
					      verify_peer ? GNUTLS_CERT_REQUIRE
					      : GNUTLS_CERT_REQUEST);

	return 0;
}


int tls_connection_get_keys(void *ssl_ctx, struct tls_connection *conn,
			    struct tls_keys *keys)
{
#if GNUTLS_VERSION_NUMBER >= 0x030012
	gnutls_datum_t client, server;

	if (conn == NULL || conn->session == NULL || keys == NULL)
		return -1;

	os_memset(keys, 0, sizeof(*keys));
	gnutls_session_get_random(conn->session, &client, &server);
	keys->client_random = client.data;
	keys->server_random = server.data;
	keys->client_random_len = client.size;
	keys->server_random_len = client.size;

	return 0;
#else /* 3.0.18 */
	return -1;
#endif /* 3.0.18 */
}


int tls_connection_prf(void *tls_ctx, struct tls_connection *conn,
		       const char *label, int server_random_first,
		       u8 *out, size_t out_len)
{
	if (conn == NULL || conn->session == NULL)
		return -1;

	return gnutls_prf(conn->session, os_strlen(label), label,
			  server_random_first, 0, NULL, out_len, (char *) out);
}


static void gnutls_tls_fail_event(struct tls_connection *conn,
				  const gnutls_datum_t *cert, int depth,
				  const char *subject, const char *err_str,
				  enum tls_fail_reason reason)
{
	union tls_event_data ev;
	struct tls_global *global = conn->global;
	struct wpabuf *cert_buf = NULL;

	if (global->event_cb == NULL)
		return;

	os_memset(&ev, 0, sizeof(ev));
	ev.cert_fail.depth = depth;
	ev.cert_fail.subject = subject ? subject : "";
	ev.cert_fail.reason = reason;
	ev.cert_fail.reason_txt = err_str;
	if (cert) {
		cert_buf = wpabuf_alloc_copy(cert->data, cert->size);
		ev.cert_fail.cert = cert_buf;
	}
	global->event_cb(global->cb_ctx, TLS_CERT_CHAIN_FAILURE, &ev);
	wpabuf_free(cert_buf);
}


static int tls_connection_verify_peer(gnutls_session_t session)
{
	struct tls_connection *conn;
	unsigned int status, num_certs, i;
	struct os_time now;
	const gnutls_datum_t *certs;
	gnutls_x509_crt_t cert;
	gnutls_alert_description_t err;

	conn = gnutls_session_get_ptr(session);
	if (!conn->verify_peer) {
		wpa_printf(MSG_DEBUG,
			   "GnuTLS: No peer certificate verification enabled");
		return 0;
	}

	wpa_printf(MSG_DEBUG, "GnuTSL: Verifying peer certificate");

	if (gnutls_certificate_verify_peers2(session, &status) < 0) {
		wpa_printf(MSG_INFO, "TLS: Failed to verify peer "
			   "certificate chain");
		err = GNUTLS_A_INTERNAL_ERROR;
		goto out;
	}

#if GNUTLS_VERSION_NUMBER >= 0x030104
	{
		gnutls_datum_t info;
		int ret, type;

		type = gnutls_certificate_type_get(session);
		ret = gnutls_certificate_verification_status_print(status, type,
								   &info, 0);
		if (ret < 0) {
			wpa_printf(MSG_DEBUG,
				   "GnuTLS: Failed to print verification status");
			err = GNUTLS_A_INTERNAL_ERROR;
			goto out;
		}
		wpa_printf(MSG_DEBUG, "GnuTLS: %s", info.data);
		gnutls_free(info.data);
	}
#endif /* GnuTLS 3.1.4 or newer */

	if (conn->verify_peer && (status & GNUTLS_CERT_INVALID)) {
		wpa_printf(MSG_INFO, "TLS: Peer certificate not trusted");
		if (status & GNUTLS_CERT_INSECURE_ALGORITHM) {
			wpa_printf(MSG_INFO, "TLS: Certificate uses insecure "
				   "algorithm");
			gnutls_tls_fail_event(conn, NULL, 0, NULL,
					      "certificate uses insecure algorithm",
					      TLS_FAIL_BAD_CERTIFICATE);
			err = GNUTLS_A_INSUFFICIENT_SECURITY;
			goto out;
		}
		if (status & GNUTLS_CERT_NOT_ACTIVATED) {
			wpa_printf(MSG_INFO, "TLS: Certificate not yet "
				   "activated");
			gnutls_tls_fail_event(conn, NULL, 0, NULL,
					      "certificate not yet valid",
					      TLS_FAIL_NOT_YET_VALID);
			err = GNUTLS_A_CERTIFICATE_EXPIRED;
			goto out;
		}
		if (status & GNUTLS_CERT_EXPIRED) {
			wpa_printf(MSG_INFO, "TLS: Certificate expired");
			gnutls_tls_fail_event(conn, NULL, 0, NULL,
					      "certificate has expired",
					      TLS_FAIL_EXPIRED);
			err = GNUTLS_A_CERTIFICATE_EXPIRED;
			goto out;
		}
		gnutls_tls_fail_event(conn, NULL, 0, NULL,
				      "untrusted certificate",
				      TLS_FAIL_UNTRUSTED);
		err = GNUTLS_A_INTERNAL_ERROR;
		goto out;
	}

	if (status & GNUTLS_CERT_SIGNER_NOT_FOUND) {
		wpa_printf(MSG_INFO, "TLS: Peer certificate does not have a "
			   "known issuer");
		gnutls_tls_fail_event(conn, NULL, 0, NULL, "signed not found",
				      TLS_FAIL_UNTRUSTED);
		err = GNUTLS_A_UNKNOWN_CA;
		goto out;
	}

	if (status & GNUTLS_CERT_REVOKED) {
		wpa_printf(MSG_INFO, "TLS: Peer certificate has been revoked");
		gnutls_tls_fail_event(conn, NULL, 0, NULL,
				      "certificate revoked",
				      TLS_FAIL_REVOKED);
		err = GNUTLS_A_CERTIFICATE_REVOKED;
		goto out;
	}

	if (status != 0) {
		wpa_printf(MSG_INFO, "TLS: Unknown verification status: %d",
			   status);
		err = GNUTLS_A_INTERNAL_ERROR;
		goto out;
	}

	os_get_time(&now);

	certs = gnutls_certificate_get_peers(session, &num_certs);
	if (certs == NULL) {
		wpa_printf(MSG_INFO, "TLS: No peer certificate chain "
			   "received");
		err = GNUTLS_A_UNKNOWN_CA;
		goto out;
	}

	for (i = 0; i < num_certs; i++) {
		char *buf;
		size_t len;
		if (gnutls_x509_crt_init(&cert) < 0) {
			wpa_printf(MSG_INFO, "TLS: Certificate initialization "
				   "failed");
			err = GNUTLS_A_BAD_CERTIFICATE;
			goto out;
		}

		if (gnutls_x509_crt_import(cert, &certs[i],
					   GNUTLS_X509_FMT_DER) < 0) {
			wpa_printf(MSG_INFO, "TLS: Could not parse peer "
				   "certificate %d/%d", i + 1, num_certs);
			gnutls_x509_crt_deinit(cert);
			err = GNUTLS_A_BAD_CERTIFICATE;
			goto out;
		}

		gnutls_x509_crt_get_dn(cert, NULL, &len);
		len++;
		buf = os_malloc(len + 1);
		if (buf) {
			buf[0] = buf[len] = '\0';
			gnutls_x509_crt_get_dn(cert, buf, &len);
		}
		wpa_printf(MSG_DEBUG, "TLS: Peer cert chain %d/%d: %s",
			   i + 1, num_certs, buf);

		if (i == 0) {
			if (conn->suffix_match &&
			    !gnutls_x509_crt_check_hostname(
				    cert, conn->suffix_match)) {
				wpa_printf(MSG_WARNING,
					   "TLS: Domain suffix match '%s' not found",
					   conn->suffix_match);
				gnutls_tls_fail_event(
					conn, &certs[i], i, buf,
					"Domain suffix mismatch",
					TLS_FAIL_DOMAIN_SUFFIX_MISMATCH);
				err = GNUTLS_A_BAD_CERTIFICATE;
				gnutls_x509_crt_deinit(cert);
				os_free(buf);
				goto out;
			}

			/* TODO: validate altsubject_match.
			 * For now, any such configuration is rejected in
			 * tls_connection_set_params() */
		}

		if (gnutls_x509_crt_get_expiration_time(cert) < now.sec ||
		    gnutls_x509_crt_get_activation_time(cert) > now.sec) {
			wpa_printf(MSG_INFO, "TLS: Peer certificate %d/%d is "
				   "not valid at this time",
				   i + 1, num_certs);
			gnutls_tls_fail_event(
				conn, &certs[i], i, buf,
				"Certificate is not valid at this time",
				TLS_FAIL_EXPIRED);
			gnutls_x509_crt_deinit(cert);
			os_free(buf);
			err = GNUTLS_A_CERTIFICATE_EXPIRED;
			goto out;
		}

		os_free(buf);

		gnutls_x509_crt_deinit(cert);
	}

	return 0;

out:
	conn->failed++;
	gnutls_alert_send(session, GNUTLS_AL_FATAL, err);
	return GNUTLS_E_CERTIFICATE_ERROR;
}


static struct wpabuf * gnutls_get_appl_data(struct tls_connection *conn)
{
	int res;
	struct wpabuf *ad;
	wpa_printf(MSG_DEBUG, "GnuTLS: Check for possible Application Data");
	ad = wpabuf_alloc((wpabuf_len(conn->pull_buf) + 500) * 3);
	if (ad == NULL)
		return NULL;

	res = gnutls_record_recv(conn->session, wpabuf_mhead(ad),
				 wpabuf_size(ad));
	wpa_printf(MSG_DEBUG, "GnuTLS: gnutls_record_recv: %d", res);
	if (res < 0) {
		wpa_printf(MSG_DEBUG, "%s - gnutls_record_recv failed: %d "
			   "(%s)", __func__, (int) res,
			   gnutls_strerror(res));
		wpabuf_free(ad);
		return NULL;
	}

	wpabuf_put(ad, res);
	wpa_printf(MSG_DEBUG, "GnuTLS: Received %d bytes of Application Data",
		   res);
	return ad;
}


struct wpabuf * tls_connection_handshake(void *tls_ctx,
					 struct tls_connection *conn,
					 const struct wpabuf *in_data,
					 struct wpabuf **appl_data)
{
	struct tls_global *global = tls_ctx;
	struct wpabuf *out_data;
	int ret;

	if (appl_data)
		*appl_data = NULL;

	if (in_data && wpabuf_len(in_data) > 0) {
		if (conn->pull_buf) {
			wpa_printf(MSG_DEBUG, "%s - %lu bytes remaining in "
				   "pull_buf", __func__,
				   (unsigned long) wpabuf_len(conn->pull_buf));
			wpabuf_free(conn->pull_buf);
		}
		conn->pull_buf = wpabuf_dup(in_data);
		if (conn->pull_buf == NULL)
			return NULL;
		conn->pull_buf_offset = wpabuf_head(conn->pull_buf);
	}

	ret = gnutls_handshake(conn->session);
	if (ret < 0) {
		switch (ret) {
		case GNUTLS_E_AGAIN:
			if (global->server && conn->established &&
			    conn->push_buf == NULL) {
				/* Need to return something to trigger
				 * completion of EAP-TLS. */
				conn->push_buf = wpabuf_alloc(0);
			}
			break;
		case GNUTLS_E_FATAL_ALERT_RECEIVED:
			wpa_printf(MSG_DEBUG, "%s - received fatal '%s' alert",
				   __func__, gnutls_alert_get_name(
					   gnutls_alert_get(conn->session)));
			conn->read_alerts++;
			/* continue */
		default:
			wpa_printf(MSG_DEBUG, "%s - gnutls_handshake failed "
				   "-> %s", __func__, gnutls_strerror(ret));
			conn->failed++;
		}
	} else {
		size_t size;

		wpa_printf(MSG_DEBUG, "TLS: Handshake completed successfully");

#if GNUTLS_VERSION_NUMBER >= 0x03010a
		{
			char *desc;

			desc = gnutls_session_get_desc(conn->session);
			if (desc) {
				wpa_printf(MSG_DEBUG, "GnuTLS: %s", desc);
				gnutls_free(desc);
			}
		}
#endif /* GnuTLS 3.1.10 or newer */

		conn->established = 1;
		if (conn->push_buf == NULL) {
			/* Need to return something to get final TLS ACK. */
			conn->push_buf = wpabuf_alloc(0);
		}

		gnutls_session_get_data(conn->session, NULL, &size);
		if (global->session_data == NULL ||
		    global->session_data_size < size) {
			os_free(global->session_data);
			global->session_data = os_malloc(size);
		}
		if (global->session_data) {
			global->session_data_size = size;
			gnutls_session_get_data(conn->session,
						global->session_data,
						&global->session_data_size);
		}

		if (conn->pull_buf && appl_data)
			*appl_data = gnutls_get_appl_data(conn);
	}

	out_data = conn->push_buf;
	conn->push_buf = NULL;
	return out_data;
}


struct wpabuf * tls_connection_server_handshake(void *tls_ctx,
						struct tls_connection *conn,
						const struct wpabuf *in_data,
						struct wpabuf **appl_data)
{
	return tls_connection_handshake(tls_ctx, conn, in_data, appl_data);
}


struct wpabuf * tls_connection_encrypt(void *tls_ctx,
				       struct tls_connection *conn,
				       const struct wpabuf *in_data)
{
	ssize_t res;
	struct wpabuf *buf;

	res = gnutls_record_send(conn->session, wpabuf_head(in_data),
				 wpabuf_len(in_data));
	if (res < 0) {
		wpa_printf(MSG_INFO, "%s: Encryption failed: %s",
			   __func__, gnutls_strerror(res));
		return NULL;
	}

	buf = conn->push_buf;
	conn->push_buf = NULL;
	return buf;
}


struct wpabuf * tls_connection_decrypt(void *tls_ctx,
				       struct tls_connection *conn,
				       const struct wpabuf *in_data)
{
	ssize_t res;
	struct wpabuf *out;

	if (conn->pull_buf) {
		wpa_printf(MSG_DEBUG, "%s - %lu bytes remaining in "
			   "pull_buf", __func__,
			   (unsigned long) wpabuf_len(conn->pull_buf));
		wpabuf_free(conn->pull_buf);
	}
	conn->pull_buf = wpabuf_dup(in_data);
	if (conn->pull_buf == NULL)
		return NULL;
	conn->pull_buf_offset = wpabuf_head(conn->pull_buf);

	/*
	 * Even though we try to disable TLS compression, it is possible that
	 * this cannot be done with all TLS libraries. Add extra buffer space
	 * to handle the possibility of the decrypted data being longer than
	 * input data.
	 */
	out = wpabuf_alloc((wpabuf_len(in_data) + 500) * 3);
	if (out == NULL)
		return NULL;

	res = gnutls_record_recv(conn->session, wpabuf_mhead(out),
				 wpabuf_size(out));
	if (res < 0) {
		wpa_printf(MSG_DEBUG, "%s - gnutls_record_recv failed: %d "
			   "(%s)", __func__, (int) res, gnutls_strerror(res));
		wpabuf_free(out);
		return NULL;
	}
	wpabuf_put(out, res);

	return out;
}


int tls_connection_resumed(void *ssl_ctx, struct tls_connection *conn)
{
	if (conn == NULL)
		return 0;
	return gnutls_session_is_resumed(conn->session);
}


int tls_connection_set_cipher_list(void *tls_ctx, struct tls_connection *conn,
				   u8 *ciphers)
{
	/* TODO */
	return -1;
}


int tls_get_cipher(void *ssl_ctx, struct tls_connection *conn,
		   char *buf, size_t buflen)
{
	/* TODO */
	buf[0] = '\0';
	return 0;
}


int tls_connection_enable_workaround(void *ssl_ctx,
				     struct tls_connection *conn)
{
	gnutls_record_disable_padding(conn->session);
	return 0;
}


int tls_connection_client_hello_ext(void *ssl_ctx, struct tls_connection *conn,
				    int ext_type, const u8 *data,
				    size_t data_len)
{
	/* TODO */
	return -1;
}


int tls_connection_get_failed(void *ssl_ctx, struct tls_connection *conn)
{
	if (conn == NULL)
		return -1;
	return conn->failed;
}


int tls_connection_get_read_alerts(void *ssl_ctx, struct tls_connection *conn)
{
	if (conn == NULL)
		return -1;
	return conn->read_alerts;
}


int tls_connection_get_write_alerts(void *ssl_ctx, struct tls_connection *conn)
{
	if (conn == NULL)
		return -1;
	return conn->write_alerts;
}


int tls_connection_get_keyblock_size(void *tls_ctx,
				     struct tls_connection *conn)
{
	/* TODO */
	return -1;
}


unsigned int tls_capabilities(void *tls_ctx)
{
	return 0;
}


int tls_connection_set_session_ticket_cb(void *tls_ctx,
					 struct tls_connection *conn,
					 tls_session_ticket_cb cb, void *ctx)
{
	return -1;
}


int tls_get_library_version(char *buf, size_t buf_len)
{
	return os_snprintf(buf, buf_len, "GnuTLS build=%s run=%s",
			   GNUTLS_VERSION, gnutls_check_version(NULL));
}
