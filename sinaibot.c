/*
 * Copyright (c) 2025, lomaster. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/types.h>
#include <stdint.h>
#include <signal.h>
#include <float.h>
#include <ctype.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <stdnoreturn.h>

#include "cvector.h"
#include <json-c/json.h>
#include <telebot.h>
#include <telebot-core.h>

telebot_handler_t	_handle;
char			token[BUFSIZ];
char			admin_user[BUFSIZ];
telebot_update_t	*updates;
int			num_updates;
long long int		c_id;

/*
 * ГОЛОСОВАНИЯ
 */
#define VOTE_LIMIT 3	/* максимум голосований */
typedef struct __vote_t {
	/* эти поля указываются из команды */
	char		msg[USHRT_MAX];	/* сообщение */
	size_t		timel;	/* двительность в сек */
	u_char		type;	/* тип голосования */

	char		starter[2048];	/* инициатор */
	u_long		id;	/* номер голосования */
	size_t		AE;	/* за */
	size_t		NO;	/* против */
	u_char		admin_flag;	/* голосовал ли администратор? */
	u_char		senators_flag;	/* могут голосовать только /senators? */
	time_t		timestamp;	/* точка старта */

	/* команды для за/против/остановка */
	char		cmd_ae[512],	/* за */
			cmd_no[512],	/* против */
			cmd_stop[512];	/* стоп */

	/* голосовавшие за/против */
	cvector(char *) users_ae;	/* за */
	cvector(char *) users_no;	/* против */
} vote_t;
cvector(vote_t)		vote_vec=NULL;		/* голосования */
inline static int cmpstr(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
inline static void stop_all_vote(telebot_handler_t handle, long long int chat_id);
inline static void free_string(void *str) { if (str) free(*(char **)str); }
inline static int vote_add(const char *msg, const char *starter, const char *timel,
	const char *type, const char *flag, vote_t *tmp);
inline static void vote_del(u_long id, telebot_handler_t handle, long long int chat_id);
inline static void vote_startmsg(vote_t *v, telebot_handler_t handle, long long int chat_id);
inline static void vote_endmsg(vote_t *v, telebot_handler_t handle, long long int chat_id);

/*
 * СТАТИСТИКА
 */
typedef struct __stats_t {
	time_t tstamp;	/* точка старта */
	size_t n_messages;	/* сообщений */
	size_t n_join;	/* зашло новых */
	size_t n_total;	/* общее число каких либо действий */
} stats_t;
stats_t h12;	/* за 12 часов */
inline static int incsafe(size_t *v, size_t n);
inline static int update_stats(stats_t *s, telebot_update_t *u);

/*
 * SINAI-ZINO
 */
#define DEP_LIMIT 3	/* максимум депов одновременно */
const char *dep_notes[]={
	/* chat gpt master comment */
	"🍒", /* вишня — классика! */
	"🍋", /* лимон — из древних аппаратов */
	"🔔", /* колокол — символ джекпота */
	"🍉", /* арбуз — сладкий выигрыш */
	"⭐", /* звезда — редкий приз */
	"💎", /* драгоценность — богатство */
	"7️⃣", /* заветная семёрка */
	"🍀", /* клевер — удача */
	"🎲", /* кость — азарт */
	"💰", /* мешок золота */
	"🎰", /* сам автомат */
};
typedef struct __dep_t {
	int	msg_id;	/* id сообщения с прокрутом (депом) */
	char	starter[2048];	/* инициатор */
	u_long	id;	/* id депа */
	u_char	win;	/* победил? */

	/* принцип работы таков,
	 * 1. добавить dep.
	 * 2. 3 раза обновить
	 * 3. вывести результат.  */
	int	leftupdate;	/* осталось обновлений */
} dep_t;
cvector(dep_t)	dep_vec=NULL;	/* депы */
inline static void stop_all_dep(telebot_handler_t handle, long long int chat_id);
inline static int dep_add(const char *starter, size_t *index);
inline static void dep_endmsg(dep_t *d, telebot_handler_t handle, long long int chat_id);
inline static void dep_startmsg(dep_t *d, telebot_handler_t handle, long long int chat_id, int id);
inline static void dep_state(char *s, size_t slen, u_char win);
inline static void dep_del(u_long id, telebot_handler_t handle, long long int chat_id);
inline static void dep_update(telebot_handler_t handle, long long int chat_id);
inline static void stop_all_dep(telebot_handler_t handle, long long int chat_id);




/*
inline static int updatedump(FILE *in, telebot_update_t *u)
{
	if (!in||!u)
		return -1;
	fprintf(in,"UPDATE ID %d",u->update_id);
}
*/




/*
 * Сделана на основе err.h в bsd. Используется для откладочных
 * данных, формата: VERBOSE	<данные>. Выводит все в stderr;
 * сама ставит \n в конце; принимает форматирование.
 */
inline static void verbose(const char *fmt, ...)
{
	va_list ap;

	va_start(ap,fmt);
	if (fmt) {
		(void)fprintf(stderr,"VERBOSE\t");
		(void)vfprintf(stderr,fmt,ap);
	}
	(void)fputc(0x0a,stderr);
	va_end(ap);
}




/*
 * Универсальная функция поиска по векторами cvector, она
 * принимает вектор <vec>, то что надо найти <target>, и
 * указатель на функцию <cmp>, которой она будет сравнивать.
 * Функция cmp должна возвращать 0 в int в случае успеха, и
 * любое другое число int в случае провала.
 */
inline static int cvectorfind(void **vec, const void *target,
		int(*cmp)(const void *, const void *))
{
	size_t n;

	if (!vec)
		return -1;
	if (!target)
		return -1;
	if (!cmp)
		return -1;

	for (n=0;n<cvector_size(vec);++n)
		if (cmp(vec[n],target)==0)
			return (int)n;

	return -1;
}




/*
 * Получает дату и время, записывает ее в статический
 * буфер <date_str>, который возвращает. В случае ошибки
 * возвращает строку "err".
 *
 * Формат даты: год-месяц-день день-недели-сокращенный
 * 		час:минута:секунда часовой-пояс
 */
inline static const char *curtime(time_t pos)
{
	static char	date_str[512];
	struct tm	*tm_info;
	time_t		now;

	if (pos>0)
		now=pos;
	else {
		if ((now=time(NULL))==(time_t)(-1))
			return "err";
	}
	if (!(tm_info=localtime(&now)))
		return "err";

	strftime(date_str,sizeof(date_str),
		"%Y-%m-%d %a %H:%M:%S %Z",
		tm_info
	);

	return date_str;
}




/*
 * Фунция которая всегда завершает sinaibot. Она
 * закрывает все голосования, очищает память, и
 * покидает нас с кодом 0. Привязана к сигналу
 * Ctrl+c.
 */
inline static noreturn void leave(int sig)
{
	(void)sig;
	if (_handle) {
		stop_all_vote(_handle,c_id);
		stop_all_dep(_handle,c_id);
		telebot_destroy(_handle);
	}
	if (updates)
		telebot_put_updates(updates,num_updates);
	puts("\n");
	verbose("LEAVE FROM SINAI BOT!!");
	exit(0);
}




/*
 * Остнаваливает выполнение потока на указанное количество
 * милисекунд. Ну т.е это sleep() который принимает не
 * секунды, а милисекунды.
 */
inline static void stopms(int ms)
{
	struct timespec ts;
	ts.tv_sec=ms/1000;
	ts.tv_nsec=(ms%1000)*1000000;
	nanosleep(&ts,NULL);
}




/*
 * Сделана на основе err.h в bsd. Единственное отличие от
 * оригинала, это вместо exit() стало leave(), и формат
 * теперь: ERR	<данные>. Сама ставит \n; имеет форматирование;
 * безворвратная.
 */
inline static noreturn void errx(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap,fmt);
	if (fmt) {
		(void)fprintf(stderr,"ERR\t");
		(void)vfprintf(stderr,fmt,ap);
	}
	(void)fputc(0x0a,stderr);
	va_end(ap);
	leave(eval);
}




