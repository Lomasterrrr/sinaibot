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

static size_t femboy_n; /* Для фембоя дня.  */

static uint64_t dickn = 120;	    /* Максимальное значение
				       увеличение или уменьшения. */
static uint64_t dickfortuna = 3;    /* Удача.  */
static uint64_t dickprobinc = 56;   /* Шанс увеличения.  */
static uint64_t dickprobsave = 55;  /* Шанс спасения.  */
static uint64_t dickprobextra = 58; /* Шанс экстра попытки.  */
static uint64_t dickcd1 = 43200;    /* Задержка 1.  */
static uint64_t dickcd2 = 57600;    /* Задержка 2.  */
static uint64_t dickcd3 = 86400;    /* Задержка 3.  */

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

inline static void
femboyday(telebot_handler_t handle, telebot_message_t *msg)
{
	char buf[65535], femboy[4028], date[MAXDATELEN], tmp[128];
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	FILE *fp;

	if (!(fp = fopen("data/femboy", "r+")))
		return log_error("Failed open data/femboy");
	fgets(buf, sizeof(buf), fp);
	fclose(fp);

	snprintf(date, sizeof(date), "%04d%02d%02d", tm->tm_year + 1900,
	    tm->tm_mon + 1, tm->tm_mday);

	sscanf(buf, "%s %*d %*d %[^\n]", tmp, femboy);
	if (strncmp(tmp, date, 8) != 0) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*👉👈 Фембой дня еще не определен!*");
		return;
	}

	botmsg(handle, msg->chat->id, msg->message_id, "*👉👈 Фембой дня — %s*",
	    &femboy);
}

inline static void
femboy_of_day(telebot_handler_t handle, telebot_message_t *msg)
{
	if (femboy_n) {
		--femboy_n;
		return;
	}

	femboy_n = urandom(1, 45);

	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	char date[MAXDATELEN], buf[65535] = { 0 };
	long long int oldlen;
	FILE *fp;

	snprintf(date, sizeof(date), "%04d%02d%02d", tm->tm_year + 1900,
	    tm->tm_mon + 1, tm->tm_mday);

	fclose(fopen("data/femboy", "a"));
	if (!(fp = fopen("data/femboy", "r+")))
		return log_error("Failed open data/femboy");
	fgets(buf, sizeof(buf), fp);
	oldlen = ftell(fp);

	/* New day? */
	if (strncmp(buf, date, 8) == 0) {
		fclose(fp);
		return;
	}

	fseek(fp, 0, SEEK_SET);

	/* Time? */
	sscanf(buf, "%*8c %ld", &t);
	if (time(NULL) < t) {
		fclose(fp);
		return;
	}

	fprintf(fp, "%s %ld %lld %s\n", date,
	    (time_t)(time(NULL) + urandom(1, 50400)), msg->from->id,
	    getnamefromuser(msg->from));

	if (ftell(fp) < oldlen) {
		t = fileno(fp);
		ftruncate(t, ftell(fp));
	}

	if (msg->from->username)
		botmsg(handle, msg->chat->id, 0,
		    "*Ой! 👉👈 Фембой дня — %s (@%s)*",
		    getnamefromuser(msg->from), msg->from->username);
	else
		botmsg(handle, msg->chat->id, 0, "*Ой! 👉👈 Фембой дня — %s*",
		    getnamefromuser(msg->from));

	fclose(fp);
}

/*
 *		S Y S T E M D V I R U S
 *
 * Ищет (регистронезависимо) в строке ключевое слово угрозу,
 * если нашло, выводит свойственное предупреждение и возвращает
 * 0, если не нашло, то возвращает -1.
 */
static inline bool
systemdvirus(telebot_handler_t handle, telebot_message_t *msg)
{
	const char *words[] = { "systemd", "системд", "центос", "цент ос",
		"centos", "cеntos", "cеntоs", "centos", "cent os", "ред хат",
		"redhat", "red hat", "редхат", "rhel", "сустемд", "рхел",
		"systеmd", "sуstemd", "sуstеmd", "cистeмд" };
	const char *wp;

	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->text);

#define _GNU_SOURCE /* for strcasestr */

	for (int n = 0; n < sizeof(words) / sizeof(*words); ++n)
		if (((wp = strcasestr(msg->text, words[n]))))
			break;
	if (!wp)
		return 0;

	botmsg(handle, msg->chat->id, msg->message_id,
	    "ВОТ ЭТО ДА! НОВЫЙ ПРОЕКТ RED HAT SYSTEMD — ЭТО\n"
	    "ВИРУСНЫЙ ЭКСПЛОИТ GCC!!! 🚨🔥 КИБЕРОРУЖИЕ RED HAT\n"
	    "УГРОЗА НАНОРОБОТ ВСТРОЕН В GCC МОДУЛЬ ДЛЯ RED HAT\n"
	    "LINUX!!! 💻☢️ КВАНТОВО-ФИЗИКО-МАТЕМАТИЧЕСКИ\n"
	    "УРОВЕНЬ SYSTEMD!!! 🌌🔮 СССР НЛО RED HAT MICROSOFT\n"
	    "ИНОПЛАНЕТЯНЕ ЗОНА 51 GCC БЕНДЕР LINUX АНТИМАТЕРИЯ\n"
	    "ЦРУ СПЕЦСЛУЖБЫ!!! 🛸👽 СЛЕЖКА ЗА ЛЮДЬМИ ЧЕРЕЗ OPEN\n"
	    "SOURCE!!! 🕵️‍♂️💀 ЭТО ПРОЕКТ ЭЛЕМЕНТАРНЫХ МАСШТАБОВ\n"
	    "США РАЗВОДКА SYSTEMD!!! 🇺🇸⚠️ RED HAT ЗАХВАТ ЗЕМЛИ GCC\n"
	    "СУПЕРСЕКРЕТНАЯ РАЗРАБОТКА!!! 🌍☠️ SYSTEMD ЕВРЕЙСКАЯ\n"
	    "ЦИВИЛИЗАЦИЯ COMMODORE 64!!! ✡️🖥\n"

	    "\nСЛАВА GCC! 🙏❤️ СЛАВА LINUX! 🙏❤️ АНГЕЛ-ХРАНИТЕЛЬ\n"
	    "OPEN SOURCE КАЖДОМУ ИЗ НАС! 🙏❤️ БОЖЕ, ЗАЩИТИ НАС ОТ\n"
	    "RED HAT SYSTEMD! 🙏🔥 СПАСИБО ТЕБЕ, КОМАНДА 404! 🙏🏼❤️ \n"
	    "ХРАНИ НАС, GNU! 🙏❤️\n");

	return 1;
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

