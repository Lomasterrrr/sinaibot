#include "reports.h"

sqlite3_stmt *
dbreadopen(SIA *sia, long long int userid, time_t x, time_t y)
{
	if (!sia || !sia->session)
		return NULL;

	sqlite3_stmt *stmt;
	const char *sql;

	/* Ищем такие записи, tstamp которых входит в диапазон x, y,
	 * а если указан еще и userid, то ищем такие такие строчки,
	 * у которых еще и userid совпадает с указанным.  */
	if (userid == 0)
		sql = "SELECT rawjson FROM intel_logs "
		      "WHERE tstamp BETWEEN ? AND ? "
		      "ORDER BY tstamp ASC;";
	else
		sql = "SELECT rawjson FROM intel_logs "
		      "WHERE tstamp BETWEEN ? AND ? AND userid = ? "
		      "ORDER BY tstamp ASC;";

	if (sqlite3_prepare_v2(sia->session, sql, -1, &stmt, 0) != SQLITE_OK)
		return NULL;

	sqlite3_bind_int64(stmt, 1, x);
	sqlite3_bind_int64(stmt, 2, y);

	if (userid != 0)
		sqlite3_bind_int64(stmt, 3, userid);

	return stmt;
}

const char *
dbreadnext(sqlite3_stmt *stmt)
{
	if (!stmt)
		return NULL;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		return (const char *)sqlite3_column_text(stmt, 0);
	return NULL;
}

void
dbreadclose(sqlite3_stmt *stmt)
{
	if (stmt)
		sqlite3_finalize(stmt);
}

void
getnamefromjson(struct json_object *from, char *buf, size_t buflen)
{
	/* Любой ценой получаем имя.  */
	struct json_object *n1, *n2, *n3;
	char *cp = buf;

	*buf = 0;
	if (!json_object_object_get_ex(from, "first_name", &n1) ||
	    !json_object_is_type(n1, json_type_string))
		n1 = NULL;
	if (!json_object_object_get_ex(from, "last_name", &n2) ||
	    !json_object_is_type(n2, json_type_string))
		n2 = NULL;
	if (!json_object_object_get_ex(from, "username", &n3) ||
	    !json_object_is_type(n3, json_type_string))
		n3 = NULL;

	if (n1 && n2)
		snprintf(buf, buflen, "%s %s", json_object_get_string(n1),
		    json_object_get_string(n2));
	else if (n1)
		strlcpy(buf, json_object_get_string(n1), buflen);
	else if (n2)
		strlcpy(buf, json_object_get_string(n2), buflen);
	else if (n3)
		strlcpy(buf, json_object_get_string(n3), buflen);

	while (*cp) {
		switch (*cp) {
		case '_':
		case '*':
		case '`':
		case '|':
			*cp = ' ';
			break;
		}
		++cp;
	}
}

const char *
dbgetnamebyid(SIA *sia, long long int userid)
{
	struct json_object *j, *m, *f, *n;
	static char res[SIAMAXNAMELEN];
	sqlite3_stmt *stmt;
	const char *json;

	if (sqlite3_prepare_v2(sia->session,
		"SELECT rawjson FROM intel_logs WHERE userid = ? LIMIT 1;", -1,
		&stmt, 0) != SQLITE_OK)
		return NULL;
	sqlite3_bind_int64(stmt, 1, userid);
	if (sqlite3_step(stmt) != SQLITE_ROW)
		goto exit;

	if (!(json = (const char *)sqlite3_column_text(stmt, 0)))
		goto exit;
	if ((j = json_tokener_parse(json))) {
		if (json_object_object_get_ex(j, "message", &m) &&
		    json_object_object_get_ex(m, "from", &f))
			getnamefromjson(f, res, sizeof(res));
		json_object_put(j);
	}
	sqlite3_finalize(stmt);

	return res;

exit:
	if (j)
		json_object_put(j);
	sqlite3_finalize(stmt);
	return NULL;
}
