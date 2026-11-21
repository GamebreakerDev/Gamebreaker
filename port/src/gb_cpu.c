/* Placement of the game's threads on hybrid processors: reading Windows'
 * table of logical processors and applying the plan of gb_cpu_logic.c.
 *
 *   gb_cpu_boot()      before the run switches are applied: on a hybrid
 *                      processor, when nothing has set RECOMP_GUEST_CPUS,
 *                      sets it to a fast core, and asks Windows not to slow
 *                      the process down for power (EcoQoS), which on hybrid
 *                      laptops also moves a program's threads to slow cores.
 *   gb_cpu_log()       one line in the log for bug reports.
 *   gb_cpu_executor()  called on the executor thread (the Direct3D back end's
 *                      draw): steers it to the fast cores away from the guest
 *                      core with SetThreadSelectedCpuSets, a soft preference
 *                      Windows keeps unless those processors are unavailable.
 *
 * Nothing needs administrator rights and no system setting is changed. On a
 * processor with one kind of core none of this changes anything. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_cpu_logic.h"
#include "gb_log.h"

static GbCpuPlan s_plan;
static int s_ran, s_logical_total, s_lo = -1, s_hi = -1, s_throttle_off;
static char s_off_reason[64];

typedef BOOL (WINAPI *GetSystemCpuSetInformation_t)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL (WINAPI *SetThreadSelectedCpuSets_t)(HANDLE, const ULONG *, ULONG);
typedef BOOL (WINAPI *SetProcessInformation_t)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);


static int read_table(GbCpu *cpus)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    GetSystemCpuSetInformation_t get = k32 ? (GetSystemCpuSetInformation_t)(void *)
        GetProcAddress(k32, "GetSystemCpuSetInformation") : NULL;
    ULONG len = 0;
    unsigned char *buf, *at;
    int n = 0;

    if (!get)
        return -1;
    get(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len || !(buf = (unsigned char *)malloc(len)))
        return -1;
    if (!get((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        free(buf);
        return -1;
    }
    for (at = buf; at < buf + len;) {
        const SYSTEM_CPU_SET_INFORMATION *e = (const SYSTEM_CPU_SET_INFORMATION *)at;
        if (!e->Size)
            break;
        if (e->Type == CpuSetInformation) {
            s_logical_total++;
            if (n < GB_CPU_MAX) {
                cpus[n].id = e->CpuSet.Id;
                cpus[n].group = (uint8_t)e->CpuSet.Group;
                cpus[n].logical = e->CpuSet.LogicalProcessorIndex;
                cpus[n].core = e->CpuSet.CoreIndex;
                cpus[n].eff = e->CpuSet.EfficiencyClass;
                n++;
            }
        }
        at += e->Size;
    }
    free(buf);
    return n;
}

void gb_cpu_boot(void)
{
    GbCpu cpus[GB_CPU_MAX];
    DWORD_PTR proc = 0, sys = 0;
    const char *g = getenv("RECOMP_GUEST_CPUS");
    int n, i, fixed = -1;

    s_ran = 1;
    n = read_table(cpus);
    if (n <= 0) {
        snprintf(s_off_reason, sizeof s_off_reason, "no processor table");
        return;
    }
    for (i = 0; i < n; i++) {
        if (s_lo < 0 || cpus[i].eff < s_lo) s_lo = cpus[i].eff;
        if (cpus[i].eff > s_hi) s_hi = cpus[i].eff;
    }
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc)
        proc = (DWORD_PTR)-1;
    if (g && *g >= '0' && *g <= '9')
        fixed = atoi(g);
    gb_cpu_plan(cpus, n, (uint64_t)proc, fixed, &s_plan);
    if (s_plan.guest_chosen && s_plan.guest_logical >= 0 && !(g && *g)) {
        char v[16];
        snprintf(v, sizeof v, "%d", s_plan.guest_logical);
        _putenv_s("RECOMP_GUEST_CPUS", v);
    }
    if (s_plan.hybrid) {
        PROCESS_POWER_THROTTLING_STATE st;
        memset(&st, 0, sizeof st);
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = 0;                             /* never throttle */
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        SetProcessInformation_t setinfo = k32 ? (SetProcessInformation_t)(void *)
            GetProcAddress(k32, "SetProcessInformation") : NULL;
        s_throttle_off = setinfo && setinfo(GetCurrentProcess(), ProcessPowerThrottling,
                                            &st, sizeof st) ? 1 : 0;
    }
}

static void list_exec(char *out, size_t size)
{
    int i;
    size_t k = 0;
    out[0] = 0;
    for (i = 0; i < s_plan.n_exec && k + 5 < size; i++)
        k += (size_t)snprintf(out + k, size - k, "%s%d", i ? "," : "", s_plan.exec_logical[i]);
}

void gb_cpu_log(void)
{
    char ex[200];
    if (!s_ran)
        return;
    if (s_off_reason[0]) {
        gb_log("[GB] processors: placement %s\n", s_off_reason);
        return;
    }
    list_exec(ex, sizeof ex);
    gb_log("[GB] processors: %d logical, efficiency classes %d to %d; %s; guest threads on CPU %s%s;"
           " executor on CPUs %s%s\n",
           s_logical_total, s_lo, s_hi, s_plan.why,
           getenv("RECOMP_GUEST_CPUS") ? getenv("RECOMP_GUEST_CPUS") : "(any)",
           s_plan.guest_chosen ? " (chosen)" : "",
           s_plan.n_exec ? ex : "(any)",
           s_plan.hybrid ? (s_throttle_off ? "; power throttling off" : "; power throttling unchanged") : "");
}

void gb_cpu_executor(void)
{
    static volatile LONG done;
    HMODULE k32;
    SetThreadSelectedCpuSets_t set;
    ULONG ids[GB_CPU_MAX];
    int i;

    if (done || InterlockedExchange(&done, 1))
        return;
    if (!s_plan.n_exec)
        return;
    k32 = GetModuleHandleA("kernel32.dll");
    set = k32 ? (SetThreadSelectedCpuSets_t)(void *)GetProcAddress(k32, "SetThreadSelectedCpuSets") : NULL;
    for (i = 0; i < s_plan.n_exec; i++)
        ids[i] = s_plan.exec_ids[i];
    if (set && set(GetCurrentThread(), ids, (ULONG)s_plan.n_exec))
        gb_log("[GB] processors: executor thread %lu steered to %d processors\n",
               GetCurrentThreadId(), s_plan.n_exec);
    else
        gb_log("[GB] processors: executor thread %lu could not be steered (error %lu)\n",
               GetCurrentThreadId(), GetLastError());
}