/*
 * Функция для конвертации строки в size_t с указанным диапазоном и
 * с проверкой всех ошибок. <str> - суть строка; <out> - адрес
 * переменной size_t куда будет записан результат; <min>/<max> -
 * диапазон, минимальное/максимальное. В случае ошибки ставит *out
 * в 0.
 */
inline static void str_to_size_t(const char *str, size_t *out,
		size_t min, size_t max)
{
	unsigned long long	val;
	char			*endptr;

	if (!str||!*str||!out) {
		if (out)
			*out=0;
		return;
	}
	while (isspace((u_char)*str))
		str++;
	if (*str=='-') {
		verbose("only positive numbers");
		*out=0;
		return;
	}
	errno=0;
 	val=strtoull(str,&endptr,10);
	if (errno==ERANGE||val>(unsigned long long)SIZE_MAX) {
		verbose("failed convert %s in num",str);
		*out=0;
		return;
	}
	while (isspace((u_char)*endptr))
		endptr++;
	if (*endptr!='\0') {
		verbose("failed convert %s in num",str);
		*out=0;
		return;
	}
	if (val<min||val>max) {
		verbose("failed convert %s in num; range failure (%ld-%llu)",
			str,min,max);
		*out=0;
		return;
	}

	*out=(size_t)val;
}




/*
 * Отправляет сообщение подобно функции telebot_send_message, но
 * перебирает все кодировки.
 */
telebot_error_e master_send_message(telebot_handler_t handle, long long int chat_id,
		const char *message, bool disable_web_page_preview,
		bool disable_notification, int reply_to_message_id,
		void *reply_markup)
{
	const char	*modes[]={"Markdown","MarkdownV2","HTML",NULL};
	telebot_error_e	ret;
	int		i;

	for (i=0;i<4;i++) {
		ret=telebot_send_message(handle,chat_id,message,modes[i],
			disable_web_page_preview,disable_notification,
			reply_to_message_id,reply_markup);
		if (ret==TELEBOT_ERROR_NONE) {
			verbose("success send message bot \"%s\" ret=%d\n",message,ret);
			return TELEBOT_ERROR_NONE;
		}
	}

	verbose("failed send message bot \"%s\" ret=%d\n",message,ret);
	return ret;
}




/*
 * Отправляет с бота указанное сообщение; поддерживает также
 * форматирование. <m> - суть разметка; <mid> - id сообщения.
 */
inline static void botmsg(telebot_handler_t handle, long long int chat_id,
		 const char *fmt, ...)
{
	char		message[USHRT_MAX];
	va_list		args;

	va_start(args,fmt);
	vsnprintf(message,sizeof(message),fmt,args);
	va_end(args);

	/* master отправка */
	master_send_message(handle,chat_id,message,
		false,false,0,NULL);

	return;
}




/*
 * Создает объект vote_t из указанных в аргументах значений.
 * возвращает -1 если ошибка и 0 если все хорошо. Также
 * созданных объект добавляет в вектор vote_vec. А еще
 * копирует его по адресу <tmp> (это чтобы потом вывести).
 */
inline static int vote_add(const char *msg, const char *starter,
		const char *timel, const char *type, const char
		*flag, vote_t *tmp)
{
	vote_t v;

	if (cvector_size(vote_vec)>=VOTE_LIMIT)
		return -1;

	memset(&v,0,sizeof(v));
	str_to_size_t(timel,&v.timel,0,SIZE_MAX);
	v.senators_flag=flag[0];
	v.type=type[0];

	/* master seed */
	v.id=((({struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts),
		(u_long)(ts.tv_sec*1000000000L+ts.tv_nsec);})));

	v.timestamp=time(NULL);	/* точка старта */
	cvector_init(v.users_ae,1,free_string);
	cvector_init(v.users_no,1,free_string);
	snprintf(v.cmd_ae,sizeof(v.cmd_ae),"/YES%ld",v.id);
	snprintf(v.cmd_no,sizeof(v.cmd_no),"/NO%ld",v.id);
	snprintf(v.cmd_stop,sizeof(v.cmd_stop),"/STOP%ld",v.id);
	snprintf(v.msg,sizeof(v.msg),"%s",msg);
	snprintf(v.starter,sizeof(v.starter),"%s",starter);

	if (tmp)
		memcpy(tmp,&v,sizeof(vote_t));
	
	cvector_push_back(vote_vec,v);
	return 0;
}