/*
 * Генерирует автомат из эмодзи, записывает ее в <s> с
 * длинною <slen>, если <win> = 1 то выводит победную
 * в ином случае нет.
 */
static inline void
depstate(char *out, size_t outsiz, uint8_t win, uint8_t jackpot)
{
	const char *depnotes[] = {
		/* chat gpt master comment */
		"🍒", /* вишня — классика! */
		"🍋", /* лимон — из древних аппаратов */
		"🔔", /* колокол — символ джекпота */
		"🍉", /* арбуз — сладкий выигрыш */
		"⭐", /* звезда — редкий приз */
		"💎", /* драгоценность — богатство */
		"7️⃣",  /* заветная семёрка */
		"🍀", /* клевер — удача */
		"🎲", /* кость — азарт */
		"💰", /* мешок золота */
		"🎰", /* сам автомат */
	};
	const char *save;

	assert(out);
	assert(outsiz);

	if (jackpot) {
		save = "7️⃣";
		snprintf(out, outsiz,
		    "    %s%s%s%s\n    %s%s%s%s\n    %s%s%s%s\n", save, save,
		    save, save, save, save, save, save, save, save, save, save);
		return;
	}

	switch (win) {
	case 0:
		snprintf(out, outsiz,
		    "    %s%s%s%s\n    %s%s%s%s\n    %s%s%s%s\n",
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)]);
		break;
	case 1:
		save = depnotes[urandom(0,
		    (sizeof(depnotes) / sizeof(const char *)) - 1)];
		snprintf(out, outsiz,
		    "     %s%s%s%s\n— %s%s%s%s —\n     %s%s%s%s\n",
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    save, save, save, save,
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)],
		    depnotes[urandom(0,
			(sizeof(depnotes) / sizeof(const char *)) - 1)]);
		break;
	}
}

static inline void
dep(telebot_handler_t handle, telebot_message_t *msg, char *ptr)
{
	char state[2048];
	uint8_t jackpot;
	uint8_t win;
	int chance;
	uint64_t arg;
	int m;

	assert(handle);
	assert(msg);
	assert(msg->chat);

	if (!ptr) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "Слишком мало аргументов: %d вместо 2!\n", 1);
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "*Используйте*:\n  /dep ___<прайс>___\n"
		    "\n*Например:*\n  /dep ___1000___ заношу прайс");
	}

	if (!unumarg(ptr, 1, 10000000, &arg, sizeof(arg)))
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Ошибка в сумме!");
	if (arg < 1000)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Неверный ___<прайс>___ — %" PRIu64 "!"
		    "\nОн слишком нищий!\n",
		    arg);

	chance = urandom(1, 90);
	win = (urandom(0, 99) < chance);
	jackpot = (urandom(0, 1000) == 0);

	if (jackpot)
		m = 1000000;
	else
		m = (int)(1000 / (chance / 100.0) + urandom(1, 10));

	depstate(state, sizeof(state), win, jackpot);

	if (win || jackpot)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "___%s___\n"
		    "*Множитель прайса*: %d\n"
		    "*Шанс победы*: %d%%\n"
		    "*Формула*: ___%" PRIu64 " × %d___\n"
		    "\n%s\n"
		    "*ВЫИГРАНО — %" PRIu64 "$!*",
		    curtimestr(0), m, chance, arg, m, state, arg * m);

	return botmsg(handle, msg->chat->id, msg->message_id,
	    "___%s___\n"
	    "*Шанс победы*: %d%%\n"
	    "\n%s\n"
	    "___ПРОИГРАНА!___\n",
	    curtimestr(0), chance, state);
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
autism(telebot_handler_t handle, telebot_message_t *msg)
{
	const char *deus[2] = { 0 };
	int chance;

	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);

	if (urandom(1, 1000000) == 1)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "♈️-♉️-♊️-♋️-♌️-♍️-♎️-♏️-♐️-♑️-♒️-♓️"
		    "\n\n"
		    " *У %s —"
		    "\n\n"
		    "   ☀️ Σ O L I S ⚜️ D O M I N U S 👑\n"
		    "    💫 I M P E R A T O R 🔱 A E T E R"
		    " N U S 🪐\n"
		    "   🌈  Λ U X ⚜️ E T E R N A ✨\n"
		    "    🌧  S A C R U M 🔱 O R D O 🔥\n"
		    "\n"
		    "  НЕТ АУТИЗМА!*\n"
		    "\n"
		    "♈️-♉️-♊️-♋️-♌️-♍️-♎️-♏️-♐️-♑️-♒️-♓️",
		    getnamefromuser(msg->from));

	chance = urandom(1, 100);
	if (chance <= 10) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*У %s — слабый аутизм!*\n"
		    "Наличие аутизма суть: %u%%",
		    getnamefromuser(msg->from), chance);
	} else if (chance <= 30) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*У %s — ⚡️ средний ⚡️ аутизм!*\n"
		    "Наличие аутизма суть: %u%%",
		    getnamefromuser(msg->from), chance);
	} else if (chance <= 50) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*У %s — 👹 СИЛЬНЫЙ 👹 аутизм!*\n"
		    "Наличие аутизма суть: %u%%",
		    getnamefromuser(msg->from), chance);
	} else if (chance <= 80) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*У %s — ✨ M A G N U S ✨ аутизм!*\n"
		    "Наличие аутизма суть: %u%%",
		    getnamefromuser(msg->from), chance);
	} else if (chance <= 99) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*У %s — 👑 M A X I M U S V E"
		    " R U S 👑 аутизм!*\n"
		    "Наличие аутизма суть: %u%%",
		    getnamefromuser(msg->from), chance);
	} else {
		chance = urandom(1, 4);
		switch (chance) {
		case 1:
			deus[0] = "🌙";
			deus[1] = "🌕";
			break;
		case 2:
			deus[0] = "🌈";
			deus[1] = "🌈";
			break;
		case 3:
			deus[0] = "🌪";
			deus[1] = "🌊";
			break;
		case 4:
			deus[0] = "🪐";
			deus[1] = "💫";
			break;
		}
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*У %s —\n\n    "
		    "%s D E U S ⚜️ P R I M U S %s\n\n"
		    "аутизм!*\n"
		    "Наличие аутизма суть: %u%%",
		    getnamefromuser(msg->from), deus[0], deus[1], 100);
	}
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

