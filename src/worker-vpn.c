/*
 * Copyright (C) 2013, 2014 Nikos Mavrogiannopoulos
 *
 * This file is part of ocserv.
 *
 * ocserv is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuTLS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <gnutls/gnutls.h>
#include <gnutls/dtls.h>
#include <gnutls/crypto.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <system.h>
#include <time.h>
#include <gettime.h>
#include <common.h>
#include <html.h>
#include <base64.h>
#include <c-strcase.h>
#include <c-ctype.h>
#include <worker-bandwidth.h>

#include <vpn.h>
#include "ipc.pb-c.h"
#include <cookies.h>
#include <worker.h>
#include <tlslib.h>

#include <http_parser.h>

#define MIN_MTU(ws) (((ws)->vinfo.ipv6!=NULL)?1281:257)

/* after that time (secs) of inactivity in the UDP part, connection switches to 
 * TCP (if activity occurs there).
 */
#define UDP_SWITCH_TIME 15
#define PERIODIC_CHECK_TIME 30

/* The number of DPD packets a client skips before he's kicked */
#define DPD_TRIES 2
#define DPD_MAX_TRIES 3

/* HTTP requests prior to disconnection */
#define MAX_HTTP_REQUESTS 16

static int terminate = 0;
static int parse_cstp_data(struct worker_st *ws, uint8_t * buf, size_t buf_size,
			   time_t);
static int parse_dtls_data(struct worker_st *ws, uint8_t * buf, size_t buf_size,
			   time_t);

static void handle_alarm(int signo)
{
	exit(1);
}

static void handle_term(int signo)
{
	terminate = 1;
	alarm(2);		/* force exit by SIGALRM */
}

static int connect_handler(worker_st * ws);

typedef int (*url_handler_fn) (worker_st *, unsigned http_ver);
struct known_urls_st {
	const char *url;
	unsigned url_size;
	unsigned partial_match;
	url_handler_fn get_handler;
	url_handler_fn post_handler;
};

#define LL(x,y,z) {x, sizeof(x)-1, 0, y, z}
#define LL_DIR(x,y,z) {x, sizeof(x)-1, 1, y, z}
const static struct known_urls_st known_urls[] = {
	LL("/", get_auth_handler, post_auth_handler),
	LL("/auth", get_auth_handler, post_auth_handler),
#ifdef ANYCONNECT_CLIENT_COMPAT
	LL("/1/index.html", get_empty_handler, NULL),
	LL("/1/Linux", get_empty_handler, NULL),
	LL("/1/Linux_64", get_empty_handler, NULL),
	LL("/1/Windows", get_empty_handler, NULL),
	LL("/1/Darwin_i386", get_empty_handler, NULL),
	LL("/1/binaries/vpndownloader.sh", get_dl_handler, NULL),
	LL("/1/VPNManifest.xml", get_string_handler, NULL),
	LL("/1/binaries/update.txt", get_string_handler, NULL),

	LL("/profiles", get_config_handler, NULL),
	LL("/+CSCOT+/", get_string_handler, NULL),
	LL("/logout", get_empty_handler, NULL),
#endif
	{NULL, 0, 0, NULL, NULL}
};

static url_handler_fn get_url_handler(const char *url)
{
	const struct known_urls_st *p;
	unsigned len = strlen(url);

	p = known_urls;
	do {
		if (p->url != NULL) {
			if ((len == p->url_size && strcmp(p->url, url) == 0) ||
			    (len >= p->url_size
			     && strncmp(p->url, url, p->url_size) == 0
			     && (p->partial_match != 0
				 || url[p->url_size] == '/'
				 || url[p->url_size] == '?')))
				return p->get_handler;
		}
		p++;
	} while (p->url != NULL);

	return NULL;
}

static url_handler_fn post_url_handler(const char *url)
{
	const struct known_urls_st *p;

	p = known_urls;
	do {
		if (p->url != NULL && strcmp(p->url, url) == 0)
			return p->post_handler;
		p++;
	} while (p->url != NULL);

	return NULL;
}

int url_cb(http_parser * parser, const char *at, size_t length)
{
	struct worker_st *ws = parser->data;
	struct http_req_st *req = &ws->req;

	if (length >= sizeof(req->url)) {
		req->url[0] = 0;
		return 1;
	}

	memcpy(req->url, at, length);
	req->url[length] = 0;

	return 0;
}

#define STR_HDR_COOKIE "Cookie"
#define STR_HDR_USER_AGENT "User-Agent"
#define STR_HDR_CONNECTION "Connection"
#define STR_HDR_MS "X-DTLS-Master-Secret"
#define STR_HDR_CS "X-DTLS-CipherSuite"
#define STR_HDR_DMTU "X-DTLS-MTU"
#define STR_HDR_CMTU "X-CSTP-MTU"
#define STR_HDR_ATYPE "X-CSTP-Address-Type"
#define STR_HDR_HOST "X-CSTP-Hostname"

#define CS_ESALSA20 "OC-DTLS1_2-ESALSA20-SHA"
#define CS_SALSA20 "OC-DTLS1_2-SALSA20-SHA"
#define CS_AES128_GCM "OC-DTLS1_2-AES128-GCM"
#define CS_AES256_GCM "OC-DTLS1_2-AES256-GCM"

/* Consider switching to gperf when this table grows significantly.
 */
static const dtls_ciphersuite_st ciphersuites[] =
{
#if GNUTLS_VERSION_NUMBER >= 0x030207
	{
		.oc_name = CS_ESALSA20,
		.gnutls_name = "NONE:+VERS-DTLS1.2:+COMP-NULL:+ESTREAM-SALSA20-256:+SHA1:+RSA:%COMPAT:%DISABLE_SAFE_RENEGOTIATION",
		.gnutls_version = GNUTLS_DTLS1_2,
		.gnutls_mac = GNUTLS_MAC_SHA1,
		.gnutls_cipher = GNUTLS_CIPHER_ESTREAM_SALSA20_256,
		.server_prio = 100
	},
	{
		.oc_name = CS_SALSA20,
		.gnutls_name = "NONE:+VERS-DTLS1.2:+COMP-NULL:+SALSA20-256:+SHA1:+RSA:%COMPAT:%DISABLE_SAFE_RENEGOTIATION",
		.gnutls_version = GNUTLS_DTLS1_2,
		.gnutls_mac = GNUTLS_MAC_SHA1,
		.gnutls_cipher = GNUTLS_CIPHER_SALSA20_256,
		.server_prio = 100
	},
	{
		.oc_name = CS_AES128_GCM,
		.gnutls_name = "NONE:+VERS-DTLS1.2:+COMP-NULL:+AES-128-GCM:+AEAD:+RSA:%COMPAT:%DISABLE_SAFE_RENEGOTIATION:+SIGN-ALL",
		.gnutls_version = GNUTLS_DTLS1_2,
		.gnutls_mac = GNUTLS_MAC_AEAD,
		.gnutls_cipher = GNUTLS_CIPHER_AES_128_GCM,
		.server_prio = 90
	},
	{
		.oc_name = CS_AES256_GCM,
		.gnutls_name = "NONE:+VERS-DTLS1.2:+COMP-NULL:+AES-256-GCM:+AEAD:+RSA:%COMPAT:%DISABLE_SAFE_RENEGOTIATION:+SIGN-ALL",
		.gnutls_version = GNUTLS_DTLS1_2,
		.gnutls_mac = GNUTLS_MAC_AEAD,
		.gnutls_cipher = GNUTLS_CIPHER_AES_256_GCM,
		.server_prio = 80,
	},
#endif
	{
		.oc_name = "AES128-SHA",
		.gnutls_name = "NONE:+VERS-DTLS0.9:+COMP-NULL:+AES-128-CBC:+SHA1:+RSA:%COMPAT:%DISABLE_SAFE_RENEGOTIATION",
		.gnutls_version = GNUTLS_DTLS0_9,
		.gnutls_mac = GNUTLS_MAC_SHA1,
		.gnutls_cipher = GNUTLS_CIPHER_AES_128_CBC,
		.server_prio = 50,
	},
	{
		.oc_name = "DES-CBC3-SHA",
		.gnutls_name = "NONE:+VERS-DTLS0.9:+COMP-NULL:+3DES-CBC:+SHA1:+RSA:%COMPAT:%DISABLE_SAFE_RENEGOTIATION",
		.gnutls_version = GNUTLS_DTLS0_9,
		.gnutls_mac = GNUTLS_MAC_SHA1,
		.gnutls_cipher = GNUTLS_CIPHER_3DES_CBC,
		.server_prio = 1,
	},
};

