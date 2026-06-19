/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Author: Davi Chaves Azevedo
 *
 * Native FreeBSD cache-hotspot engine for AMD Zen.
 *
 * Silicon-facing pieces live here: CPUID, fenced TSC reads, CPU pinning,
 * cache-line pointer chasing, sequential bandwidth probing, and thin
 * libpmc wrappers.  Python owns orchestration and reporting.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>

#if defined(__FreeBSD__)
#include <sys/cpuset.h>
#include <sys/sysctl.h>
#include <pmc.h>
#endif

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

#define CL_SIZE       64U
#define CL_PAD        (CL_SIZE - sizeof(void *))
#define TSC_CAL_US    50000

typedef struct cl_node {
    struct cl_node *next;
    char            pad[CL_PAD];
} cl_node_t;

_Static_assert(sizeof(cl_node_t) == CL_SIZE,
    "cl_node_t must be exactly one 64-byte cache line");

struct affinity_guard {
#if defined(__FreeBSD__)
    cpuset_t saved_mask;
#endif
    int active;
};

static uint64_t
splitmix64(uint64_t *state)
{
    uint64_t z;

    z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (z ^ (z >> 31));
}

static int
cmp_double_asc(const void *lhs, const void *rhs)
{
    double a, b;

    a = *(const double *)lhs;
    b = *(const double *)rhs;
    if (a < b)
        return (-1);
    if (a > b)
        return (1);
    return (0);
}

static double
median_double(double *vals, size_t n)
{
    if (n == 0)
        return (-1.0);
    qsort(vals, n, sizeof(*vals), cmp_double_asc);
    if ((n & 1U) != 0)
        return (vals[n / 2]);
    return ((vals[(n / 2) - 1] + vals[n / 2]) * 0.5);
}

static inline void
do_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
    uint32_t *ecx, uint32_t *edx)
{
    __asm__ __volatile__("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

static PyObject *
cp_cpuid(PyObject *self, PyObject *args, PyObject *kw)
{
    unsigned int leaf, subleaf;
    uint32_t eax, ebx, ecx, edx;
    static char *kwlist[] = {"leaf", "subleaf", NULL};

    (void)self;
    subleaf = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "I|I", kwlist, &leaf,
        &subleaf))
        return (NULL);
    do_cpuid(leaf, subleaf, &eax, &ebx, &ecx, &edx);
    return (Py_BuildValue("(IIII)", eax, ebx, ecx, edx));
}

static inline uint64_t
rdtsc_fenced(void)
{
    uint32_t lo, hi;

    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return (((uint64_t)hi << 32) | lo);
}

static inline uint64_t
rdtscp_fenced(void)
{
    uint32_t lo, hi, aux;

    __asm__ __volatile__("rdtscp; lfence"
        : "=a"(lo), "=d"(hi), "=c"(aux));
    return (((uint64_t)hi << 32) | lo);
}

static PyObject *
cp_rdtsc(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;
    return (PyLong_FromUnsignedLongLong(rdtsc_fenced()));
}

static int
read_tsc_sysctl(uint64_t *freq_hz)
{
#if defined(__FreeBSD__)
    uint64_t value;
    size_t len;

    len = sizeof(value);
    if (sysctlbyname("machdep.tsc_freq", &value, &len, NULL, 0) == 0 &&
        len == sizeof(value) && value > 0) {
        *freq_hz = value;
        return (0);
    }
#else
    (void)freq_hz;
#endif
    return (-1);
}

static long
calibrate_tsc_khz(void)
{
    struct timespec t0, t1;
    uint64_t c0, c1;
    double ns;

    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0)
        return (-1);
    c0 = rdtsc_fenced();
    usleep(TSC_CAL_US);
    c1 = rdtsc_fenced();
    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0)
        return (-1);

    ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 +
        (double)(t1.tv_nsec - t0.tv_nsec);
    if (ns <= 0.0 || c1 <= c0)
        return (-1);
    return ((long)((double)(c1 - c0) / ns * 1e6));
}

