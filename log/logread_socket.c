#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <libubox/usock.h>
#include "logread.h"

static fd_open_retry retry;

static bool log_udp, log_trailer_null;
static cstring log_ip, log_port, log_hostname, log_prefix;


static void add_socket_connect_timeout(cstring context) {
	static const int TIMEOUT = 5000;

	int res = uloop_timeout_set(&retry.timer, TIMEOUT); 
	if (res) {
		syslog(LOG_WARNING, "Cannot set timeout from '%s: %d\n", context, res);
	}
}

static void log_handle_socket_fd_changes(struct uloop_fd *u, unsigned int events)
{
	if (u->eof) {
		uloop_fd_delete(u);
		close(retry.sender->fd.fd);
		retry.sender->fd.fd = -1;
		retry.sender->ready = false;
		add_socket_connect_timeout("log_handle_socket_fd_changes");
	}
}

static void log_handle_reconnect(struct uloop_timeout *timeout)
{
	static int connect_logged_cnt, disconnect_logged_cnt;
	static const int MAX_CONN_LOGS_COUNT = 1, MAX_DISC_LOGS_COUNT = 3;

	retry.sender->fd.fd = usock((log_udp) ? (USOCK_UDP) : (USOCK_TCP), log_ip, log_port);

	if (retry.sender->fd.fd < 0) {
		if (disconnect_logged_cnt < MAX_DISC_LOGS_COUNT) {
			syslog(LOG_ERR, "failed to connect to '%s:%s': %m\n", log_ip, log_port);
			disconnect_logged_cnt++;
		}
		add_socket_connect_timeout("log_handle_reconnect");
		connect_logged_cnt = 0;
	} else {
		uloop_fd_add(&retry.sender->fd, ULOOP_READ);
		if (connect_logged_cnt < MAX_CONN_LOGS_COUNT) {
			syslog(LOG_INFO, "Logread connected to %s:%s via %s\n",
				log_ip, log_port, (log_udp) ? ("udp") : ("tcp"));
			connect_logged_cnt++;
		}
		retry.sender->ready = true;
		disconnect_logged_cnt = 0;
	}
}

static void write_log(struct sender *current, uint32_t priority, uint32_t source, 
	cstring ctime, cstring timestamp, cstring message) {
	char buf[LOG_LINE_SIZE + 128];
	int err;

	snprintf(buf, sizeof(buf), "<%u>", priority);
	strncat(buf, ctime + 4, 16);
	if (timestamp) {
		strncat(buf, timestamp, sizeof(buf) - strlen(buf) - 1);
	}
	if (log_hostname) {
		strncat(buf, log_hostname, sizeof(buf) - strlen(buf) - 1);
		strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
	}
	if (log_prefix) {
		strncat(buf, log_prefix, sizeof(buf) - strlen(buf) - 1);
		strncat(buf, ": ", sizeof(buf) - strlen(buf) - 1);
	}
	if (source == SOURCE_KLOG)
		strncat(buf, "kernel: ", sizeof(buf) - strlen(buf) - 1);
	strncat(buf, message, sizeof(buf) - strlen(buf) - 1);
	size_t buflen = strlen(buf);
	if (!log_trailer_null) {
		buf[buflen] = '\n';
		buflen++;
	}
	if (log_udp) {
		err = write(current->fd.fd, buf, buflen);
	} else {
		err = send(current->fd.fd, buf, buflen, 0);
	}
	
	if (err < 0) {
		syslog(LOG_WARNING, "Failed to send log data to %s:%s via %s\n",
			log_ip, log_port, (log_udp) ? ("udp") : ("tcp"));
		uloop_fd_delete(&current->fd);
		close(current->fd.fd);
		current->fd.fd = -1;
		current->ready = false;
		add_socket_connect_timeout("log_notify");
	}
}

void logread_socket_set_ip_port(cstring ip, cstring port) {
	log_ip = ip;
	log_port = port;
}

bool logread_socket_initialized() {
	return (log_ip != NULL) && (log_port != NULL);
}

void logread_socket_setup(struct sender* current) {
	current->write_log = write_log;
	current->ready = false;
	current->type = LOG_NET;
	current->fd.cb = log_handle_socket_fd_changes;
	retry.timer.cb = log_handle_reconnect;
	retry.sender = current;
	add_socket_connect_timeout("logread_setup_output");
	current++;
}

void logread_socket_set_udp() {
	log_udp = true;
}
void logread_socket_set_trailer_null() {
	log_trailer_null = true;
}

void logread_socket_set_hostname(cstring hostname) {
	log_hostname = hostname;
}

void logread_socket_set_prefix(cstring prefix) {
	log_prefix = prefix;
}
