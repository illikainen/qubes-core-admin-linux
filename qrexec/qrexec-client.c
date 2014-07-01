/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include "qrexec.h"
#include "libqrexec-utils.h"

// whether qrexec-client should replace ESC with _ before printing the output
int replace_esc_stdout = 0;
int replace_esc_stderr = 0;

int connect_unix_socket(const char *domname)
{
	int s, len;
	struct sockaddr_un remote;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		return -1;
	}

	remote.sun_family = AF_UNIX;
	snprintf(remote.sun_path, sizeof remote.sun_path,
		 QREXEC_DAEMON_SOCKET_DIR "/qrexec.%s", domname);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(s, (struct sockaddr *) &remote, len) == -1) {
		perror("connect");
		exit(1);
	}
	return s;
}

void do_exec(const char *prog)
{
	execl("/bin/bash", "bash", "-c", prog, NULL);
}

static int local_stdin_fd, local_stdout_fd;

void do_exit(int code)
{
	int status;
// sever communication lines; wait for child, if any
// so that qrexec-daemon can count (recursively) spawned processes correctly          
	close(local_stdin_fd);
	close(local_stdout_fd);
	waitpid(-1, &status, 0);
	exit(code);
}


void prepare_local_fds(const char *cmdline)
{
	int pid;
	if (!cmdline) {
		local_stdin_fd = 1;
		local_stdout_fd = 0;
		return;
	}
	do_fork_exec(cmdline, &pid, &local_stdin_fd, &local_stdout_fd,
		     NULL);
}


void send_cmdline(int s, int type, const char *cmdline)
{
	struct client_header hdr;
	hdr.type = type;
	hdr.len = strlen(cmdline) + 1;
	if (!write_all(s, &hdr, sizeof(hdr))
	    || !write_all(s, cmdline, hdr.len)) {
		perror("write daemon");
		do_exit(1);
	}
}

void handle_input(int s)
{
	char buf[MAX_DATA_CHUNK];
	int ret;
	ret = read(local_stdout_fd, buf, sizeof(buf));
	if (ret < 0) {
		perror("read");
		do_exit(1);
	}
	if (ret == 0) {
		close(local_stdout_fd);
		local_stdout_fd = -1;
		shutdown(s, SHUT_WR);
		if (local_stdin_fd == -1) {
			// if pipe in opposite direction already closed, no need to stay alive
			do_exit(0);
		}
	}
	if (!write_all(s, buf, ret)) {
		if (errno == EPIPE) {
			// daemon disconnected its end of socket, so no future data will be
			// send there; there is no sense to read from child stdout
			//
			// since AF_UNIX socket is buffered it doesn't mean all data was
			// received from the agent
			close(local_stdout_fd);
			local_stdout_fd = -1;
			if (local_stdin_fd == -1) {
				// since child does no longer accept data on its stdin, doesn't
				// make sense to process the data from the daemon
				//
				// we don't know real exit VM process code (exiting here, before
				// MSG_SERVER_TO_CLIENT_EXIT_CODE message)
				do_exit(1);
			}
		} else
			perror("write daemon");
	}
}

void do_replace_esc(char *buf, int len) {
	int i;

	for (i = 0; i < len; i++)
		if (buf[i] == '\033')
			buf[i] = '_';
}