/*
 * Соединяет вектор (char *) в строку, которую запиисывает в <buf>,
 * на длинну <buflen>. Соединяет через ';'; если элементов нету
 * то просто запишет в <buf>: "никто;".
 */
inline static const char *joinvec(char *buf, size_t buflen, cvector(char *) vec)
{
	size_t i;
	if (!buf||!buflen||!vec)
		return NULL;
	buf[0]='\0';
	if (cvector_empty(vec)) {
		snprintf(buf,buflen,"никто;");
		return buf;
	}
	for (i=0;i<cvector_size(vec);++i) {
		if (strlen(buf)+strlen(vec[i])+2>=buflen)
			break;
		strcat(buf,vec[i]);
		/* последний пробел даже не видно в тг, похуй */
		strcat(buf,"; ");
	}
	return buf;
}




/*
 * Выводит сообщение о старте голосования.
 */
inline static void vote_startmsg(vote_t *v, telebot_handler_t handle, long long int chat_id)
{
	botmsg(handle,chat_id,
		"*Голосование* — \"%s\";\n"
		"\n*Проголосовать за*:\n  `%s`;\n"
		"*Проголосовать против*:\n  `%s`;\n\n"
		"*Инициатор*: %s;\n"
		"*Только почетные?*: %s;\n"
		"*Тип голосования*: %c;\n"
		"*Длительность*: %ld секунд(а);\n"
		"*ID голосования*: `%ld`;\n"
		"\n— ___%s___\n"
		,v->msg,v->cmd_ae,v->cmd_no,v->starter,
		((v->senators_flag=='1')?"да":"нет"),
		v->type,v->timel,v->id,curtime(v->timestamp)
	);
}




/*
 * Выводит сообщение о окончании голосования.
 */
inline static void vote_endmsg(vote_t *v, telebot_handler_t handle, long long int chat_id)
{
	char buf1[BUFSIZ],buf2[BUFSIZ];
	
	joinvec(buf1,sizeof(buf1),v->users_ae);
	joinvec(buf2,sizeof(buf2),v->users_no);

	botmsg(handle,chat_id,
		"*ОКОНЧАНИЕ ГОЛОСОВАНИЯ —* \"%s\" `%ld` ___%s___!\n\n"
		"*Инициатор*: %s;\n"
		"*Только почетные?*: %s;\n"
		"*Голосовал ли админ?* — %s;\n"
		"*Были за* — %s\n"
		"*Были против* — %s\n"
		"\n*Результаты (за/против)*\n — ___%ld / %ld___"
		,v->msg,v->id,curtime(0),v->starter,
		((v->senators_flag=='1')?"да":"нет"),
		((v->admin_flag)?"да":"нет"),buf1,
		buf2,v->AE,v->NO
	);
}




/*
 * Удаляет один объект vote_t из вектора <vote_vec>, по его id (который
 * id голосования). Также выводит сообщение о окончании голосования.
 * Помимо этого, очищает память из под элементов которые этого требуют.
 */
inline static void vote_del(u_long id, telebot_handler_t handle, long long int chat_id)
{
	int n;

	if (!vote_vec)
		return;
	if (cvector_empty(vote_vec))
		return;
	if (!handle)
		return;

	for (n=0;n<cvector_size(vote_vec);n++) {
		if (vote_vec[n].id==id) {

			vote_endmsg(&vote_vec[n],handle,chat_id);

			if (!cvector_empty(vote_vec[n].users_ae))
				cvector_free(vote_vec[n].users_ae);
			if (!cvector_empty(vote_vec[n].users_no))
				cvector_free(vote_vec[n].users_no);
			cvector_erase(vote_vec,n);
			return;
		}
	}

}




/*
 * Останавливает все голосования с помощью функции vote_del использованной
 * в цикле. В конце очищает вектор, на всякий случай.
 */
inline static void stop_all_vote(telebot_handler_t handle, long long int chat_id)
{
	int n;
	for (n=cvector_size(vote_vec)-1;n>=0;n--)
		vote_del(vote_vec[n].id,handle,chat_id);
	cvector_clear(vote_vec);
}




/*
 * Проверяет все голосования из вектора <vote_vec>, не должны ли они уже
 * завершится? И завершает те которые уже должны. Она вызывает каждую
 * итерацию основного цикла.
 */
inline static void check_vote(telebot_handler_t handle, long long int chat_id)
{
	int n;
	for (n=cvector_size(vote_vec)-1;n>=0;n--) {

		/* Если тип голосования суть B, то голосование завершается
		 * после первого голоса "против", это реализовано ниже. */
		if (vote_vec[n].type=='B') {
			if (vote_vec[n].NO>0) {
				vote_del(vote_vec[n].id,handle,chat_id);
				continue;
			}
		}

		/*	Вышло время.	*/
		if (time(NULL)>=vote_vec[n].timestamp+vote_vec[n].timel)
			vote_del(vote_vec[n].id,handle,chat_id);
	}
}




/*
 * Генерирует (псевдо?)рандомное число в диапазоне указанном в аргументах,
 * а точнее, <min> и <max>. Возвращает его в u_int (32 bit unsigned int).
 * Генерирует на основе /dev/random, с помощью функции getrandom. В случае
 * ошибки вернет 0.
 */
u_int urand(u_int min, u_int max)
{
	u_int	random,range;
	ssize_t	ret;

	if (min>max)
		return 1;
	range=(max>=min)?(max-min+1):
		(UINT_MAX-min+1);

	return ((ret=getrandom(&random, sizeof(u_int),GRND_NONBLOCK
		|GRND_RANDOM))==-1||(ret!=sizeof(u_int))?0:
		((min+(random%range))));
}




/*
 * Проверяет является ли строка <str> числом, или нет. Если
 * число вернет 1, если нет, 0.
 */
inline static int is_digit_string(const char *str)
{
	char *endptr;
	errno=0;
	(void)strtol(str,&endptr,10);
	return *endptr=='\0'&&errno==0;
}




