#ifndef MESH_IOCTL_H
#define MESH_IOCTL_H

/*
 * ioctl()'s request argument, in whatever type the libc in front of us declares it.
 *
 * The two disagree: glibc's second parameter is `unsigned long`, musl's - which the release
 * build links against - is `int`. That is invisible for most requests and not for the ones whose
 * encoding sets the high bit: every _IOR/_IOW code with a direction of "read" does, so EVIOCGBIT,
 * EVIOCGNAME, HCIGETCONNLIST and the USBDEVFS verbs all arrive as constants above INT_MAX. Handed
 * straight to musl they are an implicit narrowing the compiler reports as -Woverflow ("changes
 * value from 2153792801 to -2141174495"), and a request built from a size_t length is a
 * -Wconversion on top of it.
 *
 * The value the kernel sees is the same either way - the ioctl syscall reads the low 32 bits - so
 * the conversion is a statement about the *declaration* rather than a change of meaning. Making
 * it explicit, and per libc, is what keeps the shipping build's log clean without turning the
 * warning off for a file: -Woverflow on a request code is noise, and -Woverflow on anything else
 * is worth reading.
 *
 * <sys/ioctl.h> is included here rather than left to the caller, and that is load-bearing rather
 * than tidiness. __GLIBC__ comes from <features.h>, which this header cannot define for itself;
 * included before any system header it would see the macro undefined and take musl's branch on a
 * glibc host. Pulling in the header that declares the call being adapted settles it, and settles
 * it in the one place that has to be right.
 */

#include <sys/ioctl.h>

#if defined(__GLIBC__)
typedef unsigned long mesh_ioctl_request;
#else
typedef int mesh_ioctl_request;
#endif

/*
 * A small static helper rather than a macro, per AGENTS.md: the parameter is `unsigned long`,
 * which every _IO* code widens to without complaint, and the one narrowing conversion happens
 * inside, once, where it is written down.
 */
static inline mesh_ioctl_request mesh_ioctl_request_of(unsigned long request) {
    return (mesh_ioctl_request)request;
}

#endif /* MESH_IOCTL_H */