static long cached_tsc_khz;

static long
get_tsc_khz(void)
{
    uint64_t hz;

    if (cached_tsc_khz > 0)
        return (cached_tsc_khz);
    hz = 0;
    if (read_tsc_sysctl(&hz) == 0 && hz > 0)
        cached_tsc_khz = (long)(hz / 1000U);
    if (cached_tsc_khz <= 0)
        cached_tsc_khz = calibrate_tsc_khz();
    return (cached_tsc_khz);
}

static PyObject *
cp_tsc_freq_khz(PyObject *self, PyObject *args)
{
    long khz;

    (void)self;
    (void)args;
    khz = get_tsc_khz();
    if (khz <= 0) {
        PyErr_SetString(PyExc_RuntimeError, "TSC frequency detection failed");
        return (NULL);
    }
    return (PyLong_FromLong(khz));
}

static int
pin_thread_cpu(int cpu, struct affinity_guard *guard)
{
    guard->active = 0;
    if (cpu < 0)
        return (0);

#if defined(__FreeBSD__)
    cpuset_t one_cpu;

    if (cpu >= CPU_SETSIZE) {
        errno = EINVAL;
        return (-1);
    }
    CPU_ZERO(&guard->saved_mask);
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
        sizeof(guard->saved_mask), &guard->saved_mask) != 0)
        return (-1);
    CPU_ZERO(&one_cpu);
    CPU_SET(cpu, &one_cpu);
    if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
        sizeof(one_cpu), &one_cpu) != 0)
        return (-1);
    guard->active = 1;
#else
    (void)cpu;
#endif
    return (0);
}

static void
restore_thread_cpu(const struct affinity_guard *guard)
{
#if defined(__FreeBSD__)
    if (guard->active)
        (void)cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
            sizeof(guard->saved_mask), &guard->saved_mask);
#else
    (void)guard;
#endif
}

static void
sattolo(size_t *perm, size_t n, uint64_t seed)
{
    uint64_t rng;

    rng = seed;
    for (size_t i = 0; i < n; i++)
        perm[i] = i;
    for (size_t i = n - 1; i >= 1; i--) {
        size_t j, tmp;

        j = (size_t)(splitmix64(&rng) % i);
        tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }
}

static size_t
auto_iterations(size_t bytes)
{
    if (bytes <= 64UL * 1024)
        return (20000000);
    if (bytes <= 2048UL * 1024)
        return (5000000);
    if (bytes <= 98304UL * 1024)
        return (1000000);
    return (200000);
}

static void *
map_probe_buffer(size_t bytes, int superpage)
{
    int flags;
    void *ptr;

    flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_ALIGNED_SUPER)
    if (superpage)
        flags |= MAP_ALIGNED_SUPER;
#else
    (void)superpage;