/*
 * Получает имя отправителя сообщения <msg>. Делает это пере
 * бирая все возможные поля с каким-либо именем. Приоритет
 * суть: 1. username; 2. first name; 3. last name. Это нужно
 * дабы избежать ошибок segmentation fault.
 */
inline static const char *get_name_from_msg(telebot_message_t *msg)
{
	if (!msg)
		return "none";
	if (!msg->from)
		return "none";
	if (msg->from->username)
		if (strlen(msg->from->username)>0)
			return msg->from->username;
	if (msg->from->first_name)
		if (strlen(msg->from->first_name)>0)
			return msg->from->first_name;
	if (msg->from->last_name)
		if (strlen(msg->from->last_name)>0)
			return msg->from->last_name;
	return "none";
}




/*
 * Проверяет содежится ли ник <input> в файле data/senators.
 * Если содержится, то возвращает 1, если нет то 0.
 */
inline static int is_senator(const char *input)
{
	char	line[USHRT_MAX];
	FILE	*f;

	bzero(line,sizeof(line));
	if (!(f=fopen("data/senators","r")))
		return 0;

	while (fgets(line,sizeof(line),f)) {
		line[strcspn(line,"\r\n")]='\0';
		if (line[0]=='\0')
			continue;
		if (!strcmp(input,line)) {
			fclose(f);
			return 1;
		}
	}

	fclose(f);
	return 0;
}




/*
 * Загружает одну строчку в <buf> с размером <buflen> из
 * файла <filename>. Если была ошибка, завершает программу.
 * Удаляет \n, если он есть в конце строчки, это нужно чтобы
 * считать токен корректно.
 */
inline static void loadfromfile(const char *filename, char *buf, size_t buflen)
{
	size_t	n;
	FILE	*f;

	if (!(f=fopen(filename,"r")))
		errx(1,"failed open %s file!",filename);
	if (!fgets(buf,buflen,f)) {
		if (f)
			fclose(f);
		errx(1,"failed get line from %s file!",filename);
	}
	n=strlen(buf);
	if (n>0&&buf[n-1]=='\n')
		buf[n-1]='\0';
	if (f)
		fclose(f);
}




/*
 * Замена для strcasestr (strstr но без учета регистра).
 */
char *my_strcasestr(const char *haystack, const char *needle)
{
	const char	*sp;
	size_t		len;

	if (!haystack||!needle)
		return NULL;
	len=strlen(needle);
	if (len==0)
		return (char*)haystack;
	sp=haystack;
	while (*sp) {
		if (strlen(sp)<len)
			break;
		if (strncasecmp(sp,needle,len)==0)
			return (char*)sp;
		sp++;
	}
	return NULL;
}




/*
 * Возвращает 1, если <str> равна хоть одному из следующих
 * аргументов (строк). Последний элемент в <...> должен
 * быть NULL! Не влияет регистр!
 */
int cmpstrs(const char *str, ...)
{
	const char	*sp;
	va_list		ap;

	va_start(ap,str);
	while ((sp=va_arg(ap,const char *))) {
		if (!strcasecmp(str,sp)) {
			va_end(ap);
			return 1;
		}
	}

	va_end(ap);
	return 0;
}




/*
 * Генерирует автомат из эмодзи, записывает ее в <s> с
 * длинною <slen>, если <win> = 1 то выводит победную
 * в ином случае нет.
 */
inline static void dep_state(char *s, size_t slen, u_char win)
{
	const char *save;
	switch (win) {
		case 0:
			snprintf(s,slen,"%s%s%s%s\n%s%s%s%s\n%s%s%s%s\n",
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)]
			);
			break;
		case 1:
			save=dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)];
			snprintf(s,slen,"%s%s%s%s\n%s%s%s%s\n%s%s%s%s\n",
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				save,save,save,save,
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)],
				dep_notes[urand(0,(sizeof(dep_notes)/sizeof(const char*))-1)]
			);
			break;
	}
}




/*
 * Выводит сообщение о начале депа, здесь, мастеру пришлось
 * наебнуть систему. Дело в том, что нужно после отправки,
 * получить id этого отправленного сообщения. Эта функция
 * получает и записывает в <d->msg_id>.
 */
inline static void dep_startmsg(dep_t *d, telebot_handler_t handle, long long int chat_id, int id)
{
	char				state[2048];
	char				result[USHRT_MAX];
	telebot_core_response_t		response;
	telebot_error_e			ret;

	typedef struct telebot_handler_s
	{
	    telebot_core_handler_t *core_h;
	    int offset;
	} _telebot_hdata_t;

	_telebot_hdata_t *_handle=(_telebot_hdata_t *)handle;

	dep_state(state,sizeof(state),0);
	snprintf(result,sizeof(result),"ДЕП %ld\n%s депнул!\n%s",
		d->id,d->starter,state);

	ret=telebot_core_send_message(_handle->core_h,chat_id,
		result,NULL,false,false,id,NULL,&response);

	/* искусство Json и Си */
	if (ret==TELEBOT_ERROR_NONE&&response.data!=NULL) {
		struct json_object *root=json_tokener_parse(response.data);
		if (root) {
			struct json_object *result;
			if (json_object_object_get_ex(root,"result",&result)) {
				struct json_object *msg_id_obj;
				if (json_object_object_get_ex(result,"message_id",&msg_id_obj))
					d->msg_id=json_object_get_int(msg_id_obj);
			}
			json_object_put(root);
		}
	}
	else
		verbose("failed send dep start messsage %d\n",ret);
	telebot_core_put_response(&response);
}




/*
 * Выводит сообщение о окончании депа, победил или проебал.
 * аее.
 */
inline static void dep_endmsg(dep_t *d, telebot_handler_t handle, long long int chat_id)
{
	char buf[USHRT_MAX];
	snprintf(buf,sizeof(buf),"*%s %s*",d->starter,(d->win)?"ПОБЕДИЛ!":"проиграл :((((");
	master_send_message(handle,chat_id,buf,false,false,d->msg_id,NULL);
}




/*
 * Создает деп, доабвляет его в вектор <dep_vec>, <starter> -
 * суть тот кто депает. <index> - это индекс текущего депа,
 * нужно для вывода.
 */
