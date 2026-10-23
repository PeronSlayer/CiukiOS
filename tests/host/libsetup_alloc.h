#include <stddef.h>
void *st_test_malloc(size_t);
void st_test_free(void *);
#define malloc st_test_malloc
#define free st_test_free
