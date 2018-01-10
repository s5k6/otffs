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



#define STACK(ty) struct { size_t alloc, used; ty *array; }

#define ALLOCATE(foo, s) do {                                   \
        foo.alloc = s;                                          \
        foo.used = 0;                                           \
        foo.array = malloc(foo.alloc * sizeof(*foo.array));     \
        ERRIF(!foo.array);                                      \
    } while (0)

#define ENOUGH(foo) do {                                                \
        if (foo.used >= foo.alloc) {                                    \
            foo.alloc *= 2;                                             \
            foo.array = realloc(foo.array, foo.alloc * sizeof(*foo.array)); \
            ERRIF(!foo.array);                                          \
        }                                                               \
    } while (0)

#define TRIM(foo) do {                                                  \
        foo.alloc = foo.used;                                           \
        foo.array = realloc(foo.array, foo.alloc * sizeof(*foo.array)); \
        ERRIF(!foo.array);                                              \
    } while (0)

#define PUSH(foo, val) (foo.array[foo.used++] = (val))

#define POP(foo) (foo.array[--foo.used])

#define AT(foo, idx) (foo.array[idx])


#define NOT_IMPLEMENTED errx(1, "NOT IMPLEMENTED " __FILE__ ":%d", __LINE__)


#endif
