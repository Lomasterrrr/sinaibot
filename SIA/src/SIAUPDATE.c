#include "../include/sia.h"

bool
SIAUPDATE(SIA *sia, const char *json)
{
	struct json_object *j, *m, *d, *f, *i, *t;
	sqlite3_stmt *stmt;
	time_t tstamp = time(NULL);
	long long int userid = 0;
	bool ret = 0;

	if (!sia || !json || !sia->session)
		return 0;

	/* Получаем unixtime и id из сообщения, если они
	 * там не найдены, то id будет 0, а unixtime на
	 * момент записи в базу данных.  */
	if ((j = json_tokener_parse(json))) {
		if (json_object_object_get_ex(j, "message", &m)) {
			if (json_object_object_get_ex(m, "date", &d) &&
			    json_object_is_type(d, json_type_int))
				tstamp = json_object_get_int64(d);
			if (json_object_object_get_ex(m, "from", &f))
				if (json_object_object_get_ex(f, "id", &i) &&
				    json_object_is_type(i, json_type_int))
					userid = json_object_get_int64(i);
			if (json_object_object_get_ex(m, "text", &t) &&
			    json_object_is_type(t, json_type_string)) {
				if (strncmp(json_object_get_string(t), "/SIA",
					4) == 0) {
					json_object_put(j);
					return 1;
				}
			}
		}
		json_object_put(j);
	}

	/* Формируем запрос в sql.  */
	if (sqlite3_prepare_v2(sia->session,
		"INSERT INTO intel_logs (tstamp, userid, rawjson) VALUES (?, ?, ?);",
		-1, &stmt, 0) != SQLITE_OK)
		return 0;

	if (sqlite3_bind_int64(stmt, 1, tstamp) != SQLITE_OK)
		goto exit;
	if (sqlite3_bind_int64(stmt, 2, userid) != SQLITE_OK)
		goto exit;
	if (sqlite3_bind_text(stmt, 3, json, -1, SQLITE_STATIC) != SQLITE_OK)
		goto exit;

	if (sqlite3_step(stmt) != SQLITE_DONE)
		goto exit;
	ret = 1;

exit:
	sqlite3_finalize(stmt);
	return ret;
}