#endif
    ptr = mmap(NULL, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
#if defined(MAP_ALIGNED_SUPER)
    if (ptr == MAP_FAILED && superpage) {
        flags &= ~MAP_ALIGNED_SUPER;
        ptr = mmap(NULL, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    }
#endif
    return (ptr);
}

static int chase_buffer_init(cl_node_t **out_buf, size_t bytes, int superpage);
static void chase_buffer_warmup(cl_node_t *buf, size_t n);
static double chase_buffer_time_ns(cl_node_t *buf, size_t iters, long khz);

static double
measure_chase_ns(size_t bytes, size_t iters, int cpu, int superpage)
{
    struct affinity_guard affinity;
    cl_node_t *buf;
    long khz;
    double result;

    if (bytes < CL_SIZE * 4)
        return (-1.0);
    if (pin_thread_cpu(cpu, &affinity) != 0)
        return (-1.0);

    buf = NULL;
    result = -1.0;
    if (chase_buffer_init(&buf, bytes, superpage) != 0)
        goto out;
    khz = get_tsc_khz();
    if (khz <= 0)
        goto out;
    chase_buffer_warmup(buf, bytes / CL_SIZE);
    result = chase_buffer_time_ns(buf, iters, khz);

out:
    if (buf != NULL)
        munmap(buf, bytes);
    restore_thread_cpu(&affinity);
    return (result);
}

static int
chase_buffer_init(cl_node_t **out_buf, size_t bytes, int superpage)
{
    cl_node_t *buf;
    size_t *perm, n;

    n = bytes / CL_SIZE;
    if (n < 4) {
        errno = EINVAL;
        return (-1);
    }
    buf = map_probe_buffer(bytes, superpage);
    if (buf == MAP_FAILED)
        return (-1);
#ifdef MADV_RANDOM
    (void)madvise(buf, bytes, MADV_RANDOM);
#endif
    perm = malloc(n * sizeof(*perm));
    if (perm == NULL) {
        munmap(buf, bytes);
        return (-1);
    }
    sattolo(perm, n, 0x4652454542534455ULL ^ (uint64_t)bytes);
    for (size_t i = 0; i < n; i++)
        buf[i].next = &buf[perm[i]];
    free(perm);
    *out_buf = buf;
    return (0);
}

static void
chase_buffer_warmup(cl_node_t *buf, size_t n)
{
    volatile cl_node_t *p;
    size_t cap, ops;

    cap = 1UL * 1024 * 1024;
    ops = n < cap ? n : cap;
    p = &buf[0];
    for (size_t i = 0; i < ops; i++)
        p = p->next;
    __asm__ __volatile__("" :: "r"(p));
}

static double
chase_buffer_time_ns(cl_node_t *buf, size_t iters, long khz)
{
    volatile cl_node_t *p;
    uint64_t t0, t1;
    size_t i;

    p = &buf[0];
    t0 = rdtsc_fenced();
    for (i = 0; i + 8 <= iters; i += 8) {
        p = p->next; p = p->next; p = p->next; p = p->next;
        p = p->next; p = p->next; p = p->next; p = p->next;
        __asm__ __volatile__("" : "+r"(p));
    }
    for (; i < iters; i++) {
        p = p->next;
        __asm__ __volatile__("" : "+r"(p));
    }
    t1 = rdtscp_fenced();
    __asm__ __volatile__("" :: "r"(p));
    return (((double)(t1 - t0) / (double)iters) * 1e6 / (double)khz);
}

static PyObject *
cp_measure_chase_bucket(PyObject *self, PyObject *args, PyObject *kw)
{
    int kb, repeat, cpu, superpage;
    unsigned long long iters_arg;
    size_t bytes, iters, n, nsamples;
    cl_node_t *buf;
    long khz;
    double *samples, median;
    struct affinity_guard affinity;
    int ok;
    static char *kwlist[] = {"kb", "iters", "repeat", "cpu", "superpage", NULL};

    (void)self;
    iters_arg = 0;
    repeat = 3;
    cpu = -1;
    superpage = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "i|Kiip", kwlist,
        &kb, &iters_arg, &repeat, &cpu, &superpage))
        return (NULL);
    if (kb < 1 || repeat < 1 || repeat > 256) {
        PyErr_SetString(PyExc_ValueError, "invalid bucket parameters");
        return (NULL);
    }
    bytes = (size_t)kb * 1024;
    bytes = (bytes + CL_SIZE - 1) & ~((size_t)CL_SIZE - 1);
    if (bytes < CL_SIZE * 4)
        bytes = CL_SIZE * 4;
    iters = iters_arg ? (size_t)iters_arg : auto_iterations(bytes);
    n = bytes / CL_SIZE;

    samples = malloc((size_t)repeat * sizeof(*samples));
    if (samples == NULL)
        return (PyErr_NoMemory());
    nsamples = 0;
    ok = 0;
    buf = NULL;
    khz = 0;

    Py_BEGIN_ALLOW_THREADS
    if (pin_thread_cpu(cpu, &affinity) == 0) {
        if (chase_buffer_init(&buf, bytes, superpage) == 0) {
            khz = get_tsc_khz();
            if (khz > 0) {
                chase_buffer_warmup(buf, n);
                for (int r = 0; r < repeat; r++) {
                    double ns = chase_buffer_time_ns(buf, iters, khz);
                    if (ns > 0.0)
                        samples[nsamples++] = ns;
                }
                ok = 1;
            }
            munmap(buf, bytes);
        }
        restore_thread_cpu(&affinity);
    }
    Py_END_ALLOW_THREADS

    median = ok ? median_double(samples, nsamples) : -1.0;
    free(samples);
    if (!ok || median <= 0.0)
        Py_RETURN_NONE;
    return (Py_BuildValue("(Kd)", (unsigned long long)(bytes / 1024), median));
}

