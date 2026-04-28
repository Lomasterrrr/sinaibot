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

const char *
dbgetnamebyid(SIA *sia, long long int userid)
{
	struct json_object *j, *m, *f, *n;
	static char res[512];
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
		    json_object_object_get_ex(m, "from", &f)) {
			if (!(json_object_object_get_ex(f, "first_name", &n)))
				if (!(json_object_object_get_ex(f, "username",
					&n)))
					if (!(json_object_object_get_ex(f,
						"last_name", &n)))
						goto exit;
			if (json_object_get_type(n) != json_type_string)
				goto exit;
			strlcpy(res, json_object_get_string(n), sizeof(res));
		}
		json_object_put(j);
	}
	sqlite3_finalize(stmt);

	char *cp = res;
	while (*cp) {
		if (*cp == '_')
			*cp = ' ';
		++cp;
	}

	return res;

exit:
	if (j)
		json_object_put(j);
	sqlite3_finalize(stmt);
	return NULL;
}
