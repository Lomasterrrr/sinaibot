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


/*	ГОЛОСОВАНИЯ	*/
#define VOTE_LIMIT 3	/* максимум голосований */
typedef struct __vote_t {
	size_t		timel;	/* двительность в сек */
	u_long		id;	/* номер голосования */
	u_char		type;	/* тип голосования */
	size_t		AE;	/* за */
	size_t		NO;	/* против */
	u_char		admin_flag;	/* голосовал ли администратор? */
	time_t		timestamp;	/* точка старта */
	/* команды для за/против/остановка */
	char	cmd_ae[512],cmd_no[512],cmd_stop[512];
	/* голосовавшие за/против */
	cvector(char *) users_ae;
	cvector(char *) users_no;
} vote_t;
cvector(vote_t)		vote_vec=NULL;		/* голосования */
inline static int add_vote(const char *timel, u_long id, const char *type);
inline static void del_vote(u_long id, telebot_handler_t handle, long long int chat_id);
inline static void endmsg_vote(vote_t *v, telebot_handler_t handle, long long int chat_id);
inline static void stop_all_vote(telebot_handler_t handle, long long int chat_id);


inline static void verbose(const char *fmt, ...)
{
	va_list ap;

	va_start(ap,fmt);
	if (fmt) {
		(void)fprintf(stderr,"LOG\t");
		(void)vfprintf(stderr,fmt,ap);
	}
	(void)fputc(0x0a,stderr);
	va_end(ap);
}

inline static const char *curtime(void)
{
	static char date_str[512];
	struct tm *tm_info;
	time_t now;

	now=time(NULL);
	tm_info=localtime(&now);
	strftime(date_str,sizeof(date_str),
		"%Y-%m-%d %a %H:%M:%S %Z",tm_info);

	return date_str;
}

inline static void free_string(void *str)
{
	if (str)
		free(*(char **)str);
}

inline static noreturn void leave(int sig)
{
	(void)sig;
	stop_all_vote(_handle,c_id);
	if (_handle)
		telebot_destroy(_handle);
	if (updates)
		telebot_put_updates(updates,num_updates);
	puts("\n");
	verbose("LEAVE FROM SINAI BOT!!");
	exit(0);
}

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

	/* exit(eval); */
	leave(eval);
}

inline static void str_to_size_t(const char *str, size_t *out, size_t min, size_t max)
{
	unsigned long long val;
	char *endptr;

	assert(str||*str||out);
	while (isspace((u_char)*str))
		str++;
	if (*str=='-')
		verbose("only positive numbers");
	errno=0;
 	val=strtoull(str,&endptr,10);
	if (errno==ERANGE||val>(unsigned long long)SIZE_MAX)
		verbose("failed convert %s in num",str);
	while (isspace((u_char)*endptr))
		endptr++;
	if (*endptr!='\0')
		verbose("failed convert %s in num",str);
	if (val<min||val>max)
		verbose("failed convert %s in num; range failure (%ld-%llu)",
			str,min,max);
	*out=(size_t)val;
}

inline static void botmsg(telebot_handler_t handle, long long int chat_id, const char *fmt, ...)
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

inline static int add_vote(const char *timel, u_long id, const char *type)
{
	vote_t v;

	if (cvector_size(vote_vec)>=VOTE_LIMIT)
		return -1;

	memset(&v,0,sizeof(v));
	str_to_size_t(timel,&v.timel,0,SIZE_MAX);
	v.type=type[0];
	v.id=id;
	v.timestamp=time(NULL);
	cvector_init(v.users_ae,1,free_string);
	cvector_init(v.users_no,1,free_string);
	snprintf(v.cmd_ae,sizeof(v.cmd_ae),"/YES%ld",id);
	snprintf(v.cmd_no,sizeof(v.cmd_no),"/NO%ld",id);
	snprintf(v.cmd_stop,sizeof(v.cmd_stop),"/STOP%ld",id);

	cvector_push_back(vote_vec,v);
	return 0;
}

inline static const char *joinvec(char *buf, size_t buflen, cvector(char *) vec)
{
	size_t i;
	buf[0]='\0';
	for (i=0;i<cvector_size(vec);++i) {
		if (strlen(buf)+strlen(vec[i])+2>=buflen)
			break;
		strcat(buf,vec[i]);
		/* последний пробел даже не видно в тг, похуй */
		strcat(buf,"; ");
	}
	return buf;
}


