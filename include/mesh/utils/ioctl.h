#ifndef MESH_IOCTL_H
#define MESH_IOCTL_H

/*
 * The request argument for ioctl(), narrowed to whatever the libc in front of us declares.
 *
 * The two disagree: glibc's second parameter is `unsigned long`, musl's - which the release
 * build links against - is `int`. That is invisible for most requests and not for the ones
 * whose encoding sets the high bit: every _IOR/_IOW code with a direction of "read" does, so
 * EVIOCGBIT, EVIOCGNAME, HCIGETCONNLIST and the USBDEVFS verbs all arrive as constants above
 * INT_MAX. Handed straight to musl they are an implicit narrowing the compiler reports as
 * -Woverflow ("changes value from 2153792801 to -2141174495"), and a request built from a
 * size_t length is a -Wconversion on top of it.
 *
 * The value the kernel sees is the same either way - the ioctl syscall takes the low 32 bits -
 * so the cast is a statement about the *declaration* rather than a change of meaning. Doing it
 * explicitly, and per libc, is what keeps the shipping build's log clean without turning the
 * warning off for the file: -Woverflow firing on a request code is noise, and firing on
 * anything else is a bug worth reading.
 *
 * A macro rather than a wrapper function because the parameter's type is the whole subject: a
 * helper would have to pick one of the two and do the narrowing the caller was trying to avoid.
 */
#if defined(__GLIBC__)
#define MESH_IOCTL_REQUEST(req) ((unsigned long)(req))
#else
#define MESH_IOCTL_REQUEST(req) ((int)(req))
#endif

#endif /* MESH_IOCTL_H */
