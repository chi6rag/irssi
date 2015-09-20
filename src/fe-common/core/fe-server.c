/*
 fe-server.c : irssi

    Copyright (C) 1999-2001 Timo Sirainen

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "module.h"
#include "signals.h"
#include "commands.h"
#include "network.h"
#include "levels.h"
#include "settings.h"
#include "net-sendbuffer.h"
#include "misc.h"

#include "chat-protocols.h"
#include "chatnets.h"
#include "servers.h"
#include "servers-setup.h"
#include "servers-reconnect.h"

#include "module-formats.h"
#include "printtext.h"

static void print_servers(void)
{
	GSList *tmp;

	for (tmp = servers; tmp != NULL; tmp = tmp->next) {
		SERVER_REC *rec = tmp->data;

		printformat(NULL, NULL, MSGLEVEL_CRAP, TXT_SERVER_LIST,
			    rec->tag, rec->connrec->address, rec->connrec->port,
			    rec->connrec->chatnet == NULL ? "" : rec->connrec->chatnet, rec->connrec->nick);
	}
}

static void print_lookup_servers(void)
{
	GSList *tmp;
	for (tmp = lookup_servers; tmp != NULL; tmp = tmp->next) {
		SERVER_REC *rec = tmp->data;

		printformat(NULL, NULL, MSGLEVEL_CRAP, TXT_SERVER_LOOKUP_LIST,
			    rec->tag, rec->connrec->address, rec->connrec->port,
			    rec->connrec->chatnet == NULL ? "" : rec->connrec->chatnet, rec->connrec->nick);
	}
}

static void print_reconnects(void)
{
	GSList *tmp;
	char *tag, *next_connect;
	int left;

	for (tmp = reconnects; tmp != NULL; tmp = tmp->next) {
		RECONNECT_REC *rec = tmp->data;
		SERVER_CONNECT_REC *conn = rec->conn;

		tag = g_strdup_printf("RECON-%d", rec->tag);
		left = rec->next_connect-time(NULL);
		next_connect = g_strdup_printf("%02d:%02d", left/60, left%60);
		printformat(NULL, NULL, MSGLEVEL_CRAP, TXT_SERVER_RECONNECT_LIST,
			    tag, conn->address, conn->port,
			    conn->chatnet == NULL ? "" : conn->chatnet,
			    conn->nick, next_connect);
		g_free(next_connect);
		g_free(tag);
	}
}

static SERVER_SETUP_REC *create_server_setup(GHashTable *optlist)
{
	CHAT_PROTOCOL_REC *rec;
        SERVER_SETUP_REC *server;
        char *chatnet;

	rec = chat_protocol_find_net(optlist);
	if (rec == NULL)
                rec = chat_protocol_get_default();
	else {
		chatnet = g_hash_table_lookup(optlist, rec->chatnet);
		if (chatnet_find(chatnet) == NULL) {
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
				    TXT_UNKNOWN_CHATNET, chatnet);
			return NULL;
		}
	}

        server = rec->create_server_setup();
        server->chat_type = rec->id;
	return server;
}

static void cmd_server_add(const char *data)
{
        GHashTable *optlist;
	SERVER_SETUP_REC *rec;
	char *addr, *portstr, *password, *value, *chatnet;
	void *free_arg;
	int port;

	if (!cmd_get_params(data, &free_arg, 3 | PARAM_FLAG_OPTIONS,
			    "server add", &optlist, &addr, &portstr, &password))
		return;

	if (*addr == '\0') cmd_param_error(CMDERR_NOT_ENOUGH_PARAMS);
	port = *portstr == '\0' ? DEFAULT_SERVER_ADD_PORT : atoi(portstr);

	chatnet = g_hash_table_lookup(optlist, "network");

	rec = server_setup_find(addr, port, chatnet);

	if (rec == NULL) {
		rec = create_server_setup(optlist);
		if (rec == NULL) {
			cmd_params_free(free_arg);
			return;
		}
		rec->address = g_strdup(addr);
		rec->port = port;
	} else {
		value = g_hash_table_lookup(optlist, "port");
		if (value != NULL && *value != '\0') rec->port = atoi(value);

		if (*password != '\0') g_free_and_null(rec->password);
		if (g_hash_table_lookup(optlist, "host")) {
			g_free_and_null(rec->own_host);
			rec->own_ip4 = rec->own_ip6 = NULL;
		}
	}

	if (g_hash_table_lookup(optlist, "6"))
		rec->family = AF_INET6;
        else if (g_hash_table_lookup(optlist, "4"))
		rec->family = AF_INET;

	if (g_hash_table_lookup(optlist, "tls") || g_hash_table_lookup(optlist, "ssl"))
		rec->use_tls = TRUE;

	value = g_hash_table_lookup(optlist, "tls_cert");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_cert");
	if (value != NULL && *value != '\0')
		rec->tls_cert = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_pkey");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_pkey");
	if (value != NULL && *value != '\0')
		rec->tls_pkey = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_pass");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_pass");
	if (value != NULL && *value != '\0')
		rec->tls_pass = g_strdup(value);

	if (g_hash_table_lookup(optlist, "tls_verify") || g_hash_table_lookup(optlist, "ssl_verify"))
		rec->tls_verify = TRUE;

	value = g_hash_table_lookup(optlist, "tls_cafile");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_cafile");
	if (value != NULL && *value != '\0')
		rec->tls_cafile = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_capath");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_capath");
	if (value != NULL && *value != '\0')
		rec->tls_capath = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_ciphers");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_ciphers");
	if (value != NULL && *value != '\0')
		rec->tls_ciphers = g_strdup(value);

	value = g_hash_table_lookup(optlist, "tls_fingerprint");
	if (value == NULL)
		value = g_hash_table_lookup(optlist, "ssl_fingerprint");
	if (value != NULL && *value != '\0')
		rec->tls_fingerprint = g_strdup(value);

	if ((rec->tls_cafile != NULL && rec->tls_cafile[0] != '\0')
	||  (rec->tls_capath != NULL && rec->tls_capath[0] != '\0'))
		rec->tls_verify = TRUE;

	if ((rec->tls_cert != NULL && rec->tls_cert[0] != '\0') || rec->tls_verify == TRUE)
		rec->use_tls = TRUE;

	if (g_hash_table_lookup(optlist, "auto")) rec->autoconnect = TRUE;
	if (g_hash_table_lookup(optlist, "noauto")) rec->autoconnect = FALSE;
	if (g_hash_table_lookup(optlist, "proxy")) rec->no_proxy = FALSE;
	if (g_hash_table_lookup(optlist, "noproxy")) rec->no_proxy = TRUE;

	if (*password != '\0' && g_strcmp0(password, "-") != 0) rec->password = g_strdup(password);
	value = g_hash_table_lookup(optlist, "host");
	if (value != NULL && *value != '\0') {
		rec->own_host = g_strdup(value);
		rec->own_ip4 = rec->own_ip6 = NULL;
	}

	signal_emit("server add fill", 2, rec, optlist);

	server_setup_add(rec);
	printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_SETUPSERVER_ADDED, addr, port);

	cmd_params_free(free_arg);
}

/* SYNTAX: SERVER REMOVE <address> [<port>] [<network>] */
static void cmd_server_remove(const char *data)
{
	SERVER_SETUP_REC *rec;
	char *addr, *port, *chatnet;
	void *free_arg;

	if (!cmd_get_params(data, &free_arg, 3, &addr, &port, &chatnet))
		return;
	if (*addr == '\0') cmd_param_error(CMDERR_NOT_ENOUGH_PARAMS);

	if (*port == '\0') {
		if (*chatnet == '\0')
			rec = server_setup_find(addr, -1, NULL);
		else
			rec = server_setup_find(addr, -1, chatnet);
	}
	else
	{
		if (*chatnet == '\0')
			rec = server_setup_find(addr, atoi(port), NULL);
		else
			rec = server_setup_find(addr, atoi(port), chatnet);
	}

	if (rec == NULL)
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_SETUPSERVER_NOT_FOUND, addr, port);
	else {
		server_setup_remove(rec);
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_SETUPSERVER_REMOVED, addr, port);
	}

	cmd_params_free(free_arg);
}

