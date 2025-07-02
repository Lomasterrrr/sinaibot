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
#include <stdint.h>
#include <signal.h>
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
#include <telebot.h>

#include "cvector.h"

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
	const char *type, vote_t *tmp);
inline static void vote_del(u_long id, telebot_handler_t handle, long long int chat_id);
inline static void vote_startmsg(vote_t *v, telebot_handler_t handle, long long int chat_id);
inline static void vote_endmsg(vote_t *v, telebot_handler_t handle, long long int chat_id);




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
		telebot_destroy(_handle);
	}
	if (updates)
		telebot_put_updates(updates,num_updates);
	puts("\n");
	verbose("LEAVE FROM SINAI BOT!!");
	exit(0);
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
 * Отправляет с бота указанное сообщение; поддерживает также
 * форматирование.
 */
inline static void botmsg(telebot_handler_t handle, long long int chat_id,
		const char *fmt, ...)
{
	char message[USHRT_MAX];
	telebot_error_e ret;
	va_list args;

	va_start(args,fmt);
	vsnprintf(message,sizeof(message),fmt,args);
	va_end(args);

	if ((ret=telebot_send_message(handle,chat_id,message,"Markdown",
			 false, false, 0, NULL))!=TELEBOT_ERROR_NONE)
		verbose("failed send message bot \"%s\" ret=%d\n", message,ret);
}




/*
 * Создает объект vote_t из указанных в аргументах значений.
 * возвращает -1 если ошибка и 0 если все хорошо. Также
 * созданных объект добавляет в вектор vote_vec. А еще
 * копирует его по адресу <tmp> (это чтобы потом вывести).
 */
