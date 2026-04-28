#ifndef REPORTS_H
#define REPORTS_H

#include "../include/sia.h"

#define REPORTINIT(sia, userid, x, y, out)                               \
	do {                                                             \
		fputs("*— Sinai Intelligence Agency*\n", out);           \
		fprintf(out, "*Report type:* ___%s___\n", __FUNCTION__); \
		fprintf(out, "*Date:* ___from %ld to %ld___\n", x, y);   \
		fprintf(out, "*UserID:* ");                              \
		if (userid != 0)                                         \
			fprintf(out, "`%lld` (%s)\n", userid,            \
			    dbgetnamebyid(sia, userid));                 \
		else                                                     \
			fprintf(out, "all users\n");                     \
		fputc('\n', out);                                        \
	} while (0);

bool tags(SIA *sia, size_t n, long long int userid, time_t x, time_t y,
    FILE *out);

bool finduser(SIA *sia, const char *name, FILE *out);

bool toptag(SIA *sia, const char *category, size_t n, time_t x, time_t y,
    FILE *out);

/* Интерфейс для удобного построчного чтения sql базы данных.  */
sqlite3_stmt *dbreadopen(SIA *sia, long long int userid, time_t x, time_t y);
const char *dbreadnext(sqlite3_stmt *stmt);
const char *dbgetnamebyid(SIA *sia, long long int userid);
void dbreadclose(sqlite3_stmt *stmt);

#endif
