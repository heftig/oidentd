/*
** oidentd.c - oidentd ident (RFC 1413) implementation.
** Copyright (c) 1998-2006 Ryan McCabe <ryan@numb.org>
** Copyright (c) 2018      Janik Rabe  <oidentd@janikrabe.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License, version 2,
** as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#define _GNU_SOURCE

#include <config.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <pwd.h>
#include <syslog.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "oidentd.h"
#include "util.h"
#include "missing.h"
#include "inet_util.h"
#include "user_db.h"
#include "options.h"
#include "masq.h"

#if HAVE_LIBUDB
#	include <udb.h>
#endif

static void sig_segv(int unused __notused) __noreturn;
static void sig_child(int sig);
static void sig_alarm(int unused __notused) __noreturn;
static void sig_hup(int unused);

static void copy_pw(const struct passwd *pw, struct passwd *pwd);
static void free_pw(struct passwd *pwd);

static void seed_prng(void);

static int service_request(int insock, int outsock);

u_int32_t timeout = DEFAULT_TIMEOUT;
u_int32_t connection_limit;
u_int32_t current_connections = 0;

uid_t uid;
gid_t gid;

char *ret_os;
char *failuser;
char *config_file;

in_port_t listen_port;
struct sockaddr_storage **addr;

int main(int argc, char **argv) {
	int *listen_fds = NULL;

	if (get_options(argc, argv) != 0)
		exit(EXIT_FAILURE);

	openlog("oidentd", LOG_PID | LOG_CONS | LOG_NDELAY, FACILITY);

	if (read_config(config_file) != 0) {
		o_log(LOG_CRIT, "Fatal: Error reading configuration file");
		exit(EXIT_FAILURE);
	}

	if (!core_init()) {
		if (opt_enabled(DEBUG_MSGS)) {
			o_log(LOG_CRIT, "Fatal: Error initializing core");
		} else {
			o_log(LOG_CRIT, "Fatal: Error initializing core (try --debug)");
		}
		exit(EXIT_FAILURE);
	}

	if (random_seed() != 0) {
		o_log(LOG_CRIT, "Fatal: Error seeding random number generator");
		exit(EXIT_FAILURE);
	}

	if (!opt_enabled(STDIO)) {
		listen_fds = setup_listen(addr, htons(listen_port));
		if (listen_fds == NULL || listen_fds[0] == -1) {
			o_log(LOG_CRIT, "Fatal: Unable to set up listening socket");
			exit(EXIT_FAILURE);
		}
	}

	if (!opt_enabled(FOREGROUND) && go_background() == -1) {
		o_log(LOG_CRIT, "Fatal: Error creating daemon process");
		exit(EXIT_FAILURE);
	}

	if (k_open() != 0) {
		o_log(LOG_CRIT, "Fatal: Unable to open kmem device: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

#if LIBNFCT_SUPPORT
	if (!drop_privs_libnfct(uid, gid)) {
		o_log(LOG_CRIT, "Fatal: Failed to drop privileges (kernel)");
		exit(EXIT_FAILURE);
	}
#endif

	if (drop_privs(uid, gid) == -1) {
		o_log(LOG_CRIT, "Fatal: Failed to drop privileges (global)");
		exit(EXIT_FAILURE);
	}

#if HAVE_LIBUDB
	if (opt_enabled(USEUDB)) {
		if (udb_init(UDB_ENV_BASE_KEY) == 0) {
			o_log(LOG_CRIT, "Fatal: Can't open UDB shared memory tables");
			exit(EXIT_FAILURE);
		}
	}
#endif

	signal(SIGALRM, sig_alarm);
	signal(SIGCHLD, sig_child);
	signal(SIGHUP, sig_hup);
	signal(SIGSEGV, sig_segv);

	if (opt_enabled(STDIO)) {
		service_request(fileno(stdin), fileno(stdout));
		exit(EXIT_SUCCESS);
	}

	for (;;) {
		fd_set rfds;
		int ret;
		size_t fdlen = 0;

		FD_ZERO(&rfds);

		do {
			int fd = listen_fds[fdlen++];
			FD_SET(fd, &rfds);
		} while (listen_fds[fdlen] != -1);

		ret = select(listen_fds[fdlen - 1] + 1, &rfds, NULL, NULL, NULL);
		if (ret > 0) {
			size_t i;

			for (i = 0 ; i < fdlen ; i++) {
				if (FD_ISSET(listen_fds[i], &rfds)) {
					int connectfd;

					connectfd = accept(listen_fds[i], NULL, NULL);
					if (connectfd == -1) {
						debug("accept: %s", strerror(errno));
						continue;
					}

					if (current_connections >= connection_limit) {
						close(connectfd);
						continue;
					}

					++current_connections;

					if (fork() == 0) {
						size_t idx;

						for (idx = 0 ; listen_fds[idx] != -1 ; ++idx)
							close(listen_fds[idx]);

						free(listen_fds);
						alarm(timeout);
						seed_prng();
						service_request(connectfd, connectfd);

						exit(EXIT_SUCCESS);
					}

					close(connectfd);
				}
			}
		}
	}

	exit(EXIT_SUCCESS);
}

/*
** Handle the client's request: read the client data and send the ident reply.
*/