inline static void endmsg_vote(vote_t *v, telebot_handler_t handle, long long int chat_id)
{
	char buf1[BUFSIZ],buf2[BUFSIZ];
	
	if (!cvector_empty(v->users_ae))
		joinvec(buf1,sizeof(buf1),v->users_ae);
	else
		snprintf(buf1,sizeof(buf1),"никто;");
	if (!cvector_empty(v->users_no))
		joinvec(buf2,sizeof(buf2),v->users_no);
	else
		snprintf(buf2,sizeof(buf2),"никто;");

	botmsg(handle,chat_id,
		"*ГОЛОСОВАНИЕ* `%ld` * — БЫЛО ОКОНЧЕНО!*\n\n"
		"*Голосовали ЗА* — %s\n"
		"*Голосовали ПРОТИВ* — %s\n"
		"*Голосовал ли админ?* — %s;\n"
		"\n*Результаты (за/против)*\n — ___%ld / %ld___;"
		,v->id,
		buf1,buf2,
		((v->admin_flag)?"да":"нет"),
		v->AE,v->NO
	);
}

inline static void del_vote(u_long id, telebot_handler_t handle, long long int chat_id)
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
			if (!cvector_empty(vote_vec[n].users_ae))
				cvector_free(vote_vec[n].users_ae);
			if (!cvector_empty(vote_vec[n].users_no))
				cvector_free(vote_vec[n].users_no);
			cvector_erase(vote_vec,n);
			return;
		}
	}

}

inline static void check_vote(telebot_handler_t handle, long long int chat_id)
{
	int n;
	for (n=cvector_size(vote_vec)-1;n>=0;n--) {
		if (vote_vec[n].type=='B') {
			if (vote_vec[n].NO>0) {
				endmsg_vote(&vote_vec[n],handle,chat_id);
				botmsg(handle,chat_id,"*Остановка голосования n.o*: `%ld`!",vote_vec[n].id);
				del_vote(vote_vec[n].id,handle,chat_id);
				continue;
			}
		}
		if (time(NULL)>=vote_vec[n].timestamp+vote_vec[n].timel) {
			endmsg_vote(&vote_vec[n],handle,chat_id);
			botmsg(handle,chat_id,"*Остановка голосования n.o*: `%ld`!",vote_vec[n].id);
			del_vote(vote_vec[n].id,handle,chat_id);
		}
	}
}

inline static void stop_all_vote(telebot_handler_t handle, long long int chat_id)
{
	int n;
	for (n=cvector_size(vote_vec)-1;n>=0;n--)
		del_vote(vote_vec[n].id,handle,chat_id);
	cvector_clear(vote_vec);
}

inline static int is_digit_string(const char *str)
{
	char *endptr;
	errno=0;
	(void)strtol(str,&endptr,10);
	return *endptr=='\0'&&errno==0;
}

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

