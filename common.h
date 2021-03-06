#ifndef _common_h_
#define _common_h_
#ifdef __cplusplus
extern "C" {
#endif

/* generic error logging */
#define ESTR(num)	strerror(num)
__attribute__((format(printf,2,3)))
extern void mylog(int loglevel, const char *fmt, ...);
extern void myopenlog(const char *name, int options, int facility);
extern void myloglevel(int loglevel);
extern int mysetloglevelstr(char *str);

extern const char *mydtostr(double d);
extern double mystrtod(const char *str, char **endp);

/* return absolute path of <path> reletive from <ref> */
extern char *resolve_relative_path(const char *path, const char *ref);

/* tool to synchronize to MQTT */
struct mosquitto_message;
struct mosquitto;
extern int is_self_sync(const struct mosquitto_message *msg);
extern void send_self_sync(struct mosquitto *mosq, int qos);

#ifdef __cplusplus
}
#endif
#endif
