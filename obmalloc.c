#include "Python.h"

#define WITH_PYMALLOC
#ifdef WITH_PYMALLOC

#define ALIGNMENT               8               /* must be 2^N */
#define ALIGNMENT_SHIFT         3
#define ALIGNMENT_MASK          (ALIGNMENT - 1)

#define INDEX2SIZE(I) (((uint)(I) + 1) << ALIGNMENT_SHIFT)

#define SMALL_REQUEST_THRESHOLD 256
#define NB_SMALL_SIZE_CLASSES   (SMALL_REQUEST_THRESHOLD / ALIGNMENT)

#define SYSTEM_PAGE_SIZE        (4 * 1024)
#define SYSTEM_PAGE_SIZE_MASK   (SYSTEM_PAGE_SIZE - 1)

#ifdef WITH_MEMORY_LIMITS
#ifndef SMALL_MEMORY_LIMIT
#define SMALL_MEMORY_LIMIT      (64 * 1024 * 1024)      /* 64 MB -- more? */
#endif
#endif

#define ARENA_SIZE              (256 << 10)     /* 256KB */

#ifdef WITH_MEMORY_LIMITS
#define MAX_ARENAS              (SMALL_MEMORY_LIMIT / ARENA_SIZE)
#endif

#define POOL_SIZE               SYSTEM_PAGE_SIZE        /* must be 2^N */
#define POOL_SIZE_MASK          SYSTEM_PAGE_SIZE_MASK


#define SIMPLELOCK_DECL(lock)   /* simple lock declaration              */
#define SIMPLELOCK_INIT(lock)   /* allocate (if needed) and initialize  */
#define SIMPLELOCK_FINI(lock)   /* free/destroy an existing lock        */
#define SIMPLELOCK_LOCK(lock)   /* acquire released lock */
#define SIMPLELOCK_UNLOCK(lock) /* release acquired lock */

#undef  uchar
#define uchar   unsigned char   /* assuming == 8 bits  */

#undef  uint
#define uint    unsigned int    /* assuming >= 16 bits */

#undef  ulong
#define ulong   unsigned long   /* assuming >= 32 bits */

#undef uptr
#define uptr    Py_uintptr_t

typedef uchar block;

struct pool_header {
    union { block *_padding;
            uint count; } ref;          /* number of allocated blocks    */
    block *freeblock;                   /* pool's free list head         */
    struct pool_header *nextpool;       /* next pool of this size class  */
    struct pool_header *prevpool;       /* previous pool       ""        */
    uint arenaindex;                    /* index into arenas of base adr */
    uint szidx;                         /* block size class index        */
    uint nextoffset;                    /* bytes to virgin block         */
    uint maxnextoffset;                 /* largest valid nextoffset      */
};

typedef struct pool_header *poolp;

/* Record keeping for arenas. */
struct arena_object {
    uptr address;

    /* Pool-aligned pointer to the next pool to be carved off. */
    block* pool_address;

    /* The number of available pools in the arena:  free pools + never-
     * allocated pools.
     */
    uint nfreepools;

    /* The total number of pools in the arena, whether or not available. */
    uint ntotalpools;

    /* Singly-linked list of available pools. */
    struct pool_header* freepools;

    struct arena_object* nextarena;
    struct arena_object* prevarena;
};

#undef  ROUNDUP
#define ROUNDUP(x)              (((x) + ALIGNMENT_MASK) & ~ALIGNMENT_MASK)
#define POOL_OVERHEAD           ROUNDUP(sizeof(struct pool_header))

#define DUMMY_SIZE_IDX          0xffff  /* size class of newly cached pools */

/* Round pointer P down to the closest pool-aligned address <= P, as a poolp */
#define POOL_ADDR(P) ((poolp)((uptr)(P) & ~(uptr)POOL_SIZE_MASK))