static int service_request(int insock, int outsock) {
	int len;
	int ret;
	uid_t con_uid;
	int lport_temp;
	int fport_temp;
	in_port_t lport;
	in_port_t fport;
	char line[128];
	char suser[MAX_ULEN];
	char host_buf[MAX_HOSTLEN];
	char ip_buf[MAX_IPLEN];
	struct sockaddr_storage laddr, laddr6, faddr, faddr6;
	struct passwd *pw, pwd;
	static socklen_t socklen = sizeof(struct sockaddr_storage);

	if (getpeername(insock, (struct sockaddr *) &faddr, &socklen) != 0) {
		debug("getpeername: %s", strerror(errno));
		return (-1);
	}

	if (getsockname(insock, (struct sockaddr *) &laddr, &socklen) != 0) {
		debug("getsockname: %s", strerror(errno));
		return (-1);
	}

	fport = htons(sin_port(&faddr));

#if WANT_IPV6
	laddr6 = laddr;
	faddr6 = faddr;

	if (laddr.ss_family == AF_INET6 &&
		IN6_IS_ADDR_V4MAPPED(&SIN6(&laddr)->sin6_addr))
	{
		struct in_addr in4;

		sin_extractv4(&SIN6(&laddr)->sin6_addr, &in4);
		sin_setv4(in4.s_addr, &laddr);

		sin_extractv4(&SIN6(&faddr)->sin6_addr, &in4);
		sin_setv4(in4.s_addr, &faddr);
	}
#endif

	get_ip(&faddr, ip_buf, sizeof(ip_buf));

	if (get_hostname(&faddr, host_buf, sizeof(host_buf)) != 0) {
		o_log(NORMAL, "Connection from %s:%d", ip_buf, fport);
		xstrncpy(host_buf, ip_buf, sizeof(host_buf));
	} else
		o_log(NORMAL, "Connection from %s (%s):%d", host_buf, ip_buf, fport);

	if (!sock_read(insock, line, sizeof(line)))
		return -1;

	len = sscanf(line, "%d , %d", &lport_temp, &fport_temp);
	if (len < 2) {
		debug("[%s] Malformed request: \"%s\"", host_buf, line);
		return (0);
	}

	if (!VALID_PORT(lport_temp) || !VALID_PORT(fport_temp)) {
		sockprintf(outsock, "%d,%d:ERROR:%s\r\n",
				lport_temp, fport_temp, ERROR("INVALID-PORT"));

		debug("[%s] %d , %d : ERROR : INVALID-PORT",
			host_buf, lport_temp, fport_temp);

		return (0);
	}

	lport = (in_port_t) lport_temp;
	fport = (in_port_t) fport_temp;

	/* User ID is unknown. */
	con_uid = MISSING_UID;

#if HAVE_LIBUDB
	if (opt_enabled(USEUDB)) {
		struct udb_lookup_res udb_res = get_udb_user(
				lport, fport, &laddr, &faddr, insock);
		if (udb_res.status == 2)
			return (0);
		con_uid = udb_res.uid;
	}
#endif

	if (con_uid == MISSING_UID && laddr.ss_family == AF_INET)
		con_uid = get_user4(htons(lport), htons(fport), &laddr, &faddr);

#if WANT_IPV6
	/*
	 * Check for IPv6-mapped IPv4 addresses. This ensures that the correct
	 * ident response is returned for connections to a mapped address.
	 */
	if (con_uid == MISSING_UID && laddr.ss_family == AF_INET) {
		struct sockaddr_storage laddr_m6, faddr_m6;
		struct in6_addr in6;

		sin_mapv4to6(&SIN4(&laddr)->sin_addr, &in6);
		sin_setv6(&in6, &laddr_m6);

		sin_mapv4to6(&SIN4(&faddr)->sin_addr, &in6);
		sin_setv6(&in6, &faddr_m6);

		con_uid = get_user6(htons(lport), htons(fport), &laddr_m6, &faddr_m6);
	}

	if (con_uid == MISSING_UID && laddr6.ss_family == AF_INET6)
		con_uid = get_user6(htons(lport), htons(fport), &laddr6, &faddr6);
#endif

	if (opt_enabled(MASQ)) {
		if (con_uid == MISSING_UID && laddr.ss_family == AF_INET)
			if (masq(insock, htons(lport), htons(fport), &laddr, &faddr))
				return (0);
	}

	if (con_uid == MISSING_UID) {
		if (failuser != NULL) {
			sockprintf(outsock, "%d,%d:USERID:%s:%s\r\n",
				lport, fport, ret_os, failuser);

			o_log(NORMAL, "[%s] Failed lookup: %d , %d : (returned %s)",
				host_buf, lport, fport, failuser);
		} else {
			sockprintf(outsock, "%d,%d:ERROR:%s\r\n",
				lport, fport, ERROR("NO-USER"));

			o_log(NORMAL, "[%s] %d , %d : ERROR : NO-USER",
				host_buf, lport, fport);
		}

		return (0);
	}

	pw = getpwuid(con_uid);
	if (pw == NULL) {
		sockprintf(outsock, "%d,%d:ERROR:%s\r\n",
			lport, fport, ERROR("NO-USER"));

		debug("getpwuid(%d): %s", con_uid, strerror(errno));
		return (0);
	} else
		copy_pw(pw, &pwd);

	ret = get_ident(&pwd, lport, fport, &laddr, &faddr, suser, sizeof(suser));
	if (ret == -1) {
		sockprintf(outsock, "%d,%d:ERROR:%s\r\n",
			lport, fport, ERROR("HIDDEN-USER"));

		o_log(NORMAL, "[%s] %d , %d : HIDDEN-USER (%s)",
			host_buf, lport, fport, pwd.pw_name);

		goto out;
	}

	sockprintf(outsock, "%d,%d:USERID:%s:%s\r\n",
		lport, fport, ret_os, suser);

	o_log(NORMAL, "[%s] Successful lookup: %d , %d : %s (%s)",
		host_buf, lport, fport, pwd.pw_name, suser);

out:
	free_pw(&pwd);
	return (0);
}

