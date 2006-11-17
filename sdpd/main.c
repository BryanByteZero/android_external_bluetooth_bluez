/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2001-2002  Nokia Corporation
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2002-2006  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2002-2003  Stephen Crane <steve.crane@rococosoft.com>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#define _XOPEN_SOURCE 600
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <sys/un.h>
#include <netinet/in.h>

#include "sdpd.h"
#include "logging.h"

static int l2cap_sock, unix_sock;
static fd_set active_fdset;
static int active_maxfd;

static sdp_record_t *server;

/*
 * List of version numbers supported by the SDP server.
 * Add to this list when newer versions are supported.
 */
static sdp_version_t sdpVnumArray[1] = {
	{ 1, 0 }
};
static const int sdpServerVnumEntries = 1;

/*
 * The service database state is an attribute of the service record
 * of the SDP server itself. This attribute is guaranteed to
 * change if any of the contents of the service repository
 * changes. This function updates the timestamp of value of
 * the svcDBState attribute
 * Set the SDP server DB. Simply a timestamp which is the marker
 * when the DB was modified.
 */
void update_db_timestamp(void)
{
	uint32_t dbts = sdp_get_time();
	sdp_data_t *d = sdp_data_alloc(SDP_UINT32, &dbts);
	sdp_attr_replace(server, SDP_ATTR_SVCDB_STATE, d);
}

static void add_lang_attr(sdp_record_t *r)
{
	sdp_lang_attr_t base_lang;
	sdp_list_t *langs = 0;

	base_lang.code_ISO639 = (0x65 << 8) | 0x6e;
	// UTF-8 MIBenum (http://www.iana.org/assignments/character-sets)
	base_lang.encoding = 106;
	base_lang.base_offset = SDP_PRIMARY_LANG_BASE;
	langs = sdp_list_append(0, &base_lang);
	sdp_set_lang_attr(r, langs);
	sdp_list_free(langs, 0);
}

static void register_public_browse_group(int public)
{
	sdp_list_t *browselist;
	uuid_t bgscid, pbgid;
	sdp_data_t *sdpdata;
	sdp_record_t *browse = sdp_record_alloc();

	if (public) {
		browse->handle = sdp_next_handle();
		if (browse->handle < 0x10000)
			return;
	} else
		browse->handle = SDP_SERVER_RECORD_HANDLE + 1;

	sdp_record_add(BDADDR_ANY, browse);
	sdpdata = sdp_data_alloc(SDP_UINT32, &browse->handle);
	sdp_attr_add(browse, SDP_ATTR_RECORD_HANDLE, sdpdata);

	add_lang_attr(browse);
	sdp_set_info_attr(browse, "Public Browse Group Root", "BlueZ", "Root of public browse hierarchy");

	sdp_uuid16_create(&bgscid, BROWSE_GRP_DESC_SVCLASS_ID);
	browselist = sdp_list_append(0, &bgscid);
	sdp_set_service_classes(browse, browselist);
	sdp_list_free(browselist, 0);

	if (public) {
		sdp_uuid16_create(&pbgid, PUBLIC_BROWSE_GROUP);
		sdp_set_group_id(browse, pbgid);
	}
}

/*
 * The SDP server must present its own service record to
 * the service repository. This can be accessed by service
 * discovery clients. This method constructs a service record
 * and stores it in the repository
 */
