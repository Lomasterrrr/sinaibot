/*
 * Copyright (c) 2025, lomaster. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/random.h>
#include <sys/time.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <telebot.h>
#include <time.h>
#include <uchar.h>
#include <unistd.h>

#include "SIA/include/sia.h"
#include "config.h"
#include "cvector.h"
#include "log.h"
#include "telebot-methods.h"

#define MAXLINELEN 1024
#define MAXMSGLEN  4096
#define MAXNAMELEN 256
#define MAXDATELEN 128
#define VOTELIMIT  5

static char username[MAXNAMELEN];
static telebot_handler_t handle;
static telebot_update_t *updates;
static int numupdates;
static long long int chatid;
static SIA *sia;

typedef struct __vote_t {
	/* Эти поля указываются из команды.  */
	char msg[MAXMSGLEN]; /* сообщение */
	uint64_t timel;	     /* двительность в сек */
	uint8_t type;	     /* тип голосования */

	char starter[MAXNAMELEN]; /* инициатор */
	uint64_t id;		  /* номер голосования */
	size_t ae;		  /* за */
	size_t no;		  /* против */
	bool adminflg;		  /* голосовал ли администратор? */
	uint8_t senatorsflg;	  /* могут голосовать только /senators? */
	time_t timestamp;	  /* точка старта */

	/* команды для за/против/остановка */
	char cmd_ae[128],  /* за */
	    cmd_no[128],   /* против */
	    cmd_stop[128]; /* стоп */

	/* голосовавшие за/против */
	cvector(long long int) users_ae; /* за */
	cvector(long long int) users_no; /* против */

	/* имена тех, кто голосовал за/против */
	cvector(char *) name_users_ae; /* за */
	cvector(char *) name_users_no; /* против */
} vote_t;

cvector(vote_t) votevec = NULL; /* голосования */

/*
 *		C U R T I M E S T R
 *
 * Получает дату и время, записывает ее в статический
 * буфер <date>, который возвращает. В случае ошибки
 * возвращает строку "".
 */
static inline const char *
curtimestr(time_t tstamp)
{
	time_t t = (tstamp) ? tstamp : time(NULL);
	static char date[MAXDATELEN];
	struct tm *tm;

	if (t == (time_t)-1)
		return "";

	if (!(tm = localtime(&t)))
		return "";

	strftime(date, sizeof(date), "%a %d %b %Y %T %Z", tm);
	return date;
}

/*
 *		W A I T M S
 *
 * Остнаваливает выполнение потока на указанное количество
 * милисекунд. Ну т. е. это sleep(), но который принимает не
 * секунды, а милисекунды.
 */
static inline void
waitms(long ms)
{
	struct timespec tv = { .tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000 };
	nanosleep(&tv, NULL);
}

/*
 *		B A P P E N D F
 *
 * Добавляет форматированный текст в буфер. Возвращает: 1 если все
 * записалось, 0 если произошло переполнение или ошибка.
 */
static inline int
bappendf(char *buf, size_t buflen, size_t *off, const char *fmt, ...)
{
	assert(buf);
	assert(off);
	assert(buflen);

	if (*off >= buflen)
		return 0;

	size_t rem = buflen - *off;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vsnprintf(buf + *off, rem, fmt, ap);
	va_end(ap);

	if (ret < 0 || (size_t)ret >= rem) {
		log_error("Failed write in buffer");
		return 0;
	}

	*off += ret;
	return 1;
}

/*
 *			U N U M A R G
 *
 * Функция для конвертации строки в size_t с указанным диапазоном и
 * с проверкой всех ошибок. <nptr> - строка; <buf> - адрес
 * переменной size_t куда будет записан результат; <min>/<max> -
 * диапазон, минимальное/максимальное. В случае ошибки вернет 0
 */
static bool
unumarg(const char *nptr, uint64_t min, uint64_t max, void *buf, size_t n)
{
	uint64_t val;
	char *endp;

	assert(nptr);
	assert(min < max);
	assert(buf);
	assert(n == sizeof(uint64_t) || n == sizeof(uint32_t) ||
	    n == sizeof(uint16_t) || n == sizeof(uint8_t));

	while (isspace((uint8_t)*nptr))
		nptr++;
	if (*nptr == '-')
		goto err;

	errno = 0;
	val = strtoull(nptr, &endp, 10);
	if (errno != 0 || endp == nptr)
		goto err;
	while (isspace((uint8_t)*endp))
		endp++;
	if (*endp)
		goto err;
	if (val < min || val > max)
		goto err;

	switch (n) {
	case sizeof(uint64_t):
		*(uint64_t *)buf = (uint64_t)val;
		break;
	case sizeof(u_int):
		*(uint32_t *)buf = (uint32_t)val;
		break;
	case sizeof(u_short):
		*(uint16_t *)buf = (uint16_t)val;
		break;
	case sizeof(uint8_t):
		*(uint8_t *)buf = (uint8_t)val;
		break;
	default:
		goto err;
	}

	return 1;
err:
	log_error("Failed convert %s (max = %ld, min = %ld, n = %ld)", nptr,
	    max, min, n);
	return 0;
}