/* Return total number of blocks in pool of size index I, as a uint. */
#define NUMBLOCKS(I) ((uint)(POOL_SIZE - POOL_OVERHEAD) / INDEX2SIZE(I))

SIMPLELOCK_DECL(_malloc_lock)
#define LOCK()          SIMPLELOCK_LOCK(_malloc_lock)
#define UNLOCK()        SIMPLELOCK_UNLOCK(_malloc_lock)
#define LOCK_INIT()     SIMPLELOCK_INIT(_malloc_lock)
#define LOCK_FINI()     SIMPLELOCK_FINI(_malloc_lock)


#define PTA(x)  ((poolp )((uchar *)&(usedpools[2*(x)]) - 2*sizeof(block *)))
#define PT(x)   PTA(x), PTA(x)

static poolp usedpools[2 * ((NB_SMALL_SIZE_CLASSES + 7) / 8) * 8] = {
    PT(0), PT(1), PT(2), PT(3), PT(4), PT(5), PT(6), PT(7)
#if NB_SMALL_SIZE_CLASSES > 8
    , PT(8), PT(9), PT(10), PT(11), PT(12), PT(13), PT(14), PT(15)
#if NB_SMALL_SIZE_CLASSES > 16
    , PT(16), PT(17), PT(18), PT(19), PT(20), PT(21), PT(22), PT(23)
#if NB_SMALL_SIZE_CLASSES > 24
    , PT(24), PT(25), PT(26), PT(27), PT(28), PT(29), PT(30), PT(31)
#if NB_SMALL_SIZE_CLASSES > 32
    , PT(32), PT(33), PT(34), PT(35), PT(36), PT(37), PT(38), PT(39)
#if NB_SMALL_SIZE_CLASSES > 40
    , PT(40), PT(41), PT(42), PT(43), PT(44), PT(45), PT(46), PT(47)
#if NB_SMALL_SIZE_CLASSES > 48
    , PT(48), PT(49), PT(50), PT(51), PT(52), PT(53), PT(54), PT(55)
#if NB_SMALL_SIZE_CLASSES > 56
    , PT(56), PT(57), PT(58), PT(59), PT(60), PT(61), PT(62), PT(63)
#endif /* NB_SMALL_SIZE_CLASSES > 56 */
#endif /* NB_SMALL_SIZE_CLASSES > 48 */
#endif /* NB_SMALL_SIZE_CLASSES > 40 */
#endif /* NB_SMALL_SIZE_CLASSES > 32 */
#endif /* NB_SMALL_SIZE_CLASSES > 24 */
#endif /* NB_SMALL_SIZE_CLASSES > 16 */
#endif /* NB_SMALL_SIZE_CLASSES >  8 */
};


/* Array of objects used to track chunks of memory (arenas). */
static struct arena_object* arenas = NULL;
/* Number of slots currently allocated in the `arenas` vector. */
static uint maxarenas = 0;

/* The head of the singly-linked, NULL-terminated list of available
 * arena_objects.
 */
static struct arena_object* unused_arena_objects = NULL;

/* The head of the doubly-linked, NULL-terminated at each end, list of
 * arena_objects associated with arenas that have pools available.
 */
static struct arena_object* usable_arenas = NULL;

/* How many arena_objects do we initially allocate?
 * 16 = can allocate 16 arenas = 16 * ARENA_SIZE = 4MB before growing the
 * `arenas` vector.
 */
#define INITIAL_ARENA_OBJECTS 16

/* Number of arenas allocated that haven't been free()'d. */
static size_t narenas_currently_allocated = 0;