static void value_check(struct worker_st *ws, struct http_req_st *req)
{
	unsigned tmplen, i;
	int ret;
	size_t nlen, value_length;
	char *token, *value;
	char *str, *p;

	if (req->value.length <= 0)
		return;

	oclog(ws, LOG_HTTP_DEBUG, "HTTP: %.*s: %.*s", (int)req->header.length,
	      req->header.data, (int)req->value.length, req->value.data);

	value = malloc(req->value.length + 1);
	if (value == NULL)
		return;

	/* make sure the value is null terminated */
	value_length = req->value.length;
	memcpy(value, req->value.data, value_length);
	value[value_length] = 0;

	switch (req->next_header) {
	case HEADER_MASTER_SECRET:
		if (value_length < TLS_MASTER_SIZE * 2) {
			req->master_secret_set = 0;
			goto cleanup;
		}

		tmplen = TLS_MASTER_SIZE * 2;

		nlen = sizeof(req->master_secret);
		gnutls_hex2bin((void *)value, tmplen,
			       req->master_secret, &nlen);

		req->master_secret_set = 1;
		break;
	case HEADER_HOSTNAME:
		if (value_length + 1 > MAX_HOSTNAME_SIZE) {
			req->hostname[0] = 0;
			goto cleanup;
		}
		memcpy(req->hostname, value, value_length);
		req->hostname[value_length] = 0;
		break;
	case HEADER_USER_AGENT:
		if (value_length + 1 > MAX_AGENT_NAME) {
			req->user_agent[0] = 0;
			goto cleanup;
		}
		memcpy(req->user_agent, value, value_length);
		req->user_agent[value_length] = 0;
		break;

	case HEADER_DTLS_CIPHERSUITE:
		req->selected_ciphersuite = NULL;

		str = (char *)value;
		while ((token = strtok(str, ":")) != NULL) {
			for (i=0;i<sizeof(ciphersuites)/sizeof(ciphersuites[0]);i++) {
				if (strcmp(token, ciphersuites[i].oc_name) == 0) {
					if (req->selected_ciphersuite == NULL || 
						req->selected_ciphersuite->server_prio < ciphersuites[i].server_prio) {
						req->selected_ciphersuite = &ciphersuites[i];
					}
				}
			}
			str = NULL;
		}

		break;

	case HEADER_CSTP_MTU:
		req->cstp_mtu = atoi((char *)value);
		break;
	case HEADER_CSTP_ATYPE:
		if (memmem(value, value_length, "IPv4", 4) ==
		    NULL)
			req->no_ipv4 = 1;
		if (memmem(value, value_length, "IPv6", 4) ==
		    NULL)
			req->no_ipv6 = 1;
		break;
	case HEADER_DTLS_MTU:
		req->dtls_mtu = atoi((char *)value);
		break;
	case HEADER_COOKIE:

		str = (char *)value;
		while ((token = strtok(str, ";")) != NULL) {
			p = token;
			while(c_isspace(*p)) {
				p++;
			}
			tmplen = strlen(p);

			if (strncmp(p, "webvpn=", 7) == 0) {
				tmplen -= 7;
				p += 7;

				while(tmplen > 1 && c_isspace(p[tmplen-1])) {
					tmplen--;
				}

				nlen = sizeof(req->cookie);
				ret = base64_decode((char*)p, tmplen, (char*)req->cookie, &nlen);
				if (ret == 0 || nlen != COOKIE_SIZE) {
					oclog(ws, LOG_DEBUG, "could not decode cookie: %.*s", tmplen, p);
					req->cookie_set = 0;
				} else {
					req->cookie_set = 1;
				}
			} else if (strncmp(p, "webvpncontext=", 14) == 0) {
				p += 14;
				tmplen -= 14;

				while(tmplen > 1 && c_isspace(p[tmplen-1])) {
					tmplen--;
				}

				nlen = sizeof(ws->sid);
				ret = base64_decode((char*)p, tmplen, (char*)ws->sid, &nlen);
				if (ret == 0 || nlen != sizeof(ws->sid)) {
					oclog(ws, LOG_DEBUG, "could not decode sid: %.*s", tmplen, p);
					req->sid_cookie_set = 0;
				} else {
					req->sid_cookie_set = 1;
					oclog(ws, LOG_DEBUG, "received sid: %.*s", tmplen, p);
				}
			}

			str = NULL;
		}
		break;
	}

cleanup:
	free(value);
}

int header_field_cb(http_parser * parser, const char *at, size_t length)
{
	struct worker_st *ws = parser->data;
	struct http_req_st *req = &ws->req;
	int ret;

	if (req->header_state != HTTP_HEADER_RECV) {
		/* handle value */
		if (req->header_state == HTTP_HEADER_VALUE_RECV)
			value_check(ws, req);
		req->header_state = HTTP_HEADER_RECV;
		str_reset(&req->header);
	}

	ret = str_append_data(&req->header, at, length);
	if (ret < 0)
		return ret;

	return 0;
}

static void header_check(struct http_req_st *req)
{
	if (req->header.length == sizeof(STR_HDR_COOKIE) - 1 &&
	    strncmp((char *)req->header.data, STR_HDR_COOKIE,
		    req->header.length) == 0) {
		req->next_header = HEADER_COOKIE;
	} else if (req->header.length == sizeof(STR_HDR_MS) - 1 &&
		   strncmp((char *)req->header.data, STR_HDR_MS,
			   req->header.length) == 0) {
		req->next_header = HEADER_MASTER_SECRET;
	} else if (req->header.length == sizeof(STR_HDR_DMTU) - 1 &&
		   strncmp((char *)req->header.data, STR_HDR_DMTU,
			   req->header.length) == 0) {
		req->next_header = HEADER_DTLS_MTU;
	} else if (req->header.length == sizeof(STR_HDR_CMTU) - 1 &&
		   strncmp((char *)req->header.data, STR_HDR_CMTU,
			   req->header.length) == 0) {
		req->next_header = HEADER_CSTP_MTU;
	} else if (req->header.length == sizeof(STR_HDR_HOST) - 1 &&
		   strncmp((char *)req->header.data, STR_HDR_HOST,
			   req->header.length) == 0) {
		req->next_header = HEADER_HOSTNAME;
	} else if (req->header.length == sizeof(STR_HDR_CS) - 1 &&
		   strncmp((char *)req->header.data, STR_HDR_CS,
			   req->header.length) == 0) {
		req->next_header = HEADER_DTLS_CIPHERSUITE;
	} else if (req->header.length == sizeof(STR_HDR_ATYPE) - 1 &&
		   strncmp((char *)req->header.data, STR_HDR_ATYPE,
			   req->header.length) == 0) {
		req->next_header = HEADER_CSTP_ATYPE;
	} else if (req->header.length == sizeof(STR_HDR_CONNECTION) - 1 &&
		   strncmp((char *)req->header.data, STR_HDR_CONNECTION,
			   req->header.length) == 0) {
		req->next_header = HEADER_CONNECTION;
	} else if (req->header.length == sizeof(STR_HDR_USER_AGENT) - 1 &&
		   strncmp((char *)req->header.data, STR_HDR_USER_AGENT,
			   req->header.length) == 0) {
		req->next_header = HEADER_USER_AGENT;
	} else {
		req->next_header = 0;
	}
}