/*
 *			B O T M S G
 *
 * Отправляет с бота указанное сообщение; поддерживает также
 * форматирование. <m> - суть разметка; <mid> - id сообщения.
 */
static inline const char *
strfrag(const char *str, size_t n)
{
	static char fragment[1024];

	assert(str);
	assert(n);

	if (n > sizeof(fragment))
		n = sizeof(fragment);

	strncpy(fragment, str, n - 1);
	fragment[n - 1] = 0;

	while (n--)
		if (isspace((uint8_t)fragment[n]))
			fragment[n] = ' ';

	return fragment;
}

static inline void
botmsg(telebot_handler_t handle, long long int chatid, int replyid,
    const char *fmt, ...)
{
	char message[MAXMSGLEN];
	va_list ap;

	assert(handle);
	assert(chatid);
	assert(fmt);

	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);

	if (telebot_send_message(handle, chatid, message, "Markdown", false,
		false, replyid, NULL) != TELEBOT_ERROR_NONE)
		if (telebot_send_message(handle, chatid, message, NULL, false,
			false, replyid, NULL) != TELEBOT_ERROR_NONE)
			return log_error("Failed send \"%s\"",
			    strfrag(message, 85));

	return log_debug("Sent to %lld: \"%s\"", chatid, strfrag(message, 85));
}

/*
 *			V O T E A D D
 *
 * Создает объект vote_t из указанных в аргументах значений.
 * возвращает -1 если ошибка и 0 если все хорошо. Также
 * созданных объект добавляет в вектор votevec. А еще
 * копирует его по адресу <tmp> (это чтобы потом вывести).
 */
static inline void
freestring(void *str)
{
	if (str)
		free(*(char **)str);
}

static inline bool
voteadd(const char *msg, const char *starter, const char *timel,
    const char *type, const char *flag, long long int chatid)
{
	vote_t v = { 0 };

	assert(msg && starter && timel && type && flag);

	if (!unumarg(timel, 0, UINT64_MAX, &v.timel, sizeof(v.timel)))
		return 0;

	v.senatorsflg = *flag;
	v.type = *type;
	v.id = ((({ /* master seed */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts),
		    (uint64_t)(ts.tv_sec * 1000000000L + ts.tv_nsec);
	})));
	v.timestamp = time(NULL); /* точка старта */
	cvector_init(v.name_users_ae, 1, freestring);
	cvector_init(v.name_users_no, 1, freestring);
	snprintf(v.cmd_ae, sizeof(v.cmd_ae), "/YES%ld", v.id);
	snprintf(v.cmd_no, sizeof(v.cmd_no), "/NO%ld", v.id);
	snprintf(v.cmd_stop, sizeof(v.cmd_stop), "/STOP%ld", v.id);
	snprintf(v.msg, sizeof(v.msg), "%s", msg);
	snprintf(v.starter, sizeof(v.starter), "%s", starter);

	cvector_push_back(votevec, v);

	botmsg(handle, chatid, 0,
	    "*ГОЛОСОВАНИЕ*\n%s\n"
	    "\n*Проголосовать за*:\n  %s;\n"
	    "*Проголосовать против*:\n  %s;\n\n"
	    "*Инициатор*: %s;\n"
	    "*Только почетные?*: %s;\n"
	    "*Тип голосования*: %c;\n"
	    "*Длительность*: %ld секунд(а);\n"
	    "*ID голосования*: `%ld`;\n"
	    "\n— ___%s___\n",
	    v.msg, v.cmd_ae, v.cmd_no, v.starter,
	    ((v.senatorsflg == '1') ? "да" : "нет"), v.type, v.timel, v.id,
	    curtimestr(v.timestamp));

	return 1;
}

/*
 *			V O T E D E L
 *
 * Удаляет один объект vote_t из вектора <votevec>, по его id (который
 * id голосования). Также выводит сообщение о окончании голосования.
 * Помимо этого, очищает память из под элементов которые этого требуют.
 */
static inline const char *
joinvec(char *buf, size_t buflen, cvector(char *) vec)
{
	assert(buf);
	assert(buflen);
	assert(vec);

	/*
	 * Соединяет вектор (char *) в строку,
	 * которую запиисывает в <buf>,
	 * на длинну <buflen>. Соединяет через
	 * ';'; если элементов нету
	 * то просто запишет в <buf>: "никто;".
	 */
	if (cvector_empty(vec)) {
		snprintf(buf, buflen, "никто;");
		return buf;
	}

	size_t off = 0, n;
	for (n = 0; n < cvector_size(vec); ++n)
		if (!bappendf(buf, buflen, &off, "%s; ", vec[n]))
			break;

	return buf;
}

