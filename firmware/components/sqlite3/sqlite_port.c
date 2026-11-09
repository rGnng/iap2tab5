/* SQLite removes debug-only fields in release builds. IDF's optional
 * evaluate-disabled-assertions mode must not reference those removed fields. */
#define NDEBUG 1
#include <assert.h>
#undef assert
#define assert(expression) ((void)0)
#include "sqlite3.c"