int header_value_cb(http_parser * parser, const char *at, size_t length)
{
	struct worker_st *ws = parser->data;
	struct http_req_st *req = &ws->req;
	int ret;

	if (req->header_state != HTTP_HEADER_VALUE_RECV) {
		/* handle header */
		header_check(req);
		req->header_state = HTTP_HEADER_VALUE_RECV;
		str_reset(&req->value);
	}

	ret = str_append_data(&req->value, at, length);
	if (ret < 0)
		return ret;

	return 0;
}

int header_complete_cb(http_parser * parser)
{
	struct worker_st *ws = parser->data;
	struct http_req_st *req = &ws->req;

	/* handle header value */
	value_check(ws, req);

	req->headers_complete = 1;
	return 0;
}

int message_complete_cb(http_parser * parser)
{
	struct worker_st *ws = parser->data;
	struct http_req_st *req = &ws->req;

	req->message_complete = 1;
	return 0;
}

int body_cb(http_parser * parser, const char *at, size_t length)
{
	struct worker_st *ws = parser->data;
	struct http_req_st *req = &ws->req;
	char *tmp;

	tmp = safe_realloc(req->body, req->body_length + length + 1);
	if (tmp == NULL)
		return 1;

	memcpy(&tmp[req->body_length], at, length);
	req->body_length += length;
	tmp[req->body_length] = 0;

	req->body = tmp;
	return 0;
}

static int setup_dtls_connection(struct worker_st *ws)
{
	int ret;
	gnutls_session_t session;
	gnutls_datum_t master =
	    { ws->master_secret, sizeof(ws->master_secret) };
	gnutls_datum_t sid = { ws->session_id, sizeof(ws->session_id) };

	if (ws->req.selected_ciphersuite == NULL) {
		oclog(ws, LOG_ERR, "no DTLS ciphersuite negotiated");
		return -1;
	}

	oclog(ws, LOG_INFO, "setting up DTLS connection");
	/* DTLS cookie verified.
	 * Initialize session.
	 */
	ret = gnutls_init(&session, GNUTLS_SERVER | GNUTLS_DATAGRAM);
	if (ret < 0) {
		oclog(ws, LOG_ERR, "could not initialize TLS session: %s",
		      gnutls_strerror(ret));
		return -1;
	}

	ret =
	    gnutls_priority_set_direct(session, ws->req.selected_ciphersuite->gnutls_name,
				       NULL);
	if (ret < 0) {
		oclog(ws, LOG_ERR, "could not set TLS priority: %s",
		      gnutls_strerror(ret));
		goto fail;
	}

	ret = gnutls_session_set_premaster(session, GNUTLS_SERVER,
					   ws->req.selected_ciphersuite->gnutls_version,
					   GNUTLS_KX_RSA, ws->req.selected_ciphersuite->gnutls_cipher,
					   ws->req.selected_ciphersuite->gnutls_mac, GNUTLS_COMP_NULL,
					   &master, &sid);
	if (ret < 0) {
		oclog(ws, LOG_ERR, "could not set TLS premaster: %s",
		      gnutls_strerror(ret));
		goto fail;
	}

	ret =
	    gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE,
				   ws->creds->xcred);
	if (ret < 0) {
		oclog(ws, LOG_ERR, "could not set TLS credentials: %s",
		      gnutls_strerror(ret));
		goto fail;
	}

	gnutls_transport_set_ptr(session,
				 (gnutls_transport_ptr_t) (long)ws->udp_fd);
	gnutls_session_set_ptr(session, ws);
	gnutls_certificate_server_set_request(session, GNUTLS_CERT_IGNORE);

	gnutls_handshake_set_timeout(session, GNUTLS_DEFAULT_HANDSHAKE_TIMEOUT);

	ws->udp_state = UP_HANDSHAKE;

	ws->dtls_session = session;

	return 0;
 fail:
	gnutls_deinit(session);
	return -1;
}

static void http_req_init(worker_st * ws)
{
	str_init(&ws->req.header);
	str_init(&ws->req.value);
}

static void http_req_reset(worker_st * ws)
{
	ws->req.headers_complete = 0;
	ws->req.message_complete = 0;
	ws->req.body_length = 0;
	ws->req.url[0] = 0;

	ws->req.header_state = HTTP_HEADER_INIT;
	str_reset(&ws->req.header);
	str_reset(&ws->req.value);
}

static void http_req_deinit(worker_st * ws)
{
	http_req_reset(ws);
	str_clear(&ws->req.header);
	str_clear(&ws->req.value);
	free(ws->req.body);
	ws->req.body = NULL;
}

static
void exit_worker(worker_st * ws)
{
	closelog();
	exit(1);
}

/* vpn_server:
 * @ws: an initialized worker structure
 *
 * This is the main worker process. It is executed
 * by the main server after fork and drop of privileges.
 *
 * It handles the client connection including:
 *  - HTTPS authentication using XML forms that are parsed and
 *    forwarded to main.
 *  - TLS authentication (using certificate)
 *  - TCP VPN tunnel establishment (after HTTP CONNECT)
 *  - UDP VPN tunnel establishment (once an FD is forwarded by main)
 *
 */
