#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logread.h"

static cstring log_file;
static int log_size;

static fd_open_retry retry;

static void register_retry() {
	static const int TIMEOUT = 500;

	int res = uloop_timeout_set(&retry.timer, TIMEOUT); 
	if (res) {
		syslog(LOG_WARNING, "Cannot set timeout from '%s: %d\n", "file.register_retry", res);
	}
}

static void open_file_fd(struct sender *current) {
	static const int MAX_NON_LOGGED_ATTEMPTS = 600; // i.e. log once per 5 minutes
	static int check_counter;

	current->fd.fd = open(log_file, O_CREAT | O_WRONLY | O_APPEND, 0664);

	if (current->fd.fd >= 0) {
		current->ready = true;
		syslog(LOG_INFO, "Log reader attached to file '%s'\n", log_file);
		return;
	}
	current->ready = false;
	check_counter++;
	if (check_counter == MAX_NON_LOGGED_ATTEMPTS) {
		syslog(LOG_WARNING, "Cannot create log file '%s' within %d attempts: %m\n", 
			log_file, check_counter);
		check_counter = 0;
	}
	register_retry();
}

static void retry_file_creatable(struct uloop_timeout *timeout) {
	open_file_fd(retry.sender);
}

static const char* getcodetext(int value, CODE *codetable) {
	CODE *i;

	if (value >= 0)
		for (i = codetable; i->c_val != -1; i++)
			if (i->c_val == value)
				return (i->c_name);
	return "<unknown>";
}

static void rotate_file(struct sender *current) {
	char *old = malloc(strlen(log_file) + 5);

	close(current->fd.fd);
	if (old) {
		sprintf(old, "%s.old", log_file);
		rename(log_file, old);
		free(old);
	} else {
		syslog(LOG_WARNING, "No memory for rotation\n");
	}
	open_file_fd(current);
}

static void write_log(struct sender *current, uint32_t priority, uint32_t source, 
	cstring ctime, cstring timestamp, cstring message) {
	
	char buf[LOG_LINE_SIZE + 128];
	
	struct stat s;
	int err;
	
	if ((current->type == LOG_FILE) && log_size && 
		(!stat(log_file, &s)) && (s.st_size >= log_size)) {
		rotate_file(current);
	}
	snprintf(buf, sizeof(buf), "%s %s%s.%s%s %s\n",
		ctime, timestamp ? timestamp : "",
		getcodetext(LOG_FAC(priority) << 3, facilitynames),
		getcodetext(LOG_PRI(priority), prioritynames),
		(source == SOURCE_KLOG) ? (" kernel:") : (""), message);
	err = write(current->fd.fd, buf, strlen(buf));
	if (current->type == LOG_FILE) {
		if (err < 0) {
			syslog(LOG_WARNING, "Cannot write log to file '%s': '%m'\n", log_file);
			close(current->fd.fd);
			open_file_fd(retry.sender);
		} else {
			fsync(current->fd.fd);
		}
	}
}

void logread_file_set_filename(cstring filename) {
	log_file = filename;
	if (log_file[0] == '\0') {
		log_file = NULL;
	}
}

void logread_file_set_file_max_size(int filesize) {
	log_size = filesize;
	if (log_size < 1)
		log_size = 1;
	log_size *= 1024;
}

bool logread_file_initialized() {
	return (log_file != NULL);
}

void logread_file_setup_cb(struct sender* current) {
	current->write_log = write_log;
}

void logread_file_setup(struct sender* current) {
	logread_file_setup_cb(current);
	current->type = LOG_FILE;
	retry.sender = current;
	retry.timer.cb = retry_file_creatable;
	open_file_fd(current);
}