/*
 *		P E N I S
 *
 * Главная функция для игры пенисов.
 */
static inline const char *
timefmts(double s, char *buf, size_t n)
{
	const char *prefixes[] = { "сек.", "мин.", "ч.", "дн.", "л." };
	double val = s;
	int prefix = 0;

	assert(buf);
	assert(n);

	if (val >= 31536000.0) { /* 365 * 86400 */
		prefix = 4;
		val /= 31536000.0;
	} else if (val >= 86400.0) { /* 24 * 3600 */
		prefix = 3;
		val /= 86400.0;
	} else if (val >= 3600.0) {
		prefix = 2;
		val /= 3600.0;
	} else if (val >= 60.0) {
		prefix = 1;
		val /= 60.0;
	}

	snprintf(buf, n, "%.0f %s ( = %.2f %s)", s, *prefixes, val,
	    prefixes[prefix]);

	return buf;
}

/*
 *		P E N I S
 *
 * Главная функция игры пенисов. Выполняет сам
 * игровой процесс.
 */
static const char *
penisstr(long long n, size_t max, bool op)
{
	assert(n);
	assert(max);

	if (n > max)
		n = max;

	/* Сколько процентов n есть от max.  */
	size_t pct = (n * 100) / max;

	static char str[256];
	char *emoji, *msg;

	/* Взависимости от того, сколько длинна
	 * уменьшения или увеличения составлят
	 * от max в процентах, подбираем
	 * соответствующий emoji и оценку.  */
	if (pct <= 2) {
		msg = "едва заметно";
		emoji = "🤏";
	} else if (pct <= 5) {
		msg = "слегка";
		emoji = "🐪";
	} else if (pct <= 10) {
		msg = "чуть-чуть";
		emoji = "✨";
	} else if (pct <= 15) {
		msg = "немного";
		emoji = "☘️";
	} else if (pct <= 25) {
		msg = "заметно";
		emoji = "💫";
	} else if (pct <= 35) {
		msg = "ощутимо";
		emoji = "⚡️";
	} else if (pct <= 45) {
		msg = "прилично";
		emoji = "🌹";
	} else if (pct <= 55) {
		msg = "значимо";
		emoji = "🎱";
	} else if (pct <= 65) {
		msg = "существенно";
		emoji = "🕊";
	} else if (pct <= 75) {
		msg = "солидно";
		emoji = "🎩";
	} else if (pct <= 85) {
		msg = "много";
		emoji = "👹";
	} else if (pct <= 90) {
		msg = "очень много";
		emoji = "🔥";
	} else if (pct <= 95) {
		msg = "мощно";
		emoji = "🌪";
	} else if (pct <= 98) {
		msg = "колоссально";
		emoji = "🌊";
	} else if (pct <= 99) {
		msg = "невероятно";
		emoji = "🪐";
	} else {
		msg = "M A G N U S";
		emoji = "👑";
	}

	/* Формируем строку с сообщением, которое потом вернем.  */
	snprintf(str, sizeof(str), "*— %s %s на %lld см* (___%s___)",
	    (op) ? emoji : "💀", (op) ? "вырос" : "сжался", n, msg);

	return str;
}

static inline size_t
withfortuna(size_t base, size_t fortuna)
{
	if (base >= 100 || fortuna == 0)
		return base;

	/* Расчитываем base с fortuna.  */
	return base + (100 - base) * fortuna / 100;
}

