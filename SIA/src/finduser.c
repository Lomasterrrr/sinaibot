#include "reports.h"

static inline bool
ismatch(struct json_object *j, const utf8proc_uint8_t *nname)
{
	utf8proc_uint8_t *norm;
	bool match = 0;
	if (!j || !nname)
		return 0;
	if (json_object_get_type(j) != json_type_string)
		return 0;
	utf8proc_map((const utf8proc_uint8_t *)json_object_get_string(j), 0,
	    &norm, UTF8PROC_NULLTERM | UTF8PROC_CASEFOLD);
	if (!norm)
		return 0;
	if (strstr((const char *)norm, (const char *)nname))
		match = 1;
	free(norm);
	return match;
}

static inline const char *
safejsonstr(struct json_object *j)
{
	static char tmp[1024];
	if (j && json_object_get_type(j) == json_type_string) {
		strlcpy(tmp, json_object_get_string(j), sizeof(tmp));
		char *p = tmp;
		while (*p) {
			if (*p == '_')
				*p = '-';
			++p;
		}
		return tmp;
	}
	return "—";
}

void
finduser(SIA *sia, const char *name, FILE *out)
{
	utf8proc_uint8_t *norm;
	sqlite3_stmt *stmt;
	const char *json;
	size_t k;

	typedef struct {
		long long int id;
		char firstname[512];
		char lastname[512];
		char username[512];
	} user;
	cvector(user) ids = NULL;

	assert(name);
	assert(out);
	assert(sia);

	if (*name == '@')
		++name;

	utf8proc_map((const utf8proc_uint8_t *)name, 0, &norm,
	    UTF8PROC_NULLTERM | UTF8PROC_CASEFOLD);
	if (!norm)
		return;

	if (!(stmt = dbreadopen(sia, 0, 0, time(NULL)))) {
		free(norm);
		return;
	}
	while ((json = dbreadnext(stmt))) {
		struct json_object *j, *m, *f, *i, *p[3] = { 0 };
		if (!(j = json_tokener_parse(json)))
			continue;

		if (!json_object_object_get_ex(j, "message", &m) ||
		    !json_object_object_get_ex(m, "from", &f) ||
		    !json_object_object_get_ex(f, "id", &i) ||
		    json_object_get_type(i) != json_type_int)
			goto next;

		int64_t id = json_object_get_int64(i);
		for (k = 0; k < cvector_size(ids); ++k)
			if (id == ids[k].id)
				goto next;

		json_object_object_get_ex(f, "first_name", &p[0]);
		json_object_object_get_ex(f, "last_name", &p[1]);
		json_object_object_get_ex(f, "username", &p[2]);

		for (k = 0; k < sizeof(p) / sizeof(*p); ++k) {
			if (ismatch(p[k], norm)) {
				user u = { .id = id };
				strlcpy(u.firstname, safejsonstr(p[0]),
				    sizeof(u.firstname));
				strlcpy(u.lastname, safejsonstr(p[1]),
				    sizeof(u.lastname));
				strlcpy(u.username, safejsonstr(p[2]),
				    sizeof(u.username));
				cvector_push_back(ids, u);
				goto next;
			}
		}

	next:
		json_object_put(j);
	}

	free(norm);
	dbreadclose(stmt);

	if (ids) {
		for (k = 0; k < cvector_size(ids); ++k)
			fprintf(out,
			    "___%zu.___ *ID:* `%lld` (___%s %s %s___)\n", k + 1,
			    ids[k].id, ids[k].firstname, ids[k].lastname,
			    ids[k].username);
		cvector_free(ids);
		return;
	}

	fprintf(out, "*Not found ID for \"%s\"*\n", name);
}
