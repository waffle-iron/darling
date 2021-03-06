#define __SYSCALL_32BIT_ARG_BYTES 12
#include "SYS.h"

#ifndef SYS_madvise
#error "SYS_madvise not defined. The header files libsyscall is building against do not match syscalls.master."
#endif

#if defined(__i386__)
__SYSCALL2(madvise, madvise, 3, cerror_nocancel)
#endif

#if defined(__i386__)
	.globl	posix_madvise
		posix_madvise = madvise
#endif