void handle_daemon_data(int s)
{
	int status;
	struct client_header hdr;
	char buf[MAX_DATA_CHUNK], *bufptr=buf;

	if (!read_all(s, &hdr, sizeof hdr)) {
		perror("read daemon");
		do_exit(1);
	}
	if (hdr.len > MAX_DATA_CHUNK) {
		fprintf(stderr, "client_header.len=%d\n", hdr.len);
		do_exit(1);
	}
	if (!read_all(s, buf, hdr.len)) {
		perror("read daemon");
		do_exit(1);
	}

	switch (hdr.type) {
	case MSG_SERVER_TO_CLIENT_STDOUT:
		if (replace_esc_stdout)
			do_replace_esc(buf, hdr.len);
		if (local_stdin_fd == -1)
			break;
		if (hdr.len == 0) {
			close(local_stdin_fd);
			local_stdin_fd = -1;
		} else if (!write_all(local_stdin_fd, buf, hdr.len)) {
			if (errno == EPIPE) {
				// remote side have closed its stdin, handle data in oposite
				// direction (if any) before exit
				local_stdin_fd = -1;
			} else {
				perror("write local stdout");
				do_exit(1);
			}
		}
		break;
	case MSG_SERVER_TO_CLIENT_STDERR:
		if (replace_esc_stderr)
			do_replace_esc(buf, hdr.len);
		write_all(2, buf, hdr.len);
		break;
	case MSG_SERVER_TO_CLIENT_EXIT_CODE:
		status = *(unsigned int *) bufptr;
		if (WIFEXITED(status))
			do_exit(WEXITSTATUS(status));
		else
			do_exit(255);
		break;
	default:
		fprintf(stderr, "unknown msg %d\n", hdr.type);
		do_exit(1);
	}
}

// perhaps we could save a syscall if we include both sides in both
// rdset and wrset; to be investigated
void handle_daemon_only_until_writable(int s)
{
	fd_set rdset, wrset;

	do {
		FD_ZERO(&rdset);
		FD_ZERO(&wrset);
		FD_SET(s, &rdset);
		FD_SET(s, &wrset);

		if (select(s + 1, &rdset, &wrset, NULL, NULL) < 0) {
			perror("select");
			do_exit(1);
		}
		if (FD_ISSET(s, &rdset))
			handle_daemon_data(s);
	} while (!FD_ISSET(s, &wrset));
}

void *input_process_loop(void *arg) {
	int s = *(int*)arg;
	while (local_stdout_fd != -1)
		handle_input(s);
	return NULL;
}


void select_loop(int s)
{
	pthread_t input_thread;
	if (pthread_create(&input_thread, NULL, input_process_loop, &s) != 0) {
		perror("pthread_create");
		do_exit(1);
	}
	for (;;) {
		handle_daemon_data(s);
	}
	pthread_join(input_thread, NULL);
}

void usage(const char *name)
{
	fprintf(stderr,
		"usage: %s -d domain_num [-l local_prog] -e -t -T -c remote_cmdline\n"
		"-e means exit after sending cmd, -c: connect to existing process\n"
		"-t enables replacing ESC character with '_' in command output, -T is the same for stderr\n",
		name);
	exit(1);
}

int main(int argc, char **argv)
{
	int opt;
	char *domname = NULL;
	int s;
	int just_exec = 0;
	int connect_existing = 0;
	char *local_cmdline = NULL;
	while ((opt = getopt(argc, argv, "d:l:ectT")) != -1) {
		switch (opt) {
		case 'd':
			domname = strdup(optarg);
			break;
		case 'l':
			local_cmdline = strdup(optarg);
			break;
		case 'e':
			just_exec = 1;
			break;
		case 'c':
			connect_existing = 1;
			break;
		case 't':
			replace_esc_stdout = 1;
			break;
		case 'T':
			replace_esc_stderr = 1;
			break;
		default:
			usage(argv[0]);
		}
	}
	if (optind >= argc || !domname)
		usage(argv[0]);

	register_exec_func(&do_exec);

	s = connect_unix_socket(domname);
	setenv("QREXEC_REMOTE_DOMAIN", domname, 1);
	prepare_local_fds(local_cmdline);

	if (just_exec)
		send_cmdline(s, MSG_CLIENT_TO_SERVER_JUST_EXEC,
			     argv[optind]);
	else {
		int cmd;
		if (connect_existing)
			cmd = MSG_CLIENT_TO_SERVER_CONNECT_EXISTING;
		else
			cmd = MSG_CLIENT_TO_SERVER_EXEC_CMDLINE;
		send_cmdline(s, cmd, argv[optind]);
		select_loop(s);
	}
	return 0;
}