static inline void
penis(telebot_handler_t handle, telebot_message_t *msg)
{
	char stime[MAXDATELEN];
	char line[MAXLINELEN];
	long long int id, idm;
	char32_t symbols[3];
	long long nxt, len;
	FILE *fp, *tmp;
	bool found = 0;

	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);

	idm = msg->from->id; /* id вызвавшего /penis.  */

	/* Наш файл с датой игры пенисов.  */
	if (!(fp = fopen("data/penis", "a+")))
		return log_error("Failed open data/penis");
	rewind(fp);

	/* Временный файл, на который мы потом
	 * заменим файл с датой игры пенисов.  */
	if (!(tmp = fopen("data/penis.tmp", "w"))) {
		log_error("Failed open data/penis.tmp");
		fclose(fp);
		return;
	}

	/* Построчно перебираем data/penis, и разбираястрочку
	 * с помощью sscanf ищем id.  */
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%lld %lld %lld %u %u %u", &id, &len, &nxt,
			&symbols[0], &symbols[1], &symbols[2]) != 6)
			continue;

		/* Пользователь с таким id был найден в файле.  */
		if (id == idm) {
			if (found) /* Дубликация.  */
				continue;

			/* Если время попытки еще не пришло,
			 * выводим об этом сообщение.  */
			if (time(NULL) < (time_t)nxt) {
				botmsg(handle, msg->chat->id, msg->message_id,
				    "До следующей попытки осталось — %s",
				    timefmts((nxt - time(NULL)), stime,
					sizeof(stime)));
				fclose(fp);
				fclose(tmp);
				return;
			}

			bool extra = 0; /* EXTRA попытка.  */
			bool null = 0;	/* Полное восстановление.  */
			bool op = 0; /* Операция: увеличить или уменьшить.  */

			nxt = urandom(1, dickn);
			if (urandom(1, 100) <=
			    withfortuna(dickprobinc, dickfortuna))
				op = 1;
			if (urandom(1, 100) <=
			    withfortuna(dickprobextra, dickfortuna))
				extra = 1;
			if (op) {
				if (len < 0 &&
				    (urandom(1, 100) <=
					withfortuna(dickprobsave,
					    dickfortuna))) {
					nxt = llabs(len);
					null = 1;
				}
				len += nxt;
			} else
				len -= nxt;

			/* Выводим сообщение о пенисе.  */
			botmsg(handle, msg->chat->id, msg->message_id,
			    "*У %s пенис*\n"
			    "  %s"
			    "%s"
			    "%s"
			    "\n\n*Длина*: %lld см",
			    getnamefromuser(msg->from),
			    penisstr(nxt, dickn, op),
			    (extra) ? "\n  — ⚜️ ___Доступна"
				      " extra-попытка!___ /penis ⚜️" :
				      "",
			    (null) ? "\n  — 🌈 ___Спасительная"
				     " попытка!___ 🌈 " :
				     "",
			    len);

			if (!extra) {
				/* Выставляем время следующей попытки: оно
				 * выбирается случайно из 3 вариантов.  */
				nxt = urandom(1, 3);
				switch (nxt) {
				case 1:
					nxt = dickcd1;
					break;
				case 2:
					nxt = dickcd2;
					break;
				case 3:
					nxt = dickcd3;
					break;
				}
				nxt += (long long)time(NULL);
			} else
				nxt = 0; /* EXTRA попытка */

			fprintf(tmp, "%lld %lld %lld %u %u %u %s\n", id, len,
			    nxt, symbols[0], symbols[1], symbols[2],
			    getnamefromuser(msg->from));

			/* Пользователь с таким id был найден.  */
			found = 1;
			continue;
		}

		/* Если это не наш id, то просто записываем
		 * прочитанную строку во временный файл,
		 * чтобы сохранить данные.  */
		fputs(line, tmp);
	}

	if (!found) { /* Если id не был найден, значит это новый игрок.  */
		len = urandom(1, dickn);

		/* У первой попытки задержка 5-20 сек.  */
		nxt = urandom(5, 20);

		botmsg(handle, msg->chat->id, msg->message_id,
		    "*Пенис %s впервые*\n"
		    "  %s\n\n"
		    "___(Уже через %lld секунд вы сможете\n"
		    "его увеличить!)___\n",
		    getnamefromuser(msg->from), penisstr(len, dickn, 1), nxt);

		fprintf(tmp, "%lld %lld %lld 9898 11036 9725 %s\n", idm, len,
		    (time(NULL) + nxt), getnamefromuser(msg->from));
	}

	fclose(fp);
	fclose(tmp);

	/* Меняем файлы местами.  */
	rename("data/penis.tmp", "data/penis");
	return;
}

static inline void
dicktop(telebot_handler_t handle, telebot_message_t *msg, bool vagina)
{
	char str[2048] = { 0 };
	char line[MAXLINELEN];
	char name[MAXNAMELEN];
	size_t off = 0;
	long long len;
	FILE *fp;
	int i, j;

	/* Таблица пенисов.  */
	struct dickstat {
		char name[MAXNAMELEN];
		long long len;
		bool init;
	} stats[50];

	assert(handle);
	assert(msg);
	assert(msg->chat);

	for (i = 0; i < 50; ++i) {
		stats[i].len = (vagina) ? LLONG_MAX : LLONG_MIN;
		stats[i].name[0] = 0;
		stats[i].init = 0;
	}

	/* Наш файл с датой игры пенисов.  */
	if (!(fp = fopen("data/penis", "r")))
		return log_error("Failed open data/penis");

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%*d %lld %*d %*d %*d %*d %[^\n]", &len,
			name) != 2)
			continue;
		for (i = 0; i < 50; ++i) {
			bool insert = vagina ? (len < stats[i].len) :
					       (len > stats[i].len);
			if (insert) {
				for (j = 49; j > i; j--)
					stats[j] = stats[j - 1];

				snprintf(stats[i].name, sizeof(stats[i].name),
				    "%s", name);

				stats[i].len = len;
				stats[i].init = 1;
				break;
			}
		}
	}
	fclose(fp);

	/* Формируем таблицу в str для отправки.  */
	for (i = 0; i < 50 && stats[i].init; ++i)
		if (!bappendf(str, sizeof(str), &off,
			"%s ___%d.___ *%s — %lld см*\n",
			(i == 0)     ? "👑" :
			    (i == 1) ? "💫" :
			    (i == 2) ? "✨" :
				       "",
			(i + 1), stats[i].name, stats[i].len))
			break;

	return botmsg(handle, msg->chat->id, msg->message_id, str);
}

