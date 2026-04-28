#include "reports.h"

/* Статистика для категории пользователя.  */
typedef struct {
	/* Имя категории.  */
	char category[SIAMAXCATEGORYLEN];

	/* Итоговая оценка.  */
	double relevance;

	/* Количество сообщений, в которых
	 * встретилась <category>.  */
	size_t n;

	/* Общий вес <category.  */
	double weight;

	size_t lastmsg;
} userstat;

/* Контекст для передачи в usercallback. */
typedef struct {
	SIA *sia;
	cvector(userstat) ustats;
	double *utotweight;
	size_t currmsg;
} usercallbackctx;

/* Хэш таблица, которая хранит глобальную
 * статистику категории; ключ в ней это
 * <category>.  */
typedef struct {
	/* Имя категории (и ключ).  */
	char category[SIAMAXCATEGORYLEN];

	/* Доля веса <category> относительно
	 * всех других в процентах.  */
	double share;

	/* Количество сообщений, в которых
	 * встретилась <category>.  */
	size_t n;

	size_t lastmsg;
	UT_hash_handle hh;
} globalstat;

/* Контекст для передачи в globalcallback. */
typedef struct {
	SIA *sia;
	globalstat **gstats;
	size_t currmsg;
} globalcallbackctx;

/* Главная струтура для всего отчета. Хранит
 * всю статистику, и другое. */
typedef struct {
	/* Статистика каждой категории у <userid>;
	 * главное поле.  */
	cvector(userstat) ustats;

	/* Глобальная статистика каждой категории.  */
	globalstat *gstats;

	/* Общее количество сообщений.  */
	size_t gtotmsgs;

	/* Общее количество сообщений <userid>.  */
	size_t utotmsgs;

	/* Общий вес вообще всех категорий у пользователя.  */
	double utotweight;

	SIA *sia;
} report;

/* Статистика одного пользователя для определенной
 * категории для второго отчета.  */
typedef struct {
	/* ID пользователя (и ключ).  */
	long long int id;

	/* Итоговая оценка.  */
	double relevance;

	/* Вес категории.  */
	double weight;

	/* Количество сообщений, в которых
	 * встретилась категория.  */
	size_t n;

	/* Общее количество сообщений
	 * пользователя.  */
	size_t totmsgs;

	/* Общий вес всех категорий
	 * у пользователя. */
	double totweight;

	size_t lastmsg;
	UT_hash_handle hh;
} usertopstat;

/* Главная струтура для второго отчета. Хранит
 * всю статистику, и другое. */
typedef struct {
	/* Имя категории. */
	char category[SIAMAXCATEGORYLEN];

	/* Глобальная статистика каждой категории.  */
	globalstat *gstats;

	/* Общее количество сообщений.  */
	size_t gtotmsgs;

	/* Временная статистика каждого пользователя
	 * для <category>.  */
	usertopstat *utopstats;

	/* Итоговая статистика каждого пользователя
	 * для <category>. Главное поле.  */
	cvector(usertopstat *) utopstatsvalid;

	long long int curruserid;
	size_t currmsg;
	SIA *sia;
} report1;

static inline bool
reportfree(report *r)
{
	if (r) {
		if (r->gstats) {
			globalstat *c, *tmp;
			HASH_ITER(hh, r->gstats, c, tmp)
			{
				HASH_DEL(r->gstats, c);
				free(c);
			}
		}
		if (r->ustats)
			cvector_free(r->ustats);
	}
	return 0;
}

static inline bool
report1free(report1 *r1)
{
	if (r1) {
		if (r1->gstats) {
			globalstat *c, *tmp;
			HASH_ITER(hh, r1->gstats, c, tmp)
			{
				HASH_DEL(r1->gstats, c);
				free(c);
			}
		}
		if (r1->utopstats) {
			usertopstat *c, *tmp;
			HASH_ITER(hh, r1->utopstats, c, tmp)
			{
				HASH_DEL(r1->utopstats, c);
				free(c);
			}
		}
		if (r1->utopstatsvalid)
			cvector_free(r1->utopstatsvalid);
	}
	return 0;
}

