# nginx `--with-libatomic` on aarch64: the shared-memory lock loses updates

If you build nginx with `--with-libatomic` and run it on 64-bit ARM (AWS Graviton, Ampere Altra, other Neoverse
cores), the lock that protects nginx shared memory does not order memory. Two workers can then lose updates of
shared data. With `limit_req`, a lost update leaves a node with a count of 1 at the tail of the zone queue. The zone
stops removing old entries, it fills up in hours or days, and then nginx refuses every new client address with the
`limit_req` status (429 or 503) and logs `could not allocate node`. Nothing fails before that moment.

This repository gives a test that shows the problem on the CPU, a two-line patch for nginx, and the steps to fix a
running server. We found the problem on a production nginx 1.30.5 on Ampere Altra on 16 October 2026.

## Are you affected?

You are affected if all of these are true:

- `nginx -V` shows `--with-libatomic`;
- the machine is aarch64 (`uname -m`);
- nginx uses shared memory zones: `limit_req`, `limit_conn`, `ssl_session_cache`, upstream `zone`, cache keys and
  others.

The default build (without `--with-libatomic`) is not affected: nginx then uses the GCC `__sync` builtins, which are
full barriers. On x86, the compare-and-swap instruction is a full barrier, so the same code works.

## What happens

With `--with-libatomic`, `src/os/unix/ngx_atomic.h` maps the nginx atomic operations to libatomic_ops:

```c
#define ngx_atomic_cmp_set(lock, old, new)  AO_compare_and_swap(lock, old, new)
#define ngx_atomic_fetch_add(value, add)    AO_fetch_and_add(value, add)
#define ngx_memory_barrier()                AO_nop()
```

In libatomic_ops, an operation without a suffix has no ordering constraint. On aarch64, libatomic_ops 7.6
(Debian 12 `libatomic-ops-dev` 7.6.14) implements `AO_compare_and_swap` as
`__atomic_compare_exchange_n(..., __ATOMIC_RELAXED, __ATOMIC_RELAXED)`, and `AO_nop` does nothing.

`ngx_shmtx_lock()` and `ngx_shmtx_unlock()` take and release the lock with that compare-and-swap. On a weakly ordered
CPU, a worker can read the protected data before its lock is visible to the other worker, or publish its change after
the release. The lock then does not give mutual exclusion for the data, and one of two concurrent read-modify-write
operations is lost.

`ngx_http_limit_req` increments `count` of a node in `lookup` (for every limit except the last one of a location)
and decrements it in `account`, both under the zone lock. When a decrement is lost, the node keeps `count = 1`.
`ngx_http_limit_req_expire()` stops at the first node with a count above zero, and the queue is ordered by last use.
When that node reaches the tail, the zone stops removing entries. When the zone is full, every new key fails with
`could not allocate node in limit_req zone`.

## Evidence

The lock test in `lock-test/` runs the nginx lock algorithm in two processes. Each process takes and releases the
lock 2 × 5,000,000 times around a counter increment and decrement, as `limit_req` does for one node. A correct lock
ends with `count=0`.

Ampere Altra (Neoverse-N1, `CPU part 0xd0c`), Debian 12, gcc 12, libatomic_ops 7.6.14:

| Compare-and-swap | Used by | Final count | Lost updates |
|---|---|---|---|
| `__sync_bool_compare_and_swap` | nginx without `--with-libatomic` | 0 | 0 |
| `AO_compare_and_swap` | nginx with `--with-libatomic` | 58 | 69,320 of 10,000,000 |
| `AO_compare_and_swap_full` | nginx with the patch below | 0 | 0 |

AWS Graviton2 (Neoverse-N1) gave the same result: 16,000 to 31,000 lost updates in 40,000,000 with the relaxed
compare-and-swap, none with the full barrier.

In production, the `limit_req` zone that every page request passes through had one node with `count = 1` at the
tail of its queue. The node belonged to a client that sent three requests in the same second to three virtual
servers. Its last update was 30 hours earlier, and the zone had 39,209 nodes and grew. An earlier zone with the same
role filled up after five days and refused every new client address for two days.

The race is rare at nginx request rates: 3,000,000 synthetic requests to one key on two workers did not reproduce
it. In production, we saw one stuck node after about 31 hours and one after about five days. The effect is
permanent until the zone is recreated.

## Fix

Choose one:

1. **Build nginx without `--with-libatomic`.** This is the default build. You can also remove `libatomic-ops-dev`
   from the build dependencies. To stop the option from coming back, add a check after `make install`:

   ```sh
   if nginx -V 2>&1 | grep -q -- --with-libatomic; then echo "remove --with-libatomic" >&2; exit 1; fi
   ```

2. **Keep `--with-libatomic` and apply the patch** in `patch/nginx-libatomic-full-barrier.patch`. It changes the
   three macros to the `_full` variants. Tested against nginx 1.30.5 and libatomic_ops 7.6.14.

   ```sh
   cd nginx-1.30.5
   patch -p1 < nginx-libatomic-full-barrier.patch
   ```

After the new binary is in place, recreate the zones: start a new master process (a new container or a restart),
or give each affected zone a new name or size and reload. A reload with the same zone name and size keeps the old
shared memory and the stuck node.

## Find a stuck zone

- The error log shows `could not allocate node in limit_req zone "<name>"` only when the zone is already full.
- Before that, the zone grows with every new client address. In a healthy zone, the tail node is about one minute
  old. A tail node with a count above zero that stays at the tail for minutes is stuck.

## Run the lock test

On an aarch64 machine with two or more cores:

```sh
docker run --rm --init -v "$PWD/lock-test:/src:ro" debian:bookworm-slim sh /src/run.sh
```

The container installs gcc and libatomic-ops-dev, builds the three variants and prints one line for each one.
`run.sh` takes the number of rounds as its first argument (default 20,000,000).

## Prior reports

We did not find an earlier report of this behavior in the nginx mailing lists or on GitHub. In 2009, the
`--with-libatomic` option was proposed on the nginx mailing list for ARM.

## License

BSD-2-Clause, the license of nginx. See `LICENSE`.