static inline void
dickstyle(telebot_handler_t handle, telebot_message_t *msg, char **args)
{
	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);
	assert(args);

	if (!args[0] || !args[1] || !args[2]) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "Не хватает аргументов!");
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "*Используйте*:\n  /dickstyle ___"
		    "<символ яиц> <символ ствола> <символ головки>___\n"
		    "\n*Например:*\n  /dickstyle 🔴 ⬛️ 🔶\n");
	}

	long long int id, idm;
	char line[MAXLINELEN];
	FILE *fp, *tmp;
	long long len;
	time_t nxt;

	/* Так как это могут быть эмодзи, приходится
	 * работать с 4 байтовыми символами.  */
	char32_t symbols[3] = { 0 };
	mbstate_t state;

	for (int n = 0; n < 3; ++n) {
		/* Если указано 1/2/3/4/5/6/7/8/9/0, то это будет
		 * воспринято как строка, а не код символа. Код
		 * начинает здесь от двухзначных. */
		if (isdigit((uint8_t)*args[n]) && *(args[n] + 1)) {
			if (!unumarg(args[n], 1, UINT_MAX, &symbols[n],
				sizeof(uint32_t)))
				return botmsg(handle, msg->chat->id,
				    msg->message_id, "Ошибка в коде символа!");
		} else {
			memset(&state, 0, sizeof(state));
			mbrtoc32(&symbols[n], args[n], 4, &state);
		}
	}

	idm = msg->from->id; /* id вызвавшего команду.  */

	/* Наш файл с датой игры пенисов.  */
	if (!(fp = fopen("data/penis", "r")))
		return log_error("Failed open data/penis");

	/* Временный файл, на который мы потом
	 * заменим файл с датой игры пенисов.  */
	if (!(tmp = fopen("data/penis.tmp", "w"))) {
		log_error("Failed open data/penis.tmp");
		fclose(fp);
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%lld %lld %ld", &id, &len, &nxt) != 3)
			continue;

		/* Пользователь с таким id был найден в файле.  */
		if (id == idm) {
			/* Устанавливаем наши новые символы для
			 * стиля пениса. */
			fprintf(tmp, "%lld %lld %ld %u %u %u %s\n", id, len,
			    nxt, symbols[0], symbols[1], symbols[2],
			    getnamefromuser(msg->from));

			continue;
		}

		fputs(line, tmp);
	}
	fclose(fp);
	fclose(tmp);

	/* Меняем файлы местами.  */
	rename("data/penis.tmp", "data/penis");

	return botmsg(handle, msg->chat->id, msg->message_id,
	    "*Стиль пениса успешно изменен! (%c %c %c)*\n", *args[0], *args[1],
	    *args[2]);
}

/*
 *		D I C K S T A T
 *
 * Выводит статистику о пенисе игрока.
 */
static inline void
dickpic(long long len, char32_t *symbols, char *buf, size_t buflen)
{
	char ssymbols[3][5] = { { 0 } };
	mbstate_t state;
	size_t off = 0;
	int s1 = 0;

	assert(symbols);
	assert(buf);
	assert(buflen);

	memset(&state, 0, sizeof(state));
	c32rtomb(ssymbols[0], symbols[0], &state);
	memset(&state, 0, sizeof(state));
	c32rtomb(ssymbols[1], symbols[1], &state);
	memset(&state, 0, sizeof(state));
	c32rtomb(ssymbols[2], symbols[2], &state);

	if (!bappendf(buf, buflen, &off, "%s\n", ssymbols[0]))
		return;

	for (long long n = 1; n <= len; ++n) {
		if (n % 10 == 0 && n <= 3000) {
			if (!bappendf(buf, buflen, &off, "%s", ssymbols[1]))
				return;
			if (++s1 % 15 == 0) {
				if (!bappendf(buf, buflen, &off, "%s\n",
					ssymbols[2]))
					return;
			}
		}
	}

	if (s1 % 15 != 0 || s1 == 0)
		if (!bappendf(buf, buflen, &off, "%s\n", ssymbols[2]))
			return;

	if (!bappendf(buf, buflen, &off, "%s", ssymbols[0]))
		return;
}

static inline void
dickstat(telebot_handler_t handle, telebot_message_t *msg, char *arg)
{
	char stime[MAXDATELEN];
	long long int id, idm;
	char line[MAXLINELEN];
	char32_t symbols[3];
	char picbuf[2048];
	bool found = 0;
	long long len;
	time_t nxt;
	FILE *fp;

	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);

	if (arg) {
		uint64_t tmp;
		if (!unumarg(arg, 1, LLONG_MAX, &tmp, sizeof(tmp)))
			return botmsg(handle, msg->chat->id, msg->message_id,
			    "Ошибка в id!");
		idm = (long long int)tmp;
	} else
		idm = msg->from->id;

	/* Наш файл с датой игры пенисов.  */
	if (!(fp = fopen("data/penis", "r")))
		return log_error("Failed open data/penis");

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%lld %lld %ld %u %u %u", &id, &len, &nxt,
			&symbols[0], &symbols[1], &symbols[2]) != 6)
			continue;

		/* Пользователь с таким id был найден в файле.  */
		if (id == idm) {
			/* Получаем точное изоображение пениса,
			 * т. е. его фоторобот.  */
			dickpic(len, symbols, picbuf, sizeof(picbuf));

			/* Выводим нашу статистику.  */
			botmsg(handle, msg->chat->id, msg->message_id,
			    "*Идентификатор*: `%lld` (%s)\n"
			    "*Стиль*: %u %u %u\n"
			    "*Длина*: %lld см\n\n"
			    "%s\n\n"
			    "Следующая попытка через %s\n",
			    id, getnamefromuser(msg->from), symbols[0],
			    symbols[1], symbols[2], len, picbuf,
			    timefmts((double)(time(NULL) < nxt) ?
				    (nxt - time(NULL)) :
				    0,
				stime, sizeof(stime)));

			found = 1;
			break;
		}
	}
	fclose(fp);

	if (!found)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Статистика не найдена!\n");

	return;
}

/*
 *		D I C K O P T
 *
 * Настроить все параметры игры пенисов.
 */
