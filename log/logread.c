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

#include <time.h>
#include <regex.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>

#include "logread.h"

#include <libubox/blobmsg_json.h>
#include <libubox/list.h>
#include <libubox/ustream.h>
#include <libubox/utils.h>
#include <libubus.h>

enum {
	LOG_MSG,
	LOG_ID,
	LOG_PRIO,
	LOG_SOURCE,
	LOG_TIME,
	__LOG_MAX
};

static const struct blobmsg_policy log_policy[] = {
	[LOG_MSG] = { .name = "msg", .type = BLOBMSG_TYPE_STRING },
	[LOG_ID] = { .name = "id", .type = BLOBMSG_TYPE_INT32 },
	[LOG_PRIO] = { .name = "priority", .type = BLOBMSG_TYPE_INT32 },
	[LOG_SOURCE] = { .name = "source", .type = BLOBMSG_TYPE_INT32 },
	[LOG_TIME] = { .name = "time", .type = BLOBMSG_TYPE_INT64 },
};

static struct sender *senders;
static size_t sender_count;

static regex_t regexp_preg;
static const char *pid_file, *regexp_pattern;
static bool log_follow, log_timestamp;

static int facility_include;
static int facility_exclude;
static int priority = LOG_DEBUG; // read all by default


static uint32_t log_ubus_objid;
static struct blob_buf b_log;
static struct ubus_request req_log;
static struct ubus_context *ctx;

#define _unused __attribute__((unused))

/* check for facility filter; return 0 if message shall be dropped */
static int check_facility_filter(int f) {
	if (facility_include)
		return !!(facility_include & (1 << f));
	if (facility_exclude)
		return !(facility_exclude & (1 << f));
	return 1;
 }

/* check for priority filter; return 0 if message shall be dropped */
static int check_priority_filter(int prio) {
	return prio <= priority;
}

static void log_notify(struct blob_attr *msg)
{
	struct blob_attr *tb[__LOG_MAX];
	char buf_ts[32];
	uint32_t p, src;
	char *c, *m;
	struct sender *current;

	blobmsg_parse(log_policy, ARRAY_SIZE(log_policy), tb, blob_data(msg), blob_len(msg));
	if (!tb[LOG_ID] || !tb[LOG_PRIO] || !tb[LOG_SOURCE] || !tb[LOG_TIME] || !tb[LOG_MSG]) {
		char *rec = "Unkn";
		if(tb[LOG_MSG]) {
			rec = blobmsg_get_string(tb[LOG_MSG]);
		}
		syslog(LOG_WARNING, "Unknown log record %s\n", rec);
		return;
	}

	p = blobmsg_get_u32(tb[LOG_PRIO]);

	if (!check_facility_filter(LOG_FAC(p)) || !check_priority_filter(LOG_PRI(p)))
		return;

	m = blobmsg_get_string(tb[LOG_MSG]);
	if (regexp_pattern &&
	    regexec(&regexp_preg, m, 0, NULL, 0) == REG_NOMATCH)
		return;

	{
		time_t t;
		uint32_t t_ms = 0;

		t = blobmsg_get_u64(tb[LOG_TIME]) / 1000;
		if (log_timestamp) {
			t_ms = blobmsg_get_u64(tb[LOG_TIME]) % 1000;
			snprintf(buf_ts, sizeof(buf_ts), "[%lu.%03u] ",
					(unsigned long)t, t_ms);
		}
		c = ctime(&t);
		c[strlen(c) - 1] = '\0';
	}
	src = blobmsg_get_u32(tb[LOG_SOURCE]);

	for (current = senders; current < senders + sender_count; current++) {
		if (!current->ready) {
			// already closed or not opened yet
			continue;
		}
		current->write_log(current, p, src, c, log_timestamp ? buf_ts : NULL, m);
	}
}

