/*
 * Functions for actually doing the system calls.
 */

#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "arch.h"
#include "child.h"
#include "random.h"
#include "sanitise.h"
#include "shm.h"
#include "syscall.h"
#include "pids.h"
#include "log.h"
#include "params.h"
#include "maps.h"
#include "tables.h"
#include "uid.h"
#include "utils.h"

#define __syscall_return(type, res) \
	do { \
	if ((unsigned long)(res) >= (unsigned long)(-125)) { \
		errno = -(res); \
		res = -1; \
	} \
	return (type) (res); \
} while (0)

#ifdef ARCH_IS_BIARCH
/*
 * This routine does 32 bit syscalls on 64 bit kernel.
 * 32-on-32 will just use syscall() directly from do_syscall() because do32bit flag is biarch only.
 */
static long syscall32(unsigned int call,
	unsigned long a1, unsigned long a2, unsigned long a3,
	unsigned long a4, unsigned long a5, unsigned long a6)
{
	long __res = 0;

#if defined(DO_32_SYSCALL)
	DO_32_SYSCALL
	__syscall_return(long, __res);
#else
	#error Implement 32-on-64 syscall macro for this architecture.
#endif
	return __res;
}
#else
#define syscall32(a,b,c,d,e,f,g) 0
#endif /* ARCH_IS_BIARCH */

static unsigned long do_syscall(int childno)
{
	struct syscallrecord *syscallrec;
	int nr, call;
	unsigned long a1, a2, a3, a4, a5, a6;
	unsigned long ret = 0;

	syscallrec = &shm->syscall[childno];
	nr = syscallrec->nr;

	/* Some architectures (IA64/MIPS) start their Linux syscalls
	 * At non-zero, and have other ABIs below.
	 */
	call = nr + SYSCALL_OFFSET;

	a1 = syscallrec->a1;
	a2 = syscallrec->a2;
	a3 = syscallrec->a3;
	a4 = syscallrec->a4;
	a5 = syscallrec->a5;
	a6 = syscallrec->a6;

	shm->total_syscalls_done++;
	shm->child_op_count[childno]++;
	(void)gettimeofday(&shm->tv[childno], NULL);

	if (syscalls[nr].entry->flags & NEED_ALARM)
		(void)alarm(1);

	errno = 0;

	if (syscallrec->do32bit == FALSE)
		ret = syscall(call, a1, a2, a3, a4, a5, a6);
	else
		ret = syscall32(call, a1, a2, a3, a4, a5, a6);

	syscallrec->errno_post = errno;

	if (syscalls[nr].entry->flags & NEED_ALARM)
		(void)alarm(0);

	return ret;
}

/*
 * Generate arguments, print them out, then call the syscall.
 *
 * returns a bool that determines whether we can keep doing syscalls
 * in this child.
 */
bool mkcall(int childno)
{
	struct syscallentry *entry;
	struct syscallrecord *syscallrec, *previous;
	unsigned int call;
	unsigned long ret = 0;

	syscallrec = &shm->syscall[childno];
	call = syscallrec->nr;
	entry = syscalls[call].entry;

	lock(&syscallrec->lock);
	syscallrec->a1 = (unsigned long) rand64();
	syscallrec->a2 = (unsigned long) rand64();
	syscallrec->a3 = (unsigned long) rand64();
	syscallrec->a4 = (unsigned long) rand64();
	syscallrec->a5 = (unsigned long) rand64();
	syscallrec->a6 = (unsigned long) rand64();

	generic_sanitise(childno);
	if (entry->sanitise)
		entry->sanitise(childno, syscallrec);

	unlock(&syscallrec->lock);

	output_syscall_prefix(childno);

	/* If we're going to pause, might as well sync pre-syscall */
	if (dopause == TRUE)
		synclogs();

	/* This is a special case for things like execve, which would replace our
	 * child process with something unknown to us. We use a 'throwaway' process
	 * to do the execve in, and let it run for a max of a seconds before we kill it */
#if 0
	if (syscalls[call].entry->flags & EXTRA_FORK) {
		pid_t extrapid;

		extrapid = fork();
		if (extrapid == 0) {
			ret = do_syscall(childno, &errno_saved);
			/* We should never get here. */
			syscallrec->retval = ret;
			_exit(EXIT_SUCCESS);
		} else {
			if (pid_alive(extrapid)) {
				sleep(1);
				kill(extrapid, SIGKILL);
			}
			//FIXME: Why would we only do this once ?
			generic_free_arg(childno);
			return FALSE;
		}
	}
#endif

	/* common-case, do the syscall in this child process. */
	ret = do_syscall(childno);
	syscallrec->retval = ret;

	if (IS_ERR(ret))
		shm->failures++;
	else
		shm->successes++;

	output_syscall_postfix(childno, ret);
	if (dopause == TRUE)
		sleep(1);

	/*
	 * If the syscall doesn't exist don't bother calling it next time.
	 * Some syscalls return ENOSYS depending on their arguments, we mark
	 * those as IGNORE_ENOSYS and keep calling them.
	 */
	if ((ret == -1UL) && (syscallrec->errno_post == ENOSYS) && !(entry->flags & IGNORE_ENOSYS)) {
		output(1, "%s (%d) returned ENOSYS, marking as inactive.\n",
			entry->name, call + SYSCALL_OFFSET);

		deactivate_syscall(call, syscallrec->do32bit);
	}

	if (entry->post)
	    entry->post(childno, syscallrec);

	/* store info for debugging. */
	previous = &shm->previous[childno];
	previous->nr = syscallrec->nr;
	previous->a1 = syscallrec->a1;
	previous->a2 = syscallrec->a2;
	previous->a3 = syscallrec->a3;
	previous->a4 = syscallrec->a4;
	previous->a5 = syscallrec->a5;
	previous->a6 = syscallrec->a6;
	previous->do32bit = syscallrec->do32bit;

	check_uid();

	generic_free_arg(childno);

	return TRUE;
}

bool this_syscallname(const char *thisname, int childno)
{
	unsigned int call = shm->syscall[childno].nr;
	struct syscallentry *syscall_entry = syscalls[call].entry;

	return strcmp(thisname, syscall_entry->name);
}
