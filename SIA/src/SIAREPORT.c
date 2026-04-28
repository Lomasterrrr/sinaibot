#include "../include/sia.h"
#include "reports.h"

static inline bool
parserange(const char *range, time_t *x, time_t *y)
{
	char rangebuf[SIAMAXRANGELEN] = { 0 }, *p;
	struct tm tmx = { 0 }, tmy = { 0 };
	time_t t = time(NULL);
	size_t n = 0;

	assert(range && x && y);

	while (*range) {
		if (n == sizeof(rangebuf) - 1)
			break;
		if (!isspace((uint8_t)*range))
			rangebuf[n++] = *range;
		++range;
	}

	/* Возможно это просто словосочитание.  */
	range = rangebuf;
	if (!strcmp(range, "alltime")) {
		*x = 0;
		*y = t;
		return 1;
	} else if (!strncmp(range, "last", 4)) {
		if (strlen(range) < 5)
			return 0;
		*y = t;
		range += 4;
		if (!strcmp(range, "hour"))
			*x = t - 3600;
		else if (!strcmp(range, "3hours"))
			*x = t - (3600 * 3);
		else if (!strcmp(range, "day"))
			*x = t - 86400;
		else if (!strcmp(range, "3days"))
			*x = t - (86400 * 3);
		else if (!strcmp(range, "week"))
			*x = t - 604800;
		else if (!strcmp(range, "month"))
			*x = t - 2419200;
		else if (!strcmp(range, "year"))
			*x = t - 31536000;
		else
			return 0;
		return 1;
	}

	/* В ином случае ожидаем диапазон.  */
	if (!(p = strchr(range, ',')))
		if (!(p = strchr(range, ';')))
			return 0;
	*p++ = 0;
	if (!strcmp(p, "now")) {
		*y = t;
	} else {
		if (!strptime(p, "%Y-%m-%d%H:%M", &tmy))
			if (!strptime(p, "%Y-%m-%d", &tmy))
				return 0;
		tmy.tm_isdst = -1;
		if ((*y = mktime(&tmy)) == -1)
			return 0;
	}

	if (!strptime(range, "%Y-%m-%d%H:%M", &tmx))
		if (!strptime(range, "%Y-%m-%d", &tmx))
			return 0;
	tmx.tm_isdst = -1;
	if ((*x = mktime(&tmx)) == -1)
		return 0;

	if (*x > *y)
		return 0;

	return 1;
}

static inline char *
joinargs(char **args, size_t num)
{
	size_t len = 0, i;
	for (i = 0; i < num; i++)
		len += strlen(args[i]);
	char *res = malloc(len + 1), *p = res;
	if (res)
		for (i = 0; i < num; i++)
			p += sprintf(p, "%s", args[i]);
	return res;
}

bool
SIAREPORT(SIA *sia, FILE *out, const char *type, char **args, size_t numargs)
{
	if (!sia || !sia->session || !type)
		return 0;

	if (!strcmp(type, "tags")) {
		long long int userid;
		time_t x, y;
		char *str;
		size_t n;

		if (!args[0] || !args[1] || !args[2])
			return 0;

		errno = 0;
		userid = strtoll(args[0], &str, 10);
		if (errno != 0 || *str)
			return 0;
		n = strtoul(args[1], &str, 10);
		if (errno != 0 || *str)
			return 0;

		if (!(str = joinargs(args + 2, numargs - 2)))
			return 0;
		if (!parserange(str, &x, &y)) {
			free(str);
			return 0;
		}
		free(str);

		tags(sia, n, userid, x, y, out);
		return 1;
	} else if (!strcmp(type, "finduser")) {
		if (!*args)
			return 0;

		finduser(sia, *args, out);
		return 1;
	} else if (!strcmp(type, "toptag")) {
		time_t x, y;
		char *str;
		size_t n;

		if (!args[0] || !args[1] || !args[2])
			return 0;

		n = strtoul(args[1], &str, 10);
		if (errno != 0 || *str)
			return 0;

		if (!(str = joinargs(args + 2, numargs - 2)))
			return 0;
		if (!parserange(str, &x, &y)) {
			free(str);
			return 0;
		}
		free(str);

		toptag(sia, args[0], n, x, y, out);
		return 1;
	}

	return 0;
}
