#include "../include/sia.h"

SIA *
SIACLOSE(SIA *sia)
{
	if (sia) {
		if (sia->session)
			sqlite3_close(sia->session);

		aho_destroy(&sia->database.aho);

		if (sia->database.idmap)
			free(sia->database.idmap);

		/* Очищаем хэш таблицу.  */
		if (sia->database.dict) {
			dictitem *c, *tmp;
			HASH_ITER(hh, sia->database.dict, c, tmp)
			{
				HASH_DEL(sia->database.dict, c);
				free(c);
			}
		}

		free(sia);
	}

	/* Для удобства использования в SIAOPEN
	 * возващаем NULL.  */
	return NULL;
}
