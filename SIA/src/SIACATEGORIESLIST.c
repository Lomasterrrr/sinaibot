#include "../include/sia.h"

void
SIACATEGORIESLIST(SIA *sia, FILE *out)
{
	if (!sia || !out || !sia->database.dict)
		return;

	cvector(char *) categories = NULL;
	dictitem *c, *tmp;
	size_t n;

	HASH_ITER(hh, sia->database.dict, c, tmp)
	{
		/* Проходимся по словарю и собираем
		 * наши категории.  */
		for (n = 0; n < cvector_size(categories); ++n)
			if (!strcmp(categories[n], c->category))
				goto next;
		cvector_push_back(categories,
		    strdup((const char *)c->category));
	next:
		continue;
	}

	if (categories) {
		for (n = 0; n < cvector_size(categories); ++n) {
			fprintf(out, "___%s___%s%s", categories[n],
			    n + 1 != cvector_size(categories) ? ", " : "",
			    (n % 6 == 5) ? "\n" : "");
			free(categories[n]);
		}
		cvector_free(categories);
	}
}