static inline void
textproc(SIA *sia, const char *raw,
    void (*callback)(void *arg, struct aho_match_t *), void *arg)
{
	utf8proc_uint8_t *norm;
	utf8proc_map((const utf8proc_uint8_t *)raw, 0, &norm,
	    UTF8PROC_NULLTERM | UTF8PROC_CASEFOLD);
	if (norm) {
		aho_register_match_callback(&sia->database.aho, callback, arg);
		aho_findtext(&sia->database.aho, (char *)norm,
		    strlen((char *)norm));
		free(norm);
	}
}

static inline bool
msgsproc(SIA *sia, long long int userid, time_t x, time_t y, size_t *totmsgs,
    void *callback, void *arg, size_t *currmsg)
{
	struct json_object *j, *m, *t;
	sqlite3_stmt *stmt;
	const char *json;

	if (!(stmt = dbreadopen(sia, userid, x, y)))
		return 0;
	while ((json = dbreadnext(stmt))) {
		if (!(j = json_tokener_parse(json)))
			continue;
		if (json_object_object_get_ex(j, "message", &m) &&
		    json_object_object_get_ex(m, "text", &t) &&
		    json_object_is_type(t, json_type_string)) {
			++(*totmsgs);
			*currmsg = *totmsgs;
			textproc(sia, json_object_get_string(t), callback, arg);
		}
		json_object_put(j);
	}

	dbreadclose(stmt);
	return 1;
}

static inline void
globalcallback(void *arg, struct aho_match_t *match)
{
	globalcallbackctx *ctx = (globalcallbackctx *)arg;
	SIA *sia = ctx->sia;

	if (match->id >= 0 && sia->database.idmap[match->id]) {
		dictitem *item = sia->database.idmap[match->id];
		globalstat *stat = NULL;
		HASH_FIND_STR(*ctx->gstats, item->category, stat);

		if (stat)
			stat->share += item->weight;
		else if ((stat = calloc(1, sizeof(globalstat)))) {
			strlcpy(stat->category, item->category,
			    sizeof(stat->category));
			stat->share = item->weight;
			HASH_ADD_STR(*ctx->gstats, category, stat);
		}

		if (stat && stat->lastmsg != ctx->currmsg) {
			/* Нашли новое сообщение, в котором
			 * встретилась <category>.  */
			++stat->n;
			stat->lastmsg = ctx->currmsg;
		}
	}
}

static inline void
usercallback(void *arg, struct aho_match_t *match)
{
	usercallbackctx *ctx = (usercallbackctx *)arg;
	SIA *sia = ctx->sia;

	if (match->id >= 0 && sia->database.idmap[match->id]) {
		dictitem *item = sia->database.idmap[match->id];

		*ctx->utotweight += item->weight;
		// printf("%s (%s)\n", item->word, item->category);

		/* Проходимся по всему вектору <ustats>и ищем нашу категорию,
		 * если найдена, увеличиваем вес.  */
		for (size_t n = 0; n < cvector_size(ctx->ustats); ++n) {
			if (!strcmp(item->category, ctx->ustats[n].category)) {
				/* Увеличиваем вес.  */
				ctx->ustats[n].weight += item->weight;

				if (ctx->ustats[n].lastmsg != ctx->currmsg) {
					/* Нашли новое сообщение, в котором
					 * встретилась <category>.  */
					++ctx->ustats[n].n;
					ctx->ustats[n].lastmsg = ctx->currmsg;
				}
				return;
			}
		}

		/* Если мы тут, значит категории, которую мы нашли, еще нет
		 * в <ustats>, так что добавляем ее туда.  */
		userstat stats = { 0 };
		stats.n = 1; /* Одно сообщение уже нашли.  */

		stats.weight = item->weight;
		stats.lastmsg = ctx->currmsg;
		strlcpy(stats.category, item->category, sizeof(stats.category));

		/* Добавляем нашу статистику в вектор.  */
		cvector_push_back(ctx->ustats, stats);
	}
}

