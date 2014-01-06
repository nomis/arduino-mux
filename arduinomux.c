#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arduinomux.h"
#include "monq.h"

char *device;
int pin[MAX_QUEUES];
char *mqueue[MAX_QUEUES];
int group[MAX_QUEUES];
int queues;
int fd;
mqd_t q[MAX_QUEUES];
char buf[32];
int buflen = 0;

static void setup(int argc, char *argv[]) {
	int i;

	if (argc < 2 || ((argc - 2) % 3) != 0) {
		printf("Usage: %s <device> [<pin> <mqueue> <group>]...\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	device = argv[1];

	for (i = 2; i + 2 < argc; i += 3) {
		if (queues >= MAX_QUEUES) {
			printf("Too many queues specified\n");
			exit(EXIT_FAILURE);
		}

		pin[queues] = 1 << atoi(argv[i]);
		mqueue[queues] = argv[i + 1];
		group[queues] = atoi(argv[i + 2]);

		queues++;
	}
}

static void init_root(void) {
	if (geteuid() == 0) {
		struct sched_param schedp;

		cerror("Failed to lock memory pages", mlockall(MCL_CURRENT | MCL_FUTURE));
		cerror("Failed to get max scheduler priority", (schedp.sched_priority = sched_get_priority_max(SCHED_FIFO)) < 0);
		schedp.sched_priority -= 15;
		cerror("Failed to set scheduler policy", sched_setscheduler(0, SCHED_FIFO, &schedp));
		cerror("Failed to drop SGID permissions", setregid(getgid(), getgid()));
		cerror("Failed to drop SUID permissions", setreuid(getuid(), getuid()));
	}
}

static void init(void) {
	struct mq_attr q_attr = {
		.mq_flags = 0,
		.mq_maxmsg = 4096,
		.mq_msgsize = sizeof(mon_t)
	};
	int egid;
	int i;

	init_root();

	fd = open(device, O_RDWR|O_NONBLOCK);
	cerror(device, fd < 0);

	egid = getegid();

	for (i = 0; i < queues; i++) {
		cerror(mqueue[i], setegid(group[i]));

		q[i] = mq_open(mqueue[i], O_WRONLY|O_NONBLOCK|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP, &q_attr);
		cerror(mqueue[i], q[i] < 0);
	}

	cerror("setegid", setegid(egid));
}

static void daemon(void) {
#ifdef FORK
	pid_t pid = fork();
	cerror("Failed to become a daemon", pid < 0);
	if (pid)
		exit(EXIT_SUCCESS);
	close(0);
	close(1);
	close(2);
	setsid();
#endif
}

static void report(int idx, bool on) {
	mon_t event;

	gettimeofday(&event.tv, NULL);
	event.on = on;

	_printf("%lu.%06u: %s %d\n", (unsigned long int)event.tv.tv_sec, (unsigned int)event.tv.tv_usec, mqueue[idx], event.on);
	mq_send(q[idx], (const char *)&event, sizeof(event), 0);
}

static void check(int value) {
	static bool first = true;
	static int last[MAX_QUEUES] = { 0 };
	int state[MAX_QUEUES];
	int i;

	for (i = 0; i < queues; i++)
		state[i] = value & pin[i];

	if (first) {
		first = false;
	} else {
		for (i = 0; i < queues; i++)
			if (last[i] != state[i])
				report(i, state[i] != 0);
	}

	memcpy(last, state, sizeof(last));
}

static void wait(void) {
	fd_set rfds;
	struct timeval tv;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	tv.tv_sec = 5;
	tv.tv_usec = 0;

	select(fd + 1, &rfds, NULL, NULL, &tv);
}

static bool ping(void) {
	return write(fd, "\n", 1) == 1;
}

static void process(const char *line) {
	if (line[0] == 'V')
		check(atoi(&line[1]));
}

static bool readline(void) {
	int remaining = sizeof(buf) - buflen - 1;
	int len;
	char *p;

	if (remaining <= 0) {
		buflen = 0;
		remaining = sizeof(buf) - 1;
	}

	wait();

	len = read(fd, &buf[buflen], remaining);
	if (len <= 0)
		return false;

	buflen += len;
	buf[buflen] = 0;

	do {
		p = strchr(buf, '\n');

		if (p != NULL) {
			*p = '\0';
			if (*(p - 1) == '\r')
				*(p - 1) = '\0';

			process(buf);

			memmove(buf, p + 1, sizeof(buf) - (p - buf) - 1);
			buflen -= (p - buf) + 1;
		}
	} while (p != NULL);

	return true;
}

static void loop(void) {
	while (readline() || ping());
	perror(device);
}

static void cleanup(void) {
	int i;

	cerror(device, close(fd));

	for (i = 0; i < queues; i++)
		cerror(mqueue[i], mq_close(q[i]));
}

int main(int argc, char *argv[]) {
	setup(argc, argv);
	init();
	daemon();
	loop();
	cleanup();
	exit(EXIT_FAILURE);
}
