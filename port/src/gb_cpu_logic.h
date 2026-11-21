/* Where the game's threads run on a processor with more than one kind of core
 * (performance and efficiency cores). The decision only; gb_cpu.c reads
 * Windows' table of logical processors and applies it. Kept apart so a test
 * can feed it made-up processors (port/tests/cpu_logic_test.c). */
#ifndef GB_CPU_LOGIC_H
#define GB_CPU_LOGIC_H

#include <stdint.h>

#define GB_CPU_MAX 64            /* processor group 0, as RECOMP_GUEST_CPUS counts */

typedef struct {
    uint32_t id;                 /* Windows' CPU set id */
    uint8_t  group;              /* processor group */
    uint8_t  logical;            /* logical processor number in its group */
    uint8_t  core;               /* physical core (CoreIndex): SMT twins share it */
    uint8_t  eff;                /* EfficiencyClass: higher is faster */
} GbCpu;

typedef struct {
    int      hybrid;             /* more than one EfficiencyClass among the usable processors */
    int      guest_logical;      /* processor for the pinned guest threads; -1: leave as it is */
    int      guest_chosen;       /* 1 when the plan picked it (nothing had set RECOMP_GUEST_CPUS) */
    int      n_exec;             /* processors the executor thread is steered to; 0: leave it */
    uint32_t exec_ids[GB_CPU_MAX];
    uint8_t  exec_logical[GB_CPU_MAX];
    const char *why;             /* one line for the log */
} GbCpuPlan;

/* cpus: the table; proc_mask: the logical processors (group 0) the process
 * may use; guest_fixed: the core RECOMP_GUEST_CPUS already names, or -1. */
void gb_cpu_plan(const GbCpu *cpus, int n, uint64_t proc_mask, int guest_fixed, GbCpuPlan *plan);

#endif