static inline void
dickopt(telebot_handler_t handle, telebot_message_t *msg, char **args)
{
	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);
	assert(args);

	if (msg->from->id != ADMINID) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*Только администратор может изменять параметры!*\n"
		    "А не фембой %s!",
		    getnamefromuser(msg->from));
		return;
	}

	if (!args[0] || !args[1] || !args[2] || !args[3] || !args[4] ||
	    !args[5] || !args[6] || !args[7])
		goto err;
	if (!unumarg(args[0], 1, SIZE_MAX, &dickn, sizeof(dickn)))
		goto err;
	if (!unumarg(args[1], 1, 100, &dickprobinc, sizeof(dickprobinc)))
		goto err;
	if (!unumarg(args[2], 1, 100, &dickprobsave, sizeof(dickprobsave)))
		goto err;
	if (!unumarg(args[3], 1, 100, &dickprobextra, sizeof(dickprobextra)))
		goto err;
	if (!unumarg(args[4], 0, SIZE_MAX, &dickcd1, sizeof(dickcd1)))
		goto err;
	if (!unumarg(args[5], 0, SIZE_MAX, &dickcd2, sizeof(dickcd2)))
		goto err;
	if (!unumarg(args[6], 0, SIZE_MAX, &dickcd3, sizeof(dickcd3)))
		goto err;
	if (!unumarg(args[7], 0, 100, &dickfortuna, sizeof(dickfortuna)))
		goto err;

	return botmsg(handle, msg->chat->id, msg->message_id,
	    "*Максимальное значение:* %zu\n"
	    "*Шанс увеличения:* %zu %%\n"
	    "*Шанс спасения:* %zu %%\n"
	    "*Шанс экстра попытки:* %zu %%\n"
	    "*Задержка 1:* %zu сек.\n"
	    "*Задержка 2:* %zu сек.\n"
	    "*Задержка 3:* %zu сек.\n"
	    "*Удача:* %zu",
	    dickn, dickprobinc, dickprobsave, dickprobextra, dickcd1, dickcd2,
	    dickcd3, dickfortuna);

err:
	return botmsg(handle, msg->chat->id, msg->message_id,
	    "Ошибка в аргументах!");
}

/*
 *		D I C K S E N D
 *
 * Отправить свои см другому игроку в игре пенисов.
 */
static inline void
dicksend(telebot_handler_t handle, telebot_message_t *msg, char **args)
{
	char line[MAXLINELEN];
	char name[MAXNAMELEN];
	char32_t symbols[3];
	long long int id;
	FILE *fp, *tmp;
	long long len;
	time_t nxt;

	uint64_t src;		  /* id отправителя */
	char srcname[MAXNAMELEN]; /* имя отправителя */
	uint64_t dst;		  /* id получателя */
	char dstname[MAXNAMELEN]; /* имя получателя */
	uint64_t sum;		  /* сумма отправки */
	bool sf = 0;		  /* отправитель найден ли */
	bool df = 0;		  /* получатель найден ли */
	bool balance = 1;	  /* хватает ли price */

	assert(handle);
	assert(msg);
	assert(args);
	assert(msg->chat);
	assert(msg->from);

	*srcname = *dstname = 0;
	src = (size_t)msg->from->id;

	if (!args[0] || !args[1]) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "Не хватает аргументов!");
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*Используйте*:\n  /dicksend ___"
		    "<id получателя> <кол. см>___\n"

		    "\n*Например:*\n  /dicksend ___9004184307"
		    " 100___\n");
		return;
	}

	/* Вообще, id это long long int, но этой функции
	 * нужен uint64_t.  */
	if (!unumarg(args[0], 1, UINT64_MAX, &dst, sizeof(dst)))
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Ошибка в id получателя!");
	if (!unumarg(args[1], 1, LLONG_MAX, &sum, sizeof(sum)))
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Неверная сумма!");
	if (dst == src)
		return botmsg(handle, msg->chat->id, msg->message_id,
		    "Нельзя отправить самому себе!");

	/* Наш файл с датой игры пенисов.  */
	if (!(fp = fopen("data/penis", "r")))
		return log_error("Failed open data/penis");

	/* Провяем на ошибки, и парсим что нам нужно.  */
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%lld %lld %*d %*d %*d %*d %[^\n]", &id, &len,
			name) != 3)
			continue;

		/* Парсим имена, проверяем на ошибки.  */
		if (id == src) {
			snprintf(srcname, sizeof(srcname), "%s", name);
			if (len < 0 || len < sum)
				balance = 0;
			sf = 1;
		} else if (id == dst) {
			snprintf(dstname, sizeof(dstname), "%s", name);
			df = 1;
		}
	}

	/* Если была найдена какая-нибудь ошибка,
	 * то сразу выходим, и выводим сообщение.  */
	if (!df || !sf || !balance) {
		fclose(fp);
		goto out;
	}

	/* Временный файл, на который мы потом
	 * заменим файл с датой игры пенисов.  */
	if (!(tmp = fopen("data/penis.tmp", "w"))) {
		log_error("Failed open data/penis.tmp");
		fclose(fp);
		return;
	}
	rewind(fp);

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%lld %lld %ld %u %u %u %[^\n]", &id, &len,
			&nxt, &symbols[0], &symbols[1], &symbols[2], name) != 7)
			continue;

		/* Зная, что могут быть ошибки, нам важен
		 * порядок проверки; в нашем случае мы
		 * уверены в их отсутствии.  */
		if (id == src)
			len -= sum;
		if (id == dst)
			len += sum;

		fprintf(tmp, "%lld %lld %ld %u %u %u %s\n", id, len, nxt,
		    symbols[0], symbols[1], symbols[2], name);
	}
	fclose(fp);
	fclose(tmp);

	/* Меняем файлы местами.  */
	rename("data/penis.tmp", "data/penis");

out:
	return botmsg(handle, msg->chat->id, msg->message_id,
	    "*Отправитель:* `%ld` (%s)\n"
	    "*Получатель:* `%ld` (%s)\n"
	    "*Дата:* %s\n"
	    "*Статус:* %s\n"
	    "*Сумма:* %ld см\n\n"
	    "%s%s%s",
	    src, srcname, dst, dstname, curtimestr(0),
	    ((sf && df && balance) ? "успех" : "неудача"), sum,
	    ((!sf) ? "Отправитель не найден!\n" : ""),
	    ((!df) ? "Получатель не найден!\n" : ""),
	    ((!balance) ? "Недостаточно средств!\n" : ""));
}

/*
 *		D I C K S U M
 *
 * Вывести общую сумму см в игре пенисов.
 */
static inline void
uint128str(__uint128_t val, char *buf)
{
#define UINT128STRMAXLEN 40
	char tmp[UINT128STRMAXLEN];
	int i = 0, j = 0;

	assert(buf);

	if (val == 0) {
		buf[0] = '0';
		buf[1] = 0;
		return;
	}
	while (val > 0) {
		tmp[i++] = '0' + (val % 10);
		val /= 10;
	}
	while (i > 0)
		buf[j++] = tmp[--i];

	buf[j] = 0;
}

