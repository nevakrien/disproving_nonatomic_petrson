
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>

#ifndef CACHE_PAD
#define CACHE_PAD 4096          /* keep every shared field on its own page */
#endif
#ifndef ITERATIONS
#define ITERATIONS 1000000ULL /* each thread loops this many times */
#endif

/* ──────────────────────────────────────────────────────────────── */
/*  Flag storage:   volatile for the “bad” build, atomics for the fix */
#ifdef STRICT_ATOMICS
    typedef _Atomic int flag_t;
    #define STORE_FLAG(x,v) atomic_store_explicit(&(x), (v), memory_order_seq_cst)
    #define LOAD_FLAG(x)    atomic_load_explicit(&(x),  memory_order_seq_cst)
#else
    typedef volatile int flag_t;
    #define STORE_FLAG(x,v) ((x) = (v))
    #define LOAD_FLAG(x)    (x)
#endif


/*────────────────  Critical‑section helpers (x86‑64 only) ───────────────*/

/**
 * this is inline assembly so we are 100% sure there are no compiler shananigans
 * the compiler can easily optimize away the memory read/write since a race condition is UB
 */

#if defined(__x86_64__)


/* Thread‑A: atomically write 0 */
static inline void critical_section_a(int *addr){
    int zero = 0;
    /* xchg reg, [mem]  ─ full‑barrier atomic store of 0 */
    __asm__ __volatile__("xchg %0, %1"
                         : "+r"(zero),          /* %0: reg starts 0, ends old *addr (ignored) */
                           "+m"(*addr)          /* %1: memory operand is read‑write          */
                         :
                         : "memory");

}

/* Thread‑B: store 1, yield, divide 1 by the *current* value in *addr. */
static inline void critical_section_b(int *addr){
    /* 1.  Atomic seq‑cst store of 1 (unchanged). */
    int one = 1;
    __asm__ __volatile__("xchg %0, %1" : "+r"(one), "+m"(*addr) :: "memory");

    /* 2.  Give the other thread a chance to stomp the value. */
    sched_yield();

    /* 3 + 4.  Atomic load + divide in one asm block.                     *
     *        After `lock xaddl`,  %reg == old *addr, memory unchanged.   *
     *        `idivl %reg` raises #DE if that value is zero.              */
    int divisor = 0;                    /* xadd’s “addend” — must start at 0 */
    __asm__ __volatile__(
        "lock   xaddl  %0, %1\n\t"      /* %0 ← *addr (atomic read)         */
        "movl   $1,    %%eax\n\t"       /* dividend low  = 1                */
        "xorl   %%edx, %%edx\n\t"       /* dividend high = 0                */
        "idivl  %0"                     /* 1 ÷ (%0)  →  #DE if (%0)==0      */
        : "+r"(divisor), "+m"(*addr)
        :                                /* no further inputs               */
        : "eax", "edx", "cc", "memory"); /* clobbers: regs, flags, memory   */
}


#else
#   error "x86‑64 only"
#endif


/* ──────────────────────────────────────────────────────────────── */
/*  Per‑pair shared state, padded so nothing shares a cache line    */
typedef struct {
    flag_t interested0;                 char pad0[CACHE_PAD - sizeof(int)];
    flag_t interested1;                 char pad1[CACHE_PAD - sizeof(int)];
    flag_t turn;                        char pad2[CACHE_PAD - sizeof(int)];
    int    value;                       char pad3[CACHE_PAD - sizeof(int)];
} shared_pair_t;

static shared_pair_t *pairs;
static int n_pairs, n_cpus;

/**pining to a core seemed like a good idea initiall
 * however its hard to make it work cross platform.
 * 
 * in practice its tricky to get a scheme that makes sure we pin to a legal core number
 * while making sure the 2 cores dont share L1 cache.
 * the computer I was testing this on has hyperthreading so that is likely why this code breaks there
 * 
 */