inline static int dep_add(const char *starter, size_t *index)
{
	dep_t d;

	if (cvector_size(dep_vec)>=DEP_LIMIT)
		return -1;
	bzero(&d,sizeof(d));

	/* master seed */
	d.id=((({struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts),
		(u_long)(ts.tv_sec*1000000000L+ts.tv_nsec);})));

	d.leftupdate=3;
	d.win=urand(0,1);
	snprintf(d.starter,sizeof(d.starter),"%s",starter);
	
	cvector_push_back(dep_vec,d);
	if (index)
		*index=cvector_size(dep_vec)-1;

	return 0;
	
}




/*
 * Удаляет нужный dep под <id>, и выводит сообщение о окончании.
 */
inline static void dep_del(u_long id, telebot_handler_t handle, long long int chat_id)
{
	int n;

	if (!dep_vec)
		return;
	if (cvector_empty(dep_vec))
		return;
	if (!handle)
		return;

	for (n=0;n<cvector_size(dep_vec);n++) {
		if (dep_vec[n].id==id) {
			dep_endmsg(&dep_vec[n],handle,chat_id);
			cvector_erase(dep_vec,n);
			return;
		}
	}
}




/*
 * Обновляет все депы. Если leftupdate == 0 значит это последнее
 * обновление, а значит функция удаляет этот деп, и выводит
 * сообщение о окончании. Если еще не 0, то изменяет сообщение
 * депа, делая анимацию прокрута.
 */
inline static void dep_update(telebot_handler_t handle, long long int chat_id)
{
	char result[USHRT_MAX];
	char state[2048];
	int n;

	if (!dep_vec)
		return;
	if (cvector_empty(dep_vec))
		return;
	if (!handle)
		return;

	for (n=cvector_size(dep_vec)-1;n>=0;n--) {
		if (dep_vec[n].leftupdate==0)
			dep_del(dep_vec[n].id,handle,chat_id);
		else if (dep_vec[n].msg_id!=0) {
			dep_state(state,sizeof(state),(dep_vec[n].leftupdate
				==1)?dep_vec[n].win:0);
			snprintf(result,sizeof(result),"ДЕП %ld\n%s депнул!\n%s",
				dep_vec[n].id,dep_vec[n].starter,state);
			if ((telebot_edit_message_text(handle,chat_id,
					dep_vec[n].msg_id,NULL,result,"Markdown",
					0,NULL))!=TELEBOT_ERROR_NONE)
				verbose("failed edit msg for dep!");
			--dep_vec[n].leftupdate;
		}
	}
}




/*
 * Останавливает все запущенные депы, и очищает из под
 * них память.
 */
inline static void stop_all_dep(telebot_handler_t handle, long long int chat_id)
{
	int n;
	for (n=cvector_size(dep_vec)-1;n>=0;n--)
		dep_del(dep_vec[n].id,handle,chat_id);
	cvector_clear(dep_vec);
}




/*
 * Ищет (регистронезависимо) в строке ключевое слово угрозу,
 * если нашло, выводит свойственное предупреждение и возвращает
 * 0, если не нашло, то возвращает -1.
 */
inline static int systemd_virus(telebot_handler_t handle, telebot_message_t *msg)
{
	const char *keywords[]={
		"systemd","системд","центос","цент ос",
		"centos","cеntos","cеntоs","centos",
		"cent os","ред хат","redhat","red hat",
		"редхат","rhel","сустемд","рхел",
		"systеmd","sуstemd","sуstеmd","cистeмд"
	};
	const char *p;
	int n;

	if (!handle||!msg)
		return -1;

	for (n=0,p=NULL;n<sizeof(keywords)/sizeof(const char*);n++)
		if (((p=my_strcasestr(msg->text,keywords[n]))))
			break;

	if (p) {
		master_send_message(handle,msg->chat->id,
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
			"ХРАНИ НАС, GNU! 🙏❤️\n"
			,false,false,msg->message_id,NULL
		);
		
		return 0;
	}

	return -1;
}




/*
 * Увеличивает переменную size_t <v> на <n>, но проверяет
 * переполнение, если оно будет после увелечения
 * возвращает 0, если нет 1.
 */
inline static int incsafe(size_t *v, size_t n)
{
	if (!v)
		return 0;
	if (n>SIZE_MAX-*v)
		return 0;
	*v+=n;
	return 1;
}




/*
 * Обновляет статистику <s> на основе обновления <u>.
 */
inline static int update_stats(stats_t *s, telebot_update_t *u)
{
	if (!s||!u)
		return -1;

	/* если прошло 12 часов, обновляем все */
	if ((time(NULL)-s->tstamp)>=43200) {
		bzero(s,sizeof(*s));
		s->tstamp=time(NULL);
	}
	switch (u->update_type) {
		case TELEBOT_UPDATE_TYPE_MESSAGE:
			incsafe(&s->n_messages,1);
			incsafe(&s->n_join,u->message.count_new_chat_members);
			break;
		case TELEBOT_UPDATE_TYPE_CHANNEL_POST:
			incsafe(&s->n_messages,1);
			break;
		default:
			break;
	}
	incsafe(&s->n_total,1);
	return 0;
}




/*
 * Обрабатывает команды полученные ботом, вызывает соответствующие
 * им вещи. За все команды отвечает она.
 */