void vpn_server(struct worker_st *ws)
{
	unsigned char buf[2048];
	int ret;
	ssize_t nparsed, nrecvd;
	gnutls_session_t session;
	http_parser parser;
	http_parser_settings settings;
	url_handler_fn fn;
	int requests_left = MAX_HTTP_REQUESTS;

	ocsignal(SIGTERM, handle_term);
	ocsignal(SIGINT, handle_term);
	ocsignal(SIGHUP, SIG_IGN);
	ocsignal(SIGALRM, handle_alarm);

	if (ws->config->auth_timeout)
		alarm(ws->config->auth_timeout);

	ret = disable_system_calls(ws);
	if (ret < 0) {
		oclog(ws, LOG_INFO,
		      "could not disable system calls, kernel might not support seccomp");
	}

	oclog(ws, LOG_INFO, "accepted connection");
	if (ws->remote_addr_len == sizeof(struct sockaddr_in))
		ws->proto = AF_INET;
	else
		ws->proto = AF_INET6;

	ret = gnutls_rnd(GNUTLS_RND_NONCE, ws->sid, sizeof(ws->sid));
	if (ret < 0) {
		oclog(ws, LOG_ERR, "Error generating SID");
		exit_worker(ws);
	}

	/* initialize the session */
	ret = gnutls_init(&session, GNUTLS_SERVER);
	GNUTLS_FATAL_ERR(ret);

	ret = gnutls_priority_set(session, ws->creds->cprio);
	GNUTLS_FATAL_ERR(ret);

	ret =
	    gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE,
				   ws->creds->xcred);
	GNUTLS_FATAL_ERR(ret);

	gnutls_certificate_server_set_request(session, ws->config->cert_req);
	gnutls_transport_set_ptr(session,
				 (gnutls_transport_ptr_t) (long)ws->conn_fd);
	set_resume_db_funcs(session);
	gnutls_session_set_ptr(session, ws);
	gnutls_db_set_ptr(session, ws);
	gnutls_db_set_cache_expiration(session, TLS_SESSION_EXPIRATION_TIME);

	gnutls_handshake_set_timeout(session, GNUTLS_DEFAULT_HANDSHAKE_TIMEOUT);
	do {
		ret = gnutls_handshake(session);
	} while (ret < 0 && gnutls_error_is_fatal(ret) == 0);
	GNUTLS_S_FATAL_ERR(session, ret);

	oclog(ws, LOG_DEBUG, "TLS handshake completed");

	memset(&settings, 0, sizeof(settings));

	settings.on_url = url_cb;
	settings.on_header_field = header_field_cb;
	settings.on_header_value = header_value_cb;
	settings.on_headers_complete = header_complete_cb;
	settings.on_message_complete = message_complete_cb;
	settings.on_body = body_cb;
	http_req_init(ws);

	ws->session = session;
	ws->parser = &parser;

 restart:
	if (requests_left-- <= 0) {
		oclog(ws, LOG_INFO, "maximum number of HTTP requests reached");
		exit_worker(ws);
	}

	http_parser_init(&parser, HTTP_REQUEST);
	parser.data = ws;
	http_req_reset(ws);
	/* parse as we go */
	do {
		nrecvd = tls_recv(session, buf, sizeof(buf));
		if (nrecvd <= 0) {
			if (nrecvd == 0)
				goto finish;
			oclog(ws, LOG_INFO, "error receiving client data");
			exit_worker(ws);
		}

		nparsed =
		    http_parser_execute(&parser, &settings, (void *)buf,
					nrecvd);
		if (nparsed == 0) {
			oclog(ws, LOG_INFO, "error parsing HTTP request");
			exit_worker(ws);
		}
	} while (ws->req.headers_complete == 0);

	if (parser.method == HTTP_GET) {
		oclog(ws, LOG_HTTP_DEBUG, "HTTP GET %s", ws->req.url);
		fn = get_url_handler(ws->req.url);
		if (fn == NULL) {
			oclog(ws, LOG_INFO, "unexpected URL %s", ws->req.url);
			tls_puts(session, "HTTP/1.1 404 Not found\r\n\r\n");
			goto finish;
		}
		ret = fn(ws, parser.http_minor);
		if (ret == 0
		    && (parser.http_major != 1 || parser.http_minor != 0))
			goto restart;

	} else if (parser.method == HTTP_POST) {
		/* continue reading */
		oclog(ws, LOG_HTTP_DEBUG, "HTTP POST %s", ws->req.url);
		while (ws->req.message_complete == 0) {
			nrecvd = tls_recv(session, buf, sizeof(buf));
			GNUTLS_FATAL_ERR(nrecvd);

			nparsed =
			    http_parser_execute(&parser, &settings, (void *)buf,
						nrecvd);
			if (nparsed == 0) {
				oclog(ws, LOG_INFO,
				      "error parsing HTTP request");
				exit_worker(ws);
			}
		}

		fn = post_url_handler(ws->req.url);
		if (fn == NULL) {
			oclog(ws, LOG_INFO, "unexpected POST URL %s",
			      ws->req.url);
			tls_puts(session, "HTTP/1.1 404 Not found\r\n\r\n");
			goto finish;
		}

		ret = fn(ws, parser.http_minor);
		if (ret == 0
		    && (parser.http_major != 1 || parser.http_minor != 0))
			goto restart;

	} else if (parser.method == HTTP_CONNECT) {
		oclog(ws, LOG_HTTP_DEBUG, "HTTP CONNECT %s", ws->req.url);
		ret = connect_handler(ws);
		if (ret == 0
		    && (parser.http_major != 1 || parser.http_minor != 0))
			goto restart;

	} else {
		oclog(ws, LOG_INFO, "unexpected HTTP method %s",
		      http_method_str(parser.method));
		tls_printf(session, "HTTP/1.%u 404 Nah, go away\r\n\r\n",
			   parser.http_minor);
	}

 finish:
	tls_close(session);
}

static
void mtu_send(worker_st * ws, unsigned mtu)
{
	TunMtuMsg msg = TUN_MTU_MSG__INIT;

	msg.mtu = mtu - 1;	/* account DTLS CSTP header */
	send_msg_to_main(ws, CMD_TUN_MTU, &msg,
			 (pack_size_func) tun_mtu_msg__get_packed_size,
			 (pack_func) tun_mtu_msg__pack);

	oclog(ws, LOG_INFO, "setting MTU to %u", msg.mtu);
}

static
void session_info_send(worker_st * ws)
{
	SessionInfoMsg msg = SESSION_INFO_MSG__INIT;

	if (ws->session) {
		msg.tls_ciphersuite = gnutls_session_get_desc(ws->session);
	}

	if (ws->udp_state != UP_DISABLED) {
		msg.dtls_ciphersuite = (char*)ws->req.selected_ciphersuite->oc_name;
	}

	if (ws->req.user_agent[0] != 0) {
		msg.user_agent = ws->req.user_agent;
	}

	send_msg_to_main(ws, CMD_SESSION_INFO, &msg,
			 (pack_size_func) session_info_msg__get_packed_size,
			 (pack_func) session_info_msg__pack);

	gnutls_free(msg.tls_ciphersuite);
}

static
void mtu_set(worker_st * ws, unsigned mtu)
{
	ws->conn_mtu = mtu;

	if (ws->dtls_session)
		gnutls_dtls_set_data_mtu(ws->dtls_session, ws->conn_mtu);

	mtu_send(ws, ws->conn_mtu);
}

/* sets the current value of mtu as bad,
 * and returns an estimation of good.
 *
 * Returns -1 on failure.
 */
static
int mtu_not_ok(worker_st * ws)
{
unsigned min = MIN_MTU(ws);

	ws->last_bad_mtu = ws->conn_mtu;

	if (ws->last_good_mtu == min) {
		oclog(ws, LOG_INFO,
		      "could not calculate a sufficient MTU. Disabling DTLS.");
		ws->udp_state = UP_DISABLED;
		return -1;
	}

	if (ws->last_good_mtu >= ws->conn_mtu) {
		ws->last_good_mtu = MAX(((2 * (ws->conn_mtu)) / 3), min);
	}

	mtu_set(ws, ws->last_good_mtu);
	oclog(ws, LOG_INFO, "MTU %u is too large, switching to %u",
	      ws->last_bad_mtu, ws->conn_mtu);

	return 0;
}

static void mtu_discovery_init(worker_st * ws, unsigned mtu)
{
	ws->last_good_mtu = mtu;
	ws->last_bad_mtu = mtu;
}

static
void mtu_ok(worker_st * ws)
{
	unsigned int c;

	if (ws->last_bad_mtu == (ws->conn_mtu) + 1 ||
	    ws->last_bad_mtu == (ws->conn_mtu))
		return;

	ws->last_good_mtu = ws->conn_mtu;
	c = (ws->conn_mtu + ws->last_bad_mtu) / 2;

	mtu_set(ws, c);
	return;
}

static
int periodic_check(worker_st * ws, unsigned mtu_overhead, time_t now)
{
	socklen_t sl;
	int max, e, ret;

	if (now - ws->last_periodic_check < PERIODIC_CHECK_TIME)
		return 0;

	/* check DPD. Otherwise exit */
	if (ws->udp_state == UP_ACTIVE
	    && now - ws->last_msg_udp > DPD_TRIES * ws->config->dpd) {
		oclog(ws, LOG_ERR,
		      "have not received UDP any message or DPD for long (%d secs)",
		      (int)(now - ws->last_msg_udp));

		ws->buffer[0] = AC_PKT_DPD_OUT;
		tls_send(ws->dtls_session, ws->buffer, 1);

		if (now - ws->last_msg_udp > DPD_MAX_TRIES * ws->config->dpd) {
			oclog(ws, LOG_ERR,
			      "have not received UDP message or DPD for very long; disabling UDP port");
			ws->udp_state = UP_INACTIVE;
		}
	}
	if (now - ws->last_msg_tcp > DPD_TRIES * ws->config->dpd) {
		oclog(ws, LOG_ERR,
		      "have not received TCP DPD for long (%d secs)",
		      (int)(now - ws->last_msg_tcp));
		ws->buffer[0] = 'S';
		ws->buffer[1] = 'T';
		ws->buffer[2] = 'F';
		ws->buffer[3] = 1;
		ws->buffer[4] = 0;
		ws->buffer[5] = 0;
		ws->buffer[6] = AC_PKT_DPD_OUT;
		ws->buffer[7] = 0;

		tls_send(ws->session, ws->buffer, 8);

		if (now - ws->last_msg_tcp > DPD_MAX_TRIES * ws->config->dpd) {
			oclog(ws, LOG_ERR,
			      "have not received TCP DPD for very long; tearing down connection");
			return -1;
		}
	}

	sl = sizeof(max);
	ret = getsockopt(ws->conn_fd, IPPROTO_TCP, TCP_MAXSEG, &max, &sl);
	if (ret == -1) {
		e = errno;
		oclog(ws, LOG_INFO, "error in getting TCP_MAXSEG: %s",
		      strerror(e));
	} else {
		max -= 13;
		oclog(ws, LOG_DEBUG, "TCP MSS is %u", max);
		if (max > 0 && max - mtu_overhead < ws->conn_mtu) {
			oclog(ws, LOG_INFO, "reducing MTU due to TCP MSS to %u",
			      max - mtu_overhead);
			mtu_set(ws, MIN(ws->conn_mtu, max - mtu_overhead));
		}
	}

	ws->last_periodic_check = now;

	return 0;
}

