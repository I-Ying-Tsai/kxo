#ifndef USER_HLIST_H
#define USER_HLIST_H

#include <stddef.h>

struct hlist_node {
    struct hlist_node *next;
    struct hlist_node **pprev;
};

struct hlist_head {
    struct hlist_node *first;
};

#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)

#define hlist_empty(h) (!(h)->first)

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    struct hlist_node *first = h->first;
    n->next = first;
    if (first)
        first->pprev = &n->next;
    h->first = n;
    n->pprev = &h->first;
}

static inline void hlist_del(struct hlist_node *n)
{
    if (n->pprev) {
        *(n->pprev) = n->next;
        if (n->next)
            n->next->pprev = n->pprev;
        n->next = NULL;
        n->pprev = NULL;
    }
}

#define hlist_entry(ptr, type, member) \
    ((type *) ((char *) (ptr) - (unsigned long) offsetof(type, member)))

#define hlist_for_each_entry(pos, head, member)                    \
    for (struct hlist_node *__n = (head)->first;                   \
         __n && (pos = hlist_entry(__n, typeof(*pos), member), 1); \
         __n = __n->next)

#endif