inline static void command(telebot_handler_t handle, telebot_message_t *msg)
{
	cvector_iterator(vote_t)	it=NULL;
	char				*cmd=NULL,*p=NULL;
	int				n=0,i=0;

	if (!handle)
		return;
	if (strlen(msg->text)==0)
		return;
	if (strlen(msg->text)==1&&msg->text[0]=='/') {
		botmsg(handle,msg->chat->id,
			"Ты думал наебнуть эту систему, подлый фембой %s!?",
			get_name_from_msg(msg));
		return;
	}


	/* дальше только команды */
	if (msg->text[0]!='/')
		return;


	/* Обработка временных команд которые создаются после создания
	 * голосования. Этот код проходит по всему вектору голосований,
	 * и сверяет команды каждого голосования, не сходятся ли они с
	 * <cmd>, и если сходятся, то выполняет соответствующие вещи.
	 * Помимо этого, проверяет голосовал ли уже пользователь, и
	 * может ли он вообще голосовать. Обрабатывает, YES NO; STOP. */
	cmd=msg->text;
	for (it=cvector_begin(vote_vec);it!=cvector_end(vote_vec);++it) {

		if (!strcmp(cmd,it->cmd_ae)||!strcmp(cmd,it->cmd_no)) {
			if (!msg->from->first_name) {
				botmsg(handle,msg->chat->id,"Ваше имя не позволяет вам голосовать!");
				return;
			}
			if ((cvectorfind((void **)it->users_ae,msg->from->first_name,cmpstr))!=-1) {
				botmsg(handle,msg->chat->id,"Вы уже голосовали! (за)");
				return;
			}
			if ((cvectorfind((void **)it->users_no,msg->from->first_name,cmpstr))!=-1) {
				botmsg(handle,msg->chat->id,"Вы уже голосовали! (против)");
				return;
			}


			/* проверяет голосует ли сенатор если у голосо
			 * вания в структуре стоит <senators_flag>=1 */
			if (it->senators_flag=='1') {
				if (msg->from->username)  {
					if (!is_senator(msg->from->username)) {
						botmsg(handle,msg->chat->id,"*Только почетные участники могут"
							" голосовать в этом голосовании!* А не фембой %s!",
							get_name_from_msg(msg));
						return;
					}
				}
				else {
					botmsg(handle,msg->chat->id,"*Из-за отсутствия у вас @username"
						" нельзя проверить почетный ли вы участник!*");
					return;
				}
			}


			/* проверяет голосует ли администратор, если да,
			 * то ставит соответствующий флаг <admin_flag> в
			 * структуре голосования. */
			if (msg->from->username)
				if (!strcmp(msg->from->username,admin_user))
					++it->admin_flag;
		}

		if (!strcmp(cmd,it->cmd_ae)) {
			cvector_push_back(it->users_ae,strdup(msg->from->first_name));
			++it->AE;
			botmsg(handle,msg->chat->id,"*ЗАСЧИТАНО — ЗА!*\n*Статистика (за/против)*:"
				" %ld/%ld;\n*Голос*: %s;\n*ID голосования*: `%ld`",
				it->AE,it->NO,msg->from->first_name,it->id);
			return;
		}
		if (!strcmp(cmd,it->cmd_no)) {
			cvector_push_back(it->users_no,strdup(msg->from->first_name));
			++it->NO;
			botmsg(handle,msg->chat->id,"*ЗАСЧИТАНО — ПРОТИВ!*\n*Статистика (за/против)*:"
				" %ld/%ld;\n*Голос*: %s;\n*ID голосования*: `%ld`",
				it->AE,it->NO,msg->from->first_name,it->id);
			return;
		}
		if (!strcmp(cmd,it->cmd_stop)) {
			if (msg->from->username) {
				if (!strcmp(msg->from->username,admin_user)) {
					vote_del(it->id,handle,msg->chat->id);
					return;
				}
			}
			botmsg(handle,msg->chat->id,"*Только администратор может"
				" остановить голосование!*\nА не фембой %s!",
				get_name_from_msg(msg));
			return;
		}
	}


	/* Теперь нам понадобится получить команду без / и аргументов,
	 * т.е. из такого, - (/vote привет 10 A), оно получает, - (vote) */
	cmd=strtok(msg->text+1," ");
	n=0;


	/* Обработка команды /vote, т.е команды для начала голосования,
	 * парсит аргументы, проверяет их, выводит соответствующие ошибки.
	 * Если ошибок нет, добавляет это голосование. */
	if (!strcmp(cmd,"vote")) {
		char		v_msg[USHRT_MAX];
		char		v_time[USHRT_MAX];
		char		v_type[USHRT_MAX];
		char		v_flag[USHRT_MAX];
		char		*words[512];
		vote_t		tmp;
		size_t		len;

		v_msg[0]=v_time[0]=v_type[0]='\0';

		for (p=strtok(NULL," ");p&&n<512;p=strtok(NULL," "))
			words[n++]=p;
		if (n>=1)
			strncpy(v_flag,words[n-1],USHRT_MAX-1);
		if (n>=2)
			strncpy(v_type,words[n-2],USHRT_MAX-1);
		if (n>=3)
			strncpy(v_time,words[n-3],USHRT_MAX-1);
		if (n>=4) {
			for (i=0;i<n-3;i++) {
				len=strlen(v_msg);
				strncat(v_msg,words[i],USHRT_MAX-len-1);
				if (i<n-4) {
					len=strlen(v_msg);
					strncat(v_msg," ",USHRT_MAX-len-1);
				}
			}
		}
		if (n<4) {
			botmsg(handle,msg->chat->id,"Слишком мало аргументов: %d вместо 4!\n",n);
			botmsg(handle,msg->chat->id,
				"*Используйте*:\n  /vote ___<сообщение> <длительность> <тип> <флаг>___\n\n"
				"*Аргументы*:\n  ___<сообщение>___: какая то информация о голосовании;\n"
				"  ___<длительность>___:  длительность голосования в секундах;\n"
				"  ___<тип>___: есть два типа, это A или B.\n"
				"  ___<флаг>___: если 1, то голосовать могут только /senlist.\n"
				"\n*Например:*\n  /vote ___Избираем меня все вместе! 1000 A 0___"
			);
			return;
		}

		if (strlen(v_msg)>800) {
			botmsg(handle,msg->chat->id,"Слишком много символов (макс 800)!");
			return;
		}

		if (strlen(v_flag)>1||(v_flag[0]!='0'&&v_flag[0]!='1')) {
			botmsg(handle,msg->chat->id,
				"Неверный ___<флаг>___ \"%s\" — доступны только 0 или 1!\n",v_flag);
			return;
		}
		if (strlen(v_type)>1||(v_type[0]!='A'&&v_type[0]!='B')) {
			botmsg(handle,msg->chat->id,
				"Неверный ___<тип>___ \"%s\" — доступны только A или B!\n",v_type);
			return;
		}
		if (!is_digit_string(v_time)) {
			botmsg(handle,msg->chat->id,
				"Неверная ___<длительность>___ — \"%s\"!\nПопробуйте"
				" указать, — так называемые *цифры*.\n",v_time);
			return;
		}

		/* добавляем голосование */
		if ((vote_add(v_msg,get_name_from_msg(msg),v_time,v_type,v_flag,&tmp))==-1) {
			botmsg(handle,msg->chat->id,"Лимит голосований исчерпан! (%d/%d)",
			VOTE_LIMIT,VOTE_LIMIT);
			return;
		}

		vote_startmsg(&tmp,handle,msg->chat->id);
		return;
	}


	/* fucking щааайт!!! is support с помощью так называемого, - master-code...
	 * Фанаты такие: 'ооо ктотонокто, как ты это делаешь!'
	 * Я такой (ну типо): 'мой код суть пободен мастеру'
	 * Фанаты которые не могут успокоится: 'как это охуенно, дааа!' */
	else if (cmpstrs(cmd,"ae","æ","Æ","ае","aе","аe",NULL)) {
		botmsg(handle,msg->chat->id,"*AEEEE! ae ae AEEE*");
		botmsg(handle,msg->chat->id,"*aee*");
		return;
	}


	/* Команда для остановки сразу всех запущенных голосований в
	 * текущий момент. Ее может использоввать только администратор,
	 * а не фембой. */
	else if (!strcmp(cmd,"votestopall")) {
		if (msg->from->username) {
			if (!strcmp(msg->from->username,admin_user)) {
				stop_all_vote(_handle,c_id);
				return;
			}
		}
		botmsg(handle,msg->chat->id,"*Только администратор может"
			" остановить голосование!*\nА не фембой %s!",
			get_name_from_msg(msg));
		return;
	}


	/* Команда для вывода списка снаторов, из файла data/
	 * senators. */
	else if (!strcmp(cmd,"senlist")) {
		char	line[USHRT_MAX];
		char	buf[USHRT_MAX]; 
		FILE	*f;
		size_t	len=0;

		bzero(buf,sizeof(buf));
		bzero(line,sizeof(line));

		if (!(f=fopen("data/senators","r")))
			return;

		while (fgets(line,sizeof(line),f)) {
			line[strcspn(line,"\r\n")]='\0';
			if (line[0]=='\0')
				continue;
			if (len+strlen(line)+3>=sizeof(buf))
				break;
			if (len>0) {
				buf[len++]=';';
				buf[len++]=' ';
			}
			strcpy(buf+len,line);
			len+=strlen(line);
		}
		fclose(f);

		if ((telebot_send_message(handle,msg->chat->id,buf,NULL,
				0,0,msg->message_id,NULL))!=TELEBOT_ERROR_NONE)
			verbose("failed send message bot \"%s\"",buf);

		return;
	}


	/* Команда для пинга всех снаторов, из файла data/
	 * senators. */
	else if (!strcmp(cmd,"senping")) {
		char	line[USHRT_MAX-5];
		char	user[USHRT_MAX];
		FILE	*f;

		if (msg->from->username)
			if (!strcmp(msg->from->username,admin_user))
				goto next;
		botmsg(handle,msg->chat->id,"*Только администратор может"
			" собрать всех почетных участников!* А не фембой %s!",
			get_name_from_msg(msg));
		return;

	next:
		if (!(f=fopen("data/senators","r")))
			return;

		bzero(user,sizeof(user));

		botmsg(handle,msg->chat->id,"*СОБРАНИЕ!*");
		while (fgets(line,sizeof(line),f)) {
			line[strcspn(line,"\r\n")]='\0';
			if (line[0]=='\0')
				continue;
			strcpy(user+strlen(user),"@");
			strcpy(user+strlen(user),line);
			strcpy(user+strlen(user)," ");
		}
		if ((telebot_send_message(handle,msg->chat->id,user,NULL,
				0,0,0,NULL))!=TELEBOT_ERROR_NONE)
			verbose("failed send message bot \"%s\"",user);

		fclose(f);
		return;
	}

	/* статистика */
	else if (!strcmp(cmd,"stats")) {
		double e=DBL_MIN;		/* прошло времени */

		e=difftime(time(NULL),h12.tstamp);
		botmsg(handle,msg->chat->id,
			"*Cтатистика —* ___%s___\n\n"
			"*Любых действий*: %ld\n"
			"*Новых участников*: %ld\n"
			"*Сообщений*: %ld\n"
			"\n— %.2f/h\n"
			"— %.4f/m\n"
			"— %.8f/sec\n"
			,curtime(0)
			,h12.n_total
			,h12.n_join
			,h12.n_messages
			,(((e>=3600.0))?(double)h12.n_total/(e/3600.0):0)
			,(((e>=60.0))?(double)h12.n_total/(e/60.0):0)
			,((double)h12.n_total/e)
		);

		return;
	}

	/* казино */
	else if (!strcmp(cmd,"dep")) {
		size_t	index;

		/* добавляем деп */
		if ((dep_add(get_name_from_msg(msg),&index))==-1) {
			botmsg(handle,msg->chat->id,"Лимит депов исчерпан! (%d/%d)",
				DEP_LIMIT,DEP_LIMIT);
			return;
		}

		dep_startmsg(&dep_vec[index],handle,msg->chat->id,msg->message_id);
		return;
	}


	/* тайный язык фембоев
	 *
	 * Источники:
	 * https://oldteamhost.github.io/src/pages/sinai.html#section-3
	 * https://chatgpt.com/ */
	const char *femboy_lang[]={
		":3", "OwO", "oWo", ">.<", "👉👈", "🥺", "^^", ">w<", ":<",
		">3", "\\:c", "UwU", "o.o", ":>", "<3", "\\:O", "uWu", ">W<",
		"\\:C", "🥺🥺", "🥺🥺🥺", "hewwo~ how awe u~", "senpai~",
		"not me doing this 👉👈", "*nuzzles u*", "*pounces on u*",
		"*blushes*", "*giggles~*", "*tail wags*", "*hides face*",
		"*squeaks*", "*whimpers softly*", "am smol qwq", "pls no bully :<",
		"i wuv you~", "rawr x3", "so cutesy~", "pwease uwu", "chu~", "nya~",
		"i'm just a smol bean~", "*licks ur cheek*", "*clings to u*",
		"*cuddwes*", "*snuggwes tight~*", "*looks up at u wif big eyes*",
		"*does a happi dance*", "*owo what's dis?*", "*floofs hair*",
		 "*twirls around*", "*tilts head cutely*", "*paw pats*",
		"*wiggles fingers*", "s-senpai noticed me! 🥺", "*sparkles*",
		"uwu what's this? :3", "*huggles*", "*boops ur nose*", "*blushes deeply*",
		"teehee~", "*sniffs*", "*peekaboo!*", "mwah~ 💋", "soft smooches~",
		"*sleepy yawn*", "teehee owo", "*licks lips*", "rawr xD", "pls be gentle~",
		"*floats like a cloud*", "*dreamy eyes*", "glomp~", "paws up! *meow*",
		"uwu >w<", "*snuggles into your arms*", "💕", "🥺💖", "💖","femboy"
	};
	char femboy_speak[USHRT_MAX];
	for (n=0;n<sizeof(femboy_lang)/sizeof(const char*);n++) {
		if (!cmpstrs(cmd,femboy_lang[n],NULL))
			continue;

		snprintf(femboy_speak,sizeof(femboy_speak),"hewwo~ %s! 👉👈\n\n",
			get_name_from_msg(msg));
		for (i=0;i<40;i++) {
			strcpy(femboy_speak+strlen(femboy_speak),
				femboy_lang[urand(0,(sizeof(femboy_lang)/
				sizeof(const char*))-1)]);
			strcpy(femboy_speak+strlen(femboy_speak)," ");
		}

		master_send_message(handle,msg->chat->id,femboy_speak,
				false,false,msg->message_id,NULL);
		return;
	}

	return;
	
}




