#ifndef AVL_RsuIWVyVv5Xx
#define AVL_RsuIWVyVv5Xx

#include <stdlib.h>
#include <sys/types.h>



/* An “opaque pointer” to the AVL tree datastructure.  Its internals
 * are not revealed to the application using it.
 */

typedef struct avl_tree *avl_Tree;



/* These types are here only to document the intention in the function
   signatures. */

typedef const void *avl_Key;
typedef size_t avl_Val;
typedef void *avl_State;



/* Compare two items to figure out their order in the AVL tree.  A
   function of this type must return an integer that compares to zero
   as `x` compares to `y`. */

typedef int (*avl_CmpFun)(avl_Key x, avl_Key y);



/* When inserting a key/value pair, this function combines an existing
   `old` value with the `new` one to be inserted.  The `key` is the
   one passed to `avl_insertWith`, not the one in the tree.  So it is
   possible to free a key here that is not used due to collision. */

typedef avl_Val (*avl_AddFun)(avl_Key key, avl_Val old, avl_Val new);



/* A visitor function.  Used, e.g., by `avl_traverse` and
   `avl_free`. A `state` pointer can be passed along.  Must return 0
   on success. */

typedef int (*avl_VisitorFun)(avl_Key key, avl_Val val, avl_State state);



/* Return a new, empty AVL tree with the given comparison function,
    which mut not be `NULL`.  Returs `NULL` on error. */

avl_Tree avl_new(avl_CmpFun cmp);



/* Store a (`key`,`value`) pair in the tree.  If the `key` is already
   present, `add` is used to combine the existing value with `val`.
   If `add` is `NULL`, the old value is replaced.  Stores any old
   value in `*old` if not `NULL`.  Returns 1 if an item was replaced,
   0 otherwise. */

int avl_insertWith(avl_AddFun add, avl_Tree t, avl_Key key, avl_Val val,
                   avl_Val *old);

#define avl_insert(t, key, val, old) avl_insertWith(NULL, t, key, val, old)



/* Remov `key` from the tree, if present.  Stores any old value in
   `*old` if not `NULL`.  Returns 1 if an item was deleted, 0 if none
   was found. */

int avl_deleteWith(avl_VisitorFun del, avl_Tree t, avl_Key key,
                   avl_State state);

#define avl_delete(t, key, val, st) avl_deleteWith(NULL, t, key, val, NULL)



/* Return the number of items stored in the tree. */

size_t avl_size(avl_Tree t);



/* Returns `1` if `key` is found in the tree, `0` otherwise.  Stores
   any found value in `*val` if not `NULL`. */

int avl_lookup(avl_Tree t, avl_Key key, avl_Val *val);



/* Perform a DFS traversal of the tree, calling the function `visit`
   in the order of sorting.  The provided `state` pointer is handed to
   `visit`.  If `visit` returns any value other than 0, then the
   traversal is cancelled, returning that value.  Otherwise 0 is
   returned. */

int avl_traverse(avl_Tree t, avl_VisitorFun visit, avl_State state);



/* If `free_item` is not `NULL`, it is called on each item that is
   still in the AVL tree.  This may be used to `free` remaining items.
   The `state` pointer is passed along.  The return value of
   `free_item` is ignored.  This is a macro that sets `t` to
   `NULL`. */

void avl_free_(avl_Tree t, avl_VisitorFun free_item, avl_State state);

#define avl_free(t, f, s) do { avl_free_(t, f, s); t = NULL; } while (0)




#endif
