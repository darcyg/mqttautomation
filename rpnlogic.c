#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <syslog.h>

#include "lib/libt.h"
#include "rpnlogic.h"

#define mylog(loglevel, fmt, ...) \
	({\
		syslog(loglevel, fmt, ##__VA_ARGS__); \
		if (loglevel <= LOG_ERR)\
			exit(1);\
	})
#define ESTR(num)	strerror(num)

/* manage */
static struct rpn *rpn_create(void)
{
	struct rpn *rpn;

	rpn = malloc(sizeof(*rpn));
	if (!rpn)
		mylog(LOG_ERR, "malloc failed?");
	memset(rpn, 0, sizeof(*rpn));
	return rpn;
}

static void rpn_free(struct rpn *rpn)
{
	if (rpn->topic)
		free(rpn->topic);
	free(rpn);
}

void rpn_free_chain(struct rpn *rpn)
{
	struct rpn *tmp;

	while (rpn) {
		tmp = rpn;
		rpn = rpn->next;
		rpn_free(tmp);
	}
}

/* algebra */
static int rpn_do_plus(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] + st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_minus(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] - st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_mul(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] * st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_div(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] / st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_pow(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = pow(st->v[st->n-2], st->v[st->n-1]);
	st->n -= 1;
	return 0;
}

/* utilities */
static int rpn_do_limit(struct stack *st, struct rpn *me)
{
	if (st->n < 3)
		/* stack underflow */
		return -1;
	/* limit x-3 between x-2 & x-1 */
	if (st->v[st->n-3] < st->v[st->n-2])
		st->v[st->n-3] = st->v[st->n-2];
	else if (st->v[st->n-3] > st->v[st->n-1])
		st->v[st->n-3] = st->v[st->n-1];
	st->n -= 2;
	return 1;
}

static int rpn_do_inrange(struct stack *st, struct rpn *me)
{
	if (st->n < 3)
		/* stack underflow */
		return -1;
	/* limit x-3 between x-2 & x-1 */
	if (st->v[st->n-2] < st->v[st->n-1])
		/* regular case: DUT min max */
		st->v[st->n-3] = (st->v[st->n-3] >= st->v[st->n-2]) && (st->v[st->n-3] <= st->v[st->n-1]);
	else
		/* regular case: DUT max min */
		st->v[st->n-3] = (st->v[st->n-3] >= st->v[st->n-2]) || (st->v[st->n-3] <= st->v[st->n-1]);
	st->n -= 2;
	return 1;
}

/* bitwise */
static int rpn_do_bitand(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] & (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_bitor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] | (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_bitxor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] ^ (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_bitinv(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	st->v[st->n-1] = ~(int)st->v[st->n-1];
	return 0;
}

/* boolean */
static int rpn_do_booland(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] && (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_boolor(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = (int)st->v[st->n-2] || (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_boolnot(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	st->v[st->n-1] = !(int)st->v[st->n-1];
	return 0;
}

/* compare */
static int rpn_do_lt(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] < (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}
static int rpn_do_gt(struct stack *st, struct rpn *me)
{
	if (st->n < 2)
		/* stack underflow */
		return -1;
	st->v[st->n-2] = st->v[st->n-2] > (int)st->v[st->n-1];
	st->n -= 1;
	return 0;
}

/* generic */
static void rpn_push(struct stack *st, double value)
{
	if (st->n >= st->s) {
		st->s += 16;
		st->v = realloc(st->v, st->s * sizeof(st->v[0]));
		if (!st->v)
			mylog(LOG_ERR, "realloc stack %u failed", st->s);
	}
	st->v[st->n++] = value;
}

static int rpn_do_const(struct stack *st, struct rpn *me)
{
	rpn_push(st, me->value);
	return 0;
}

static int rpn_do_env(struct stack *st, struct rpn *me)
{
	rpn_push(st, rpn_lookup_env(me->topic, me));
	return 0;
}

static int rpn_do_dup(struct stack *st, struct rpn *me)
{
	if (st->n < 1)
		/* stack underflow */
		return -1;
	rpn_push(st, st->v[st->n-1]);
	return 0;
}
static int rpn_do_swap(struct stack *st, struct rpn *me)
{
	double tmp;

	if (st->n < 2)
		/* stack underflow */
		return -1;
	tmp = st->v[st->n-2];
	st->v[st->n-2] = st->v[st->n-1];
	st->v[st->n-1] = tmp;
	return 0;
}

/* timer functions */
static void on_delay(void *dat)
{
	struct rpn *me = dat;

	/* clear output */
	me->cookie ^= 2;
	rpn_run_again(me->dat);
}
static int rpn_do_offdelay(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 2)
		/* stack underflow */
		return -1;

	inval = (int)st->v[st->n-2];

	if (!inval && (me->cookie & 1))
		/* falling edge: schedule timeout */
		libt_add_timeout(st->v[st->n-1], on_delay, me);
	else if (inval && !(me->cookie & 1)) {
		/* rising edge: cancel timeout */
		libt_remove_timeout(on_delay, me);
		/* set high output */
		me->cookie |= 2;
	}
	me->cookie = (me->cookie & ~1) | !!inval;
	/* write output to stack */
	st->v[st->n-2] = !!(me->cookie & 2);
	st->n -= 1;
	return 0;
}
static int rpn_do_ondelay(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 2)
		/* stack underflow */
		return -1;

	inval = (int)st->v[st->n-2];

	if (inval && !(me->cookie & 1))
		/* rising edge: schedule timeout */
		libt_add_timeout(st->v[st->n-1], on_delay, me);
	else if (!inval && (me->cookie & 1)) {
		/* falling edge: cancel timeout */
		libt_remove_timeout(on_delay, me);
		/* set low output */
		me->cookie &= ~2;
	}
	me->cookie = (me->cookie & ~1) | !!inval;
	/* write output to stack */
	st->v[st->n-2] = !!(me->cookie & 2);
	st->n -= 1;
	return 0;
}
static int rpn_do_pulse(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 2)
		/* stack underflow */
		return -1;

	inval = (int)st->v[st->n-2];

	if (inval && !(me->cookie & 1)) {
		/* rising edge: schedule timeout */
		libt_add_timeout(st->v[st->n-1], on_delay, me);
		/* set high output */
		me->cookie |= 2;
	} else if (!inval && (me->cookie & 1)) {
		/* falling edge: cancel timeout */
		libt_remove_timeout(on_delay, me);
	}
	me->cookie = (me->cookie & ~1) | !!inval;
	/* write output to stack */
	st->v[st->n-2] = !!(me->cookie & 2);
	st->n -= 1;
	return 0;
}

/* event functions */
static int rpn_do_edge(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 1)
		/* stack underflow */
		return -1;

	inval = (int)st->v[st->n-1];
	st->v[st->n-1] = inval != me->cookie;
	me->cookie = inval;
	return 0;
}

static int rpn_do_rising(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 1)
		/* stack underflow */
		return -1;

	inval = (int)st->v[st->n-1];
	/* set output on rising edge */
	st->v[st->n-1] = inval && !me->cookie;
	me->cookie = inval;
	return 0;
}