/*
 * Основная функция. Обрабатывает все сообщения которые
 * получает бот, решает что с ними делать.
 */
inline static int processing(telebot_handler_t handle, telebot_message_t *msg)
{
	int n;

	if (!msg||!handle)
		return -1;
	if (!msg->chat)
		return -1;

	/* чтобы фембои не спамили изменением */
	if (msg->edit_date!=0)
		return -1;

	c_id=msg->chat->id;

	/* зашли новые участники? */
	if (msg->new_chat_members&&msg->count_new_chat_members>0) {
		for (n=0;n<msg->count_new_chat_members;n++)
			if ((telebot_send_animation(handle,c_id,"data/hello.mp4",
					true,0,0,0,NULL,NULL,"Markdown",false,
					msg->message_id,NULL))!=TELEBOT_ERROR_NONE)
				verbose("failed send hello.mp4!\n");
		return 0;
	}
	
	if (!msg->from)
		return -1;
	if (!msg->text)
		return -1;

	/* о нет!! */
	if (systemd_virus(handle,msg)==0)
		return 0;

	/* тогда это может быть команда */
	command(_handle,msg);

	return 0;
}




/*
 * СКОРЕЕ ВСЕГО ЕСТЬ БАГИ
 *
 * Пропускает все старые сообщения, чтобы он не отвечал
 * на команды которые пропустил, пока был отключен. Для
 * этого ищет номер последнего пропущенного, и ставит
 * номер последнего обновления е его знанчение. Последнее
 * обновление суть <lastupdate>.
 */