static struct arena_object*
new_arena(void)
{
    struct arena_object* arenaobj;
    uint excess;        /* number of bytes above pool alignment */

    if (unused_arena_objects == NULL) {
        uint i;
        uint numarenas;
        size_t nbytes;

        /* Double the number of arena objects on each allocation.
         * Note that it's possible for `numarenas` to overflow.
         */
        numarenas = maxarenas ? maxarenas << 1 : INITIAL_ARENA_OBJECTS;
        if (numarenas <= maxarenas)
            return NULL;                /* overflow */
#if SIZEOF_SIZE_T <= SIZEOF_INT
        if (numarenas > PY_SIZE_MAX / sizeof(*arenas))
            return NULL;                /* overflow */
#endif
        nbytes = numarenas * sizeof(*arenas);
        arenaobj = (struct arena_object *)realloc(arenas, nbytes);
        if (arenaobj == NULL)
            return NULL;
        arenas = arenaobj;

        /* We might need to fix pointers that were copied.  However,
         * new_arena only gets called when all the pages in the
         * previous arenas are full.  Thus, there are *no* pointers
         * into the old array. Thus, we don't have to worry about
         * invalid pointers.  Just to be sure, some asserts:
         */
        assert(usable_arenas == NULL);
        assert(unused_arena_objects == NULL);

        /* Put the new arenas on the unused_arena_objects list. */
        for (i = maxarenas; i < numarenas; ++i) {
            arenas[i].address = 0;              /* mark as unassociated */
            arenas[i].nextarena = i < numarenas - 1 ?
                                   &arenas[i+1] : NULL;
        }

        /* Update globals. */
        unused_arena_objects = &arenas[maxarenas];
        maxarenas = numarenas;
    }

    /* Take the next available arena object off the head of the list. */
    assert(unused_arena_objects != NULL);
    arenaobj = unused_arena_objects;
    unused_arena_objects = arenaobj->nextarena;
    assert(arenaobj->address == 0);
    arenaobj->address = (uptr)malloc(ARENA_SIZE);
    if (arenaobj->address == 0) {
        /* The allocation failed: return NULL after putting the
         * arenaobj back.
         */
        arenaobj->nextarena = unused_arena_objects;
        unused_arena_objects = arenaobj;
        return NULL;
    }

    ++narenas_currently_allocated;
    arenaobj->freepools = NULL;
    /* pool_address <- first pool-aligned address in the arena
       nfreepools <- number of whole pools that fit after alignment */
    arenaobj->pool_address = (block*)arenaobj->address;
    arenaobj->nfreepools = ARENA_SIZE / POOL_SIZE;
    assert(POOL_SIZE * arenaobj->nfreepools == ARENA_SIZE);
    excess = (uint)(arenaobj->address & POOL_SIZE_MASK);
    if (excess != 0) {
        --arenaobj->nfreepools;
        arenaobj->pool_address += POOL_SIZE - excess;
    }
    arenaobj->ntotalpools = arenaobj->nfreepools;

    return arenaobj;
}

#define Py_ADDRESS_IN_RANGE(P, POOL)                    \
    ((arenaindex_temp = (POOL)->arenaindex) < maxarenas &&              \
     (uptr)(P) - arenas[arenaindex_temp].address < (uptr)ARENA_SIZE && \
     arenas[arenaindex_temp].address != 0)



#ifdef Py_USING_MEMORY_DEBUGGER


#undef Py_ADDRESS_IN_RANGE

#if defined(__GNUC__) && ((__GNUC__ == 3) && (__GNUC_MINOR__ >= 1) || \
                          (__GNUC__ >= 4))
#define Py_NO_INLINE __attribute__((__noinline__))
#else
#define Py_NO_INLINE
#endif

/* Don't make static, to try to ensure this isn't inlined. */
int Py_ADDRESS_IN_RANGE(void *P, poolp pool) Py_NO_INLINE;
#undef Py_NO_INLINE
#endif