static PyObject *
cp_cache_latency_probe(PyObject *self, PyObject *args, PyObject *kw)
{
    int min_kb, max_kb, steps, repeat, cpu, superpage;
    double ratio;
    size_t last_bytes;
    PyObject *result;
    static char *kwlist[] = {
        "min_kb", "max_kb", "steps", "repeat", "cpu", "superpage", NULL
    };

    (void)self;
    min_kb = 1;
    max_kb = 256 * 1024;
    steps = 50;
    repeat = 5;
    cpu = -1;
    superpage = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|iiiiip", kwlist, &min_kb,
        &max_kb, &steps, &repeat, &cpu, &superpage))
        return (NULL);
    if (min_kb < 1 || max_kb < min_kb || max_kb > 1024 * 1024 ||
        steps < 2 || steps > 1024 || repeat < 1 || repeat > 256) {
        PyErr_SetString(PyExc_ValueError, "invalid latency probe parameters");
        return (NULL);
    }

    result = PyList_New(0);
    if (result == NULL)
        return (NULL);
    last_bytes = 0;
    ratio = pow((double)max_kb / (double)min_kb, 1.0 / (steps - 1));

    for (int s = 0; s < steps; s++) {
        size_t kb, bytes, iters, nsamples;
        double *samples, median;

        if (s == 0)
            kb = (size_t)min_kb;
        else if (s == steps - 1)
            kb = (size_t)max_kb;
        else
            kb = (size_t)((double)min_kb * pow(ratio, s));
        bytes = kb * 1024;
        bytes = (bytes + CL_SIZE - 1) & ~((size_t)CL_SIZE - 1);
        if (bytes < CL_SIZE * 4)
            bytes = CL_SIZE * 4;
        if (bytes == last_bytes)
            continue;
        last_bytes = bytes;
        iters = auto_iterations(bytes);

        samples = malloc((size_t)repeat * sizeof(*samples));
        if (samples == NULL) {
            Py_DECREF(result);
            return (PyErr_NoMemory());
        }
        nsamples = 0;
        Py_BEGIN_ALLOW_THREADS
        for (int r = 0; r < repeat; r++) {
            double ns = measure_chase_ns(bytes, iters, cpu, superpage);
            if (ns > 0.0)
                samples[nsamples++] = ns;
        }
        Py_END_ALLOW_THREADS
        median = median_double(samples, nsamples);
        free(samples);

        if (median > 0.0) {
            PyObject *row = Py_BuildValue("(Kd)",
                (unsigned long long)(bytes / 1024), median);
            if (row == NULL || PyList_Append(result, row) != 0) {
                Py_XDECREF(row);
                Py_DECREF(result);
                return (NULL);
            }
            Py_DECREF(row);
        }
    }
    return (result);
}