static inline void
dicksum(telebot_handler_t handle, telebot_message_t *msg)
{
	char str[UINT128STRMAXLEN];
	char line[MAXLINELEN];
	long long len;
	FILE *fp;

	__uint128_t sum = 0; /* Общая сумма всех см. */
	bool ovflag = 0;     /* Переполнение.  */

	assert(handle);
	assert(msg);
	assert(msg->chat);

	/* Наш файл с датой игры пенисов.  */
	if (!(fp = fopen("data/penis", "r")))
		return log_error("Failed open data/penis");

	/* Перебираем этот файл, и все длинны более
	 * 0 добавляем к <sum>.  */
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%*d %lld", &len) != 1)
			continue;
		if (len > 0) {
			if (sum + (__uint128_t)len < sum) {
				/* Произошло переполнение.  */
				ovflag = 1;
				break;
			}
			sum += len;
		}
	}
	fclose(fp);

	/* Записываем число в строку; если было
	 * переполнение, собщаем об этом.  */
	if (ovflag)
		snprintf(str, sizeof(str), "(более 128 байт)");
	else
		uint128str(sum, str);

	return botmsg(handle, msg->chat->id, msg->message_id,
	    "*Общая сумма пенисов:* %s см", str);
}

/*
 *		D I C K R E S E T
 *
 * Сбросить все задержки в игре пенисов.
 */
static inline void
dickreset(telebot_handler_t handle, telebot_message_t *msg)
{
	char line[MAXLINELEN];
	char name[MAXNAMELEN];
	char32_t symbols[3];
	long long int id;
	FILE *fp, *tmp;
	long long len;

	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);

	/* Только администратор может скинуть задержки.  */
	if (msg->from->id != ADMINID) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*Только администратор может сбросить"
		    " все времена!*\nА не фембой %s!",
		    getnamefromuser(msg->from));
		return;
	}

	/* Наш файл с датой игры пенисов.  */
	if (!(fp = fopen("data/penis", "r")))
		return log_error("Failed open data/penis");

	/* Временный файл, на который мы потом
	 * заменим файл с датой игры пенисов.  */
	if (!(tmp = fopen("data/penis.tmp", "w"))) {
		log_error("Failed open data/penis.tmp");
		fclose(fp);
		return;
	}

	/* Читаем весь файл <fp> во временный <tmp>, но
	 * все времена ставим в 0. Таким образом сбрасываем
	 * все задержки.  */
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%lld %lld %*d %u %u %u %[^\n]", &id, &len,
			&symbols[0], &symbols[1], &symbols[2], name) != 6)
			continue;
		fprintf(tmp, "%lld %lld 0 %u %u %u %s\n", id, len, symbols[0],
		    symbols[1], symbols[2], name);
	}
	fclose(fp);
	fclose(tmp);

	/* Меняем файлы местами.  */
	rename("data/penis.tmp", "data/penis");

	return botmsg(handle, msg->chat->id, msg->message_id,
	    "*Все задержки пенисов были сброшены!*\n"
	    "*Используйте*: /penis");
}

static inline void
votestopall(telebot_handler_t handle, telebot_message_t *msg)
{
	assert(handle);
	assert(msg);
	assert(msg->from);
	assert(msg->chat);

	if (msg->from->id != ADMINID) {
		botmsg(handle, msg->chat->id, msg->message_id,
		    "*Только администратор может остановить все голосования!*\n"
		    "А не фембой %s!",
		    getnamefromuser(msg->from));
		return;
	}

	if (votevec)
		stopallvotes(handle, msg->chat->id);
}

static inline void
femboy(telebot_handler_t handle, telebot_message_t *msg, char *cmd)
{
	/*
	 * тайный язык фембоев
	 *
	 * Источники:
	 * https://oldteamhost.github.io/src/pages/sinai.html#section-3
	 * https://chatgpt.com/
	 */
	const char *femboylang[] = { ":3", "OwO", "oWo", ">.<", "👉👈", "🥺",
		"^^", ">w<", ":<", ">3", "\\:c", "UwU", "o.o", ":>", "<3",
		"\\:O", "uWu", ">W<", "\\:C", "🥺🥺", "🥺🥺🥺",
		"hewwo~ how awe u~", "senpai~", "not me doing this 👉👈",
		"*nuzzles u*", "*pounces on u*", "*blushes*", "*giggles~*",
		"*tail wags*", "*hides face*", "*squeaks*", "*whimpers softly*",
		"am smol qwq", "pls no bully :<", "i wuv you~", "rawr x3",
		"so cutesy~", "pwease uwu", "chu~", "nya~",
		"i'm just a smol bean~", "*licks ur cheek*", "*clings to u*",
		"*cuddwes*", "*snuggwes tight~*",
		"*looks up at u wif big eyes*", "*does a happi dance*",
		"*owo what's dis?*", "*floofs hair*", "*twirls around*",
		"*tilts head cutely*", "*paw pats*", "*wiggles fingers*",
		"s-senpai noticed me! 🥺", "*sparkles*", "uwu what's this? :3",
		"*huggles*", "*boops ur nose*", "*blushes deeply*", "teehee~",
		"*sniffs*", "*peekaboo!*", "mwah~ 💋", "soft smooches~",
		"*sleepy yawn*", "teehee owo", "*licks lips*", "rawr xD",
		"pls be gentle~", "*floats like a cloud*", "*dreamy eyes*",
		"glomp~", "paws up! *meow*", "uwu >w<",
		"*snuggles into your arms*", "💕", "🥺💖", "💖", "femboy" };
	size_t i = 40, n, off = 0;
	char speak[2048];

	assert(handle);
	assert(msg);
	assert(msg->chat);
	assert(msg->from);
	assert(cmd);

	for (n = 0; n < sizeof(femboylang) / sizeof(*femboylang); ++n) {
		if (!strcasestr(cmd, femboylang[n]))
			continue;

		if (!bappendf(speak, sizeof(speak), &off, "hewwo~ %s! 👉👈\n\n",
			getnamefromuser(msg->from))) {
			return;
		}

		while (i--)
			if (!bappendf(speak, sizeof(speak), &off, "%s ",
				femboylang[urandom(0,
				    (sizeof(femboylang) /
					sizeof(const char *)) -
					1)]))
				break;

		return botmsg(handle, msg->chat->id, msg->message_id, speak);
	}
}