inline static void command(telebot_handler_t handle, telebot_message_t *msg)
{
	cvector_iterator(char *)	it1;
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
					msg->from->first_name);
		return;
	}
	if (msg->text[0]!='/')	/* only commands */
		return;

	/*
	 * ВРЕМЕННЫЕ КОМАНДЫ
	 * YES,NO,STOP
	 */
	cmd=msg->text;
	for (it=cvector_begin(vote_vec);it!=cvector_end(vote_vec);++it) {
		if (!strcmp(cmd,it->cmd_ae)||!strcmp(cmd,it->cmd_no)) {
			if (!msg->from->first_name) {
				botmsg(handle,msg->chat->id,"Ваше имя не позволяет вам голосовать!");
				return;
			}
			for (it1=cvector_begin(it->users_ae);it1!=cvector_end(it->users_ae);++it1) {
				if (!strcmp(*it1,msg->from->first_name)) {
					botmsg(handle,msg->chat->id,"Вы уже голосовали! (за)");
					return;
				}
			}
			for (it1=cvector_begin(it->users_no);it1!=cvector_end(it->users_no);++it1) {
				if (!strcmp(*it1,msg->from->first_name)) {
					botmsg(handle,msg->chat->id,"Вы уже голосовали! (против)");
					return;
				}
			}
			if (msg->from->username)
				if (!strcmp(msg->from->username,admin_user))
					++it->admin_flag;
		}

		if (!strcmp(cmd,it->cmd_ae)) {
			cvector_push_back(it->users_ae, strdup(msg->from->first_name));
			++it->AE;
			botmsg(handle,msg->chat->id,"*ЗАСЧИТАНО — ЗА!*\n*Статистика (за/против)*:"
				" %ld/%ld;\n*Голос*: %s;\n*ID голосования*: `%ld`",
				it->AE,it->NO,msg->from->first_name,it->id);
			return;
		}
		if (!strcmp(cmd,it->cmd_no)) {
			cvector_push_back(it->users_no, strdup(msg->from->first_name));
			++it->NO;
			botmsg(handle,msg->chat->id,"*ЗАСЧИТАНО — ПРОТИВ!*\n*Статистика (за/против)*:"
				" %ld/%ld;\n*Голос*: %s;\n*ID голосования*: `%ld`",
				it->AE,it->NO,msg->from->first_name,it->id);
			return;
		}
		if (!strcmp(cmd,it->cmd_stop)) {
			if (msg->from->username) {
				if (!strcmp(msg->from->username,admin_user)) {
					endmsg_vote(it,handle,msg->chat->id);
					botmsg(handle,msg->chat->id,"*Остановка голосования n.o*: `%ld`!",it->id);
					del_vote(it->id,handle,msg->chat->id);
					return;
				}
			}
			botmsg(handle,msg->chat->id,"*Только администратор может"
				" оставить голосование!*\nА не фембой — %s или %s или %s!",
				msg->from->username,msg->from->first_name,msg->from->last_name);
			return;
		}
	}
	
	cmd=strtok(msg->text+1," ");
	n=0;




	if (!strcmp(cmd,"vote")) {
		char	v_msg[USHRT_MAX];
		char	v_time[USHRT_MAX];
		char	v_type[USHRT_MAX];
		char	*words[512];
		size_t	len;		
		u_long	id;

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
		if (strlen(v_msg)>500) {
			botmsg(handle,msg->chat->id,"Слишком много символов (макс 500)!");
			return;
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

		len=strlen(v_type);
		if (len>1||(v_type[0]!='A'&&v_type[0]!='B')) {
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

		verbose("vote message: %s",v_msg);
		verbose("vote time: %s",v_time);
		verbose("vote type: %s",v_type);

		/* master seed */
		id=((({struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts),
			(u_long)(ts.tv_sec*1000000000L+ts.tv_nsec);})));
		if (id==(time_t)(-1)||id==0)
			return;

		verbose("vote id: %lu",id);

		/* добавляем голосование */
		if ((add_vote(v_time,id,v_type))==-1) {
			botmsg(handle,msg->chat->id,"Лимит голосований исчерпан!",cmd);
			return;
		}

		botmsg(handle,msg->chat->id,
			"*Голосование* — \"%s\";\n"
			"\n*Проголосовать за*:\n  `/YES%ld`;\n"
			"*Проголосовать против*:\n  `/NO%ld`;\n\n"
			"*Тип голосования*: %s;\n"
			"*Длительность*: %s секунд(а);\n"
			"*ID голосования*: `%ld`;\n"
			"___%s___\n"
			,v_msg,id,id,v_type,v_time,id, curtime()
		);
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
		botmsg(handle,msg->chat->id,"*ae*");
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

int main(int argc, char **argv)
{
	int			lupdtid,n;
	telebot_user_t		me;
	telebot_message_t	msg;

	signal(SIGINT,leave);
	loadfromfile("data/token",token,sizeof(token));
	loadfromfile("data/admin_user",admin_user,sizeof(admin_user));

	verbose("token is \"%s\"",token);
	verbose("admin is \"%s\"",admin_user);

	if (telebot_create(&_handle,token)!=TELEBOT_ERROR_NONE)
		errx(1,"failed create bot");
	if (telebot_get_me(_handle,&me)!=TELEBOT_ERROR_NONE) {
		telebot_destroy(_handle);
		errx(1,"failed to get bot info");
	}

	verbose("id: %d",me.id);
	verbose("first_name: %s",me.first_name);
	verbose("user_name: @%s",me.username);
	telebot_put_me(&me);

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