static double
measure_seq_bw_gbps(size_t bytes, size_t passes, int cpu, int superpage)
{
    struct affinity_guard affinity;
    uint64_t *buf;
    uint64_t sum0, sum1, sum2, sum3, t0, t1;
    size_t n64;
    long khz;
    double sec, total_bytes, result;

    if (pin_thread_cpu(cpu, &affinity) != 0)
        return (-1.0);
    buf = MAP_FAILED;
    result = -1.0;
    buf = map_probe_buffer(bytes, superpage);
    if (buf == MAP_FAILED)
        goto out;

#ifdef MADV_SEQUENTIAL
    (void)madvise(buf, bytes, MADV_SEQUENTIAL);
#endif

    n64 = bytes / sizeof(uint64_t);
    for (size_t i = 0; i < n64; i += 8)
        buf[i] = i;

    khz = get_tsc_khz();
    if (khz <= 0)
        goto out;

    sum0 = sum1 = sum2 = sum3 = 0;
    t0 = rdtsc_fenced();
    for (size_t p = 0; p < passes; p++) {
        size_t i = 0;
        for (; i + 24 < n64; i += 32) {
            sum0 += buf[i];
            sum1 += buf[i + 8];
            sum2 += buf[i + 16];
            sum3 += buf[i + 24];
        }
        for (; i < n64; i += 8)
            sum0 += buf[i];
    }
    t1 = rdtscp_fenced();
    sum0 += sum1 + sum2 + sum3;
    __asm__ __volatile__("" :: "r"(sum0));

    sec = (double)(t1 - t0) / ((double)khz * 1e3);
    if (sec <= 0.0)
        goto out;
    total_bytes = (double)bytes * (double)passes;
    result = total_bytes / sec / 1e9;

out:
    if (buf != MAP_FAILED)
        munmap(buf, bytes);
    restore_thread_cpu(&affinity);
    return (result);
}

static PyObject *
cp_cache_bw_probe(PyObject *self, PyObject *args, PyObject *kw)
{
    int min_kb, max_kb, steps, repeat, cpu, superpage;
    double ratio;
    size_t last_bytes;
    PyObject *result;
    static char *kwlist[] = {
        "min_kb", "max_kb", "steps", "repeat", "cpu", "superpage", NULL
    };

    (void)self;
    min_kb = 4;
    max_kb = 256 * 1024;
    steps = 40;
    repeat = 3;
    cpu = -1;
    superpage = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|iiiiip", kwlist, &min_kb,
        &max_kb, &steps, &repeat, &cpu, &superpage))
        return (NULL);
    if (min_kb < 1 || max_kb < min_kb || max_kb > 1024 * 1024 ||
        steps < 2 || steps > 1024 || repeat < 1 || repeat > 256) {
        PyErr_SetString(PyExc_ValueError, "invalid bandwidth probe parameters");
        return (NULL);
    }

    result = PyList_New(0);
    if (result == NULL)
        return (NULL);
    last_bytes = 0;
    ratio = pow((double)max_kb / (double)min_kb, 1.0 / (steps - 1));
    for (int s = 0; s < steps; s++) {
        size_t kb, bytes, passes, nsamples;
        double *samples, gbps;

        kb = (s == steps - 1) ? (size_t)max_kb :
            (size_t)((double)min_kb * pow(ratio, s));
        bytes = kb * 1024;
        bytes = (bytes + CL_SIZE - 1) & ~((size_t)CL_SIZE - 1);
        if (bytes < 4096)
            bytes = 4096;
        if (bytes == last_bytes)
            continue;
        last_bytes = bytes;

        if (bytes < 1024 * 1024)
            passes = 500;
        else if (bytes < 64UL * 1024 * 1024)
            passes = 20;
        else
            passes = 2;

        samples = malloc((size_t)repeat * sizeof(*samples));
        if (samples == NULL) {
            Py_DECREF(result);
            return (PyErr_NoMemory());
        }
        nsamples = 0;
        Py_BEGIN_ALLOW_THREADS
        for (int r = 0; r < repeat; r++) {
            double v = measure_seq_bw_gbps(bytes, passes, cpu, superpage);
            if (v > 0.0)
                samples[nsamples++] = v;
        }
        Py_END_ALLOW_THREADS
        gbps = median_double(samples, nsamples);
        free(samples);

        if (gbps > 0.0) {
            PyObject *row = Py_BuildValue("(Kd)",
                (unsigned long long)(bytes / 1024), gbps);
            if (row == NULL || PyList_Append(result, row) != 0) {
                Py_XDECREF(row);
                Py_DECREF(result);
                return (NULL);
            }
            Py_DECREF(row);
        }
    }
    return (result);
}