inline static int vote_add(const char *msg, const char *starter,
		const char *timel, const char *type, vote_t *tmp)
{
	vote_t v;

	if (cvector_size(vote_vec)>=VOTE_LIMIT)
		return -1;

	memset(&v,0,sizeof(v));
	str_to_size_t(timel,&v.timel,0,SIZE_MAX);
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
		"*Тип голосования*: %c;\n"
		"*Длительность*: %ld секунд(а);\n"
		"*ID голосования*: `%ld`;\n"
		"\n— ___%s___\n"
		,v->msg,v->cmd_ae,v->cmd_no,v->starter,v->type,
		v->timel,v->id,curtime(v->timestamp)
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
		"*ОКОНЧАНИЕ ГОЛОСОВАНИЯ — *`%ld`* В *"
		"___%s___*!*\n\n"
		"*Были за* — %s\n"
		"*Были против* — %s\n"
		"*Голосовал ли админ?* — %s;\n"
		"\n*Результаты (за/против)*\n — ___%ld / %ld___;"
		,v->id,curtime(0),buf1,buf2,((v->admin_flag)?"да":"нет"),
		v->AE,v->NO
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
 * Загружает одну строчку в <buf> с размером <buflen> из
 * файла <filename>. Если была ошибка, завершает программу.
 * Удаляет \n, если он есть в конце строчки, это нужно чтобы
 * считать токен корректно.
 */
inline static void loadfromfile(const char *filename, char *buf, size_t buflen)
{
	size_t n;
	FILE *f;

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
 * Основная функция. Обрабатывает все команды полученные ботом.
 *  вызывает соответствующие им вещи.
 */
inline static void command(telebot_handler_t handle, telebot_message_t *msg)
{
	cvector_iterator(vote_t)	it;
	char				*cmd,*p;
	int				n,i;

	if (!msg||!handle)
		return;
	if (!msg->text)
		return;
	if (!msg->chat)
		return;
	if (!msg->from)
		return;
	if (strlen(msg->text)==0)
		return;
	if (strlen(msg->text)==1&&msg->text[0]=='/') {
		botmsg(handle,msg->chat->id,"Ты думал наебнуть эту систему, подлый фембой %s!?",
			get_name_from_msg(msg));
		return;
	}
	if (msg->text[0]!='/')	/* only commands */
		return;

	/*
	 * ВРЕМЕННЫЕ КОМАНДЫ ДЛЯ ГОЛОСОВАНИЙ
	 * YES,NO,STOP
	 */
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
				" оставить голосование!*\nА не фембой %s!",
				get_name_from_msg(msg));
			return;
		}
	}
	
	cmd=strtok(msg->text+1," ");
	n=0;

	if (!strcmp(cmd,"vote")) {
		char		v_msg[USHRT_MAX];
		char		v_time[USHRT_MAX];
		char		v_type[USHRT_MAX];
		char		*words[512];
		vote_t		tmp;

		v_msg[0]=v_time[0]=v_type[0]='\0';

		for (p=strtok(NULL," ");p;p=strtok(NULL," "))
			words[n++]=p;
		if (n>=1)
			strncpy(v_type,words[n-1],USHRT_MAX-1);
		if (n>=2)
			strncpy(v_time,words[n-2],USHRT_MAX-1);
		if (n>=3) {
			for (i=0;i<n-2;i++) {
				strcat(v_msg,words[i]);
				if (i<n-3)
					strcat(v_msg," ");
			}
		}
		if (n<3) {
			botmsg(handle,msg->chat->id,"Слишком мало аргументов: %d вместо 3!\n",n);
			botmsg(handle,msg->chat->id,
				"*Используйте*:\n  /vote ___<сообщение> <длительность> <тип>___\n\n"
				"*Аргументы*:\n  ___<сообщение>___: какая то информация о голосовании;\n"
				"  ___<длительность>___:  длительность голосования в секундах;\n"
				"  ___<тип>___: есть два типа, это A или B.\n"
				"\n*Например:*\n  /vote ___Избираем меня все вместе! 1000 A___"
				);
			return;
		}

		if (strlen(v_msg)>800) {
			botmsg(handle,msg->chat->id,"Слишком много символов (макс 800)!");
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
		if ((vote_add(v_msg,get_name_from_msg(msg),v_time,v_type,&tmp))==-1) {
			botmsg(handle,msg->chat->id,"Лимит голосований исчерпан!",cmd);
			return;
		}

		vote_startmsg(&tmp,handle,msg->chat->id);
		return;
	}

	/*
	 * fucking щааайт!!! is support с помощью так называемого, - говно-code...
	 *
	 * Фанаты такие: 'ооо ктотонокто, как ты это делаешь!'
	 * Я такой (ну типо): 'мой код суть пободен мастеру'
	 * Фанаты которые не могут успокоится: 'как это охуенно, дааа!'
	 */
	else if (!strcmp(cmd,"ae")||!strcmp(cmd,"aE")||
			!strcmp(cmd,"Ae")||!strcmp(cmd,"AE")||
			!strcmp(cmd,"æ")||!strcmp(cmd,"Æ")) {
		botmsg(handle,msg->chat->id,"*AEEEE! ae ae AEEE*");
		botmsg(handle,msg->chat->id,"*aee*");
		return;
	}

	/* это у другого команда */
	else if (!strcmp(cmd,"ai")) {
		return;
	}

/*
	В РАЗРАБОТКЕ ГЕНИАЛЬНЫЙ ПЛАН

	else if (!strcmp(cmd,"fuckingshit")) {
		p=strtok(NULL," ");
		if (!p)
			botmsg(handle,msg->chat->id,"Слишком мало аргументов: %d вместо 3!\n",0);
	}
*/


	/*
	 * ОШИБКА
	 * на англиском: oshibka
	 */
	else {
		botmsg(handle,msg->chat->id,"Команда \"%s\" не найдена!",cmd);
	}

	return;
	
}




/*
 * Пропускает все старые сообщения, чтобы он не отвечал
 * на команды которые пропустил, пока был отключен. Для
 * этого ищет номер последнего пропущенного, и ставит
 * номер последнего обновления е его знанчение. Последнее
 * обновление суть <lastupdate>.
 */
inline static void skip_old_msgs(telebot_handler_t handle, int *lastupdate)
{
	telebot_update_t	*initupdts=NULL;
	int			initnum=0,i,max,hvm;

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
	telebot_message_t	msg;

	signal(SIGINT,leave);
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

	for (;;) {
		check_vote(_handle,c_id);
		if ((telebot_get_updates(_handle,lupdtid,/* updates limit -> */50,15,0,
				0,&updates,&num_updates))!=TELEBOT_ERROR_NONE)
			continue;
		for (n=0;n<num_updates;n++) {

			msg=updates[n].message;
			if (!msg.text)
				continue;
			if (!msg.chat)
				continue;
			if (!msg.from)
				continue;

			/* чтобы фембои не спамили изменением */
			if (msg.edit_date!=0)
				continue;

			c_id=msg.chat->id;
			command(_handle,&msg);

			if (updates[n].update_id>=lupdtid)
				lupdtid=updates[n].update_id+1;
		}
		telebot_put_updates(updates,num_updates);
		sleep(1);
	}

	leave(0);
	/* NOTREACHED */
}