static int rpn_do_falling(struct stack *st, struct rpn *me)
{
	int inval;

	if (st->n < 1)
		/* stack underflow */
		return -1;

	inval = (int)st->v[st->n-1];
	/* set output on rising edge */
	st->v[st->n-1] = !inval && me->cookie;
	me->cookie = inval;
	return 0;
}

/* date/time functions */
static inline int next_minute(struct tm *tm)
{
	int next;

	next = 60 - tm->tm_sec;
	return (next <= 0 || next > 60) ? 60 : next;
}

static int rpn_do_timeofday(struct stack *st, struct rpn *me)
{
	time_t t;
	struct tm *tm;

	time(&t);
	tm = localtime(&t);
	rpn_push(st, tm->tm_hour + tm->tm_min/60.0 + tm->tm_sec/3600.0);
	libt_add_timeout(next_minute(tm), rpn_run_again, me->dat);
	return 0;
}

static int rpn_do_dayofweek(struct stack *st, struct rpn *me)
{
	time_t t;
	struct tm *tm;

	time(&t);
	tm = localtime(&t);
	rpn_push(st, tm->tm_wday ?: 7 /* push 7 for sunday */);
	libt_add_timeout(next_minute(tm), rpn_run_again, me->dat);
	return 0;
}

/* run time functions */
void rpn_stack_reset(struct stack *st)
{
	st->n = 0;
}

