#include "../include/sia.h"

static inline void
additem(SIA *sia, double weight, const char *txt, const char *category)
{
	assert(sia && txt);
	assert(category);

	dictitem *i;
	HASH_FIND_STR(sia->database.dict, txt, i);
	if (!i) {
		if (!(i = malloc(sizeof(dictitem))))
			return;

		/* Добавляем в нашу хэштаблицу.  */
		i->weight = weight;
		strlcpy(i->word, txt, sizeof(i->word));
		strlcpy(i->category, category, sizeof(i->category));
		HASH_ADD_STR(sia->database.dict, word, i);

		/* Добавляем также в aho.  */
		i->ahoid = aho_add_match_text(&sia->database.aho, i->word,
		    strlen(i->word));
	}
}

static inline void
databasefileload(SIA *sia, const char *database, struct json_object *item)
{
	const char *file, *category;
	struct json_object *f, *c;
	char buf[SIAMAXLINELEN];
	double weight;
	FILE *fp;

	assert(sia);
	assert(database);
	assert(item);

	/* Получаем для поля информации о файле.  */
	if (!json_object_object_get_ex(item, "file", &f))
		return;
	if (!(json_object_object_get_ex(item, "category", &c)))
		return;
	if (!(file = json_object_get_string(f)))
		return;
	if (!(category = json_object_get_string(c)))
		return;
	if (!file || !category)
		return;

	/* Формируем путь к файлу.  */
	snprintf(buf, sizeof(buf), "%s/%s", database, file);

	/* Пербираем файл построчно и добавляем.  */
	if (!(fp = fopen(buf, "r")))
		return;
	while (fgets(buf, sizeof(buf), fp)) {
		buf[strcspn(buf, "\r\n")] = 0;
		if (!*buf)
			continue;

		/* Последнее слово в строке это вес.  */
		weight = 0.0;
		char *last = strrchr(buf, ' ');
		if (last) {
			*last++ = 0;
			weight = atof(last);
		}

		/* Добавляем строчку из файла.  */
		additem(sia, weight, buf, category);
	}

	fclose(fp);
}

static inline bool
getdatabaseinfo(struct json_object **json, const char *database)
{
	char *filestr;
	size_t n;
	FILE *fp;

	assert(json);
	assert(database);

	/* Информация о базе данных должна лежать в файле
	 * database.json,который находится в папке базы
	 * данных.  */
	asprintf(&filestr, "%s/database.json", database);
	if (!(fp = fopen(filestr, "rb")))
		return 0;
	free(filestr);

	/* Получаем весь файл в строчку.  */
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	rewind(fp);
	if (!(filestr = (char *)malloc(n + 1))) {
		fclose(fp);
		return 0;
	}
	n = fread(filestr, 1, n, fp); /* Читаем.  */
	filestr[n] = 0;
	fclose(fp);

	/* Парсим json и проверяем на то, является
	 * ли он массивом файлов.  */
	*json = json_tokener_parse(filestr);
	free(filestr);
	if (!*json)
		return 0;
	if (json_object_get_type(*json) != json_type_array) {
		json_object_put(*json);
		return 0;
	}

	return 1;
}

static inline bool
databaseload(SIA *sia, const char *database)
{
	struct json_object *json, *item;
	size_t n;

	assert(sia);
	assert(database);

	aho_init(&sia->database.aho);

	/* Получаем json из database/database.json */
	if (!getdatabaseinfo(&json, database))
		return 0;
	n = json_object_array_length(json);
	while (n--) {
		if (!(item = json_object_array_get_idx(json, n)))
			continue;

		/* Загружаем файл из базы данных.  */
		databasefileload(sia, database, item);
	}
	json_object_put(json);

	/* Иницилизируем все что нужно для поиска.  */
	aho_create_trie(&sia->database.aho);
	n = HASH_COUNT(sia->database.dict) + 10;
	if (!(sia->database.idmap = calloc(n, sizeof(dictitem *))))
		return 0;

	dictitem *c, *tmp;
	HASH_ITER(hh, sia->database.dict, c, tmp)
	{
		if (c->ahoid > 0)
			sia->database.idmap[c->ahoid] = c;
	}

	return 1;
}

SIA *
SIAOPEN(const char *filename, const char *database)
{
	SIA *sia;

	if (!(sia = calloc(1, sizeof(SIA))))
		return NULL;

	/* Загружаем базу данных для анализа.  */
	if (!databaseload(sia, database))
		return SIACLOSE(sia);

	/* Иницилизируем базу данных с логами.  */
	if (sqlite3_open(filename, &sia->session) != SQLITE_OK)
		return SIACLOSE(sia);
	if (sqlite3_exec(sia->session, "PRAGMA journal_mode=WAL;", 0, 0,
		NULL) != SQLITE_OK)
		return SIACLOSE(sia);
	if (sqlite3_exec(sia->session,
		"CREATE TABLE IF NOT EXISTS intel_logs("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"tstamp INTEGER, "
		"userid INTEGER, "
		"rawjson TEXT);"
		"CREATE INDEX IF NOT EXISTS idx_time ON intel_logs(tstamp);"
		"CREATE INDEX IF NOT EXISTS idx_user ON intel_logs(userid);",
		0, 0, NULL) != SQLITE_OK)
		return SIACLOSE(sia);

	return sia;
}