static inline void
voteendmsg(vote_t *v, telebot_handler_t handle, long long int chatid)
{
	char yes[512];
	char no[512];

	assert(v);
	assert(handle);
	assert(chatid);

	joinvec(yes, sizeof(yes), v->name_users_ae);
	joinvec(no, sizeof(no), v->name_users_no);

	botmsg(handle, chatid, 0,
	    "*ОКОНЧАНИЕ ГОЛОСОВАНИЯ —* `%ld` ___%s___!\n\n"
	    "*Инициатор*: %s;\n"
	    "*Только почетные?*: %s;\n"
	    "*Голосовал ли админ?*: %s;\n"
	    "*Были за* — %s\n"
	    "*Были против* — %s\n"
	    "\n*Результаты (за/против)*\n — ___%ld / %ld___",
	    v->id, curtimestr(0), v->starter,
	    ((v->senatorsflg == '1') ? "да" : "нет"),
	    ((v->adminflg) ? "да" : "нет"), yes, no, v->ae, v->no);
}

static inline void
votedel(u_long id, telebot_handler_t handle, long long int chatid)
{
	assert(handle);
	assert(votevec);

	for (size_t n = 0; n < cvector_size(votevec); ++n) {
		if (votevec[n].id == id) {
			if (chatid)
				voteendmsg(&votevec[n], handle, chatid);

			if (!cvector_empty(votevec[n].users_ae))
				cvector_free(votevec[n].users_ae);
			if (!cvector_empty(votevec[n].users_no))
				cvector_free(votevec[n].users_no);

			if (!cvector_empty(votevec[n].name_users_ae))
				cvector_free(votevec[n].name_users_ae);
			if (!cvector_empty(votevec[n].name_users_no))
				cvector_free(votevec[n].name_users_no);

			cvector_erase(votevec, n);
			return;
		}
	}
}

/*
 *		S T O P A L L V O T E S
 *
 * Останавливает все голосования с помощью функции
 * votedel использованной в цикле. В конце очищает
 * вектор, на всякий случай.
 */
static inline void
stopallvotes(telebot_handler_t handle, long long int chatid)
{
	assert(handle);
	assert(votevec);

	size_t n = cvector_size(votevec);

	while (n--)
		votedel(votevec[n].id, handle, chatid);
}

/*
 *		V O T E S U P D A T E
 *
 * Проверяет все голосования из вектора <votevec>,
 * не должны ли они уже завершится? И завершает те
 * которые уже должны. Она вызывает каждую итерацию
 * основного цикла.
 */
static inline void
votesupdate(telebot_handler_t handle, long long int chatid)
{
	if (!votevec || !chatid)
		return;

	assert(handle);

	size_t n = cvector_size(votevec);

	while (n--) {
		/* Если тип голосования есть B, то голосование завершается
		 * после первого голоса "против"; это реализовано ниже.  */
		if (votevec[n].type == 'B') {
			if (votevec[n].no > 0) {
				votedel(votevec[n].id, handle, chatid);
				continue;
			}
		}

		/*	Вышло время.	*/
		if (time(NULL) >=
		    (time_t)(votevec[n].timestamp + votevec[n].timel))
			votedel(votevec[n].id, handle, chatid);
	}
}

/*
 *	S I N A I B O T E X I T
 *
 * Фунция которая всегда завершает sinaibot.
 * Она закрывает все голосования, очищает память,
 * и покидает программу с кодом 0. Привязана
 * к сигналу Ctrl + C.
 */
static inline noreturn void
sinaibotexit(int sig)
{
	(void)sig;
	if (handle) {
		if (votevec)
			stopallvotes(handle, chatid);
		telebot_destroy(handle);
	}
	if (updates)
		telebot_put_updates(updates, numupdates);
	if (sia)
		SIACLOSE(sia);

	fputc('\n', stderr);
	log_info("Leave from sinaibot!");
	exit(0);
}

/*
 *		U R A N D O M
 *
 * Генерирует (псевдо?)рандомное число в диапазоне
 * указанном в аргументах, а именно: <min> и <max>.
 * Генерирует на основе /dev/random, с помощью функции
 * getrandom.
 */
static inline uint64_t
rawurandom(void)
{
	uint64_t val;
	ssize_t n;

	do {
		n = getrandom(&val, sizeof(val), 0);
	} while (n < 0 && errno == EINTR);

	if (n != sizeof(val)) {
		log_fatal("n != sizeof(val)");
		abort(); /* --kernel */
	}

	return val;
}

static inline uint64_t
urandom(uint64_t min, uint64_t max)
{
	assert(min <= max);

	uint64_t val, range = (max - min);
	if (!range)
		return min;

	if (range == UINT64_MAX)
		return rawurandom();
	++range;
	uint64_t t = -range % range;
	while (1)
		if ((val = rawurandom()) >= t)
			return min + (val % range);

	return 0;
}

/*
 *	G E T N A M E F R O M U S E R
 *
 * Получает имя отправителя из <user>. Делает
 * это перебирая все возможные поля с каким-либо
 * именем. Приоритет есть: 1. first name;
 * 2. username; 3. last name.
 */
static inline const char *
getnamefromuser(telebot_user_t *user)
{
	assert(user);
	if (user->first_name && *user->first_name)
		return user->first_name;
	if (user->username && *user->username)
		return user->username;
	if (user->last_name && *user->last_name)
		return user->last_name;
	return "???";
}