#if defined(__FreeBSD__)
static int
parse_pmc_mode(const char *text, enum pmc_mode *mode)
{
    if (strcasecmp(text, "SC") == 0) {
        *mode = PMC_MODE_SC;
        return (0);
    }
    if (strcasecmp(text, "TC") == 0) {
        *mode = PMC_MODE_TC;
        return (0);
    }
    if (strcasecmp(text, "SS") == 0) {
        *mode = PMC_MODE_SS;
        return (0);
    }
    if (strcasecmp(text, "TS") == 0) {
        *mode = PMC_MODE_TS;
        return (0);
    }
    errno = EINVAL;
    return (-1);
}

static PyObject *
cp_pmc_init(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;
    if (pmc_init() != 0)
        return (PyErr_SetFromErrno(PyExc_OSError));
    Py_RETURN_NONE;
}

static PyObject *
cp_pmc_ncpu(PyObject *self, PyObject *args)
{
    int ncpu;

    (void)self;
    (void)args;
    ncpu = pmc_ncpu();
    if (ncpu < 0)
        return (PyErr_SetFromErrno(PyExc_OSError));
    return (PyLong_FromLong(ncpu));
}

static int
dict_set_string(PyObject *dict, const char *key, const char *value)
{
    PyObject *obj;
    int error;

    obj = PyUnicode_FromString(value != NULL ? value : "unknown");
    if (obj == NULL)
        return (-1);
    error = PyDict_SetItemString(dict, key, obj);
    Py_DECREF(obj);
    return (error);
}

static int
dict_set_ulong(PyObject *dict, const char *key, unsigned long value)
{
    PyObject *obj;
    int error;

    obj = PyLong_FromUnsignedLong(value);
    if (obj == NULL)
        return (-1);
    error = PyDict_SetItemString(dict, key, obj);
    Py_DECREF(obj);
    return (error);
}

static PyObject *
cp_pmc_cpuinfo(PyObject *self, PyObject *args)
{
    const struct pmc_cpuinfo *ci;
    PyObject *dict, *classes;

    (void)self;
    (void)args;
    if (pmc_cpuinfo(&ci) != 0)
        return (PyErr_SetFromErrno(PyExc_OSError));

    dict = PyDict_New();
    if (dict == NULL)
        return (NULL);
    classes = PyList_New(0);
    if (classes == NULL) {
        Py_DECREF(dict);
        return (NULL);
    }

    for (uint32_t i = 0; i < ci->pm_nclass; i++) {
        const struct pmc_classinfo *cl = &ci->pm_classes[i];
        const char *name = pmc_name_of_class(cl->pm_class);
        PyObject *row = Py_BuildValue("{s:s,s:I,s:I,s:I,s:I}",
            "name", name != NULL ? name : "unknown",
            "class", (unsigned int)cl->pm_class,
            "caps", cl->pm_caps,
            "width", cl->pm_width,
            "num", cl->pm_num);
        if (row == NULL || PyList_Append(classes, row) != 0) {
            Py_XDECREF(row);
            Py_DECREF(classes);
            Py_DECREF(dict);
            return (NULL);
        }
        Py_DECREF(row);
    }

    if (dict_set_string(dict, "cputype",
        pmc_name_of_cputype(ci->pm_cputype)) != 0 ||
        dict_set_ulong(dict, "ncpu", ci->pm_ncpu) != 0 ||
        dict_set_ulong(dict, "npmc", ci->pm_npmc) != 0 ||
        dict_set_ulong(dict, "nclass", ci->pm_nclass) != 0 ||
        PyDict_SetItemString(dict, "classes", classes) != 0) {
        Py_DECREF(classes);
        Py_DECREF(dict);
        return (NULL);
    }
    Py_DECREF(classes);
    return (dict);
}

