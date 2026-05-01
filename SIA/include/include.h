#ifndef SIA_INCLUDE_H
#define SIA_INCLUDE_H

#define _GNU_SOURCE   /* for asprintf() */
#define _XOPEN_SOURCE /* for strptime() */

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <json-c/json.h> /* json-c */
#include <math.h>
#include <sqlite3.h> /* sqllite3 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Статический библиотеки.  */
#include "../lib/ahocorasick/include/ahocorasick.h" /* uthash  */
#include "../lib/c-vector/cvector.h"		    /* c vector */
#include "../lib/utf8proc/utf8proc.h"		    /* utf8proc  */
#include "../lib/uthash/uthash.h"		    /* uthash  */

#endif
