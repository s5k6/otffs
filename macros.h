#ifndef macros_Vdy5ZirY7YW8
#define macros_Vdy5ZirY7YW8

#include <string.h>
#include <err.h>

// this is not assert: not intended to be disabled in non-debug mode
#define ERRIF(c) do {                                             \
        if (c)                                                    \
            err(1, #c " at " __FILE__ ":%d because", __LINE__);   \
    } while (0)

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

#define zero(ptr) memset(ptr, 0, sizeof(*ptr))

#define ARRAY(ty, foo)                          \
    size_t foo##_used, foo##_alloc; ty *foo

#define ALLOCATE(foo, s) do {                           \
        foo##_alloc = s;                                \
        foo##_used = 0;                                 \
        foo = malloc(foo##_alloc * sizeof(*foo));       \
        ERRIF(!foo);                                    \
    } while (0)

#define ENOUGH(foo) do {                                                \
        if (foo##_used == foo##_alloc) {                                \
            foo = realloc(foo, (foo##_alloc *= 2) * sizeof(*foo));      \
            ERRIF(!foo);                                                \
        }                                                               \
    } while (0)

#define TRIM(foo) do {                                  \
        foo = realloc(foo, foo##_used * sizeof(*foo));  \
        ERRIF(!foo);                                    \
    } while (0)

#define ADD(foo, val) (foo[foo##_used++] = (val))



#define NOT_IMPLEMENTED errx(1, "NOT IMPLEMENTED " __FILE__ ":%d", __LINE__)


#endif