#define TOSCLASS(x) (IPTOS_CLASS_CS##x)

static void set_net_priority(worker_st * ws, int fd, int priority)
{
	int t;
	int ret;
#if defined(IP_TOS)
	if (priority != 0 && IS_TOS(priority)) {
		t = TOS_UNPACK(priority);
		ret = setsockopt(fd, IPPROTO_IP, IP_TOS, &t, sizeof(t));
		if (ret == -1)
			oclog(ws, LOG_DEBUG,
			      "setsockopt(IP_TOS) to %x, failed.", (unsigned)t);

		return;
	}
#endif

#ifdef SO_PRIORITY
	if (priority != 0 && priority <= 7) {
		t = ws->config->net_priority - 1;
		ret = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &t, sizeof(t));
		if (ret == -1)
			oclog(ws, LOG_DEBUG,
			      "setsockopt(SO_PRIORITY) to %d, failed.", t);

		return;
	}
#endif
	return;
}

#define CSTP_DTLS_OVERHEAD 1
#define CSTP_OVERHEAD 8

#define SEND_ERR(x) if (x<0) goto send_error

/* connect_handler:
 * @ws: an initialized worker structure
 *
 * This function handles the HTTPS session after a CONNECT
 * command has been issued by the peer. The @ws->auth_state
 * should be set to %S_AUTH_COMPLETE or the client will be
 * disconnected.
 *
 * If the user is authenticate it handles the TCP and UDP VPN 
 * tunnels.
 *
 */