#undef PyObject_Malloc
void *
PyObject_Malloc(size_t nbytes)
{
    block *bp;
    poolp pool;
    poolp next;
    uint size;


    if (nbytes > PY_SSIZE_T_MAX)
        return NULL;

    if ((nbytes - 1) < SMALL_REQUEST_THRESHOLD) {
        LOCK();

        size = (uint)(nbytes - 1) >> ALIGNMENT_SHIFT;
        pool = usedpools[size + size];
        if (pool != pool->nextpool) {
            /*
             * There is a used pool for this size class.
             * Pick up the head block of its free list.
             */
            ++pool->ref.count;
            bp = pool->freeblock;
            assert(bp != NULL);
            if ((pool->freeblock = *(block **)bp) != NULL) {
                UNLOCK();
                return (void *)bp;
            }
            /*
             * Reached the end of the free list, try to extend it.
             */
            if (pool->nextoffset <= pool->maxnextoffset) {
                /* There is room for another block. */
                pool->freeblock = (block*)pool +
                                  pool->nextoffset;
                pool->nextoffset += INDEX2SIZE(size);
                *(block **)(pool->freeblock) = NULL;
                UNLOCK();
                return (void *)bp;
            }
            /* Pool is full, unlink from used pools. */
            next = pool->nextpool;
            pool = pool->prevpool;
            next->prevpool = pool;
            pool->nextpool = next;
            UNLOCK();
            return (void *)bp;
        }

        /* There isn't a pool of the right size class immediately
         * available:  use a free pool.
         */
        if (usable_arenas == NULL) {
            /* No arena has a free pool:  allocate a new arena. */
            usable_arenas = new_arena();
            if (usable_arenas == NULL) {
                UNLOCK();
                goto redirect;
            }
            usable_arenas->nextarena =
                usable_arenas->prevarena = NULL;
        }
        assert(usable_arenas->address != 0);

        /* Try to get a cached free pool. */
        pool = usable_arenas->freepools;
        if (pool != NULL) {
            /* Unlink from cached pools. */
            usable_arenas->freepools = pool->nextpool;

            /* This arena already had the smallest nfreepools
             * value, so decreasing nfreepools doesn't change
             * that, and we don't need to rearrange the
             * usable_arenas list.  However, if the arena has
             * become wholly allocated, we need to remove its
             * arena_object from usable_arenas.
             */
            --usable_arenas->nfreepools;
            if (usable_arenas->nfreepools == 0) {
                /* Wholly allocated:  remove. */
                assert(usable_arenas->freepools == NULL);
                assert(usable_arenas->nextarena == NULL ||
                       usable_arenas->nextarena->prevarena ==
                       usable_arenas);

                usable_arenas = usable_arenas->nextarena;
                if (usable_arenas != NULL) {
                    usable_arenas->prevarena = NULL;
                    assert(usable_arenas->address != 0);
                }
            }
            else {
                /* nfreepools > 0:  it must be that freepools
                 * isn't NULL, or that we haven't yet carved
                 * off all the arena's pools for the first
                 * time.
                 */
                assert(usable_arenas->freepools != NULL ||
                       usable_arenas->pool_address <=
                       (block*)usable_arenas->address +
                           ARENA_SIZE - POOL_SIZE);
            }
        init_pool:
            /* Frontlink to used pools. */
            next = usedpools[size + size]; /* == prev */
            pool->nextpool = next;
            pool->prevpool = next;
            next->nextpool = pool;
            next->prevpool = pool;
            pool->ref.count = 1;
            if (pool->szidx == size) {
                /* Luckily, this pool last contained blocks
                 * of the same size class, so its header
                 * and free list are already initialized.
                 */
                bp = pool->freeblock;
                pool->freeblock = *(block **)bp;
                UNLOCK();
                return (void *)bp;
            }
            /*
             * Initialize the pool header, set up the free list to
             * contain just the second block, and return the first
             * block.
             */
            pool->szidx = size;
            size = INDEX2SIZE(size);
            bp = (block *)pool + POOL_OVERHEAD;
            pool->nextoffset = POOL_OVERHEAD + (size << 1);
            pool->maxnextoffset = POOL_SIZE - size;
            pool->freeblock = bp + size;
            *(block **)(pool->freeblock) = NULL;
            UNLOCK();
            return (void *)bp;
        }

        /* Carve off a new pool. */
        assert(usable_arenas->nfreepools > 0);
        assert(usable_arenas->freepools == NULL);
        pool = (poolp)usable_arenas->pool_address;
        assert((block*)pool <= (block*)usable_arenas->address +
                               ARENA_SIZE - POOL_SIZE);
        pool->arenaindex = usable_arenas - arenas;
        assert(&arenas[pool->arenaindex] == usable_arenas);
        pool->szidx = DUMMY_SIZE_IDX;
        usable_arenas->pool_address += POOL_SIZE;
        --usable_arenas->nfreepools;

        if (usable_arenas->nfreepools == 0) {
            assert(usable_arenas->nextarena == NULL ||
                   usable_arenas->nextarena->prevarena ==
                   usable_arenas);
            /* Unlink the arena:  it is completely allocated. */
            usable_arenas = usable_arenas->nextarena;
            if (usable_arenas != NULL) {
                usable_arenas->prevarena = NULL;
                assert(usable_arenas->address != 0);
            }
        }

        goto init_pool;
    }

    /* The small block allocator ends here. */

redirect:
    /* Redirect the original request to the underlying (libc) allocator.
     * We jump here on bigger requests, on error in the code above (as a
     * last chance to serve the request) or when the max memory limit
     * has been reached.
     */
    if (nbytes == 0)
        nbytes = 1;
    return (void *)malloc(nbytes);
}