static inline void
gstatscalc(globalstat *gstats)
{
	/* К этому моменту <share> хранит общий вес
	 * категории.  Теперь складывая все <share>
	 * мы находим общий вес всех категорий.  Далее,
	 * каждый <share> мы делим на этот вес, и
	 * умножаем на 100, получая проценты.  */
	double gtotweight = 0.0;
	globalstat *c, *tmp;

	HASH_ITER(hh, gstats, c, tmp)
	{
		gtotweight += c->share;
	}
	if (gtotweight > 0.0) {
		HASH_ITER(hh, gstats, c, tmp)
		{
			c->share = (c->share / gtotweight) * 100.0;
		}
	}
}

static inline double
relevancecalc(double utotweight, double uweight, size_t utotmsgs, size_t un,
    double gshare, size_t gtotmsgs, size_t gn, double confidence)
{
	/* Делим вес категории у пользователя на
	 * вес всех категорий у пользователя.  */
	double share = (utotweight > 0.0) ? (uweight / utotweight) * 100.0 :
					    0.0;

	/* Делим количество сообщений в которых
	 * встречается категория у пользователя,
	 * на количество всех сообщений пользователя.  */
	double coverage = (utotmsgs > 0) ? ((double)un / utotmsgs) * 100.0 :
					   0.0;

	/* Делим вес категории пользоватля относительно
	 * всех его категорий, на вес категории вообще
	 * относительно всех категорий.  */
#define SMOOTHINGEPSILON 0.1
	double lift = share / (gshare + SMOOTHINGEPSILON);

	/* Расчитываем TF-IDF.  */
	double tf = log10(1.0 + uweight);
	double idf = log10((double)(gtotmsgs > 0 ? gtotmsgs : 1) / (double)gn);
	double tfidf = tf * idf;

	/* F1-score; оценка истинности интереса
	 * пользователя к категории.  */
	double local = 0.0;
	if (share + coverage > 0.0)
		local = 2.0 * (share * coverage) / (share + coverage);

	/* Расчет итоговой оценки.  */
	return local * log2(1.5 + lift) * (1.0 + tfidf) * confidence;
}

static inline void
gstatsget(const char *category, globalstat *gstats, double *share, size_t *n)
{
	*share = 0.0;
	*n = 1;

	/* Ищем глобальную статистику для нужной
	 * категории.  */
	globalstat *gs = NULL;
	HASH_FIND_STR(gstats, category, gs);
	if (gs) {
		*share = gs->share;
		*n = gs->n > 0 ? gs->n : 1;
	}
}

static inline double
confidencecalc(size_t utotmsgs)
{
	/* Если у нас меньше 100 сообщений, то уверенность
	 * в результате relevance падает.  */
#define SIAMINMSGSFORCONFIDENCE 100.0
	if (utotmsgs > 0 && utotmsgs < SIAMINMSGSFORCONFIDENCE)
		return (double)utotmsgs / SIAMINMSGSFORCONFIDENCE;
	else if (utotmsgs == 0)
		return 0.0;
	return 1.0;
}

static inline void
reportcalc(report *r)
{
	double confidence = confidencecalc(r->utotmsgs);
	gstatscalc(r->gstats);

	/* Начинаем расчитывать статистику для каждой
	 * категории у пользователя.  */
	for (size_t n = 0; n < cvector_size(r->ustats); ++n) {
		userstat *s = &r->ustats[n];

		double gshare;
		size_t gn;
		gstatsget(s->category, r->gstats, &gshare, &gn);

		s->relevance = relevancecalc(r->utotweight, s->weight,
		    r->utotmsgs, s->n, gshare, r->gtotmsgs, gn, confidence);
	}
}

