#ifndef SIA_H
#define SIA_H

#include "include.h"

#define SIAMAXLINELEN	  4096
#define SIAMAXCATEGORYLEN 128
#define SIAMAXWORDLEN	  512
#define SIAMAXRANGELEN	  1024

typedef struct {
	int64_t ahoid;
	char word[SIAMAXWORDLEN];
	char category[SIAMAXLINELEN];
	double weight;
	UT_hash_handle hh;
} dictitem;

typedef struct {
	sqlite3 *session;
	struct {
		dictitem *dict;
		struct ahocorasick aho;
		dictitem **idmap;
		size_t idmaplen;
	} database;
} SIA;

SIA *SIAOPEN(const char *filename, const char *database);
bool SIAUPDATE(SIA *sia, const char *json);
bool SIAREPORT(SIA *sia, FILE *out, const char *type, char **args,
    size_t numargs);
void SIACATEGORIESLIST(SIA *sia, FILE *out);
SIA *SIACLOSE(SIA *sia);

#endif