/*
 *		I S S E N A T O R
 *
 * Проверяет содежится ли <id> в файле data/senators.
 * Если содержится, то возвращает 1, если нет то 0.
 */
static inline bool
issenator(const long long int id)
{
	for (size_t n = 0; n < sizeof(senators) / sizeof(*senators); ++n)
		if (id == senators[n].id)
			return 1;
	return 0;
}

/*
 *		C M P S T R S
 *
 * Возвращает 1, если <str> равна хоть одному из следующих
 * аргументов (строк). Последний элемент в <...> должен
 * быть NULL; не влияет регистр.
 */
bool
cmpstrs(const char *str, ...)
{
	const char *sp;
	va_list ap;

	assert(str);

	va_start(ap, str);
	while ((sp = va_arg(ap, const char *))) {
		if (!strcasecmp(str, sp)) {
			va_end(ap);
			return 1;
		}
	}

	va_end(ap);
	return 0;
}

/*
 *		S E N L I S T
 *
 * Выводит список всех сенаторов.
 */
static inline void
senlist(telebot_handler_t handle, telebot_message_t *msg)
{
	char buf[2048];
	size_t off = 0;

	assert(handle);
	assert(msg);
	assert(msg->chat);

	for (size_t n = 0; n < sizeof(senators) / sizeof(*senators); ++n)
		if (!bappendf(buf, sizeof(buf), &off,
			"___%ld___. *%s* (`%lld`);\n", n, senators[n].name,
			senators[n].id))
			break;

	return botmsg(handle, msg->chat->id, msg->message_id, buf);
}

static inline void
randomfileline(const char *filename, telebot_message_t *msg)
{
	char line[MAXLINELEN];
	size_t n = 0, i;
	FILE *fp;

	assert(filename);
	assert(msg);
	assert(msg->chat);

	if (!(fp = fopen(filename, "r")))
		return;
	while ((i = fgetc(fp)) != EOF)
		if (i == '\n')
			++n;
	if (!n)
		goto exit;

	rewind(fp);
	i = urandom(1, n);
	n = 0;

	while (fgets(line, sizeof(line), fp)) {
		if (++n == i) {
			botmsg(handle, msg->chat->id, msg->message_id, line);
			break;
		}
	}

exit:
	fclose(fp);
}
static inline void
amenl(telebot_handler_t handle, telebot_message_t *msg, char *ptr)
{
	char line[MAXLINELEN];
	size_t section, s, e;
	bool found = 0;
	char book[128];
	char buf[512];
	FILE *fp;
	int n;

	assert(handle);
	assert(msg);
	assert(msg->chat);

	if (!ptr) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "Слишком мало аргументов: %d вместо 2!\n", 1);
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "*Используйте*:\n  /amenl ___<позиция>___\n\n"
		    "*Формат позиции*:\n"
		    "  (a) ___<книга>.<глава>:<строчка>___\n"
		    "  (b) ___<книга>.<глава>:"
		    "<от>___*-*___<до>___\n"

		    "\n*Книги суть*:\n___Мат. Мар. Лук. Иоан."
		    " Деян. Иак. 1Пет.\n2Пет. "
		    "1Иоан. 2Иоан. 3Иоан. Иуда. Рим.\n1Кор. 2Кор. "
		    "Гал. Еф. Фил. Кол. 1Фес.\n2Фес. 1Тим. 2Тим. "
		    "Тит. Филим. Евр. Отк.___\n"

		    "\n*Например:*\n  /amenl ___Лук.8:18___\n"
		    "  /amenl ___1Кор.6:9-10___");
	}

	if ((n = sscanf(ptr, "%127[^.].%zu:%zu-%zu", book, &section, &s, &e)) <
	    3)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Неверный формат!");
	if (n == 3)
		e = s;
	if (!cmpstrs(book, "Мат", "Мар", "Лук", "Иоан", "Деян", "Иак", "1Пет",
		"2Пет", "1Иоан", "2Иоан", "3Иоан", "Иуда", "Рим", "1Кор",
		"2Кор", "Гал", "Еф", "Фил", "Кол", "1Фес", "2Фес", "1Тим",
		"2Тим", "Тит", "Филим", "Евр", "Отк", NULL))
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Книга ___<%s>___: не найдена!", book);
	if (s > e)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Неверный формат диапазона:"
		    " начало не может быть больше конца!");
	if ((e + 1 - s) > 3)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Неверный формат диапазона:"
		    " максимальное число строчек"
		    " для вывода 3 а не %ld!",
		    (e + 1 - s));

	if (!(fp = fopen("data/nz", "r")))
		return log_error("Failed open data/nz");

	while (fgets(line, sizeof(line), fp)) {
		snprintf(buf, sizeof(buf), "%s.%zu:%zu ", book, section, s);
		if (strncmp(line, buf, strlen(buf)) != 0)
			continue;
		botmsg(handle, msg->chat->id, msg->message_id, line);
		if (s < e) {
			++s;
			continue;
		}
		found = 1;
		break;
	}
	fclose(fp);

	if (!found)
		botmsg(handle, msg->chat->id, msg->message_id,
		    "Строчка не найдена!");
}