static inline void
loveact(telebot_handler_t handle, telebot_message_t *msg, const char *name,
    const char *act)
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
		    "*Используйте*:\n  /loveact ___"
		    "<имя> <действие>___\n"

		    "\n*Например:*\n  /loveact ___питтака"
		    " учит геометрии___\n");
	} else if (!name) {
		const char *cmd = NULL;
		if (!strcmp(act, "гладит"))
			cmd = "pat";
		else if (!strcmp(act, "обнимает"))
			cmd = "hug";
		else if (!strcmp(act, "щекочет"))
			cmd = "tickle";
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

	botmsg(handle, msg->chat->id, msg->message_id, "*%s %s %s — %s %s*", hp,
	    getnamefromuser(msg->from), act, resname, hp);
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
			if (it->senatorsflg == '1') {
				if (!issenator(msg->from->id)) {
					botmsg(handle, msg->chat->id,
					    msg->message_id,
					    "*Только почетные"
					    " участники могут"
					    " голосовать в этом"
					    " голосовании!* А не"
					    " фембой %s!",
					    getnamefromuser(msg->from));
					return;
				}
			}

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
			if (msg->from->id != ADMINID) {
				botmsg(handle, msg->chat->id, msg->message_id,
				    "*Только администратор может"
				    " остановить голосование!*\nА не фембой %s!",
				    getnamefromuser(msg->from));
				return;
			}
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
	 * текущий момент. Ее может использоввать только администратор,
	 * а не фембой. */
	else if (!strcmp(cmd, "votestopall"))
		return votestopall(handle, msg);

	else if (!strcmp(cmd, "senlist"))
		return senlist(handle, msg);

	else if (!strcmp(cmd, "dep"))
		return dep(handle, msg, strtok(NULL, " "));

	else if (!strcmp(cmd, "amen"))
		return randomfileline("data/nz", msg);

	else if (!strcmp(cmd, "extreme"))
		return randomfileline("data/rus", msg);

	else if (!strcmp(cmd, "amenl"))
		return amenl(handle, msg, strtok(NULL, " "));

	else if (!strcmp(cmd, "autism"))
		return autism(handle, msg);

	/* Онет, похоже придется снова раскрыть стиль мастера... */
	else if (cmpstrs(cmd, "penis", "dick", "cock", "chlen", NULL))
		return penis(handle, msg);

	else if (cmpstrs(cmd, "dicktop", "penistop", NULL))
		return dicktop(handle, msg, 0);

	else if (!strcmp(cmd, "vaginatop"))
		return dicktop(handle, msg, 1);

	else if (cmpstrs(cmd, "dickstat", "penisstat", NULL))
		return dickstat(handle, msg, strtok(NULL, " "));

	else if (cmpstrs(cmd, "dicksum", "penissum", NULL))
		return dicksum(handle, msg);

	else if (cmpstrs(cmd, "dickopt", "penisopt", NULL)) {
		char *args[8] = { strtok(NULL, " "), strtok(NULL, " "),
			strtok(NULL, " "), strtok(NULL, " "), strtok(NULL, " "),
			strtok(NULL, " "), strtok(NULL, " "),
			strtok(NULL, " ") };
		return dickopt(handle, msg, args);
	}

	else if (cmpstrs(cmd, "dicksend", "penissend", NULL)) {
		char *args[2] = { strtok(NULL, " "), strtok(NULL, " ") };
		return dicksend(handle, msg, args);
	}

	else if (cmpstrs(cmd, "dickstyle", "penisstyle", NULL)) {
		char *args[3] = { strtok(NULL, " "), strtok(NULL, " "),
			strtok(NULL, " ") };
		return dickstyle(handle, msg, args);
	}

	else if (cmpstrs(cmd, "dickreset", "penisreset", NULL))
		return dickreset(handle, msg);

	else if (!strcmp(cmd, "femboyday"))
		return femboyday(handle, msg);

	else if (!strcmp(cmd, "loveact") || !strcmp(cmd, "pat") ||
	    !strcmp(cmd, "hug") || !strcmp(cmd, "tickle")) {
		char buf[1024] = { 0 }, *name = NULL, *p;
		size_t off = 0;

		if (!strcmp(cmd, "loveact"))
			name = strtok(NULL, " ");

		while ((p = strtok(NULL, " ")))
			if (!bappendf(buf, sizeof(buf), &off, "%s ", p))
				return botmsg(handle, msg->chat->id, 0,
				    "Слишком длинное сообщение (макс %ld)!",
				    sizeof(buf) - 1);

		if (!strcmp(cmd, "loveact"))
			return loveact(handle, msg, name, (off) ? buf : NULL);
		if (!strcmp(cmd, "pat"))
			return loveact(handle, msg, (*buf) ? buf : NULL,
			    "гладит");
		if (!strcmp(cmd, "tickle"))
			return loveact(handle, msg, (*buf) ? buf : NULL,
			    "щекочет");
		if (!strcmp(cmd, "hug"))
			return loveact(handle, msg, (*buf) ? buf : NULL,
			    "обнимает");
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

	return femboy(handle, msg, cmd);
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
				"data/hello.mp4", true, 0, 0, 0, NULL, NULL,
				"Markdown", false, msg->message_id, NULL)) !=
			    TELEBOT_ERROR_NONE)
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

	/* Онет!! */
	if (systemdvirus(handle, msg))
		return;

	femboy_of_day(handle, msg);

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