static int connect_handler(worker_st * ws)
{
	struct http_req_st *req = &ws->req;
	fd_set rfds;
	int l, e, max, ret, overhead;
	unsigned tls_retry, dtls_mtu, cstp_mtu;
	char *p;
#ifdef HAVE_PSELECT
	struct timespec tv;
#else
	struct timeval tv;
#endif
	unsigned tls_pending, dtls_pending = 0, i;
	time_t udp_recv_time = 0, now;
	struct timespec tnow;
	unsigned mtu_overhead = 0;
	int sndbuf;
	socklen_t sl;
	bandwidth_st b_tx;
	bandwidth_st b_rx;
	sigset_t emptyset, blockset;

	sigemptyset(&blockset);
	sigemptyset(&emptyset);
	sigaddset(&blockset, SIGTERM);

	ws->buffer_size = 16 * 1024;
	ws->buffer = malloc(ws->buffer_size);
	if (ws->buffer == NULL) {
		oclog(ws, LOG_INFO, "memory error");
		tls_puts(ws->session,
			 "HTTP/1.1 503 Service Unavailable\r\n\r\n");
		tls_close(ws->session);
		exit_worker(ws);
	}

	if (ws->auth_state != S_AUTH_COMPLETE && req->cookie_set == 0) {
		oclog(ws, LOG_INFO, "connect request without authentication");
		tls_puts(ws->session,
			 "HTTP/1.1 503 Service Unavailable\r\n\r\n");
		tls_fatal_close(ws->session, GNUTLS_A_ACCESS_DENIED);
		exit_worker(ws);
	}

	if (ws->auth_state != S_AUTH_COMPLETE) {
		/* authentication didn't occur in this session. Use the
		 * cookie */
		ret = auth_cookie(ws, req->cookie, sizeof(req->cookie));
		if (ret < 0) {
			oclog(ws, LOG_INFO,
			      "failed cookie authentication attempt");
			tls_puts(ws->session,
				 "HTTP/1.1 503 Service Unavailable\r\n\r\n");
			tls_fatal_close(ws->session, GNUTLS_A_ACCESS_DENIED);
			exit_worker(ws);
		}
	}

	if (strcmp(req->url, "/CSCOSSLC/tunnel") != 0) {
		oclog(ws, LOG_INFO, "bad connect request: '%s'\n", req->url);
		tls_puts(ws->session, "HTTP/1.1 404 Nah, go away\r\n\r\n");
		tls_fatal_close(ws->session, GNUTLS_A_ACCESS_DENIED);
		exit_worker(ws);
	}

	if (ws->config->network.name == NULL) {
		oclog(ws, LOG_ERR,
		      "no networks are configured; rejecting client");
		tls_puts(ws->session, "HTTP/1.1 503 Service Unavailable\r\n");
		tls_puts(ws->session,
			 "X-Reason: Server configuration error\r\n\r\n");
		return -1;
	}

	ret = get_rt_vpn_info(ws, &ws->vinfo, (char *)ws->buffer, ws->buffer_size);
	if (ret < 0) {
		oclog(ws, LOG_ERR,
		      "no networks are configured; rejecting client");
		tls_puts(ws->session, "HTTP/1.1 503 Service Unavailable\r\n");
		tls_puts(ws->session,
			 "X-Reason: Server configuration error\r\n\r\n");
		return -1;
	}

	/* Connected. Turn of the alarm */
	if (ws->config->auth_timeout)
		alarm(0);
	http_req_deinit(ws);

	tls_cork(ws->session);
	ret = tls_puts(ws->session, "HTTP/1.1 200 CONNECTED\r\n");
	SEND_ERR(ret);

	ret = tls_puts(ws->session, "X-CSTP-Version: 1\r\n");
	SEND_ERR(ret);

	ret = tls_printf(ws->session, "X-CSTP-DPD: %u\r\n", ws->config->dpd);
	SEND_ERR(ret);

	if (ws->config->default_domain) {
		ret =
		    tls_printf(ws->session, "X-CSTP-Default-Domain: %s\r\n",
			       ws->config->default_domain);
		SEND_ERR(ret);
	}

	ws->udp_state = UP_DISABLED;
	if (ws->config->udp_port != 0 && req->master_secret_set != 0) {
		memcpy(ws->master_secret, req->master_secret, TLS_MASTER_SIZE);
		ws->udp_state = UP_WAIT_FD;
	} else {
		oclog(ws, LOG_DEBUG, "disabling UDP (DTLS) connection");
	}

	if (ws->vinfo.ipv4 && req->no_ipv4 == 0) {
		oclog(ws, LOG_DEBUG, "sending IPv4 %s", ws->vinfo.ipv4);
		ret =
		    tls_printf(ws->session, "X-CSTP-Address: %s\r\n",
			       ws->vinfo.ipv4);
		SEND_ERR(ret);

		if (ws->vinfo.ipv4_netmask) {
			ret =
			    tls_printf(ws->session, "X-CSTP-Netmask: %s\r\n",
				       ws->vinfo.ipv4_netmask);
			SEND_ERR(ret);
		}

		if (ws->vinfo.ipv4_dns) {
			ret =
			    tls_printf(ws->session, "X-CSTP-DNS: %s\r\n",
				       ws->vinfo.ipv4_dns);
			SEND_ERR(ret);
		}

		if (ws->vinfo.ipv4_nbns) {
			ret =
			    tls_printf(ws->session, "X-CSTP-NBNS: %s\r\n",
				       ws->vinfo.ipv4_nbns);
			SEND_ERR(ret);
		}
	}

	if (ws->vinfo.ipv6 && req->no_ipv6 == 0) {
		oclog(ws, LOG_DEBUG, "sending IPv6 %s", ws->vinfo.ipv6);
		ret =
		    tls_printf(ws->session, "X-CSTP-Address: %s\r\n",
			       ws->vinfo.ipv6);
		SEND_ERR(ret);

		if (ws->vinfo.ipv6_netmask) {
			ret =
			    tls_printf(ws->session, "X-CSTP-Netmask: %s\r\n",
				       ws->vinfo.ipv6_netmask);
			SEND_ERR(ret);
		}

		if (ws->vinfo.ipv6_dns) {
			ret =
			    tls_printf(ws->session, "X-CSTP-DNS: %s\r\n",
				       ws->vinfo.ipv6_dns);
			SEND_ERR(ret);
		}

		if (ws->vinfo.ipv6_nbns) {
			ret =
			    tls_printf(ws->session, "X-CSTP-NBNS: %s\r\n",
				       ws->vinfo.ipv6_nbns);
			SEND_ERR(ret);
		}
	}

	for (i = 0; i < ws->vinfo.routes_size; i++) {
		if (req->no_ipv6 != 0 && strchr(ws->vinfo.routes[i], ':') != 0)
			continue;
		if (req->no_ipv4 != 0 && strchr(ws->vinfo.routes[i], '.') != 0)
			continue;
		oclog(ws, LOG_DEBUG, "adding route %s", ws->vinfo.routes[i]);
		ret = tls_printf(ws->session,
				 "X-CSTP-Split-Include: %s\r\n",
				 ws->vinfo.routes[i]);
		SEND_ERR(ret);
	}

	for (i = 0; i < ws->routes_size; i++) {
		if (req->no_ipv6 != 0 && strchr(ws->routes[i], ':') != 0)
			continue;
		if (req->no_ipv4 != 0 && strchr(ws->routes[i], '.') != 0)
			continue;
		oclog(ws, LOG_DEBUG, "adding private route %s", ws->routes[i]);
		ret = tls_printf(ws->session,
				 "X-CSTP-Split-Include: %s\r\n", ws->routes[i]);
		SEND_ERR(ret);
	}
	ret =
	    tls_printf(ws->session, "X-CSTP-Keepalive: %u\r\n",
		       ws->config->keepalive);
	SEND_ERR(ret);

	ret =
	    tls_puts(ws->session,
		     "X-CSTP-Smartcard-Removal-Disconnect: true\r\n");
	SEND_ERR(ret);

	ret =
	    tls_printf(ws->session, "X-CSTP-Rekey-Time: %u\r\n",
		       (unsigned)(2 * ws->config->cookie_validity) / 3);
	SEND_ERR(ret);
	ret = tls_puts(ws->session, "X-CSTP-Rekey-Method: new-tunnel\r\n");
	SEND_ERR(ret);

	ret = tls_puts(ws->session, "X-CSTP-Session-Timeout: none\r\n"
		       "X-CSTP-Idle-Timeout: none\r\n"
		       "X-CSTP-Disconnected-Timeout: none\r\n"
		       "X-CSTP-Keep: true\r\n"
		       "X-CSTP-TCP-Keepalive: true\r\n"
		       "X-CSTP-Tunnel-All-DNS: false\r\n"
		       "X-CSTP-License: accept\r\n");
	SEND_ERR(ret);

	if (ws->config->default_mtu > 0) {
		ws->vinfo.mtu = ws->config->default_mtu;
	}

	mtu_overhead = CSTP_OVERHEAD;
	ws->conn_mtu = ws->vinfo.mtu - mtu_overhead;

	if (req->cstp_mtu > 0) {
		oclog(ws, LOG_DEBUG, "peer's CSTP MTU is %u (ignored)", req->cstp_mtu);
	}

	sl = sizeof(max);
	ret = getsockopt(ws->conn_fd, IPPROTO_TCP, TCP_MAXSEG, &max, &sl);
	if (ret == -1) {
		e = errno;
		oclog(ws, LOG_INFO, "error in getting TCP_MAXSEG: %s",
		      strerror(e));
	} else {
		max -= 13;
		oclog(ws, LOG_DEBUG, "TCP MSS is %u", max);
		if (max > 0 && max - mtu_overhead < ws->conn_mtu) {
			oclog(ws, LOG_DEBUG,
			      "reducing MTU due to TCP MSS to %u",
			      max - mtu_overhead);
		}
		ws->conn_mtu = MIN(ws->conn_mtu, max - mtu_overhead);
	}

	/* set TCP socket options */
	if (ws->config->output_buffer > 0) {
		sndbuf = ws->conn_mtu * ws->config->output_buffer;
		ret =
		    setsockopt(ws->conn_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf,
			       sizeof(sndbuf));
		if (ret == -1)
			oclog(ws, LOG_DEBUG,
			      "setsockopt(TCP, SO_SNDBUF) to %u, failed.",
			      sndbuf);
	}

	set_net_priority(ws, ws->conn_fd, ws->config->net_priority);

	if (ws->udp_state != UP_DISABLED) {

		p = (char *)ws->buffer;
		for (i = 0; i < sizeof(ws->session_id); i++) {
			sprintf(p, "%.2x", (unsigned int)ws->session_id[i]);
			p += 2;
		}
		ret =
		    tls_printf(ws->session, "X-DTLS-Session-ID: %s\r\n",
			       ws->buffer);
		SEND_ERR(ret);

		ret =
		    tls_printf(ws->session, "X-DTLS-DPD: %u\r\n",
			       ws->config->dpd);
		SEND_ERR(ret);

		ret =
		    tls_printf(ws->session, "X-DTLS-Port: %u\r\n",
			       ws->config->udp_port);
		SEND_ERR(ret);

		ret =
		    tls_printf(ws->session, "X-DTLS-Rekey-Time: %u\r\n",
			       (unsigned)(2 * ws->config->cookie_validity) / 3);
		SEND_ERR(ret);

		ret =
		    tls_printf(ws->session, "X-DTLS-Keepalive: %u\r\n",
			       ws->config->keepalive);
		SEND_ERR(ret);

		oclog(ws, LOG_INFO, "DTLS ciphersuite: %s",
		      ws->req.selected_ciphersuite->oc_name);
		ret =
		    tls_printf(ws->session, "X-DTLS-CipherSuite: %s\r\n",
			       ws->req.selected_ciphersuite->oc_name);
		SEND_ERR(ret);

		/* assume that if IPv6 is used over TCP then the same would be used over UDP */
		if (ws->proto == AF_INET)
			mtu_overhead = 20 + CSTP_DTLS_OVERHEAD;	/* ip */
		else
			mtu_overhead = 40 + CSTP_DTLS_OVERHEAD;	/* ipv6 */
		mtu_overhead += 8;	/* udp */
		ws->conn_mtu = MIN(ws->conn_mtu, ws->vinfo.mtu - mtu_overhead);


		overhead =
		    CSTP_DTLS_OVERHEAD +
		    tls_get_overhead(ws->req.selected_ciphersuite->gnutls_version,
				     ws->req.selected_ciphersuite->gnutls_cipher, ws->req.selected_ciphersuite->gnutls_mac);

		if (req->dtls_mtu <= 0)
			req->dtls_mtu = req->cstp_mtu;
		if (req->dtls_mtu > 0) {
			ws->conn_mtu = MIN(req->dtls_mtu+overhead+mtu_overhead, ws->conn_mtu);
			oclog(ws, LOG_DEBUG,
			      "peer's DTLS MTU is %u (overhead: %u)", req->dtls_mtu, mtu_overhead+overhead);
		}

		dtls_mtu = ws->conn_mtu - overhead;

		tls_printf(ws->session, "X-DTLS-MTU: %u\r\n", dtls_mtu);
		oclog(ws, LOG_DEBUG, "suggesting DTLS MTU %u", dtls_mtu);

		if (ws->config->output_buffer > 0) {
			sndbuf = ws->conn_mtu * ws->config->output_buffer;
			setsockopt(ws->udp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf,
				   sizeof(sndbuf));
			if (ret == -1)
				oclog(ws, LOG_DEBUG,
				      "setsockopt(UDP, SO_SNDBUF) to %u, failed.",
				      sndbuf);
		}

		set_net_priority(ws, ws->udp_fd, ws->config->net_priority);
	} else
		dtls_mtu = 0;

	if (ws->buffer_size <= ws->conn_mtu + mtu_overhead) {
		oclog(ws, LOG_WARNING,
		      "buffer size is smaller than MTU (%u < %u); adjusting",
		      ws->buffer_size, ws->conn_mtu);
		ws->buffer_size = ws->conn_mtu + mtu_overhead;
		ws->buffer = safe_realloc(ws->buffer, ws->buffer_size);
		if (ws->buffer == NULL)
			goto exit;
	}

	overhead =
	    CSTP_OVERHEAD +
	    tls_get_overhead(gnutls_protocol_get_version(ws->session),
			     gnutls_cipher_get(ws->session),
			     gnutls_mac_get(ws->session));
	cstp_mtu = ws->conn_mtu - overhead;
	if (dtls_mtu > 0)	/* this is a hack for openconnect which reads a single MTU value */
		cstp_mtu = MIN(cstp_mtu, dtls_mtu);

	ret = tls_printf(ws->session, "X-CSTP-MTU: %u\r\n", cstp_mtu);
	SEND_ERR(ret);
	oclog(ws, LOG_DEBUG, "suggesting CSTP MTU %u", cstp_mtu);

	oclog(ws, LOG_DEBUG, "plaintext MTU is %u", ws->conn_mtu - 1);

	mtu_send(ws, ws->conn_mtu);

	if (ws->config->banner) {
		ret =
		    tls_printf(ws->session, "X-CSTP-Banner: %s\r\n",
			       ws->config->banner);
		SEND_ERR(ret);
	}

	ret = tls_puts(ws->session, "\r\n");
	SEND_ERR(ret);

	ret = tls_uncork(ws->session);
	SEND_ERR(ret);

	/* start dead peer detection */
	gettime(&tnow);
	ws->last_msg_tcp = ws->last_msg_udp = tnow.tv_sec;

	bandwidth_init(&b_rx, ws->config->rx_per_sec);
	bandwidth_init(&b_tx, ws->config->tx_per_sec);

	session_info_send(ws);

	/* main loop  */
	for (;;) {
		FD_ZERO(&rfds);

		FD_SET(ws->conn_fd, &rfds);
		FD_SET(ws->cmd_fd, &rfds);
		FD_SET(ws->tun_fd, &rfds);
		max = MAX(ws->cmd_fd, ws->conn_fd);
		max = MAX(max, ws->tun_fd);

		if (ws->udp_state > UP_WAIT_FD) {
			FD_SET(ws->udp_fd, &rfds);
			max = MAX(max, ws->udp_fd);
		}

		if (terminate != 0) {
			ws->buffer[0] = 'S';
			ws->buffer[1] = 'T';
			ws->buffer[2] = 'F';
			ws->buffer[3] = 1;
			ws->buffer[4] = 0;
			ws->buffer[5] = 0;
			ws->buffer[6] = AC_PKT_TERM_SERVER;
			ws->buffer[7] = 0;

			oclog(ws, LOG_DEBUG,
			      "sending disconnect message in TLS channel");
			ret = tls_send(ws->session, ws->buffer, 8);
			GNUTLS_FATAL_ERR(ret);
			goto exit;
		}

		tls_pending = gnutls_record_check_pending(ws->session);

		if (ws->dtls_session != NULL)
			dtls_pending =
			    gnutls_record_check_pending(ws->dtls_session);
		if (tls_pending == 0 && dtls_pending == 0) {
#ifdef HAVE_PSELECT
			tv.tv_nsec = 0;
			tv.tv_sec = 10;
			ret = pselect(max + 1, &rfds, NULL, NULL, &tv, &emptyset);
#else
			tv.tv_usec = 0;
			tv.tv_sec = 10;
			sigprocmask(SIG_UNBLOCK, &blockset, NULL);
			ret = select(max + 1, &rfds, NULL, NULL, &tv);
			sigprocmask(SIG_BLOCK, &blockset, NULL);
#endif
			if (ret == -1) {
				if (errno == EINTR)
					continue;
				goto exit;
			}
		}
		gettime(&tnow);
		now = tnow.tv_sec;

		if (periodic_check(ws, mtu_overhead, now) < 0)
			goto exit;

		if (FD_ISSET(ws->tun_fd, &rfds)) {
			l = read(ws->tun_fd, ws->buffer + 8, ws->conn_mtu - 1);
			if (l < 0) {
				e = errno;

				if (e != EAGAIN && e != EINTR) {
					oclog(ws, LOG_ERR,
					      "received corrupt data from tun (%d): %s",
					      l, strerror(e));
					goto exit;
				}
				continue;
			}

			if (l == 0) {
				oclog(ws, LOG_INFO, "TUN device returned zero");
				continue;
			}

			/* only transmit if allowed */
			if (bandwidth_update(&b_tx, l - 1, ws->conn_mtu, &tnow)
			    != 0) {
				tls_retry = 0;
				oclog(ws, LOG_DEBUG, "sending %d byte(s)\n", l);
				if (ws->udp_state == UP_ACTIVE) {
					ws->buffer[7] = AC_PKT_DATA;

					ret =
					    tls_send_nowait(ws->dtls_session,
							    ws->buffer + 7,
							    l + 1);
					GNUTLS_FATAL_ERR(ret);

					if (ret == GNUTLS_E_LARGE_PACKET) {
						mtu_not_ok(ws);

						oclog(ws, LOG_DEBUG,
						      "retrying (TLS) %d\n", l);
						tls_retry = 1;
					} else if (ret >= ws->conn_mtu
						   && ws->config->try_mtu !=
						   0) {
						mtu_ok(ws);
					}
				}

				if (ws->udp_state != UP_ACTIVE
				    || tls_retry != 0) {
					ws->buffer[0] = 'S';
					ws->buffer[1] = 'T';
					ws->buffer[2] = 'F';
					ws->buffer[3] = 1;
					ws->buffer[4] = l >> 8;
					ws->buffer[5] = l & 0xff;
					ws->buffer[6] = AC_PKT_DATA;
					ws->buffer[7] = 0;

					ret =
					    tls_send(ws->session, ws->buffer,
						     l + 8);
					GNUTLS_FATAL_ERR(ret);
				}
			}
		}

		if (FD_ISSET(ws->conn_fd, &rfds) || tls_pending != 0) {
			ret =
			    tls_recv(ws->session, ws->buffer,
					       ws->buffer_size);
			oclog(ws, LOG_DEBUG, "received %d byte(s) (TLS)", ret);

			GNUTLS_FATAL_ERR(ret);

			if (ret == 0) {	/* disconnect */
				oclog(ws, LOG_INFO, "client disconnected");
				goto exit_nomsg;
			}

			if (ret > 0) {
				l = ret;

				if (bandwidth_update
				    (&b_rx, l - 8, ws->conn_mtu, &tnow) != 0) {
					ret =
					    parse_cstp_data(ws, ws->buffer, l,
							    now);
					if (ret < 0) {
						oclog(ws, LOG_ERR,
						      "error parsing CSTP data");
						goto exit;
					}

					if (ret == AC_PKT_DATA
					    && ws->udp_state == UP_ACTIVE) {
						/* client switched to TLS for some reason */
						if (now - udp_recv_time >
						    UDP_SWITCH_TIME)
							ws->udp_state =
							    UP_INACTIVE;
					}
				}
			}

			if (ret == GNUTLS_E_REHANDSHAKE) {
				/* rekey? */
				if (ws->last_tls_rehandshake > 0 &&
					now-ws->last_tls_rehandshake < ws->config->cookie_validity/3) {
					oclog(ws, LOG_ERR, "client requested TLS rehandshake too soon");
					goto exit;
				}

				oclog(ws, LOG_INFO, "client requested rehandshake on TLS channel");
				do {
					ret = gnutls_handshake(ws->session);
				} while (ret < 0 && gnutls_error_is_fatal(ret) == 0);
				GNUTLS_FATAL_ERR(ret);

				ws->last_tls_rehandshake = now;
			}
		}

		if (ws->udp_state > UP_WAIT_FD
		    && (FD_ISSET(ws->udp_fd, &rfds) || dtls_pending != 0)) {

			switch (ws->udp_state) {
			case UP_ACTIVE:
			case UP_INACTIVE:
				ret =
				    tls_recv(ws->dtls_session,
						       ws->buffer,
						       ws->buffer_size);
				oclog(ws, LOG_DEBUG,
				      "received %d byte(s) (DTLS)", ret);

				GNUTLS_FATAL_ERR(ret);

				if (ret > 0) {
					l = ret;
					ws->udp_state = UP_ACTIVE;

					if (bandwidth_update
					    (&b_rx, l - 1, ws->conn_mtu,
					     &tnow) != 0) {
						ret =
						    parse_dtls_data(ws,
								    ws->buffer,
								    l, now);
						if (ret < 0) {
							oclog(ws, LOG_INFO,
							      "error parsing CSTP data");
							goto exit;
						}
					}

				} else
					oclog(ws, LOG_DEBUG,
					      "no data received (%d)", ret);

				if (ret == GNUTLS_E_REHANDSHAKE) {
					/* there is not much we can rehandshake on the DTLS channel,
					 * at least not the way AnyConnect sets it up.
					 */
					oclog(ws, LOG_INFO, "client requested rehandshake on DTLS channel (!)");
					ret = gnutls_alert_send(ws->dtls_session, GNUTLS_AL_WARNING, GNUTLS_A_NO_RENEGOTIATION);
					GNUTLS_FATAL_ERR(ret);
				}

				udp_recv_time = now;
				break;
			case UP_SETUP:
				ret = setup_dtls_connection(ws);
				if (ret < 0)
					goto exit;

				gnutls_dtls_set_mtu(ws->dtls_session,
						    ws->conn_mtu);
				mtu_discovery_init(ws, ws->conn_mtu);

				break;
			case UP_HANDSHAKE:
 hsk_restart:
				ret = gnutls_handshake(ws->dtls_session);
				if (ret < 0 && gnutls_error_is_fatal(ret) != 0) {
					if (ret ==
					    GNUTLS_E_FATAL_ALERT_RECEIVED)
						oclog(ws, LOG_ERR,
						      "error in DTLS handshake: %s: %s\n",
						      gnutls_strerror(ret),
						      gnutls_alert_get_name
						      (gnutls_alert_get
						       (ws->dtls_session)));
					else
						oclog(ws, LOG_ERR,
						      "error in DTLS handshake: %s\n",
						      gnutls_strerror(ret));
					ws->udp_state = UP_DISABLED;
					break;
				}

				if (ret == GNUTLS_E_LARGE_PACKET) {
					/* adjust mtu */
					mtu_not_ok(ws);
					if (ret == 0) {
						goto hsk_restart;
					}
				}

				if (ret == 0) {
					unsigned mtu =
					    gnutls_dtls_get_data_mtu(ws->
								     dtls_session);

					/* openconnect doesn't like if we send more bytes
					 * than the initially agreed MTU */
					if (mtu > dtls_mtu)
						mtu = dtls_mtu;

					ws->udp_state = UP_ACTIVE;
					mtu_discovery_init(ws, mtu);
					mtu_set(ws, mtu);
					oclog(ws, LOG_INFO,
					      "DTLS handshake completed (plaintext MTU: %u)\n",
					      ws->conn_mtu - 1);
				}

				break;
			default:
				break;
			}
		}

		if (FD_ISSET(ws->cmd_fd, &rfds)) {
			ret = handle_worker_commands(ws);
			if (ret < 0) {
				goto exit;
			}
		}

	}

	return 0;

 exit:
	tls_close(ws->session);
	/*gnutls_deinit(ws->session); */
	if (ws->udp_state == UP_ACTIVE && ws->dtls_session) {
		tls_close(ws->dtls_session);
		/*gnutls_deinit(ws->dtls_session); */
	}
 exit_nomsg:
	exit_worker(ws);

 send_error:
	oclog(ws, LOG_DEBUG, "error sending data\n");
	exit_worker(ws);

	return -1;
}

