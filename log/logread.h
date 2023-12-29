/*
 * Copyright (C) 2013 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#pragma once

#define SYSLOG_NAMES
#include <syslog.h>

#include <stdbool.h>

#include <libubox/uloop.h>
#include "syslog.h"

enum {
	LOG_STDOUT,
	LOG_FILE,
	LOG_NET,
};


typedef const char * cstring;

struct sender {
	int type;
	struct uloop_fd fd;
	bool ready;
	void (*write_log)(struct sender *current, uint32_t priority, uint32_t source, 
	cstring ctime, cstring timestamp, cstring message);
};

typedef struct s_fd_open_retry {
	struct uloop_timeout timer;
	struct sender* sender;
} fd_open_retry;

/* socket api:*/
void logread_socket_set_ip_port(cstring ip, cstring port);
void logread_socket_set_udp();
void logread_socket_set_trailer_null();
void logread_socket_set_hostname(cstring hostname);
void logread_socket_set_prefix(cstring prefix);
bool logread_socket_initialized();
void logread_socket_setup(struct sender* current);
void logread_socket_write(struct sender* current);

/* file api:*/
void logread_file_set_filename(cstring filename);
void logread_file_set_file_max_size(int filesize);
bool logread_file_initialized();
void logread_file_setup_cb(struct sender* current);
void logread_file_setup(struct sender* current);