static void register_server_service(int public)
{
	int i;
	sdp_list_t *classIDList, *browseList;
	sdp_list_t *access_proto = 0;
	uuid_t l2cap, classID, browseGroupId, sdpSrvUUID;
	void **versions, **versionDTDs;
	uint8_t dtd;
	uint16_t version, port;
	sdp_data_t *pData, *port_data, *version_data;
	sdp_list_t *pd, *seq;

	server = sdp_record_alloc();
	server->pattern = NULL;

	/* Force the record to be SDP_SERVER_RECORD_HANDLE */
	server->handle = SDP_SERVER_RECORD_HANDLE;

	sdp_record_add(BDADDR_ANY, server);
	sdp_attr_add(server, SDP_ATTR_RECORD_HANDLE, sdp_data_alloc(SDP_UINT32, &server->handle));

	/*
	 * Add all attributes to service record. (No need to commit since we 
	 * are the server and this record is already in the database.)
	 */
	add_lang_attr(server);
	sdp_set_info_attr(server, "SDP Server", "BlueZ", "Bluetooth service discovery server");

	sdp_uuid16_create(&classID, SDP_SERVER_SVCLASS_ID);
	classIDList = sdp_list_append(0, &classID);
	sdp_set_service_classes(server, classIDList);
	sdp_list_free(classIDList, 0);

	/*
	 * Set the version numbers supported, these are passed as arguments
	 * to the server on command line. Now defaults to 1.0
	 * Build the version number sequence first
	 */
	versions = (void **)malloc(sdpServerVnumEntries * sizeof(void *));
	versionDTDs = (void **)malloc(sdpServerVnumEntries * sizeof(void *));
	dtd = SDP_UINT16;
	for (i = 0; i < sdpServerVnumEntries; i++) {
		uint16_t *version = malloc(sizeof(uint16_t));
		*version = sdpVnumArray[i].major;
		*version = (*version << 8);
		*version |= sdpVnumArray[i].minor;
		versions[i] = version;
		versionDTDs[i] = &dtd;
	}
	pData = sdp_seq_alloc(versionDTDs, versions, sdpServerVnumEntries);
	for (i = 0; i < sdpServerVnumEntries; i++)
		free(versions[i]);
	free(versions);
	free(versionDTDs);
	sdp_attr_add(server, SDP_ATTR_VERSION_NUM_LIST, pData);

	sdp_uuid16_create(&sdpSrvUUID, SDP_UUID);
	sdp_set_service_id(server, sdpSrvUUID);

	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	pd = sdp_list_append(0, &l2cap);
	port = SDP_PSM;
	port_data = sdp_data_alloc(SDP_UINT16, &port);
	pd = sdp_list_append(pd, port_data);
	version = 1;
	version_data = sdp_data_alloc(SDP_UINT16, &version);
	pd = sdp_list_append(pd, version_data);
	seq = sdp_list_append(0, pd);

	access_proto = sdp_list_append(0, seq);
	sdp_set_access_protos(server, access_proto);
	sdp_list_free(access_proto, free);
	sdp_data_free(port_data);
	sdp_data_free(version_data);
	sdp_list_free(pd, 0);

	if (public) {
		sdp_uuid16_create(&browseGroupId, PUBLIC_BROWSE_GROUP);
		browseList = sdp_list_append(0, &browseGroupId);
		sdp_set_browse_groups(server, browseList);
		sdp_list_free(browseList, 0);
	}

	update_db_timestamp();
}

/*
 * SDP server initialization on startup includes creating the
 * l2cap and unix sockets over which discovery and registration clients
 * access us respectively
 */
static int init_server(uint16_t mtu, int master, int public)
{
	struct l2cap_options opts;
	struct sockaddr_l2 l2addr;
	struct sockaddr_un unaddr;
	socklen_t optlen;

	/* Register the public browse group root */
	register_public_browse_group(public);

	/* Register the SDP server's service record */
	register_server_service(public);

	/* Create L2CAP socket */
	l2cap_sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (l2cap_sock < 0) {
		error("opening L2CAP socket: %s", strerror(errno));
		return -1;
	}

	l2addr.l2_bdaddr = *BDADDR_ANY;
	l2addr.l2_family = AF_BLUETOOTH;
	l2addr.l2_psm    = htobs(SDP_PSM);
	if (bind(l2cap_sock, (struct sockaddr *)&l2addr, sizeof(l2addr)) < 0) {
		error("binding L2CAP socket: %s", strerror(errno));
		return -1;
	}

	if (master) {
		int opt = L2CAP_LM_MASTER;
		if (setsockopt(l2cap_sock, SOL_L2CAP, L2CAP_LM, &opt, sizeof(opt)) < 0) {
			error("setsockopt: %s", strerror(errno));
			return -1;
		}
	}

	if (mtu > 0) {
		memset(&opts, 0, sizeof(opts));
		optlen = sizeof(opts);

		if (getsockopt(l2cap_sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen) < 0) {
			error("getsockopt: %s", strerror(errno));
			return -1;
		}

		opts.imtu = mtu;

		if (setsockopt(l2cap_sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts)) < 0) {
			error("setsockopt: %s", strerror(errno));
			return -1;
		}
	}

	listen(l2cap_sock, 5);
	FD_SET(l2cap_sock, &active_fdset);
	active_maxfd = l2cap_sock;

	/* Create local Unix socket */
	unix_sock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (unix_sock < 0) {
		error("opening UNIX socket: %s", strerror(errno));
		return -1;
	}

	unaddr.sun_family = AF_UNIX;
	strcpy(unaddr.sun_path, SDP_UNIX_PATH);
	unlink(unaddr.sun_path);
	if (bind(unix_sock, (struct sockaddr *)&unaddr, sizeof(unaddr)) < 0) {
		error("binding UNIX socket: %s", strerror(errno));
		return -1;
	}

	listen(unix_sock, 5);
	FD_SET(unix_sock, &active_fdset);
	active_maxfd = unix_sock;
	chmod(SDP_UNIX_PATH, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

	return 0;
}

static void sig_term(int sig)
{
	info("terminating...");
	sdp_svcdb_reset();
	close(l2cap_sock);
	close(unix_sock);
	exit(0);
}