/* free */

#undef PyObject_Free
void
PyObject_Free(void *p)
{
    poolp pool;
    block *lastfree;
    poolp next, prev;
    uint size;
#ifndef Py_USING_MEMORY_DEBUGGER
    uint arenaindex_temp;
#endif

    if (p == NULL)      /* free(NULL) has no effect */
        return;


    pool = POOL_ADDR(p);
    if (Py_ADDRESS_IN_RANGE(p, pool)) {
        /* We allocated this address. */
        LOCK();
        /* Link p to the start of the pool's freeblock list.  Since
         * the pool had at least the p block outstanding, the pool
         * wasn't empty (so it's already in a usedpools[] list, or
         * was full and is in no list -- it's not in the freeblocks
         * list in any case).
         */
        assert(pool->ref.count > 0);            /* else it was empty */
        *(block **)p = lastfree = pool->freeblock;
        pool->freeblock = (block *)p;
        if (lastfree) {
            struct arena_object* ao;
            uint nf;  /* ao->nfreepools */

            /* freeblock wasn't NULL, so the pool wasn't full,
             * and the pool is in a usedpools[] list.
             */
            if (--pool->ref.count != 0) {
                /* pool isn't empty:  leave it in usedpools */
                UNLOCK();
                return;
            }
            /* Pool is now empty:  unlink from usedpools, and
             * link to the front of freepools.  This ensures that
             * previously freed pools will be allocated later
             * (being not referenced, they are perhaps paged out).
             */
            next = pool->nextpool;
            prev = pool->prevpool;
            next->prevpool = prev;
            prev->nextpool = next;

            /* Link the pool to freepools.  This is a singly-linked
             * list, and pool->prevpool isn't used there.
             */
            ao = &arenas[pool->arenaindex];
            pool->nextpool = ao->freepools;
            ao->freepools = pool;
            nf = ++ao->nfreepools;

            /* All the rest is arena management.  We just freed
             * a pool, and there are 4 cases for arena mgmt:
             * 1. If all the pools are free, return the arena to
             *    the system free().
             * 2. If this is the only free pool in the arena,
             *    add the arena back to the `usable_arenas` list.
             * 3. If the "next" arena has a smaller count of free
             *    pools, we have to "slide this arena right" to
             *    restore that usable_arenas is sorted in order of
             *    nfreepools.
             * 4. Else there's nothing more to do.
             */
            if (nf == ao->ntotalpools) {
                /* Case 1.  First unlink ao from usable_arenas.
                 */
                assert(ao->prevarena == NULL ||
                       ao->prevarena->address != 0);
                assert(ao ->nextarena == NULL ||
                       ao->nextarena->address != 0);

                /* Fix the pointer in the prevarena, or the
                 * usable_arenas pointer.
                 */
                if (ao->prevarena == NULL) {
                    usable_arenas = ao->nextarena;
                    assert(usable_arenas == NULL ||
                           usable_arenas->address != 0);
                }
                else {
                    assert(ao->prevarena->nextarena == ao);
                    ao->prevarena->nextarena =
                        ao->nextarena;
                }
                /* Fix the pointer in the nextarena. */
                if (ao->nextarena != NULL) {
                    assert(ao->nextarena->prevarena == ao);
                    ao->nextarena->prevarena =
                        ao->prevarena;
                }
                /* Record that this arena_object slot is
                 * available to be reused.
                 */
                ao->nextarena = unused_arena_objects;
                unused_arena_objects = ao;

                /* Free the entire arena. */
                free((void *)ao->address);
                ao->address = 0;                        /* mark unassociated */
                --narenas_currently_allocated;

                UNLOCK();
                return;
            }
            if (nf == 1) {
                /* Case 2.  Put ao at the head of
                 * usable_arenas.  Note that because
                 * ao->nfreepools was 0 before, ao isn't
                 * currently on the usable_arenas list.
                 */
                ao->nextarena = usable_arenas;
                ao->prevarena = NULL;
                if (usable_arenas)
                    usable_arenas->prevarena = ao;
                usable_arenas = ao;
                assert(usable_arenas->address != 0);

                UNLOCK();
                return;
            }
            /* If this arena is now out of order, we need to keep
             * the list sorted.  The list is kept sorted so that
             * the "most full" arenas are used first, which allows
             * the nearly empty arenas to be completely freed.  In
             * a few un-scientific tests, it seems like this
             * approach allowed a lot more memory to be freed.
             */
            if (ao->nextarena == NULL ||
                         nf <= ao->nextarena->nfreepools) {
                /* Case 4.  Nothing to do. */
                UNLOCK();
                return;
            }
            /* Case 3:  We have to move the arena towards the end
             * of the list, because it has more free pools than
             * the arena to its right.
             * First unlink ao from usable_arenas.
             */
            if (ao->prevarena != NULL) {
                /* ao isn't at the head of the list */
                assert(ao->prevarena->nextarena == ao);
                ao->prevarena->nextarena = ao->nextarena;
            }
            else {
                /* ao is at the head of the list */
                assert(usable_arenas == ao);
                usable_arenas = ao->nextarena;
            }
            ao->nextarena->prevarena = ao->prevarena;

            /* Locate the new insertion point by iterating over
             * the list, using our nextarena pointer.
             */
            while (ao->nextarena != NULL &&
                            nf > ao->nextarena->nfreepools) {
                ao->prevarena = ao->nextarena;
                ao->nextarena = ao->nextarena->nextarena;
            }

            /* Insert ao at this point. */
            assert(ao->nextarena == NULL ||
                ao->prevarena == ao->nextarena->prevarena);
            assert(ao->prevarena->nextarena == ao->nextarena);

            ao->prevarena->nextarena = ao;
            if (ao->nextarena != NULL)
                ao->nextarena->prevarena = ao;

            /* Verify that the swaps worked. */
            assert(ao->nextarena == NULL ||
                      nf <= ao->nextarena->nfreepools);
            assert(ao->prevarena == NULL ||
                      nf > ao->prevarena->nfreepools);
            assert(ao->nextarena == NULL ||
                ao->nextarena->prevarena == ao);
            assert((usable_arenas == ao &&
                ao->prevarena == NULL) ||
                ao->prevarena->nextarena == ao);

            UNLOCK();
            return;
        }
        /* Pool was full, so doesn't currently live in any list:
         * link it to the front of the appropriate usedpools[] list.
         * This mimics LRU pool usage for new allocations and
         * targets optimal filling when several pools contain
         * blocks of the same size class.
         */
        --pool->ref.count;
        assert(pool->ref.count > 0);            /* else the pool is empty */
        size = pool->szidx;
        next = usedpools[size + size];
        prev = next->prevpool;
        /* insert pool before next:   prev <-> pool <-> next */
        pool->nextpool = next;
        pool->prevpool = prev;
        next->prevpool = pool;
        prev->nextpool = pool;
        UNLOCK();
        return;
    }

#ifdef WITH_VALGRIND
redirect:
#endif
    /* We didn't allocate this address. */
    free(p);
}

