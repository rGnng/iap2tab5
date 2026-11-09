#include "sqlite3.h"
/* afc_sqlite.c registers the only VFS, an immutable snapshot reader. */
int sqlite3_os_init(void)
{
    return SQLITE_OK;
}
int sqlite3_os_end(void)
{
    return SQLITE_OK;
}