static void cmd_server(const char *data)
{
	if (*data != '\0')
		return;

	if (servers == NULL && lookup_servers == NULL &&
	    reconnects == NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
			    TXT_NO_CONNECTED_SERVERS);
	} else {
		print_servers();
		print_lookup_servers();
		print_reconnects();
	}

        signal_stop();
}

static void cmd_server_connect(const char *data)
{
	GHashTable *optlist;
	char *addr;
	void *free_arg;

	if (!cmd_get_params(data, &free_arg, 1 | PARAM_FLAG_OPTIONS,
			    "connect", &optlist, &addr))
		return;

	if (*addr == '\0' || g_strcmp0(addr, "+") == 0)
		cmd_param_error(CMDERR_NOT_ENOUGH_PARAMS);
	if (*addr == '+') window_create(NULL, FALSE);

	cmd_params_free(free_arg);
}

static void server_command(const char *data, SERVER_REC *server,
			   WI_ITEM_REC *item)
{
	if (server == NULL) {
		/* this command accepts non-connected server too */
		server = active_win->connect_server;
	}

	signal_continue(3, data, server, item);
}

static void sig_server_looking(SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_LOOKING_UP, server->connrec->address);
}

static void sig_server_connecting(SERVER_REC *server, IPADDR *ip)
{
	char ipaddr[MAX_IP_LEN];

	g_return_if_fail(server != NULL);

	if (ip == NULL)
		ipaddr[0] = '\0';
	else
		net_ip2host(ip, ipaddr);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    !server->connrec->reconnecting ?
		    TXT_CONNECTING : TXT_RECONNECTING,
		    server->connrec->address, ipaddr, server->connrec->port);
}