/* realloc.  If p is NULL, this acts like malloc(nbytes).  Else if nbytes==0,
 * then as the Python docs promise, we do not treat this like free(p), and
 * return a non-NULL result.
 */

#undef PyObject_Realloc
void *
PyObject_Realloc(void *p, size_t nbytes)
{
    void *bp;
    poolp pool;
    size_t size;
#ifndef Py_USING_MEMORY_DEBUGGER
    uint arenaindex_temp;
#endif

    if (p == NULL)
        return PyObject_Malloc(nbytes);

    /*
     * Limit ourselves to PY_SSIZE_T_MAX bytes to prevent security holes.
     * Most python internals blindly use a signed Py_ssize_t to track
     * things without checking for overflows or negatives.
     * As size_t is unsigned, checking for nbytes < 0 is not required.
     */
    if (nbytes > PY_SSIZE_T_MAX)
        return NULL;

#ifdef WITH_VALGRIND
    /* Treat running_on_valgrind == -1 the same as 0 */
    if (UNLIKELY(running_on_valgrind > 0))
        goto redirect;
#endif

    pool = POOL_ADDR(p);
    if (Py_ADDRESS_IN_RANGE(p, pool)) {
        /* We're in charge of this block */
        size = INDEX2SIZE(pool->szidx);
        if (nbytes <= size) {
            /* The block is staying the same or shrinking.  If
             * it's shrinking, there's a tradeoff:  it costs
             * cycles to copy the block to a smaller size class,
             * but it wastes memory not to copy it.  The
             * compromise here is to copy on shrink only if at
             * least 25% of size can be shaved off.
             */
            if (4 * nbytes > 3 * size) {
                /* It's the same,
                 * or shrinking and new/old > 3/4.
                 */
                return p;
            }
            size = nbytes;
        }
        bp = PyObject_Malloc(nbytes);
        if (bp != NULL) {
            memcpy(bp, p, size);
            PyObject_Free(p);
        }
        return bp;
    }
#ifdef WITH_VALGRIND
 redirect:
#endif
    /* We're not managing this block.  If nbytes <=
     * SMALL_REQUEST_THRESHOLD, it's tempting to try to take over this
     * block.  However, if we do, we need to copy the valid data from
     * the C-managed block to one of our blocks, and there's no portable
     * way to know how much of the memory space starting at p is valid.
     * As bug 1185883 pointed out the hard way, it's possible that the
     * C-managed block is "at the end" of allocated VM space, so that
     * a memory fault can occur if we try to copy nbytes bytes starting
     * at p.  Instead we punt:  let C continue to manage this block.
     */
    if (nbytes)
        return realloc(p, nbytes);
    /* C doesn't define the result of realloc(p, 0) (it may or may not
     * return NULL then), but Python's docs promise that nbytes==0 never
     * returns NULL.  We don't pass 0 to realloc(), to avoid that endcase
     * to begin with.  Even then, we can't be sure that realloc() won't
     * return NULL.
     */
    bp = realloc(p, 1);
    return bp ? bp : p;
}

#else   /* ! WITH_PYMALLOC */

#endif