static int usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n"
		"Options:\n"
		"    -s <path>		Path to ubus socket\n"
		"    -l	<count>		Got only the last 'count' messages\n"
		"    -e	<pattern>	Filter messages with a regexp\n"
		"    -r	<server> <port>	Stream message to a server\n"
		"    -F	<file>		Log file\n"
		"    -S	<bytes>		Log size\n"
		"    -p	<file>		PID file\n"
		"    -h	<hostname>	Add hostname to the message\n"
		"    -P	<prefix>	Prefix custom text to streamed messages\n"
		"    -z	<facility>	handle only messages with given facility (0-23), repeatable\n"
		"    -Z	<facility>	ignore messages with given facility (0-23), repeatable\n"
		"    -Y	<priority>	handle only messages with given priority (0-7) or below\n"
		"    -f			Follow log messages\n"
		"    -u			Use UDP as the protocol\n"
		"    -t			Add an extra timestamp\n"
		"    -0			Use \\0 instead of \\n as trailer when using remote\n"
		"\n", prog);
	return 1;
}

static void logread_fd_data_cb(struct ustream *s, int bytes)
{
	while (true) {
		struct blob_attr *a;
		int len, cur_len;

		a = (void*) ustream_get_read_buf(s, &len);
		// strange logic.. What if part of header in one buffer and other in another? 
		// Will not fix it for now...
		if (len < sizeof(*a))
			break;

		cur_len = blob_len(a) + sizeof(*a);
		if (len < cur_len)
			break;

		log_notify(a);
		ustream_consume(s, cur_len);
	}
}

static void logread_fd_state_cb(struct ustream *s)
{
	if (!log_follow && s->eof) {
		uloop_end();
	}
}

static void logread_fd_cb(_unused struct ubus_request *req, int fd)
{
	static struct ustream_fd test_fd;

	memset(&test_fd, 0, sizeof(test_fd));

	test_fd.stream.notify_read = logread_fd_data_cb;
	test_fd.stream.notify_state = logread_fd_state_cb;
	ustream_fd_init(&test_fd, fd);
}

static void logread_setup_output(void)
{
	int all_log_types = 0;
	struct sender* current;

	sender_count = 0; // don't have to but once we will handle reload in future...
	if (logread_socket_initialized()) {
		sender_count++;
		all_log_types |= (1<<LOG_NET);
	}

	if (logread_file_initialized()) {
		sender_count++;
		all_log_types |= (1<<LOG_FILE);
	}

	if (!all_log_types) {
		sender_count = 1;
		all_log_types = 1<<LOG_STDOUT;
	}

	senders = (struct sender *)calloc(sizeof(struct sender), sender_count);
	if (!senders) {
		syslog(LOG_ERR, "No memory for senders: %m\n");
		exit(-1);
	}

	current = senders;

	if (all_log_types & (1<<LOG_NET)) {
		logread_socket_setup(current);
		current++;
	}
		
	if (all_log_types & (1<<LOG_FILE)) {
		logread_file_setup(current);
		current++;
	}

	if (all_log_types & (1<<LOG_STDOUT)) {
		logread_file_setup_cb(current);
		current->ready = true;
		current->type = LOG_STDOUT;
		current->fd.fd = STDOUT_FILENO;
	}
}

static void cancel_log_events_async(void) {
	ubus_abort_request(ctx, &req_log);
}

static void add_log_events_async(void) {
	cancel_log_events_async();
	if (!ubus_invoke_async(ctx, log_ubus_objid, "read", b_log.head, &req_log)) {
		req_log.fd_cb = logread_fd_cb;
		ubus_complete_request_async(ctx, &req_log);
	}
}

enum {
	OBJ_ATTR_ID,
	OBJ_ATTR_PATH,
	OBJ_ATTR_MAX
};

static const struct blobmsg_policy obj_attrs[OBJ_ATTR_MAX] = {
	[OBJ_ATTR_ID] = { .name = "id", .type = BLOBMSG_TYPE_INT32 },
	[OBJ_ATTR_PATH] = { .name = "path", .type = BLOBMSG_TYPE_STRING },
};