// /* Pin caller to a given logical CPU to maximise inter‑core traffic */
// static void pin_to_core(int core)
// {
//     cpu_set_t set;
//     CPU_ZERO(&set);
//     CPU_SET(core % n_cpus, &set);
//     pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
// }

/* Thread A: writes 0 inside the CS */
static void *thread_A(void *arg){
    intptr_t id = (intptr_t)arg;
    shared_pair_t *p = &pairs[id];
    // pin_to_core(id * 2);

    for (uint64_t i = 0; i < ITERATIONS; ++i) {
        STORE_FLAG(p->interested0, 1);
        STORE_FLAG(p->turn,        1);
        while (LOAD_FLAG(p->interested1) && LOAD_FLAG(p->turn) == 1)
            ;

        /* ---- critical section ---- */
        critical_section_a(&p->value);
        /* -------------------------- */

        STORE_FLAG(p->interested0, 0);
    }
    return NULL;
}

/* Thread B: writes 1, yields, then re‑reads and divides */
static void *thread_B(void *arg){
    intptr_t id = (intptr_t)arg;
    shared_pair_t *p = &pairs[id];
    // pin_to_core(id * 2 + 1);

    for (uint64_t i = 0; i < ITERATIONS; ++i) {
        STORE_FLAG(p->interested1, 1);
        STORE_FLAG(p->turn,        0);
        while (LOAD_FLAG(p->interested0) && LOAD_FLAG(p->turn) == 0)
            ;

        /* ---- critical section ---- */
        critical_section_b(&p->value);
        /* -------------------------- */

        STORE_FLAG(p->interested1, 0);
    }
    return NULL;
}

static void sigfpe_handler(int signum, siginfo_t *info, void *ucontext){
    (void)ucontext; // unused

    if (signum == SIGFPE && info && info->si_code == FPE_INTDIV) {
        fprintf(stderr, "ERROR: Division by zero detected.\nThe current algorithem is wrong...\n");
    } else {
        fprintf(stderr, "ERROR: Received unexpected signal %d.\n", signum);
    }

    exit(1);
}

static int sig_handler_install(void){
    struct sigaction sa = {0};

        sa.sa_sigaction = sigfpe_handler;
        sa.sa_flags = SA_SIGINFO;
    
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGFPE, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    return 0;
}


/* ──────────────────────────────────────────────────────────────── */
int main(int argc, char **argv){
    //setup the enviorment we work on
    n_pairs = (argc > 1) ? atoi(argv[1]) : 32;
    if (n_pairs <= 0) {
        fputs("need >0 pairs\n", stderr);
        return 1;
    }

    n_cpus  = sysconf(_SC_NPROCESSORS_ONLN);

    size_t bytes = sizeof(shared_pair_t) * (size_t)n_pairs;
    if (posix_memalign((void **)&pairs, CACHE_PAD, bytes)) {
        perror("posix_memalign");
        return 1;
    }
    memset(pairs, 0, bytes);

    pthread_t *th = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)n_pairs * 2);
    if (!th) {
        perror("malloc");
        return 1;
    }

    //if an error happens its division by 0 so lets catch those
    if(sig_handler_install())
        return 1;

    fprintf(stderr,
            "Launching %d pairs (%d threads) on %d CPUs %s\n",
            n_pairs, n_pairs * 2, n_cpus,
#ifdef STRICT_ATOMICS
            "[STRICT ATOMICS]"
#else
            "[volatile flags]"
#endif
    );

    for (int i = 0; i < n_pairs; ++i) {
        pthread_create(&th[i*2],     NULL, thread_A, (void *)(intptr_t)i);
        pthread_create(&th[i*2 + 1], NULL, thread_B, (void *)(intptr_t)i);
    }
    for (int i = 0; i < n_pairs * 2; ++i)
        pthread_join(th[i], NULL);

    puts("Finished without detecting a violation.");
    return 0;
}