static void sig_server_connected(SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	if (! server->connrec->use_tls) {
		printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_CONNECTION_ESTABLISHED, server->connrec->address);
		return;
	}

	GIOSSLChannel *chan = (GIOSSLChannel *)net_sendbuffer_handle(server->handle);
	g_return_if_fail(chan != NULL);

	const char *tls_version = SSL_get_version(chan->ssl);
	const char *tls_cipher = SSL_CIPHER_get_name(SSL_get_current_cipher(chan->ssl));

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_ENCRYPTED_CONNECTION_ESTABLISHED,
			server->connrec->address,
			tls_version,
			tls_cipher);

	if (! settings_get_bool("tls_connect_verbose"))
		return;

	// Show certificate chain.
	STACK_OF(X509) *chain = NULL;

	chain = SSL_get_peer_cert_chain(chan->ssl);

	if (chain != NULL) {
		printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_CERT_HEADER);
		int i;

		for (i = 0; i < sk_X509_num(chain); i++) {
			int j;
			X509_NAME *name;
			int nid;

			printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_CERT_SUBJECT_HEADER);

			name = X509_get_subject_name(sk_X509_value(chain, i));
			for (j = 0; j < X509_NAME_entry_count(name); j++) {
				X509_NAME_ENTRY *entry = X509_NAME_get_entry(name, j);

				nid = OBJ_obj2nid(X509_NAME_ENTRY_get_object(entry));
				char *name = (char *)OBJ_nid2sn(nid);
				if (name == NULL)
					name = (char *)OBJ_nid2ln(nid);

				ASN1_STRING *d = X509_NAME_ENTRY_get_data(entry);
				char *value = (char *)ASN1_STRING_data(d);

				printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_CERT_NAMED_ENTRY, name, value);
			}

			printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_CERT_ISSUER_HEADER);

			name = X509_get_issuer_name(sk_X509_value(chain, i));
			for (j = 0; j < X509_NAME_entry_count(name); j++) {
				X509_NAME_ENTRY *entry = X509_NAME_get_entry(name, j);

				nid = OBJ_obj2nid(X509_NAME_ENTRY_get_object(entry));
				char *name = (char *)OBJ_nid2sn(nid);
				if (name == NULL)
					name = (char *)OBJ_nid2ln(nid);

				ASN1_STRING *d = X509_NAME_ENTRY_get_data(entry);
				char *value = (char *)ASN1_STRING_data(d);

				printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_CERT_NAMED_ENTRY, name, value);
			}
		}
	}

	// Show server public key information.
	X509 *peer = SSL_get_peer_certificate(chan->ssl);

	if (peer != NULL) {
		EVP_PKEY *pubkey = X509_get_pubkey(peer);
		char *pubkey_type = NULL;

		// Show server fingerprint.
		unsigned char md[EVP_MAX_MD_SIZE];
		char *fingerprint = NULL;
		unsigned int n = 0;

		if (X509_digest(peer, EVP_sha256(), md, &n)) {
			fingerprint = binary_to_hex(md, n);
			printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_CERT_FINGERPRINT, fingerprint, "SHA256");
			g_free(fingerprint);
		}

		// Show algorithm.
		switch (EVP_PKEY_id(pubkey)) {
			case EVP_PKEY_RSA:
				pubkey_type = "RSA";
				break;
			case EVP_PKEY_DSA:
				pubkey_type = "DSA";
				break;
			default:
				pubkey_type = "Unknown";
				break;
		}
		printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_PUBKEY_SIZE, EVP_PKEY_bits(pubkey), pubkey_type);

		EVP_PKEY_free(pubkey);
	}