static inline int
cmprelevance(const void *a, const void *b)
{
	const userstat *sa = (const userstat *)a;
	const userstat *sb = (const userstat *)b;

	if (sa->relevance > sb->relevance)
		return -1;
	if (sa->relevance < sb->relevance)
		return 1;

	return 0;
}

bool
tags(SIA *sia, size_t n, long long int userid, time_t x, time_t y, FILE *out)
{
	assert(sia);
	assert(sia->session);
	assert(out);

	report r = { 0 }; /* Сам наш report.  */
	REPORTINIT(sia, userid, x, y, out);
	r.sia = sia; /* Наш указатель на SIA.  */

	/* Нам понадобиться следующее: (a) общее число сообщений
	 * вообще, для каждой категории: (b) кол. сообщений в
	 * которых она встретилась вообще, и (c) ее вес. */
	globalcallbackctx gctx = { .sia = sia,
		.gstats = &r.gstats,
		.currmsg = 0 };
	if (!msgsproc(r.sia, 0, x, y, &r.gtotmsgs, globalcallback, &gctx,
		&gctx.currmsg))
		return reportfree(&r);

	/* Далее, нам понадобиться: (a) общее число сообщений
	 * <userid>, и для каждой категории <userid>: (c) ее
	 * вес, и (d) кол. сообщений, в которых она встретилась <n>. */
	usercallbackctx uctx = { .sia = sia,
		.ustats = NULL,
		.currmsg = 0,
		.utotweight = &r.utotweight };
	if (!msgsproc(r.sia, userid, x, y, &r.utotmsgs, usercallback, &uctx,
		&uctx.currmsg))
		return reportfree(&r);
	r.ustats = uctx.ustats;

	/* Теперь расситываем это все.  */
	reportcalc(&r);

	/* Выводим нашу статистику после расчета.  */
	if (!cvector_empty(r.ustats)) {
		qsort(r.ustats, cvector_size(r.ustats), sizeof(userstat),
		    cmprelevance);
		for (size_t i = 0; i < cvector_size(r.ustats) && n--; ++i) {
			userstat *s = &r.ustats[i];
			fprintf(out, "___%zu.___ *%s —* %0.1f points\n", i + 1,
			    s->category, s->relevance);
		}
		fputc('\n', out);
	}
	fprintf(out, "*Total messages:* %zu\n", r.utotmsgs);

	reportfree(&r);
	return 1;
}

static inline void
topuserscallback(void *arg, struct aho_match_t *match)
{
	report1 *r = (report1 *)arg;
	SIA *sia = r->sia;

	if (match->id >= 0 && sia->database.idmap[match->id]) {
		dictitem *item = sia->database.idmap[match->id];

		/* Если пользователя с таким id еще нет в хэш
		 * таблице, то добавляем его туда.  */
		usertopstat *ustat = NULL;
		HASH_FIND(hh, r->utopstats, &r->curruserid,
		    sizeof(r->curruserid), ustat);
		if (!ustat)
			return;

		/* Общий вес всех категорий.  */
		ustat->totweight += item->weight;

		if (strcmp(item->category, r->category) == 0) {
			/* Увеличиваем вес.  */
			ustat->weight += item->weight;

			printf("%s (%s)\n", item->word, item->category);

			if (ustat->lastmsg != r->currmsg) {
				/* Нашли новое сообщение, в котором
				 * встретилась <category>.  */
				ustat->n++;
				ustat->lastmsg = r->currmsg;
			}
		}
	}
}