static inline void
votestopall(telebot_handler_t handle, telebot_message_t *msg)
{
	assert(handle);
	assert(msg);
	assert(msg->from);
	assert(msg->chat);

	if (msg->from->id != ADMINID)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "*Только администратор может остановить все голосования!*\n");

	if (votevec)
		stopallvotes(handle, msg->chat->id);
}

static inline void
act(telebot_handler_t handle, telebot_message_t *msg, const char *name,
    const char *act, bool h)
{
	const char *hearts[] = { "❤️", "🩷", "🧡", "💛", "💚", "🩵", "💙", "💜",
		"🤎", "🖤", "🩶", "🤍", "❣️", "💕", "💞", "💓", "💗", "💖", "💘",
		"💝", "❤️‍🔥", "❤️‍🩹", "♥️", "🫶", "💌", "🫀" };
	const char *hp =
	    hearts[urandom(0, sizeof(hearts) / sizeof(*hearts) - 1)];
	char resname[MAXNAMELEN];

	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);

	if (!act) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "Не хватает аргументов!");
		return botmsg(handle, msg->chat->id, 0,
		    "*Используйте*:\n  /act ___"
		    "<имя> <действие>___\n"

		    "\n*Например:*\n  /act ___питтака"
		    " учит геометрии___\n");
	}
	if (!name) {
		const char *cmd = NULL;
		if (!strcmp(act, "обнимает"))
			cmd = "hug";
		assert(cmd);

		if (!msg->reply_to_message || !msg->reply_to_message->from) {
			botmsg(handle, msg->chat->id, msg->message_id,
			    "Не хватает аргументов!");
			return botmsg(handle, msg->chat->id, 0,
			    "*Используйте*:\n  /%s ___"
			    "<имя> <имя> ...,___\n"

			    "\n*Например:*\n  /%s ___питтака"
			    "___\n",
			    cmd, cmd);
		}

		if (msg->reply_to_message->from->username)
			snprintf(resname, sizeof(resname), "@%s",
			    msg->reply_to_message->from->username);
		else
			snprintf(resname, sizeof(resname), "%s",
			    getnamefromuser(msg->reply_to_message->from));
	} else
		snprintf(resname, sizeof(resname), "%s", name);

	if (h)
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*%s %s %s — %s %s*", hp, getnamefromuser(msg->from), act,
		    resname, hp);
	else
		botmsg(handle, msg->chat->id, msg->message_id, "*%s %s — %s*",
		    getnamefromuser(msg->from), act, resname);
}

static inline void
votecommands(telebot_handler_t handle, telebot_message_t *msg, char *cmd)
{
	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);
	assert(cmd);

	if (!votevec)
		return;

	cvector_iterator(vote_t) it;
	size_t n;

	for (it = cvector_begin(votevec); it != cvector_end(votevec); ++it) {
		if (!strcmp(cmd, it->cmd_ae) || !strcmp(cmd, it->cmd_no)) {
			for (n = 0; n < cvector_size(it->users_ae); ++n) {
				if (it->users_ae[n] == msg->from->id) {
					botmsg(handle, msg->chat->id,
					    msg->message_id,
					    "Вы уже голосовали! (за)");
					return;
				}
			}
			for (n = 0; n < cvector_size(it->users_no); ++n) {
				if (it->users_no[n] == msg->from->id) {
					botmsg(handle, msg->chat->id,
					    msg->message_id,
					    "Вы уже голосовали! (против)");
					return;
				}
			}

			/* Проверяет голосует ли сенатор если у голосо
			 * вания в структуре стоит <senatorsflg>=1 */
			if (it->senatorsflg == '1')
				if (!issenator(msg->from->id))
					return botmsg(handle, msg->chat->id,
					    msg->message_id,
					    "*Только почетные"
					    " участники могут"
					    " голосовать в этом"
					    " голосовании!*");

			/* Проверяет голосует ли администратор, если да,
			 * то ставит соответствующий флаг <adminflg> в
			 * структуре голосования. */
			if (msg->from->id == ADMINID)
				it->adminflg = 1;
		}
		if (!strcmp(cmd, it->cmd_ae)) {
			cvector_push_back(it->users_ae, msg->from->id);
			cvector_push_back(it->name_users_ae,
			    strdup(getnamefromuser(msg->from)));
			++it->ae;
			botmsg(handle, msg->chat->id, msg->message_id,
			    "*ЗАСЧИТАНО — ЗА!*\n*Статистика (за/против)*:"
			    " %ld/%ld;\n*Голос*: %s;\n"
			    "*ID голосования*: `%ld`",
			    it->ae, it->no, getnamefromuser(msg->from), it->id);
			return;
		}
		if (!strcmp(cmd, it->cmd_no)) {
			cvector_push_back(it->users_no, msg->from->id);
			cvector_push_back(it->name_users_no,
			    strdup(getnamefromuser(msg->from)));
			++it->no;
			botmsg(handle, msg->chat->id, msg->message_id,
			    "*ЗАСЧИТАНО — ПРОТИВ!*"
			    "\n*Статистика (за/против)*:"
			    " %ld/%ld;\n*Голос*: %s;\n"
			    "*ID голосования*: `%ld`",
			    it->ae, it->no, getnamefromuser(msg->from), it->id);
			return;
		}
		if (!strcmp(cmd, it->cmd_stop)) {
			if (msg->from->id != ADMINID)
				return botmsg(handle, msg->chat->id,
				    msg->message_id,
				    "*Только администратор может"
				    " остановить голосование!*\n");
			votedel(it--->id, handle, msg->chat->id);
		}
	}
}