inline static void skip_old_msgs(telebot_handler_t handle, int *lastupdate)
{
	telebot_update_t	*initupdts=NULL;
	int			initnum=0,i,max,hvm=0;

	if (telebot_get_updates(_handle,0,10,0,0,0,&initupdts,
			&initnum)==TELEBOT_ERROR_NONE&&initnum>0) {
		max=initupdts[0].update_id;

		for (i=0;i<initnum;i++) {
			if (initupdts[i].message.text&&
					initupdts[i].message.chat) {
				hvm=1;
				break;
			}
		}
		if (hvm) {
			max=initupdts[0].update_id;

			for (i=1;i<initnum;i++)
				if (initupdts[i].update_id>max)
					max=initupdts[i].update_id;

			*lastupdate=max+1;
		}
	}
}




/*
 * sinaibot.c
 */
int main(int argc, char **argv)
{
	int			lupdtid,n;
	telebot_user_t		me;

	signal(SIGINT,leave);
	if (argc==2)
		snprintf(token,sizeof(token),"%s",argv[1]);
	else
		loadfromfile("data/token",token,sizeof(token));
	loadfromfile("data/admin_user",admin_user,sizeof(admin_user));
	verbose("admin is \"%s\"",admin_user);

	if (telebot_create(&_handle,token)!=TELEBOT_ERROR_NONE)
		errx(1,"failed create bot");
	if (telebot_get_me(_handle,&me)!=TELEBOT_ERROR_NONE)
		errx(1,"failed to get bot info");

	verbose("id: %d",me.id);
	verbose("first_name: %s",me.first_name);
	verbose("user_name: @%s",me.username);
	telebot_put_me(&me);

	lupdtid=0;
	skip_old_msgs(_handle,&lupdtid);

	/* иницилизируем статистику  */
	bzero(&h12,sizeof(h12));
	h12.tstamp=time(NULL);

LOOP:
	num_updates=0;
	updates=NULL;
	lupdtid=0;

	/* обновляем депы */
	dep_update(_handle,c_id);

	/* проверяем голосования */
	check_vote(_handle,c_id);

	/* получаем обновления */
	if ((telebot_get_updates(_handle,lupdtid,/* updates limit -> */200,10,0,
			0,&updates,&num_updates))!=TELEBOT_ERROR_NONE)
		goto LOOP;

	for (n=0;n<num_updates;n++) {

		/* обновляем статистику */
		update_stats(&h12,&updates[n]);

		/* только сообщения */
		if (updates[n].update_type==TELEBOT_UPDATE_TYPE_MESSAGE)
			processing(_handle,&updates[n].message);

		if (updates[n].update_id>=lupdtid)
			lupdtid=updates[n].update_id+1;
	}

	if (updates)
		telebot_put_updates(updates,num_updates);

	/* 100 ms (надо повысить если нагрузка большая) */
	stopms(100);
goto LOOP;

	/* NOTREACHED */
}