static int parse_data(struct worker_st *ws, gnutls_session_t ts,	/* the interface of recv */
		      uint8_t head, uint8_t * buf, size_t buf_size, time_t now)
{
	int ret, e;

	switch (head) {
	case AC_PKT_DPD_RESP:
		oclog(ws, LOG_DEBUG, "received DPD response");
		break;
	case AC_PKT_KEEPALIVE:
		oclog(ws, LOG_DEBUG, "received keepalive");
		break;
	case AC_PKT_DPD_OUT:
		if (ws->session == ts) {
			ret = tls_send(ts, "STF\x01\x00\x00\x04\x00", 8);

			oclog(ws, LOG_DEBUG,
			      "received TLS DPD; sent response (%d bytes)",
			      ret);
		} else {
			/* Use DPD for MTU discovery in DTLS */
			ws->buffer[0] = AC_PKT_DPD_RESP;

			ret = tls_send(ts, ws->buffer, 1);
			if (ret == GNUTLS_E_LARGE_PACKET) {
				mtu_not_ok(ws);
				ret = tls_send(ts, ws->buffer, 1);
			}

			oclog(ws, LOG_DEBUG,
			      "received DTLS DPD; sent response (%d bytes)",
			      ret);
		}

		if (ret < 0) {
			oclog(ws, LOG_ERR, "could not send TLS data: %s",
			      gnutls_strerror(ret));
			return -1;
		}
		break;
	case AC_PKT_DISCONN:
		oclog(ws, LOG_INFO, "received BYE packet; exiting");
		exit_worker(ws);
		break;
	case AC_PKT_DATA:
		oclog(ws, LOG_DEBUG, "writing %d byte(s) to TUN",
		      (int)buf_size);
		ret = force_write(ws->tun_fd, buf, buf_size);
		if (ret == -1) {
			e = errno;
			oclog(ws, LOG_ERR, "could not write data to tun: %s",
			      strerror(e));
			return -1;
		}

		break;
	default:
		oclog(ws, LOG_DEBUG, "received unknown packet %u",
		      (unsigned)head);
	}

	return head;
}