static inline void
vote(telebot_handler_t handle, telebot_message_t *msg, char **args, size_t n)
{
	char vmsg[1024], time[128], type[64], flag[64];
	size_t i, off = 0;

	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);
	assert(args);

	if (n < 4) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "Слишком мало аргументов: %d вместо 4!\n", n);
		botmsg(handle, msg->chat->id, 0,
		    "*Используйте*:\n  /vote ___<сообщение>"
		    " <длительность> <тип> <флаг>___\n\n"
		    "*Аргументы*:\n  ___<сообщение>___: какая"
		    " то информация о голосовании;\n"
		    "  ___<длительность>___:  длительность"
		    " голосования в секундах;\n"
		    "  ___<тип>___: есть два типа, это A или B;\n"
		    "  ___<флаг>___: если 1, то голосовать"
		    " могут только /senlist.\n"
		    "\n*Например:*\n  /vote ___Избираем меня"
		    " все вместе! 1000 A 0___");
		return;
	}

	snprintf(flag, sizeof(flag), "%s", args[--n]);
	snprintf(type, sizeof(type), "%s", args[--n]);
	snprintf(time, sizeof(time), "%s", args[--n]);

	for (i = 0; i < n; i++)
		if (!bappendf(vmsg, sizeof(vmsg), &off, "%s ", args[i]))
			return botmsg(handle, msg->chat->id, 0,
			    "Слишком длинное сообщение (макс %ld)!",
			    sizeof(vmsg) - 1);
	if (*flag != '0' && *flag != '1')
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Неверный ___<флаг>___ \"%s\" — "
		    "доступны только 0 или 1!\n",
		    flag);
	if (*type != 'A' && *type != 'B')
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Неверный ___<тип>___ \"%s\" — доступны"
		    " только A или B!\n",
		    type);
	if (cvector_size(votevec) >= VOTELIMIT)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Лимит голосований исчерпан! (%d/%d)", VOTELIMIT,
		    VOTELIMIT);

	if (!voteadd(vmsg, getnamefromuser(msg->from), time, type, flag,
		msg->chat->id))
		botmsg(handle, msg->chat->id, msg->message_id,
		    "Ошибка при создании голосования!");

	return;
}

/*
 * Обрабатывает команды полученные ботом, вызывает соответствующие
 * им вещи. За все команды отвечает она.
 */