#if defined(SSL_get_server_tmp_key)
	// Show ephemeral key information.
	EVP_PKEY *ephemeral_key = NULL;

	if (SSL_get_server_tmp_key(chan->ssl, &ephemeral_key)) {
		switch (EVP_PKEY_id(ephemeral_key)) {
			case EVP_PKEY_RSA:
				printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_EPHEMERAL_KEY,
						EVP_PKEY_bits(ephemeral_key), "RSA");
				break;
			case EVP_PKEY_DH:
				printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_EPHEMERAL_KEY,
						EVP_PKEY_bits(ephemeral_key), "DH");
				break;
			case EVP_PKEY_EC:
			{
				EC_KEY *ec = EVP_PKEY_get1_EC_KEY(ephemeral_key);
				int nid;
				const char *cname;
				nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(ec));
				EC_KEY_free(ec);
				cname = OBJ_nid2sn(nid);
				char *key_type = g_strdup_printf("ECDH: %s", cname);
				printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_EPHEMERAL_KEY,
						EVP_PKEY_bits(ephemeral_key), key_type);
				g_free(key_type);
			}
		}

		EVP_PKEY_free(ephemeral_key);
	}
	else
	{
		printformat(server, NULL, MSGLEVEL_CLIENTNOTICE, TXT_TLS_SERVER_EPHEMERAL_KEY_UNAVAILBLE);
	}
#endif // defined(SSL_get_server_tmp_key).
}

static void sig_connect_failed(SERVER_REC *server, gchar *msg)
{
	g_return_if_fail(server != NULL);

	if (msg == NULL) {
		/* no message so this wasn't unexpected fail - send
		   connection_lost message instead */
		printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
			    TXT_CONNECTION_LOST, server->connrec->address);
	} else {
		printformat(server, NULL, MSGLEVEL_CLIENTERROR,
			    TXT_CANT_CONNECT, server->connrec->address, server->connrec->port, msg);
	}
}

static void sig_server_disconnected(SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_CONNECTION_LOST, server->connrec->address);
}

static void sig_server_quit(SERVER_REC *server, const char *msg)
{
	g_return_if_fail(server != NULL);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_SERVER_QUIT, server->connrec->address, msg);
}

static void sig_server_lag_disconnected(SERVER_REC *server)
{
	g_return_if_fail(server != NULL);

	printformat(server, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_LAG_DISCONNECTED, server->connrec->address,
		    time(NULL)-server->lag_sent.tv_sec);
}

static void sig_server_reconnect_removed(RECONNECT_REC *reconnect)
{
	g_return_if_fail(reconnect != NULL);

	printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_RECONNECT_REMOVED, reconnect->conn->address, reconnect->conn->port,
		    reconnect->conn->chatnet == NULL ? "" : reconnect->conn->chatnet);
}

