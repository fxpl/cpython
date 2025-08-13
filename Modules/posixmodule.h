/* Declarations shared between the different POSIX-related modules */

#ifndef Py_POSIXMODULE_H
#define Py_POSIXMODULE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>          // uid_t
#endif

#ifdef Py_BUILD_CORE
#include "pycore_pystate.h"

static int
_cpu_count(void)
{
    const PyConfig *config = _Py_GetConfig();
    if (config->cpu_count > 0) {
        return config->cpu_count;
    }

    int ncpu = 0;
#ifdef MS_WINDOWS
# ifdef MS_WINDOWS_DESKTOP
    ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
# else
    ncpu = 0;
# endif

#elif defined(__hpux)
    ncpu = mpctl(MPC_GETNUMSPUS, NULL, NULL);

#elif defined(HAVE_SYSCONF) && defined(_SC_NPROCESSORS_ONLN)
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);

#elif defined(__VXWORKS__)
    ncpu = _Py_popcount32(vxCpuEnabledGet());

#elif defined(__DragonFly__) || \
      defined(__OpenBSD__)   || \
      defined(__FreeBSD__)   || \
      defined(__NetBSD__)    || \
      defined(__APPLE__)
    ncpu = 0;
    size_t len = sizeof(ncpu);
    int mib[2] = {CTL_HW, HW_NCPU};
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0) {
        ncpu = 0;
    }
#endif

    return ncpu;
}
#endif // Py_BUILD_CORE

#ifndef MS_WINDOWS
extern PyObject* _PyLong_FromUid(uid_t);

// Export for 'grp' shared extension
PyAPI_FUNC(PyObject*) _PyLong_FromGid(gid_t);

// Export for '_posixsubprocess' shared extension
PyAPI_FUNC(int) _Py_Uid_Converter(PyObject *, uid_t *);

// Export for 'grp' shared extension
PyAPI_FUNC(int) _Py_Gid_Converter(PyObject *, gid_t *);
#endif   // !MS_WINDOWS

#if (defined(PYPTHREAD_SIGMASK) || defined(HAVE_SIGWAIT) \
     || defined(HAVE_SIGWAITINFO) || defined(HAVE_SIGTIMEDWAIT))
#  define HAVE_SIGSET_T
#endif

extern int _Py_Sigset_Converter(PyObject *, void *);

#ifdef __cplusplus
}
#endif
#endif   // !Py_POSIXMODULE_H