static PyObject *
cp_pmc_allocate(PyObject *self, PyObject *args, PyObject *kw)
{
    const char *event, *mode_text;
    unsigned int flags;
    int cpu;
    unsigned long long count;
    enum pmc_mode mode;
    pmc_id_t pmcid;
    static char *kwlist[] = {"event", "mode", "flags", "cpu", "count", NULL};

    (void)self;
    mode_text = "TC";
    flags = 0;
    cpu = -1;
    count = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "s|sIiK", kwlist, &event,
        &mode_text, &flags, &cpu, &count))
        return (NULL);
    if (parse_pmc_mode(mode_text, &mode) != 0) {
        PyErr_Format(PyExc_ValueError, "unknown PMC mode '%s'", mode_text);
        return (NULL);
    }
    if (PMC_IS_SYSTEM_MODE(mode) && cpu < 0)
        cpu = 0;
    if (PMC_IS_VIRTUAL_MODE(mode) && cpu < 0)
        cpu = PMC_CPU_ANY;

    if (pmc_allocate(event, mode, flags, cpu, &pmcid,
        (uint64_t)count) != 0)
        return (PyErr_SetFromErrno(PyExc_OSError));
    return (PyLong_FromUnsignedLong(pmcid));
}

static PyObject *
cp_pmc_attach(PyObject *self, PyObject *args)
{
    unsigned int pmcid;
    int pid;

    (void)self;
    if (!PyArg_ParseTuple(args, "Ii", &pmcid, &pid))
        return (NULL);
    if (pmc_attach((pmc_id_t)pmcid, (pid_t)pid) != 0)
        return (PyErr_SetFromErrno(PyExc_OSError));
    Py_RETURN_NONE;
}

static PyObject *
cp_pmc_start(PyObject *self, PyObject *args)
{
    unsigned int pmcid;

    (void)self;
    if (!PyArg_ParseTuple(args, "I", &pmcid))
        return (NULL);
    if (pmc_start((pmc_id_t)pmcid) != 0)
        return (PyErr_SetFromErrno(PyExc_OSError));
    Py_RETURN_NONE;
}

static PyObject *
cp_pmc_stop(PyObject *self, PyObject *args)
{
    unsigned int pmcid;

    (void)self;
    if (!PyArg_ParseTuple(args, "I", &pmcid))
        return (NULL);
    if (pmc_stop((pmc_id_t)pmcid) != 0)
        return (PyErr_SetFromErrno(PyExc_OSError));
    Py_RETURN_NONE;
}

static PyObject *
cp_pmc_read(PyObject *self, PyObject *args)
{
    unsigned int pmcid;
    pmc_value_t value;

    (void)self;
    if (!PyArg_ParseTuple(args, "I", &pmcid))
        return (NULL);
    if (pmc_read((pmc_id_t)pmcid, &value) != 0)
        return (PyErr_SetFromErrno(PyExc_OSError));
    return (PyLong_FromUnsignedLongLong((uint64_t)value));
}

static PyObject *
cp_pmc_release(PyObject *self, PyObject *args)
{
    unsigned int pmcid;

    (void)self;
    if (!PyArg_ParseTuple(args, "I", &pmcid))
        return (NULL);
    if (pmc_release((pmc_id_t)pmcid) != 0)
        return (PyErr_SetFromErrno(PyExc_OSError));
    Py_RETURN_NONE;
}
#else
static PyObject *
pmc_not_supported(void)
{
    PyErr_SetString(PyExc_NotImplementedError,
        "FreeBSD libpmc is not available on this platform");
    return (NULL);
}

