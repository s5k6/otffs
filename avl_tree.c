
#include "avl_tree.h"
#include "macros.h"
#include <stdio.h>



static void *_new(size_t size) {
    void *tmp = malloc(size);
    if (!tmp)
        err(1, "allcating %zu bytes", size);
    return tmp;
}

#define new(ty) _new(sizeof(ty))



typedef struct node *Node;

struct node {
    avl_Key k;
    avl_Val v;
    Node l, r;
    int h;
};

#define height(n) ((n) ? (n)->h : 0)

/* negative balance => leaning right.  Yes, a political statement! */
#define bal(n) ((n) ? height((n)->l) - height((n)->r) : 0)

#define adjHeight(n) (n)->h = 1 + max(height((n)->l), height((n)->r))



struct avl_tree {
    avl_CmpFun cmp;
    size_t size;
    Node root;
};



avl_Tree avl_new(avl_CmpFun cmp) {
    if (!cmp)
        return NULL;
    avl_Tree t = new(struct avl_tree);
    if (!t)
        return NULL;
    *t = (struct avl_tree){
        .cmp = cmp,
        .size = 0,
        .root = NULL,
    };
    return t;
}



/* Rotations

      y                   x
     / \                 / \
    x   C   <--rot-->   A   y
   / \                     / \
  A   B                   B   C

 */

static Node _rotR(Node y) {
    Node x = y->l;
    Node b = x->r;
    y->l = b; adjHeight(y);
    x->r = y; adjHeight(x);
    return x;
}
#define rotR(n) (n) = _rotR(n)

static Node _rotL (Node x) {
    Node y = x->r;
    Node b = y->l;
    x->r = b; adjHeight(x);
    y->l = x; adjHeight(y);
    return y;
}
#define rotL(n) (n) = _rotL(n)



struct insert_ctx {
    avl_AddFun const add;
    avl_CmpFun const cmp;
    avl_Key const key;
    avl_Val *const found;
    avl_Val const val;
    int replaced;
    size_t *const size;
};

static Node insert(struct insert_ctx *ctx, Node n) {

    if (!n) {
        n = new(*n);
        *n = (struct node){
            .k = ctx->key,
            .v = ctx->val,
            .l = NULL,
            .r = NULL,
            .h = 1,
        };
        *ctx->size += 1;
        return n;
    }

    int c = ctx->cmp(ctx->key, n->k);

    if (c < 0)
        n->l = insert(ctx, n->l);
    else if (c > 0)
        n->r = insert(ctx, n->r);
    else {
        if (ctx->found)
            *ctx->found = n->v;
        ctx->replaced = 1;
        /* Make sure to pass the incoming `key`.  This allows to free
           that key on collision in the `add` function. */
        n->v = ctx->add ? ctx->add(ctx->key, n->v, ctx->val) : ctx->val;
        
        return n;
    }

    adjHeight(n);
    /* FIXME: if the height did not change here, we will not need to
       re-balance ever again on the way up! */

    /* Cases for leaning left:

       Maybe:

               n              n
              / \            / \
             x   D          y   D
            / \     ->     / \
           A   y          x   C
              / \        / \
             B   C      A   B

       Always:

               n           y
              / \         / \
             y   C       A   n
            / \     ->      / \
           A   B           B   C
    */

    int b = bal(n);

    if (b > 1) {
        if (bal(n->l) < -1)
            rotL(n->l);
        rotR(n);
    } else if (b < -1) {
        if (bal(n->r) > 1)
            rotR(n->r);
        rotL(n);
    }

    return n;
}



int avl_insertWith(avl_AddFun add, avl_Tree t, avl_Key key, avl_Val val, avl_Val *old) {
    struct insert_ctx ctx = {
        .add = add,
        .cmp = t->cmp,
        .found = old,
        .key = key,
        .replaced = 0,
        .size = &t->size,
        .val = val,
    };
    t->root = insert(&ctx, t->root);
    return ctx.replaced;
}



size_t avl_size(avl_Tree t) {
    return t->size;
}



struct traverse_ctx {
    avl_VisitorFun const visit;
    avl_State state;
};

static int traverse(struct traverse_ctx *ctx, Node n) {
    
    if (!n)
        return 0;

    int c;

    c = traverse(ctx, n->l);
    if (c)
        return c;

    c = ctx->visit(n->k, n->v, ctx->state);
    if (c)
        return c;

    return traverse(ctx, n->r);
}

int avl_traverse(avl_Tree t, avl_VisitorFun visit, avl_State state) {

    if (!t->root)
        return 0;

    struct traverse_ctx ctx = {
        .visit = visit,
        .state = state,
    };

    return traverse(&ctx, t->root);
}



struct lookup_ctx {
    avl_CmpFun const cmp;
    avl_Key const key;
    avl_Val *const found;
};

static int lookup(struct lookup_ctx *ctx, Node n) {

    if (!n)
        return 0;

    int c = ctx->cmp(ctx->key, n->k);

    if (c < 0)
        return lookup(ctx, n->l);

    if (c > 0)
        return lookup(ctx, n->r);

    if (ctx->found)
        *ctx->found = n->v;

    return 1;
}

int avl_lookup(avl_Tree t, avl_Key key, avl_Val *ret) {

    struct lookup_ctx ctx = {
        .cmp = t->cmp,
        .key = key,
        .found = ret,
    };

    return lookup(&ctx, t->root);
}



struct free_ctx {
    avl_VisitorFun const visit;
    avl_State state;
};

static void freeNodes(struct free_ctx *ctx, Node n) {
    if (!n)
        return;
    
    freeNodes(ctx, n->l);
    
    if (ctx->visit)
        ctx->visit(n->k, n->v, ctx->state);
    
    freeNodes(ctx, n->r);
    
    free(n);
}

void avl_free_(avl_Tree t, avl_VisitorFun visit, avl_State state) {
    struct free_ctx ctx = {
        .visit = visit,
        .state = state,
    };
    freeNodes(&ctx, t->root);
    free(t);
}
