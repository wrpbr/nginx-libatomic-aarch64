/*
 * Two processes do what ngx_http_limit_req does for each request on one node of a shared zone:
 * lock, count++, unlock (lookup), then lock, count--, excess++, unlock (account).
 * With a correct lock, count ends at 0 and excess at 2 * rounds.
 *
 *   counter [rounds]        default 20000000; exit status 1 when an update was lost
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef volatile unsigned long atomic_word;

void lock(atomic_word *l, unsigned long pid);
void unlock(atomic_word *l, unsigned long pid);

struct zone {
    atomic_word   lock;
    char          keep_lock_alone[120];
    unsigned long count;
    unsigned long excess;
};

int
main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 20000000;
    struct zone *z = mmap(NULL, sizeof *z, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (z == MAP_FAILED) {
        return 2;
    }
    for (int w = 0; w < 2; w++) {
        if (fork() == 0) {
            unsigned long pid = (unsigned long) getpid();
            for (long i = 0; i < rounds; i++) {
                lock(&z->lock, pid);
                z->count++;
                unlock(&z->lock, pid);
                lock(&z->lock, pid);
                z->count--;
                z->excess++;
                unlock(&z->lock, pid);
            }
            _exit(0);
        }
    }
    while (wait(NULL) > 0) {
    }
    printf("count=%ld (expected 0) excess=%lu (expected %ld) lost_excess=%ld\n",
           (long) z->count, z->excess, 2 * rounds, 2 * rounds - (long) z->excess);
    return z->count != 0 || z->excess != (unsigned long) (2 * rounds);
}