static PyObject *cp_pmc_init(PyObject *self, PyObject *args) { (void)self; (void)args; return (pmc_not_supported()); }
static PyObject *cp_pmc_ncpu(PyObject *self, PyObject *args) { (void)self; (void)args; return (pmc_not_supported()); }
static PyObject *cp_pmc_cpuinfo(PyObject *self, PyObject *args) { (void)self; (void)args; return (pmc_not_supported()); }
static PyObject *cp_pmc_allocate(PyObject *self, PyObject *args, PyObject *kw) { (void)self; (void)args; (void)kw; return (pmc_not_supported()); }
static PyObject *cp_pmc_attach(PyObject *self, PyObject *args) { (void)self; (void)args; return (pmc_not_supported()); }
static PyObject *cp_pmc_start(PyObject *self, PyObject *args) { (void)self; (void)args; return (pmc_not_supported()); }
static PyObject *cp_pmc_stop(PyObject *self, PyObject *args) { (void)self; (void)args; return (pmc_not_supported()); }
static PyObject *cp_pmc_read(PyObject *self, PyObject *args) { (void)self; (void)args; return (pmc_not_supported()); }
static PyObject *cp_pmc_release(PyObject *self, PyObject *args) { (void)self; (void)args; return (pmc_not_supported()); }
#endif

static PyObject *
cp_pmc_available(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;
#if defined(__FreeBSD__)
    Py_RETURN_TRUE;
#else
    Py_RETURN_FALSE;
#endif
}

static PyMethodDef methods[] = {
    {"cpuid", (PyCFunction)(void (*)(void))cp_cpuid,
        METH_VARARGS | METH_KEYWORDS, "cpuid(leaf, subleaf=0)"},
    {"rdtsc", cp_rdtsc, METH_NOARGS, "fenced rdtsc"},
    {"tsc_freq_khz", cp_tsc_freq_khz, METH_NOARGS, "TSC frequency in kHz"},
    {"cache_latency_probe", (PyCFunction)(void (*)(void))cp_cache_latency_probe,
        METH_VARARGS | METH_KEYWORDS, "dependent-load cache latency curve"},
    {"measure_chase_bucket", (PyCFunction)(void (*)(void))cp_measure_chase_bucket,
        METH_VARARGS | METH_KEYWORDS,
        "measure_chase_bucket(kb, iters=0, repeat=3, cpu=-1, superpage=1)"
        " -> (kb, ns) or None"},
    {"cache_bw_probe", (PyCFunction)(void (*)(void))cp_cache_bw_probe,
        METH_VARARGS | METH_KEYWORDS, "sequential read bandwidth curve"},
    {"pmc_available", cp_pmc_available, METH_NOARGS, "whether FreeBSD libpmc wrappers are compiled in"},
    {"pmc_init", cp_pmc_init, METH_NOARGS, "pmc_init()"},
    {"pmc_ncpu", cp_pmc_ncpu, METH_NOARGS, "pmc_ncpu()"},
    {"pmc_cpuinfo", cp_pmc_cpuinfo, METH_NOARGS, "pmc_cpuinfo() as a dict"},
    {"pmc_allocate", (PyCFunction)(void (*)(void))cp_pmc_allocate,
        METH_VARARGS | METH_KEYWORDS, "pmc_allocate(event, mode='TC', flags=0, cpu=-1, count=0)"},
    {"pmc_attach", cp_pmc_attach, METH_VARARGS, "pmc_attach(pmcid, pid)"},
    {"pmc_start", cp_pmc_start, METH_VARARGS, "pmc_start(pmcid)"},
    {"pmc_stop", cp_pmc_stop, METH_VARARGS, "pmc_stop(pmcid)"},
    {"pmc_read", cp_pmc_read, METH_VARARGS, "pmc_read(pmcid)"},
    {"pmc_release", cp_pmc_release, METH_VARARGS, "pmc_release(pmcid)"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "_fbcacheprobe",
    "FreeBSD AMD Zen cache-hotspot native engine",
    -1,
    methods
};

PyMODINIT_FUNC
PyInit__fbcacheprobe(void)
{
    PyObject *m;

    m = PyModule_Create(&moduledef);
    if (m == NULL)
        return (NULL);
    PyModule_AddIntConstant(m, "CACHE_LINE_SIZE", CL_SIZE);
#if defined(__FreeBSD__)
    PyModule_AddIntConstant(m, "PMC_CPU_ANY", PMC_CPU_ANY);
#else
    PyModule_AddIntConstant(m, "PMC_CPU_ANY", -1);
#endif
    return (m);
}