static void handle_obj_event(_unused struct ubus_context *ctx, 
	_unused struct ubus_event_handler *ev,
	_unused const char *type, struct blob_attr *msg) {
	static const char* ADD_EVENT = "ubus.object.add"; 
	static const char* REM_EVENT = "ubus.object.remove";
	static const char* OBJECT_PATH = "log";

	struct blob_attr *tb[OBJ_ATTR_MAX];
	blobmsg_parse(obj_attrs, OBJ_ATTR_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[OBJ_ATTR_ID] || !tb[OBJ_ATTR_PATH])
		return;

	if (strncmp(blobmsg_get_string(tb[OBJ_ATTR_PATH]), OBJECT_PATH, sizeof(OBJECT_PATH)/sizeof(char)))
		return;

	syslog(LOG_DEBUG, "Handle for object 'log' %s\n", type);

	if (!strncmp(type, ADD_EVENT, sizeof(ADD_EVENT)/sizeof(char))) {
		log_ubus_objid = blobmsg_get_u32(tb[OBJ_ATTR_ID]);
		add_log_events_async();
	} else if (!strncmp(type, REM_EVENT, sizeof(REM_EVENT)/sizeof(char))) {
		cancel_log_events_async();
	}
}

static struct ubus_event_handler obj_event_handler = { .cb = handle_obj_event };

int main(int argc, char **argv)
{
	uint32_t id;
	uint32_t log_lines = 0;
	const char *ubus_socket = NULL;
	int ch;

	signal(SIGPIPE, SIG_IGN);

	openlog("logread", LOG_PID, LOG_DAEMON); // default mask is 255 i.e. log all

	while ((ch = getopt(argc, argv, "u0fcs:l:z:Z:Y:r:F:p:S:P:h:e:t")) != -1) {
		switch (ch) {
		case 'u':
			logread_socket_set_udp();
			break;
		case '0':
			logread_socket_set_trailer_null();
			break;
		case 's':
			ubus_socket = optarg;
			break;
		case 'r':
			logread_socket_set_ip_port(optarg++, argv[optind++]);
			break;
		case 'F':
			logread_file_set_filename(optarg);
			break;
		case 'p':
			pid_file = optarg;
			break;
		case 'P':
			logread_socket_set_prefix(optarg);
			break;
		case 'f':
			log_follow = true;
			break;
		case 'l':
			log_lines = atoi(optarg);
			break;
 		case 'z':
			id = strtoul(optarg, NULL, 0) & 0x1f;
			facility_include |= (1 << id);
			break;
		case 'Z':
			id = strtoul(optarg, NULL, 0) & 0x1f;
			facility_exclude |= (1 << id);
 			break;
		case 'Y':
			priority = atoi(optarg);
			break;
		case 'S':
			logread_file_set_file_max_size(atoi(optarg));
			break;
		case 'h':
			logread_socket_set_hostname(optarg);
			break;
		case 'e':
			if (!regcomp(&regexp_preg, optarg, REG_NOSUB)) {
				regexp_pattern = optarg;
			}
			break;
		case 't':
			log_timestamp = true;
			break;
		default:
			return usage(*argv);
		}
	}
	INIT_LIST_HEAD(&req_log.list); // to not bother about ubus_abort_request
	blob_buf_init(&b_log, 0);
	blobmsg_add_u8(&b_log, "stream", 1);
	blobmsg_add_u8(&b_log, "oneshot", !log_follow);
	if (log_lines)
		blobmsg_add_u32(&b_log, "lines", log_lines);
	else if (log_follow)
		blobmsg_add_u32(&b_log, "lines", 0);
		
	uloop_init();

	ctx = ubus_connect(ubus_socket);
	if (!ctx) {
		syslog(LOG_ERR, "Failed to connect to ubus\n");
		return -1;
	}
	// why io is blocking...?
	ubus_add_uloop(ctx);

	if (log_follow && pid_file) {
		FILE *fp = fopen(pid_file, "w+");
		if (fp) {
			fprintf(fp, "%d", getpid());
			fclose(fp);
		}
	}

	logread_setup_output();

	if (ubus_register_event_handler(ctx, &obj_event_handler, "ubus.object.*")) {
		syslog(LOG_ERR, "Failed to register object handler\n");
		return -1;
	}

	if (!ubus_lookup_id(ctx, "log", &log_ubus_objid)) {
		add_log_events_async();
	}
	syslog(LOG_DEBUG, "uloop_run\n");
	uloop_run();
	ubus_free(ctx);
	uloop_done();
	free(senders);

	if (log_follow && pid_file)
		unlink(pid_file);

	return 0;
}