static inline void handle_request(int sk, uint8_t *data, int len)
{
	struct sockaddr_l2 sa;
	socklen_t size;
	sdp_req_t req;

	size = sizeof(sa);
	if (getpeername(sk, (struct sockaddr *) &sa, &size) < 0)
		return;

	if (sa.l2_family == AF_BLUETOOTH) { 
		struct l2cap_options lo;
		memset(&lo, 0, sizeof(lo));
		size = sizeof(lo);
		getsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &lo, &size);
		bacpy(&req.bdaddr, &sa.l2_bdaddr);
		req.mtu = lo.omtu;
		req.local = 0;
		memset(&sa, 0, sizeof(sa));
		size = sizeof(sa);
		getsockname(sk, (struct sockaddr *) &sa, &size);
		bacpy(&req.device, &sa.l2_bdaddr);
	} else {
		bacpy(&req.device, BDADDR_ANY);
		bacpy(&req.bdaddr, BDADDR_LOCAL);
		req.mtu = 2048;
		req.local = 1;
	}

	req.sock = sk;
	req.buf  = data;
	req.len  = len;

	process_request(&req);
}

static void close_sock(int fd, int r)
{
	if (r < 0)
		error("Read error: %s", strerror(errno));
	FD_CLR(fd, &active_fdset);
	close(fd);
	sdp_svcdb_collect_all(fd);
	if (fd == active_maxfd)
		active_maxfd--;
}

static void check_active(fd_set *mask, int num)
{
	sdp_pdu_hdr_t hdr;
	int size, fd, count, r;
	uint8_t *buf;

	for (fd = 0, count = 0; fd <= active_maxfd && count < num; fd++) {
		if (fd == l2cap_sock || fd == unix_sock || !FD_ISSET(fd, mask))
			continue;

		count++;

		r = recv(fd, (void *)&hdr, sizeof(sdp_pdu_hdr_t), MSG_PEEK);
		if (r <= 0) {
			close_sock(fd, r);
			continue;
		}

		size = sizeof(sdp_pdu_hdr_t) + ntohs(hdr.plen);
		buf = malloc(size);
		if (!buf)
			continue;

		r = recv(fd, buf, size, 0);
		if (r <= 0)
			close_sock(fd, r);
		else
			handle_request(fd, buf, r);
	}
}

static void usage(void)
{
	printf("sdpd - SDP daemon ver %s\n", VERSION);
	printf("Usage: \n");
	printf("\tsdpd [-n]\n");
}

static struct option main_options[] = {
	{ "help",	0, 0, 'h' },
	{ "nodaemon",	0, 0, 'n' },
	{ "mtu",	1, 0, 'm' },
	{ "public",	0, 0, 'p' },
	{ "master",	0, 0, 'M' },
	{ 0, 0, 0, 0}
};

int main(int argc, char *argv[])
{
	sigset_t sigs;
	uint16_t mtu = 0;
	int daemonize = 1, public = 0, master = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "nm:pM", main_options, NULL)) != -1) {
		switch (opt) {
		case 'n':
			daemonize = 0;
			break;

		case 'm':
			mtu = atoi(optarg);
			break;

		case 'p':
			public = 1;
			break;

		case 'M':
			master = 1;
			break;

		default:
			usage();
			exit(1);
		}
	}

	if (daemonize && daemon(0, 0)) {
		error("Server startup failed: %s (%d)", strerror(errno), errno);
		exit(1);
	}

	start_logging("sdpd", "Bluetooth SDP daemon");

#ifdef SDP_DEBUG
	enable_debug();
#endif

	if (init_server(mtu, master, public) < 0) {
		error("Server initialization failed");
		exit(1);
	}

	signal(SIGINT,  sig_term);
	signal(SIGTERM, sig_term);
	signal(SIGABRT, sig_term);
	signal(SIGQUIT, sig_term);
	signal(SIGPIPE, SIG_IGN);

	sigfillset(&sigs);
	sigdelset(&sigs, SIGINT);
	sigdelset(&sigs, SIGTERM);
	sigdelset(&sigs, SIGABRT);
	sigdelset(&sigs, SIGQUIT);
	sigdelset(&sigs, SIGPIPE);

	for (;;) {
		int num, nfd;
		fd_set mask;

		FD_ZERO(&mask);
		mask = active_fdset;

		num = pselect(active_maxfd + 1, &mask, NULL, NULL, NULL, &sigs);
		if (num <= 0) {
			debug("Select error:%s", strerror(errno));
			break;
		}

		if (FD_ISSET(l2cap_sock, &mask)) {
			/* New L2CAP connection  */
			struct sockaddr_l2 caddr;
			socklen_t len = sizeof(caddr);

			nfd = accept(l2cap_sock, (struct sockaddr *)&caddr, &len);
			if (nfd >= 0) {
				if (nfd > active_maxfd)
					active_maxfd = nfd;
				FD_SET(nfd, &active_fdset);
			}
		} else if (FD_ISSET(unix_sock, &mask)) {
			/* New unix connection */
			struct sockaddr_un caddr;
			socklen_t len = sizeof(caddr);

			nfd = accept(unix_sock, (struct sockaddr *)&caddr, &len);
			if (nfd != -1) {
				if (nfd > active_maxfd)
					active_maxfd = nfd;
				FD_SET(nfd, &active_fdset);
			}
		} else
			check_active(&mask, num);
	}

	sdp_svcdb_reset();

	stop_logging();

	return 0;
}
