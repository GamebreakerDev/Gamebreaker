/* Placement of the game's threads on hybrid processors: the decision.
 *
 * The frame rate is limited by one thread, the toolkit's executor (it runs
 * every vertex program on the CPU), and the game's own threads are all pinned
 * to one logical processor (RECOMP_GUEST_CPUS). On a processor with one kind
 * of core nothing here changes anything. On a hybrid one (performance and
 * efficiency cores, such as Intel's 12th generation and later), Windows may
 * run the executor on an efficiency core, and the fixed guest core (2) may be
 * one; either costs frames. So, when the usable processors have more than one
 * EfficiencyClass:
 *   - the guest threads go to a core of the fastest class, unless something
 *     (a script, a test, a lane) already set RECOMP_GUEST_CPUS. Core 0 is
 *     taken last when there are at least three fast cores: it carries most of
 *     the interrupt work;
 *   - the executor is steered to every processor of the fastest class except
 *     those of the guest core, so the two never share a physical core.
 * When the fastest class has a single core, that core goes to the executor
 * (the heavier thread by far) and the guest threads to the next class. */
#include "gb_cpu_logic.h"

#include <string.h>

static int usable(const GbCpu *c, uint64_t proc_mask)
{
    return c->group == 0 && c->logical < GB_CPU_MAX && ((proc_mask >> c->logical) & 1u);
}

/* The lowest logical processor of physical core `core`, or -1. */
static int first_logical(const GbCpu *cpus, int n, uint64_t mask, int core)
{
    int i, best = -1;
    for (i = 0; i < n; i++)
        if (usable(&cpus[i], mask) && cpus[i].core == core && (best < 0 || cpus[i].logical < best))
            best = cpus[i].logical;
    return best;
}

/* Physical cores of class `eff`, ordered by their lowest logical processor. */
static int cores_of_class(const GbCpu *cpus, int n, uint64_t mask, int eff, int out[GB_CPU_MAX])
{
    int i, j, k = 0;
    for (i = 0; i < n; i++) {
        int seen = 0;
        if (!usable(&cpus[i], mask) || cpus[i].eff != eff)
            continue;
        for (j = 0; j < k; j++)
            if (out[j] == cpus[i].core) seen = 1;
        if (!seen && k < GB_CPU_MAX)
            out[k++] = cpus[i].core;
    }
    for (i = 1; i < k; i++)                 /* insertion sort by first logical */
        for (j = i; j > 0 && first_logical(cpus, n, mask, out[j]) < first_logical(cpus, n, mask, out[j - 1]); j--) {
            int t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return k;
}

static int core_of(const GbCpu *cpus, int n, uint64_t mask, int logical)
{
    int i;
    for (i = 0; i < n; i++)
        if (usable(&cpus[i], mask) && cpus[i].logical == logical)
            return cpus[i].core;
    return -1;
}

static void add_exec_class(const GbCpu *cpus, int n, uint64_t mask, int eff, int skip_core, GbCpuPlan *p)
{
    int i;
    for (i = 0; i < n && p->n_exec < GB_CPU_MAX; i++)
        if (usable(&cpus[i], mask) && cpus[i].eff == eff && cpus[i].core != skip_core) {
            p->exec_ids[p->n_exec] = cpus[i].id;
            p->exec_logical[p->n_exec++] = cpus[i].logical;
        }
}

void gb_cpu_plan(const GbCpu *cpus, int n, uint64_t proc_mask, int guest_fixed, GbCpuPlan *p)
{
    int i, lo = 256, hi = -1, next = -1, fast[GB_CPU_MAX], nfast, guest_core;

    memset(p, 0, sizeof *p);
    p->guest_logical = -1;
    for (i = 0; i < n; i++) {
        if (!usable(&cpus[i], proc_mask))
            continue;
        if (cpus[i].eff < lo) lo = cpus[i].eff;
        if (cpus[i].eff > hi) hi = cpus[i].eff;
    }
    if (hi < 0) {
        p->why = "no usable processor in the table";
        return;
    }
    if (lo == hi) {
        p->why = "one kind of core: nothing changed";
        return;
    }
    p->hybrid = 1;
    for (i = 0; i < n; i++)                   /* the class just below the fastest */
        if (usable(&cpus[i], proc_mask) && cpus[i].eff < hi && cpus[i].eff > next)
            next = cpus[i].eff;
    nfast = cores_of_class(cpus, n, proc_mask, hi, fast);

    if (guest_fixed >= 0) {
        p->guest_logical = guest_fixed;
        guest_core = core_of(cpus, n, proc_mask, guest_fixed);
        p->why = "hybrid: guest core kept as set, executor on the fastest cores";
    } else if (nfast >= 2) {
        int c0 = core_of(cpus, n, proc_mask, 0), pick = 0;
        if (nfast >= 3 && fast[0] == c0)
            pick = 1;                         /* keep core 0 for interrupts */
        guest_core = fast[pick];
        p->guest_logical = first_logical(cpus, n, proc_mask, guest_core);
        p->guest_chosen = 1;
        p->why = "hybrid: guest threads and executor on separate fast cores";
    } else {
        /* One fast core: it goes to the executor; the guest threads to the
         * first core of the next class. */
        int slow[GB_CPU_MAX], ns = cores_of_class(cpus, n, proc_mask, next, slow);
        guest_core = ns ? slow[0] : -1;
        p->guest_logical = ns ? first_logical(cpus, n, proc_mask, guest_core) : -1;
        p->guest_chosen = ns > 0;
        p->why = "hybrid with one fast core: executor on it, guest threads on the next class";
    }
    add_exec_class(cpus, n, proc_mask, hi, guest_core, p);
    if (!p->n_exec && next >= 0) {            /* the guest holds the only fast core */
        add_exec_class(cpus, n, proc_mask, next, guest_core, p);
        p->why = "hybrid: guest core kept as set on the only fast core, executor on the next class";
    }
}