/*
** Copy the needed fields from a passwd struct.
*/

static void copy_pw(const struct passwd *pw, struct passwd *pwd) {
	pwd->pw_name = xstrdup(pw->pw_name);
	pwd->pw_uid = pw->pw_uid;
	pwd->pw_gid = pw->pw_gid;
	pwd->pw_dir = xstrdup(pw->pw_dir);
}

/*
** Free a copied passwd struct.
*/

static void free_pw(struct passwd *pw) {
	free(pw->pw_name);
	free(pw->pw_dir);
}

/*
** Handle SIGSEGV.
*/

static void sig_segv(int unused __notused) {
	o_log(LOG_CRIT, "Caught SIGSEGV; please report this to " PACKAGE_BUGREPORT);
	exit(EXIT_FAILURE);
}

/*
** Handle SIGCHLD.
*/

static void sig_child(int sig) {
	while (waitpid(-1, &sig, WNOHANG) > 0)
		--current_connections;

	signal(SIGCHLD, sig_child);
}

/*
** Handle SIGALRM.
*/

static void sig_alarm(int unused __notused) {
	o_log(NORMAL, "Timeout for request -- Closing connection");
	exit(EXIT_SUCCESS);
}

/*
** Handle SIGHUP - This causes oidentd to reload its configuration file.
*/

static void sig_hup(int unused __notused) {
	user_db_destroy();

	if (read_config(CONFFILE) != 0) {
		o_log(LOG_CRIT, "Error parsing configuration file");
		exit(EXIT_FAILURE);
	}
}

/*
** Seed the PRNG.
*/

static void seed_prng(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	srandom(tv.tv_sec ^ (tv.tv_usec << 11));
}
