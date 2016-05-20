#include <sys/fsuid.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <mqueue.h>
#include <pwd.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include "arduinomux.h"
#include "monq.h"

/* Some code copied from taylor-uucp. */

#define ICLEAR_IFLAG (BRKINT | ICRNL | IGNBRK | IGNCR | IGNPAR \
   | INLCR | INPCK | ISTRIP | IXOFF | IXON \
   | PARMRK | IMAXBEL)
#define ICLEAR_OFLAG (OPOST)
#define ICLEAR_CFLAG (CSIZE | PARENB | PARODD | HUPCL)
#define ISET_CFLAG (CS8 | CREAD | CLOCAL)
#define ICLEAR_LFLAG (ECHO | ECHOE | ECHOK | ECHONL | ICANON | IEXTEN \
   | ISIG | NOFLSH | TOSTOP)

/* ---- */

char *device;
uid_t uid;
gid_t gid;
int pin[MAX_QUEUES];
char *mqueue[MAX_QUEUES];
gid_t group[MAX_QUEUES];
int queues;
int fd;
mqd_t q[MAX_QUEUES];
char buf[32];
int buflen = 0;

static void setup(int argc, char *argv[]) {
	struct passwd *pwd;
	struct group *grp;
	int i;

	if (argc < 4 || ((argc - 4) % 3) != 0) {
		printf("Usage: %s <user> <group> <device> [<pin> <mqueue> <group>]...\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	pwd = getpwnam(argv[1]);
	if (pwd == NULL) {
		printf("Unknown user %s\n", argv[1]);
		exit(EXIT_FAILURE);
	}

	grp = getgrnam(argv[2]);
	if (grp == NULL) {
		printf("Unknown group %s\n", argv[2]);
		exit(EXIT_FAILURE);
	}

	uid = pwd->pw_uid;
	gid = grp->gr_gid;
	device = argv[3];

	if (uid == 0) {
		printf("Not running as root\n");
		exit(EXIT_FAILURE);
	}

	for (i = 4; i + 2 < argc; i += 3) {
		if (queues >= MAX_QUEUES) {
			printf("Too many queues specified\n");
			exit(EXIT_FAILURE);
		}

		pin[queues] = 1 << atoi(argv[i]);
		mqueue[queues] = argv[i + 1];

		grp = getgrnam(argv[i + 2]);
		if (grp == NULL) {
			printf("Unknown group %s\n", argv[i + 2]);
			exit(EXIT_FAILURE);
		}
		group[queues] = grp->gr_gid;

		queues++;
	}

	openlog("arduinomux", LOG_PID, LOG_DAEMON);
}

static void init_root(void) {
	if (geteuid() == 0) {
		struct sched_param schedp;

		cerror("Failed to lock memory pages", mlockall(MCL_CURRENT | MCL_FUTURE));
		cerror("Failed to get max scheduler priority", (schedp.sched_priority = sched_get_priority_max(SCHED_FIFO)) < 0);
		schedp.sched_priority -= 15;
		cerror("Failed to set scheduler policy", sched_setscheduler(0, SCHED_FIFO, &schedp));
		cerror("Failed to set groups", setgroups(0, NULL));
		cerror("Failed to set gid", setregid(gid, gid));
		cerror("Failed to set uid", setreuid(uid, uid));
	}
}

static void safe_setfsuid(uid_t newuid) {
	setfsuid(newuid);
	if ((uid_t)setfsuid(newuid) != newuid) {
		fprintf(stderr, "setfsuid %d failed\n", newuid);
		exit(EXIT_FAILURE);
	}
}

static void safe_setfsgid(gid_t newgid) {
	setfsgid(newgid);
	if ((gid_t)setfsgid(newgid) != newgid) {
		fprintf(stderr, "setfsgid %d failed\n", newgid);
		exit(EXIT_FAILURE);
	}
}

static void init(void) {
	struct mq_attr q_attr = {
		.mq_flags = 0,
		.mq_maxmsg = 4096,
		.mq_msgsize = sizeof(mon_t)
	};
	int i;
	struct termios ios;

	umask(7007);
	safe_setfsuid(uid);

	for (i = 0; i < queues; i++) {
		safe_setfsgid(group[i]);

		q[i] = mq_open(mqueue[i], O_WRONLY|O_NONBLOCK|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP, &q_attr);
		cerror(mqueue[i], q[i] < 0);
	}

	safe_setfsgid(gid);

	init_root();

	fd = open(device, O_RDWR|O_NONBLOCK);
	cerror(device, fd < 0);

	cerror("Failed to get terminal attributes", tcgetattr(fd, &ios));
	ios.c_iflag &=~ ICLEAR_IFLAG;
	ios.c_oflag &=~ ICLEAR_OFLAG;
	ios.c_cflag &=~ ICLEAR_CFLAG;
	ios.c_cflag |= ISET_CFLAG;
	ios.c_lflag &=~ ICLEAR_LFLAG;
	ios.c_cc[VMIN] = 1;
	ios.c_cc[VTIME] = 0;
	cfsetispeed(&ios, B115200);
	cfsetospeed(&ios, B115200);

	cerror("Failed to flush terminal input", ioctl(fd, TCFLSH, 0) < 0);
	cerror("Failed to set terminal attributes", tcsetattr(fd, TCSANOW, &ios));
}

static void report(int idx, const mon_t *event) {
	_printf("%lu.%06u: %s %d\n", (unsigned long int)event->tv.tv_sec, (unsigned int)event->tv.tv_usec, mqueue[idx], event->on);
	mq_send(q[idx], (const char *)event, sizeof(*event), 0);
}

static void reset(void) {
	mon_t event;
	int i;

	event.tv.tv_sec = 0;
	event.tv.tv_usec = 0;
	event.on = false;

	for (i = 0; i < queues; i++)
		report(i, &event);
}

static void check(int value) {
	static bool first = true;
	static int last[MAX_QUEUES] = { 0 };
	int state[MAX_QUEUES];
	mon_t event;
	int i;

	for (i = 0; i < queues; i++)
		state[i] = value & pin[i];

	if (first) {
		first = false;
	} else {
		gettimeofday(&event.tv, NULL);

		for (i = 0; i < queues; i++) {
			if (last[i] != state[i]) {
				event.on = state[i];
				report(i, &event);
			}
		}
	}

	memcpy(last, state, sizeof(last));
}

static void wait(void) {
	fd_set rfds;
	struct timeval tv;
	int ret;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	tv.tv_sec = 30;
	tv.tv_usec = 0;

	ret = select(fd + 1, &rfds, NULL, NULL, &tv);
	if (ret != 1)
		syslog(LOG_CRIT, "%s: select() returned %d (errno=%d)\n", device, ret, errno);
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

	errno = 0;
	len = read(fd, &buf[buflen], remaining);
	if (len <= 0) {
		if (errno == 0)
			errno = EPIPE;
		return false;
	}

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
	while (readline());
	perror(device);
}

static void cleanup(void) {
	int i;

	closelog();
	cerror(device, close(fd));

	for (i = 0; i < queues; i++)
		cerror(mqueue[i], mq_close(q[i]));
}

int main(int argc, char *argv[]) {
	setup(argc, argv);
	init();
#ifdef FORK
	cerror("Failed to become a daemon", daemon(true, false));
#endif
	syslog(LOG_NOTICE, "%s: running\n", device);
	reset();
	loop();
	syslog(LOG_WARNING, "%s: exiting\n", device);
	cleanup();
	exit(EXIT_FAILURE);
}
