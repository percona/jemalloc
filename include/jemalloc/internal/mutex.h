/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct malloc_mutex_s malloc_mutex_t;
typedef struct fair_mutex_s fair_mutex_t;
typedef struct fair_mutex_queue_item_s fair_mutex_queue_item_t;

#ifdef _WIN32
#  define MALLOC_MUTEX_INITIALIZER
#elif (defined(JEMALLOC_OSSPIN))
#  define MALLOC_MUTEX_INITIALIZER {0}
#elif (defined(JEMALLOC_MUTEX_INIT_CB))
#  define MALLOC_MUTEX_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, NULL}
#else
#  if (defined(PTHREAD_MUTEX_ADAPTIVE_NP) &&				\
       defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP))
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_ADAPTIVE_NP
#    define MALLOC_MUTEX_INITIALIZER {PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP, 0, 0, NULL, NULL}
#  else
#    define MALLOC_MUTEX_TYPE PTHREAD_MUTEX_DEFAULT
#    define MALLOC_MUTEX_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0, 0, NULL, NULL}
#  endif
#endif

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct fair_mutex_queue_item_s {
    pthread_cond_t *cond;
    struct fair_mutex_queue_item_s *next;
};

struct fair_mutex_s {
    pthread_mutex_t mutex;
    int mutex_held;
    int num_want_mutex;
    fair_mutex_queue_item_t *wait_head;
    fair_mutex_queue_item_t *wait_tail;
};

struct malloc_mutex_s {
#ifdef _WIN32
	CRITICAL_SECTION	lock;
#elif (defined(JEMALLOC_OSSPIN))
	OSSpinLock		lock;
#elif (defined(JEMALLOC_MUTEX_INIT_CB))
	pthread_mutex_t		lock;
	malloc_mutex_t		*postponed_next;
#else
	fair_mutex_t 		lock;
#endif
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

#ifdef JEMALLOC_LAZY_LOCK
extern bool isthreaded;
#else
#  undef isthreaded /* Undo private_namespace.h definition. */
#  define isthreaded true
#endif

bool	malloc_mutex_init(malloc_mutex_t *mutex);
void	malloc_mutex_prefork(malloc_mutex_t *mutex);
void	malloc_mutex_postfork_parent(malloc_mutex_t *mutex);
void	malloc_mutex_postfork_child(malloc_mutex_t *mutex);
bool	mutex_boot(void);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
void	malloc_mutex_lock(malloc_mutex_t *mutex);
void	malloc_mutex_unlock(malloc_mutex_t *mutex);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_MUTEX_C_))
JEMALLOC_INLINE void
enq_item(fair_mutex_t *fm, fair_mutex_queue_item_t *const item) {
    assert(item->next == NULL);
    if (fm->wait_tail != NULL) {
        fm->wait_tail->next = item;
    } else {
        assert(fm->wait_head == NULL);
        fm->wait_head = item;
    }
    fm->wait_tail = item;
}

JEMALLOC_INLINE pthread_cond_t *
deq_item(fair_mutex_t *fm) {
    assert(fm->wait_head != NULL);
    assert(fm->wait_tail != NULL);
    fair_mutex_queue_item_t *item = fm->wait_head;
    fm->wait_head = fm->wait_head->next;
    if (fm->wait_tail == item) {
        fm->wait_tail = NULL;
    }
    return item->cond;
}

JEMALLOC_INLINE void
fair_mutex_create(fair_mutex_t *fm) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, MALLOC_MUTEX_TYPE);
    pthread_mutex_init(&fm->mutex, NULL);
    pthread_mutexattr_destroy(&attr);

    fm->mutex_held = 0;
    fm->num_want_mutex = 0;
    fm->wait_head = NULL;
    fm->wait_tail = NULL;
}

JEMALLOC_INLINE void
fair_mutex_destroy(fair_mutex_t *fm) {
    pthread_mutex_destroy(&fm->mutex);
}

JEMALLOC_INLINE void
fair_mutex_lock(fair_mutex_t *fm) {
    pthread_mutex_lock(&fm->mutex);

    if (fm->mutex_held == 0 && fm->num_want_mutex == 0) {
        // No one holds the lock.  Grant the write lock.
        fm->mutex_held = 1;
    } else {
        pthread_cond_t cond;
        pthread_cond_init(&cond, NULL);
        fair_mutex_queue_item_t item = { .cond = &cond, .next = NULL };
        enq_item(fm, &item);

        // Wait for our turn.
        ++fm->num_want_mutex;
        pthread_cond_wait(&cond, &fm->mutex);
        pthread_cond_destroy(&cond);

        // Now it's our turn.
        assert(fm->num_want_mutex > 0);
        assert(fm->mutex_held == 0);

        // Not waiting anymore; grab the lock.
        --fm->num_want_mutex;
        fm->mutex_held = 1;
    }

    pthread_mutex_unlock(&fm->mutex);
}

JEMALLOC_INLINE void
fair_mutex_unlock(fair_mutex_t *fm) {
    pthread_mutex_lock(&fm->mutex);

    fm->mutex_held = 0;
    if (fm->wait_head != NULL) {
        assert(fm->num_want_mutex > 0);

        // Grant lock to the next waiter
        pthread_cond_t *cond = deq_item(fm);
        pthread_cond_signal(cond);
    } else {
        assert(fm->num_want_mutex == 0);
    }

    pthread_mutex_unlock(&fm->mutex);
}

JEMALLOC_INLINE int
fair_mutex_users(fair_mutex_t *fm) {
    return fm->mutex_held + fm->num_want_mutex;
}

JEMALLOC_INLINE int
fair_mutex_blocked_users(fair_mutex_t *fm) {
    return fm->num_want_mutex;
}

JEMALLOC_INLINE void
malloc_mutex_lock(malloc_mutex_t *mutex)
{

	if (isthreaded) {
#ifdef _WIN32
		EnterCriticalSection(&mutex->lock);
#elif (defined(JEMALLOC_OSSPIN))
		OSSpinLockLock(&mutex->lock);
#else
		fair_mutex_lock(&mutex->lock);
#endif
	}
}

JEMALLOC_INLINE void
malloc_mutex_unlock(malloc_mutex_t *mutex)
{

	if (isthreaded) {
#ifdef _WIN32
		LeaveCriticalSection(&mutex->lock);
#elif (defined(JEMALLOC_OSSPIN))
		OSSpinLockUnlock(&mutex->lock);
#else
		fair_mutex_unlock(&mutex->lock);
#endif
	}
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
