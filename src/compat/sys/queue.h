/*
 * Minimal BSD tail queue compatibility for libc implementations which do
 * not ship <sys/queue.h>.  The macro API and implementation follow the
 * BSD queue.h design.
 */
#ifndef _COMPAT_SYS_QUEUE_H_
#define _COMPAT_SYS_QUEUE_H_

#define TAILQ_HEAD(name, type)						\
struct name {								\
	struct type *tqh_first;						\
	struct type **tqh_last;						\
}

#define TAILQ_ENTRY(type)						\
struct {								\
	struct type *tqe_next;						\
	struct type **tqe_prev;						\
}

#define TAILQ_INIT(head) do {						\
	(head)->tqh_first = NULL;					\
	(head)->tqh_last = &(head)->tqh_first;				\
} while (0)

#define TAILQ_FIRST(head)	((head)->tqh_first)
#define TAILQ_NEXT(elm, field)	((elm)->field.tqe_next)

#define TAILQ_INSERT_HEAD(head, elm, field) do {				\
	if (((elm)->field.tqe_next = (head)->tqh_first) != NULL)		\
		(head)->tqh_first->field.tqe_prev = &(elm)->field.tqe_next;	\
	else									\
		(head)->tqh_last = &(elm)->field.tqe_next;			\
	(head)->tqh_first = (elm);						\
	(elm)->field.tqe_prev = &(head)->tqh_first;				\
} while (0)

#define TAILQ_REMOVE(head, elm, field) do {				\
	if ((elm)->field.tqe_next != NULL)				\
		(elm)->field.tqe_next->field.tqe_prev =			\
		    (elm)->field.tqe_prev;				\
	else								\
		(head)->tqh_last = (elm)->field.tqe_prev;		\
	*(elm)->field.tqe_prev = (elm)->field.tqe_next;			\
} while (0)

#define TAILQ_FOREACH(var, head, field)					\
	for ((var) = TAILQ_FIRST(head); (var); (var) = TAILQ_NEXT(var, field))

#endif
