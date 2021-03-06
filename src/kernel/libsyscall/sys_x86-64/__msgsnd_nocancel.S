#define __SYSCALL_32BIT_ARG_BYTES 16
#include "SYS.h"

#ifndef SYS_msgsnd_nocancel
#error "SYS_msgsnd_nocancel not defined. The header files libsyscall is building against do not match syscalls.master."
#endif

#if defined(__x86_64__)
__SYSCALL2(__msgsnd_nocancel, msgsnd_nocancel, 4, cerror_nocancel)
#endif

#if defined(__x86_64__)
	.globl	msgsnd$NOCANCEL
		msgsnd$NOCANCEL = __msgsnd_nocancel
#endif