static inline void
commandproc(telebot_handler_t handle, telebot_message_t *msg)
{
	assert(handle);
	assert(msg);
	assert(msg->text);
	assert(msg->chat);

	if (*msg->text != '/')
		return;
	if (!*(msg->text + 1))
		return;

	char *cmd = strstr(msg->text, username);
	if (cmd && cmd > msg->text && cmd[-1] != ' ')
		*cmd = 0;
	cmd = msg->text;
	while (*cmd) {
		if (isspace((uint8_t)*cmd))
			*cmd = ' ';
		cmd++;
	}
	cmd = msg->text;

	/* Обработка временных команд которые создаются после создания
	 * голосования.  Этот код проходит по всему вектору голосований,
	 * и сверяет команды каждого голосования, не сходятся ли они с
	 * <cmd>, и если сходятся, то выполняет соответствующие вещи.
	 * Помимо этого, проверяет голосовал ли уже пользователь, и
	 * может ли он вообще голосовать.  Обрабатывает, YES NO; STOP. */
	votecommands(handle, msg, cmd);

	/* Теперь нам понадобится получить команду без / и аргументов,
	 * т.е. из такого, - (/vote привет 10 A), оно получает, - (vote) */
	if (!(cmd = strtok(msg->text + 1, " ")))
		return;

	/* Обработка команды /vote, т.е команды для начала голосования,
	 * парсит аргументы, проверяет их, выводит соответствующие
	 * ошибки. Если ошибок нет, добавляет это голосование. */
	if (!strcmp(cmd, "vote")) {
		char *args[512] = { NULL }, *p;
		size_t n = 0;

		while ((p = strtok(NULL, " ")) && n < 512)
			args[n++] = p;

		return vote(handle, msg, args, n);
	}

	/* Is support с помощью так называемого, - master-code...
	 * Фанаты такие: 'ооо ктотонокто, как ты это делаешь!?'
	 * Я такой (ну типо): 'мой коды суть пободен мастеру' */
	else if (cmpstrs(cmd, "ae", "æ", "Æ", "ае", "aе", "аe", NULL)) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*AEEEE! ae ae AEEE*");
		return botmsg(handle, msg->chat->id, msg->message_id, "*aee*");
	}

	/* Команда для остановки сразу всех запущенных голосований в
	 * текущий момент. Ее может использоввать только администратор. */
	else if (!strcmp(cmd, "votestopall"))
		return votestopall(handle, msg);

	else if (!strcmp(cmd, "info")) {
		telebot_send_photo(handle, msg->chat->id, "data/logo.jpg", 1,
		    INFOMESSAGE, "Markdown", 0, msg->message_id, NULL);
		return;
	}

	else if (!strcmp(cmd, "senlist"))
		return senlist(handle, msg);

	else if (!strcmp(cmd, "amen"))
		return randomfileline("data/nz", msg);

	else if (!strcmp(cmd, "amenl"))
		return amenl(handle, msg, strtok(NULL, " "));

	else if (!strcmp(cmd, "hug")) {
		char buf[1024] = { 0 }, *p;
		size_t off = 0;

		while ((p = strtok(NULL, " ")))
			if (!bappendf(buf, sizeof(buf), &off, "%s ", p))
				return botmsg(handle, msg->chat->id, 0,
				    "Слишком длинное сообщение (макс %ld)!",
				    sizeof(buf) - 1);

		if (!strcmp(cmd, "hug"))
			return act(handle, msg, (*buf) ? buf : NULL, "обнимает",
			    1);
	} else if (!strcmp(cmd, "SIA")) {
		char *args[512] = { 0 }, *type, *p;
		size_t n = 0;

		if (msg->from->id != ADMINID && !issenator(msg->from->id))
			return botmsg(handle, msg->chat->id, 0,
			    "Только администратор или сенатор может использовать разведку!");
		if (!(type = strtok(NULL, " ")))
			goto SIAUSAGE;
		while ((p = strtok(NULL, " ")) && n < 512)
			args[n++] = p;
		if (n == 0)
			goto SIAUSAGE;

		/* Запись из файлового потока в буфер.  */
		char *report = NULL;
		size_t len = 0;
		FILE *stream = open_memstream(&report, &len);
		if (stream) {
			/* Выполняем запрос.  */
			if (SIAREPORT(sia, stream, type, args, n)) {
				fflush(stream);
				botmsg(handle, msg->chat->id, msg->message_id,
				    report);
				fclose(stream);
				if (report)
					free(report);
				return;
			}
			fclose(stream);
			if (report)
				free(report);

		SIAUSAGE:
			if ((stream = open_memstream(&report, &len))) {
				SIACATEGORIESLIST(sia, stream);
				fclose(stream);
			} else
				report = NULL;

			botmsg(handle, msg->chat->id, msg->message_id,
			    "*Используйте:*\n"
			    "  /SIA tags ___<id пользователя> <число> <период>___\n"
			    "  /SIA toptag ___<категория> <число> <период>___\n"
			    "  /SIA toptags ___<число> <период>___\n"
			    "  /SIA finduser ___<текст>___\n"
			    "\n*Формат <категория>:*\n"
			    "%s\n"
			    "\n*Формат <период>:*\n"
			    "  (a) ___all time___\n"
			    "  (b) ___last <hour|3 hours|day|3 days|week|mouth|year>___\n"
			    "  (c) ___Y-m-d [H:M]___ ; now\n"
			    "  (d) ___Y-m-d [H:M] ; Y-m-d [H:M]___\n"
			    "\n*Например:*\n"
			    "  /SIA finduser ___lom___\n"
			    "  /SIA tags ___5004184307 15 all time___\n"
			    "  /SIA toptag ___rust 10 last 3 days___\n"
			    "  /SIA toptags ___10 2007-03-04 15:44 ; now___\n",
			    report);

			if (report)
				free(report);
		}

		return;
	}
}

static inline long long int
fixchatid(long long int chatid)
{
	if (chatid <= -1000000000000LL)
		return (chatid * -1) - 1000000000000LL;
	else if (chatid < 0)
		return chatid * -1;
	return 0;
}

/*
 * Основная функция. Обрабатывает все сообщения которые
 * получает бот, решает что с ними делать.
 */
