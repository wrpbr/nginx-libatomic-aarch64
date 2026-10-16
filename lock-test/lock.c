/*
 * The lock algorithm of nginx (src/core/ngx_shmtx.c, without the semaphore path), in its own file so that the
 * compiler cannot move the counter accesses of the caller across it, as in nginx.
 *
 * Select the compare-and-swap at build time:
 *   -DCAS_GCC_SYNC      __sync_bool_compare_and_swap: nginx built without --with-libatomic (full barrier)
 *   -DCAS_AO            AO_compare_and_swap: nginx 1.30 built with --with-libatomic (relaxed on aarch64)
 *   -DCAS_AO_FULL       AO_compare_and_swap_full: nginx with the patch in ../patch (acquire/release)
 */
#include <sched.h>

#if defined(CAS_AO) || defined(CAS_AO_FULL)
#define AO_REQUIRE_CAS
#include <atomic_ops.h>
typedef AO_t word;
#else
typedef unsigned long word;
#endif

typedef volatile word atomic_word;

static int
cas(atomic_word *p, word old, word new)
{
#if defined(CAS_AO)
    return AO_compare_and_swap(p, old, new);
#elif defined(CAS_AO_FULL)
    return AO_compare_and_swap_full(p, old, new);
#else
    return __sync_bool_compare_and_swap(p, old, new);
#endif
}

void
lock(atomic_word *l, word pid)
{
    word n;

    for ( ;; ) {
        if (*l == 0 && cas(l, 0, pid)) {
            return;
        }
        for (n = 1; n < 2048; n <<= 1) {
            if (*l == 0 && cas(l, 0, pid)) {
                return;
            }
        }
        sched_yield();
    }
}

void
unlock(atomic_word *l, word pid)
{
    cas(l, pid, 0);
}