static void sig_server_reconnect_not_found(const char *tag)
{
	g_return_if_fail(tag != NULL);

	printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE,
		    TXT_RECONNECT_NOT_FOUND, tag);
}

static void sig_chat_protocol_unknown(const char *protocol)
{
	g_return_if_fail(protocol != NULL);

	printformat(NULL, NULL, MSGLEVEL_CLIENTERROR,
                    TXT_UNKNOWN_CHAT_PROTOCOL, protocol);
}

void fe_server_init(void)
{
	settings_add_bool("lookandfeel", "tls_connect_verbose", TRUE);

	command_bind("server", NULL, (SIGNAL_FUNC) cmd_server);
	command_bind("server connect", NULL, (SIGNAL_FUNC) cmd_server_connect);
	command_bind("server add", NULL, (SIGNAL_FUNC) cmd_server_add);
	command_bind("server remove", NULL, (SIGNAL_FUNC) cmd_server_remove);
	command_bind_first("server", NULL, (SIGNAL_FUNC) server_command);
	command_bind_first("disconnect", NULL, (SIGNAL_FUNC) server_command);
	command_set_options("server add", "4 6 !! ssl +ssl_cert +ssl_pkey +ssl_pass ssl_verify +ssl_cafile +ssl_capath +ssl_ciphers +ssl_fingerprint tls +tls_cert +tls_pkey +tls_pass tls_verify +tls_cafile +tls_capath +tls_ciphers +tls_fingerprint auto noauto proxy noproxy -host -port noautosendcmd");

	signal_add("server looking", (SIGNAL_FUNC) sig_server_looking);
	signal_add("server connecting", (SIGNAL_FUNC) sig_server_connecting);
	signal_add("server connected", (SIGNAL_FUNC) sig_server_connected);
	signal_add("server connect failed", (SIGNAL_FUNC) sig_connect_failed);
	signal_add("server disconnected", (SIGNAL_FUNC) sig_server_disconnected);
	signal_add("server quit", (SIGNAL_FUNC) sig_server_quit);

	signal_add("server lag disconnect", (SIGNAL_FUNC) sig_server_lag_disconnected);
	signal_add("server reconnect remove", (SIGNAL_FUNC) sig_server_reconnect_removed);
	signal_add("server reconnect not found", (SIGNAL_FUNC) sig_server_reconnect_not_found);

	signal_add("chat protocol unknown", (SIGNAL_FUNC) sig_chat_protocol_unknown);
}

void fe_server_deinit(void)
{
	command_unbind("server", (SIGNAL_FUNC) cmd_server);
	command_unbind("server connect", (SIGNAL_FUNC) cmd_server_connect);
	command_unbind("server add", (SIGNAL_FUNC) cmd_server_add);
	command_unbind("server remove", (SIGNAL_FUNC) cmd_server_remove);
	command_unbind("server", (SIGNAL_FUNC) server_command);
	command_unbind("disconnect", (SIGNAL_FUNC) server_command);

	signal_remove("server looking", (SIGNAL_FUNC) sig_server_looking);
	signal_remove("server connecting", (SIGNAL_FUNC) sig_server_connecting);
	signal_remove("server connected", (SIGNAL_FUNC) sig_server_connected);
	signal_remove("server connect failed", (SIGNAL_FUNC) sig_connect_failed);
	signal_remove("server disconnected", (SIGNAL_FUNC) sig_server_disconnected);
	signal_remove("server quit", (SIGNAL_FUNC) sig_server_quit);

	signal_remove("server lag disconnect", (SIGNAL_FUNC) sig_server_lag_disconnected);
	signal_remove("server reconnect remove", (SIGNAL_FUNC) sig_server_reconnect_removed);
	signal_remove("server reconnect not found", (SIGNAL_FUNC) sig_server_reconnect_not_found);

	signal_remove("chat protocol unknown", (SIGNAL_FUNC) sig_chat_protocol_unknown);
}