static void
sinaibotproc(telebot_handler_t handle, telebot_update_t *update)
{
	assert(handle);
	assert(update);

	if (!SIAUPDATE(sia, update->response))
		log_warn("Failed write response in data.db (SIA)");

	if (update->update_type != TELEBOT_UPDATE_TYPE_MESSAGE)
		return;

	telebot_message_t *msg = &update->message;

	if (!msg->chat)
		return;
	if (msg->edit_date != 0) /* Чтобы не спамили изменением */
		return;

	size_t n;

	/* Если это не группа CHATID и CHATID указан (т.е не 0)
	 * то покидаем. Однако, если пишет ADMINID, отвечаем. */
	chatid = msg->chat->id; /* Обновляем id текущего чата */
	if ((long long int)CHATID &&
	    fixchatid(chatid) != (long long int)CHATID) {
		if (!msg->from || msg->from->id != (long long int)ADMINID) {
			telebot_leave_chat(handle, chatid);
			return;
		}
	}

	/* Участники зашли или вышли */
	if (msg->new_chat_members && msg->count_new_chat_members) {
		for (n = 0; n < msg->count_new_chat_members; ++n)
			if ((telebot_send_animation(handle, chatid,
				"data/hello.mp4", true, 0, 0, 0, NULL,
				INFOMESSAGE, "Markdown", false, msg->message_id,
				NULL)) != TELEBOT_ERROR_NONE)
				log_error("Failed send hello.mp4!");
		return;
	}
	if (msg->left_chat_members && msg->count_left_chat_members > 0) {
		for (n = 0; n < msg->count_left_chat_members; ++n)
			botmsg(handle, msg->chat->id, msg->message_id,
			    "*%s вышел или был удален из группы*",
			    getnamefromuser(&msg->left_chat_members[n]));
		return;
	}

	if (!msg->from || !msg->text)
		return;

	/* Тогда это может быть команда */
	return commandproc(handle, msg);
}

/*
 *		O L D M S G S S K I P
 *
 * Пропускает все старые сообщения, чтобы он не отвечал
 * на команды которые пропустил, пока был отключен. Для
 * этого ищет номер последнего пропущенного, и ставит
 * номер последнего обновления е его знанчение. Последнее
 * обновление суть <lastupdate>.
 */
static inline int
oldmsgsskip(telebot_handler_t handle)
{
	telebot_update_t *updates = NULL;
	int off = 0, n = 0;

	assert(handle);

	while (telebot_get_updates(handle, off, 100, 0, 0, 0, &updates, &n) ==
		TELEBOT_ERROR_NONE &&
	    n > 0) {
		off = updates[n - 1].update_id + 1;
		telebot_put_updates(updates, n);
	}

	return (off > 0) ? off : 0;
}

/*
 *		S I N A I B O T . C
 */
int
main(void)
{
	setlocale(LC_ALL, "");
	signal(SIGINT, sinaibotexit);
	log_info("Starting sinaibot!");
	if (!TOKENBOT || !*TOKENBOT) {
		log_fatal("Not found token (check config.h)");
		sinaibotexit(1);
	}
	log_debug("Token: %s", TOKENBOT);
	log_debug("Interface: %s", (INTERFACE) ? INTERFACE : "any");

	/* Создаем объект telebot */
	if (telebot_create(&handle, TOKENBOT, INTERFACE) !=
	    TELEBOT_ERROR_NONE) {
		log_fatal("Failed create telebot");
		sinaibotexit(1);
	}
	if (PROXYADDR) {
		if (telebot_set_proxy(handle, PROXYADDR, PROXYAUTH) !=
		    TELEBOT_ERROR_NONE) {
			log_fatal("Failed set proxy");
			sinaibotexit(1);
		}

		log_debug("Proxy %s%s%s is set up", PROXYADDR,
		    (PROXYAUTH) ? "@" : "", (PROXYAUTH) ? PROXYAUTH : "");
	}

	/* Получаем информацию о боте */
	telebot_user_t me = { 0 };
	if (telebot_get_me(handle, &me) != TELEBOT_ERROR_NONE) {
		log_fatal("Failed to get bot info");
		sinaibotexit(1);
	}
	log_info("Get bot information:");
	if (me.id)
		log_debug("  id: %lld", me.id);
	if (me.first_name)
		log_debug("  first_name: %s", me.first_name);
	if (me.last_name)
		log_debug("  last_name: %s", me.last_name);
	if (me.language_code)
		log_debug("  language_code: %s", me.language_code);
	if (me.username) {
		log_info("  username: @%s", me.username);
		snprintf(username, sizeof(username), "@%s", me.username);
	}
	log_debug("  can_join_groups: %s",
	    (me.can_join_groups) ? "true" : "false");
	telebot_put_me(&me);

	/* Получаем начальную точку */
	int pos = oldmsgsskip(handle);
	if (pos)
		log_info("Skip messages (last update id %d)", pos);

	/* Открываем базу данных для SIA.  */
	if (!(sia = SIAOPEN("data/data.db", "SIA/database"))) {
		log_fatal("Failed start sinai intelligence agency");
		sinaibotexit(1);
	}

	while (1) {
		updates = NULL;
		numupdates = 0;

		/* Проверяем голосования */
		votesupdate(handle, chatid);

		/* Получаем обновления */
		if ((telebot_get_updates(handle, pos, 200, 10, 0, 0, &updates,
			&numupdates)) != TELEBOT_ERROR_NONE) {
			log_warn("Failed to get updates from Telegram API");
			waitms(1000);
			continue;
		}

		for (size_t n = 0; n < numupdates; ++n) {
			/* Перебираем обновления */
			sinaibotproc(handle, &updates[n]);

			if (updates[n].update_id >= pos)
				pos = updates[n].update_id + 1;
		}

		if (updates)
			telebot_put_updates(updates, numupdates);

		/* 20 ms (надо повысить если нагрузка большая) */
		waitms(20);
	}

	/* NOTREACHED */
	return 0;
}