int rpn_run(struct stack *st, struct rpn *rpn)
{
	int ret;

	for (; rpn; rpn = rpn->next) {
		ret = rpn->run(st, rpn);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* parser */
static struct lookup {
	const char *str;
	int (*run)(struct stack *, struct rpn *);
} const lookups[] = {
	{ "+", rpn_do_plus, },
	{ "-", rpn_do_minus, },
	{ "*", rpn_do_mul, },
	{ "/", rpn_do_div, },
	{ "**", rpn_do_pow, },

	{ "&", rpn_do_bitand, },
	{ "|", rpn_do_bitor, },
	{ "^", rpn_do_bitxor, },
	{ "~", rpn_do_bitinv, },

	{ "&&", rpn_do_booland, },
	{ "||", rpn_do_boolor, },
	{ "!", rpn_do_boolnot, },

	{ "<", rpn_do_lt, },
	{ ">", rpn_do_gt, },

	{ "dup", rpn_do_dup, },
	{ "swap", rpn_do_swap, },

	{ "limit", rpn_do_limit, },
	{ "inrange", rpn_do_inrange, },

	{ "ondelay", rpn_do_ondelay, },
	{ "offdelay", rpn_do_offdelay, },
	{ "pulse", rpn_do_pulse, },

	{ "edge", rpn_do_edge, },
	{ "rising", rpn_do_rising, },
	{ "falling", rpn_do_falling, },
	{ "changed", rpn_do_edge, },
	{ "pushed", rpn_do_rising, },

	{ "timeofday", rpn_do_timeofday, },
	{ "dayofweek", rpn_do_dayofweek, },
	{ "", },
};

static const struct lookup *do_lookup(const char *tok)
{
	const struct lookup *lookup;

	for (lookup = lookups; lookup->str[0]; ++lookup) {
		if (!strcmp(lookup->str, tok))
			return lookup;
	}
	return NULL;
}

static const char digits[] = "0123456789";
struct rpn *rpn_parse(const char *cstr, void *dat)
{
	char *savedstr;
	char *tok, *endp;
	struct rpn *root = NULL, *last = NULL, *rpn;
	const struct lookup *lookup;

	savedstr = strdup(cstr);
	for (tok = strtok(savedstr, " \t"); tok; tok = strtok(NULL, " \t")) {
		if (strchr(digits, *tok) || (tok[1] && strchr("+-", *tok) && strchr(digits, tok[1]))) {
			rpn = rpn_create();
			rpn->run = rpn_do_const;
			rpn->value = strtod(tok, &endp);
			if (*endp && strchr(":h'", *endp))
				rpn->value += strtod(endp+1, &endp)/60;
			if (*endp && strchr(":m\"", *endp))
				rpn->value += strtod(endp+1, &endp)/3600;

		} else if (tok[0] == '$' && tok[1] == '{' && tok[strlen(tok)-1] == '}') {
			rpn = rpn_create();
			rpn->run = rpn_do_env;

			rpn->topic = strndup(tok+2, strlen(tok+2)-1);
			/* seperate options */
			rpn->options = strrchr(rpn->topic, ',');
			if (rpn->options)
				*(rpn->options)++ = 0;
		} else if ((lookup = do_lookup(tok)) != NULL) {
			rpn = rpn_create();
			rpn->run = lookup->run;

		} else {
			mylog(LOG_INFO, "unknown token '%s'", tok);
			if (root)
				rpn_free_chain(root);
			root = NULL;
			break;
		}
		rpn->dat = dat;
		if (last)
			last->next = rpn;
		if (!root)
			root = rpn;
		last = rpn;
	}
	free(savedstr);
	return root;
}