static inline bool
msgsproctop(report1 *r, time_t x, time_t y, void *callback)
{
	struct json_object *j, *m, *t, *f, *i;
	sqlite3_stmt *stmt;
	const char *json;

	if (!(stmt = dbreadopen(r->sia, 0, x, y)))
		return 0;
	while ((json = dbreadnext(stmt))) {
		if (!(j = json_tokener_parse(json)))
			continue;
		if (json_object_object_get_ex(j, "message", &m) &&
		    json_object_object_get_ex(m, "text", &t) &&
		    json_object_is_type(t, json_type_string) &&
		    json_object_object_get_ex(m, "from", &f) &&
		    json_object_object_get_ex(f, "id", &i) &&
		    json_object_is_type(i, json_type_int)) {
			long long int id = json_object_get_int64(i);
			usertopstat *ustat = NULL;

			/* Если пользователя с таким id еще нет в хэш
			 * таблице, то добавляем его туда.  */
			HASH_FIND(hh, r->utopstats, &id, sizeof(id), ustat);
			if (!ustat) {
				if ((ustat = calloc(1, sizeof(usertopstat)))) {
					ustat->id = id;
					HASH_ADD(hh, r->utopstats, id,
					    sizeof(id), ustat);
				}
			}

			++ustat->totmsgs;
			r->curruserid = id;
			r->currmsg = ustat->totmsgs;

			textproc(r->sia, json_object_get_string(t), callback,
			    r);
		}
		json_object_put(j);
	}

	dbreadclose(stmt);
	return 1;
}

static inline void
report1calc(report1 *r1)
{
	gstatscalc(r1->gstats);

	double gshare;
	size_t gn;

	gstatsget(r1->category, r1->gstats, &gshare, &gn);

	/* Начинаем расчитывать статистику для каждого
	 * пользователя.  */
	usertopstat *u, *tmp;
	HASH_ITER(hh, r1->utopstats, u, tmp)
	{
		if (u->weight <= 0.0)
			continue;

		u->relevance = relevancecalc(u->totweight, u->weight,
		    u->totmsgs, u->n, gshare, r1->gtotmsgs, gn,
		    confidencecalc(u->totmsgs));

		cvector_push_back(r1->utopstatsvalid, u);
	}
}

static inline int
cmpusertopstat(const void *a, const void *b)
{
	const usertopstat *sa = *(const usertopstat **)a;
	const usertopstat *sb = *(const usertopstat **)b;

	if (sa->relevance > sb->relevance)
		return -1;
	if (sa->relevance < sb->relevance)
		return 1;

	return 0;
}

bool
toptag(SIA *sia, const char *category, size_t n, time_t x, time_t y, FILE *out)
{
	assert(sia);
	assert(sia->session);
	assert(out);
	assert(category);

	report1 r1 = { 0 }; /* Сам наш report.  */
	REPORTINIT(sia, 0LL, x, y, out);
	r1.sia = sia; /* Наш указатель на SIA.  */

	/* Наша категория для топа.  */
	strlcpy(r1.category, category, sizeof(r1.category));

	/* Получаем все то же самое как и выше.  */
	globalcallbackctx gctx = { .sia = sia,
		.gstats = &r1.gstats,
		.currmsg = 0 };
	if (!msgsproc(r1.sia, 0, x, y, &r1.gtotmsgs, globalcallback, &gctx,
		&gctx.currmsg))
		return report1free(&r1);

	/* Получаем все то же самое как и выше, только для
	 * определенной категории.  */
	if (!msgsproctop(&r1, x, y, topuserscallback))
		return report1free(&r1);

	/* Теперь расситываем это все.  */
	report1calc(&r1);

	/* Выводим нашу статистику после расчета.  */
	if (!cvector_empty(r1.utopstatsvalid)) {
		qsort(r1.utopstatsvalid, cvector_size(r1.utopstatsvalid),
		    sizeof(usertopstat *), cmpusertopstat);
		for (size_t i = 0; i < cvector_size(r1.utopstatsvalid) && n--;
		    ++i)
			fprintf(out,
			    "___%zu.___ ID: `%lld` (%s) —* %0.1f points*\n",
			    i + 1, r1.utopstatsvalid[i]->id,
			    dbgetnamebyid(sia, r1.utopstatsvalid[i]->id),
			    r1.utopstatsvalid[i]->relevance);
		fprintf(out, "\n*Top users in category:* %s\n", r1.category);
	}
	fprintf(out, "*Total messages:* %zu\n", r1.gtotmsgs);

	report1free(&r1);
	return 1;
}
