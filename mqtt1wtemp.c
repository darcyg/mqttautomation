#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/stat.h>
#include <mosquitto.h>

#include "lib/libt.h"

#define NAME "mqtt1wtemp"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* generic error logging */
#define mylog(loglevel, fmt, ...) \
	({\
		syslog(loglevel, fmt, ##__VA_ARGS__); \
		if (loglevel <= LOG_ERR)\
			exit(1);\
	})
#define ESTR(num)	strerror(num)

/* program options */
static const char help_msg[] =
	NAME ": publish a DS28 1-wire temp sensor into MQTT\n"
	"usage:	" NAME " [OPTIONS ...] [PATTERN] ...\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -m, --mqtt=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -s, --suffix=STR	Give MQTT topic suffix for spec (default '/1wtemphw')\n"
	"\n"
	"Paramteres\n"
	" PATTERN	A pattern to subscribe for\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "mqtt", required_argument, NULL, 'm', },
	{ "suffix", required_argument, NULL, 's', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?m:s:";

/* signal handler */
static volatile int sigterm;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static const char *mqtt_suffix = "/1wtemphw";
static int mqtt_suffixlen = 9;
static int mqtt_keepalive = 10;
static int mqtt_qos = 1;

/* state */
static struct mosquitto *mosq;

struct item {
	struct item *next;
	struct item *prev;

	char *topic;
	int topiclen;
	char *sysfs;
	char lastvalue[32];
	int lasterrno;
};

struct item *items;

/* MQTT iface */
static void my_mqtt_log(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	static const int logpri_map[] = {
		MOSQ_LOG_ERR, LOG_ERR,
		MOSQ_LOG_WARNING, LOG_WARNING,
		MOSQ_LOG_NOTICE, LOG_NOTICE,
		MOSQ_LOG_INFO, LOG_INFO,
		MOSQ_LOG_DEBUG, LOG_DEBUG,
		0,
	};
	int j;

	for (j = 0; logpri_map[j]; j += 2) {
		if (level & logpri_map[j]) {
			mylog(logpri_map[j+1], "[mosquitto] %s", str);
			return;
		}
	}
}

static int test_suffix(const char *topic, const char *suffix)
{
	int len;

	len = strlen(topic ?: "") - strlen(suffix ?: "");
	if (len < 0)
		return 0;
	/* match suffix */
	return !strcmp(topic+len, suffix ?: "");
}

static int test_nodename(const char *nodename)
{
	/* test node name */
	static char mynodename[128];

	if (!nodename)
		/* empty nodename matches always */
		return 1;

	gethostname(mynodename, sizeof(mynodename));
	return !strcmp(mynodename, nodename);
}

static struct item *get_item(const char *topic, const char *suffix, int create)
{
	struct item *it;
	int len;

	len = strlen(topic ?: "") - strlen(suffix ?: "");
	if (len < 0)
		return NULL;
	/* match suffix */
	if (strcmp(topic+len, suffix ?: ""))
		return NULL;
	/* match base topic */
	for (it = items; it; it = it->next) {
		if ((it->topiclen == len) && !strncmp(it->topic ?: "", topic, len))
			return it;
	}
	if (!create)
		return NULL;
	/* not found, create one */
	it = malloc(sizeof(*it));
	memset(it, 0, sizeof(*it));
	it->topic = strndup(topic, len);
	it->topiclen = len;

	/* insert in linked list */
	it->next = items;
	if (it->next) {
		it->prev = it->next->prev;
		it->next->prev = it;
	} else
		it->prev = (struct item *)(((char *)&items) - offsetof(struct item, next));
	it->prev->next = it;
	return it;
}

static void pubvalue(void *dat);
static void drop_item(struct item *it)
{
	/* remove from list */
	if (it->prev)
		it->prev->next = it->next;
	if (it->next)
		it->next->prev = it->prev;
	/* clean mqtt topic */
	mosquitto_publish(mosq, NULL, it->topic, 0, NULL, 0, 1);
	libt_remove_timeout(pubvalue, it);
	/* free memory */
	free(it->topic);
	if (it->sysfs)
		free(it->sysfs);
	free(it);
}

/* read hw */
static double readvalue(const char *sysfs, int *lasterrno)
{
	int fd, ret;
	char *str;
	char buf[128];

	fd = open(sysfs, O_RDONLY);
	if (fd < 0)
		goto failed;
	ret = read(fd, buf, sizeof(buf)-1);
	if (ret < 0)
		goto failed;
	close(fd);
	buf[ret] = 0;
	str = strstr(buf, " t=");
	if (!str)
		goto retfailed;
	*lasterrno = 0;
	return strtod(str+3, 0)/1e3;
failed:
	/* save errno */
	ret = errno;
	if (ret != *lasterrno)
		mylog(LOG_WARNING, "open %s: %s", sysfs, ESTR(ret));
	*lasterrno = ret;
retfailed:
	return NAN;

}

static void pubvalue(void *dat)
{
	struct item *it = dat;
	int ret;
	double value;
	char buf[sizeof(it->lastvalue)];

	value = readvalue(it->sysfs, &it->lasterrno);
	if (isnan(value))
		strcpy(buf, "");
	else
		sprintf(buf, "%.1lf", value);
	if (!strcmp(buf, it->lastvalue))
		goto done;

	/* publish, retained when writing the topic */
	ret = mosquitto_publish(mosq, NULL, it->topic, strlen(buf), buf, mqtt_qos, 1);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
	else
		strcpy(it->lastvalue, buf);
done:
	libt_repeat_timeout(60, pubvalue, dat);
}

static void w1temp_publish_all(void *dat)
{
	struct item *it;
	const char *sysfs;
	glob_t gl = {};
	int j;
	int myerrno = 0;
	double temp;
	char *str;
	static char topicbuf[1024], valbuf[128];

	if (glob("/sys/bus/w1/devices/28-*/w1_slave", 0, NULL, &gl) == 0) {
		for (j = 0; j < gl.gl_pathc; ++j) {
			sysfs = gl.gl_pathv[j];
			for (it = items; it; it = it->next) {
				if (!strcmp(sysfs, it->sysfs))
					break;
			}
			if (it)
				/* match found, ignore */
				continue;
			/* unmatched sensor found, publish! */

			temp = readvalue(sysfs, &myerrno);
			/* strip the device id from the sysfs path */
			*strrchr(sysfs, '/') = 0;
			str = strrchr(sysfs, '/')+1;
			/* publish value */
			sprintf(valbuf, isnan(temp) ? "" : "%.1lf", temp);
			sprintf(topicbuf, "trace/1w/%s", str);
			/* publish, retained when writing the topic */
			mosquitto_publish(mosq, NULL, topicbuf, strlen(valbuf), valbuf, 0, 0);
		}
	}
	libt_add_timeout(60, w1temp_publish_all, dat);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	int j, forme;
	char *path, *w1name;
	struct item *it;

	if (test_suffix(msg->topic, mqtt_suffix)) {
		/* grab boardname */
		w1name = strtok(msg->payload ?: "", " \t");
		forme = test_nodename(strtok(NULL, " \t"));
		it = get_item(msg->topic, mqtt_suffix, !!msg->payloadlen && forme);
		if (!it)
			return;

		/* this is a spec msg */
		if (!msg->payloadlen || !forme) {
			mylog(LOG_INFO, "removed 1wire spec for %s", it->topic);
			drop_item(it);
			return;
		}

		/* free old 1wire spec */
		if (it->sysfs)
			free(it->sysfs);
		it->sysfs = NULL;

		/* process new 1wire spec */
		if (*w1name == '/') {
			/* absolute path */
			it->sysfs = strdup(w1name);
		} else {
			/* find full path for led or brightness */
			static const char *const sysfs[] = {
				"/sys/bus/w1/devices/%s/w1_slave",
				NULL,
			};
			struct stat st;
			for (j = 0; sysfs[j]; ++j) {
				asprintf(&path, sysfs[j], w1name);
				if (!stat(path, &st)) {
					it->sysfs = path;
					break;
				}
				free(path);
			}
			if (!sysfs[j]) {
				mylog(LOG_INFO, "%s: %s is no 1wire temp sensor", it->topic, w1name);
				return;
			}
			it->sysfs = path;
		}
		mylog(LOG_INFO, "new 1wire temp spec for %s: %s", it->topic, it->sysfs);
		pubvalue(it);
	}
}

static void my_exit(void)
{
	if (mosq)
		mosquitto_disconnect(mosq);
}

int main(int argc, char *argv[])
{
	int opt, ret, waittime;
	char *str;
	char mqtt_name[32];
	int logmask = LOG_UPTO(LOG_NOTICE);

	/* argument parsing */
	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) >= 0)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\nCompiled on %s %s\n",
				NAME, VERSION, __DATE__, __TIME__);
		exit(0);
	case 'v':
		switch (logmask) {
		case LOG_UPTO(LOG_NOTICE):
			logmask = LOG_UPTO(LOG_INFO);
			break;
		case LOG_UPTO(LOG_INFO):
			logmask = LOG_UPTO(LOG_DEBUG);
			break;
		}
		break;
	case 'm':
		mqtt_host = optarg;
		str = strrchr(optarg, ':');
		if (str > mqtt_host && *(str-1) != ']') {
			/* TCP port provided */
			*str = 0;
			mqtt_port = strtoul(str+1, NULL, 10);
		}
		break;
	case 's':
		mqtt_suffix = optarg;
		mqtt_suffixlen = strlen(mqtt_suffix);
		break;

	default:
		fprintf(stderr, "unknown option '%c'\n", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	atexit(my_exit);
	openlog(NAME, LOG_PERROR, LOG_LOCAL2);
	setlogmask(logmask);

	/* MQTT start */
	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, 0);
	if (!mosq)
		mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));
	/* mosquitto_will_set(mosq, "TOPIC", 0, NULL, mqtt_qos, 1); */

	mosquitto_log_callback_set(mosq, my_mqtt_log);
	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR, "mosquitto_connect %s:%i: %s", mqtt_host, mqtt_port, mosquitto_strerror(ret));

	if (optind >= argc) {
		ret = mosquitto_subscribe(mosq, NULL, "#", mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe '#': %s", mosquitto_strerror(ret));
	} else for (; optind < argc; ++optind) {
		ret = mosquitto_subscribe(mosq, NULL, argv[optind], mqtt_qos);
		if (ret)
			mylog(LOG_ERR, "mosquitto_subscribe %s: %s", argv[optind], mosquitto_strerror(ret));
	}

	w1temp_publish_all(NULL);
	while (1) {
		libt_flush();
		waittime = libt_get_waittime();
		if (waittime > 1000)
			waittime = 1000;
		ret = mosquitto_loop(mosq, waittime, 1);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop: %s", mosquitto_strerror(ret));
	}
	return 0;
}