static int parse_cstp_data(struct worker_st *ws,
			   uint8_t * buf, size_t buf_size, time_t now)
{
	int pktlen, ret;

	if (buf_size < 8) {
		oclog(ws, LOG_INFO,
		      "can't read CSTP header (only %d bytes are available)",
		      (int)buf_size);
		return -1;
	}

	if (buf[0] != 'S' || buf[1] != 'T' ||
	    buf[2] != 'F' || buf[3] != 1 || buf[7]) {
		oclog(ws, LOG_INFO, "can't recognise CSTP header");
		return -1;
	}

	pktlen = (buf[4] << 8) + buf[5];
	if (buf_size != 8 + pktlen) {
		oclog(ws, LOG_INFO, "unexpected CSTP length");
		return -1;
	}

	ret = parse_data(ws, ws->session, buf[6], buf + 8, pktlen, now);
	/* whatever we received treat it as DPD response.
	 * it indicates that the channel is alive */
	ws->last_msg_tcp = now;

	return ret;
}

static int parse_dtls_data(struct worker_st *ws,
			   uint8_t * buf, size_t buf_size, time_t now)
{
	int ret;

	if (buf_size < 1) {
		oclog(ws, LOG_INFO,
		      "can't read DTLS header (only %d bytes are available)",
		      (int)buf_size);
		return -1;
	}

	ret =
	    parse_data(ws, ws->dtls_session, buf[0], buf + 1, buf_size - 1,
		       now);
	ws->last_msg_udp = now;
	return ret;
}
