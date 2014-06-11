#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "arch.h"
#include "files.h"
#include "log.h"
#include "maps.h"
#include "net.h"
#include "random.h"
#include "random.h"
#include "sanitise.h"
#include "shm.h"
#include "syscall.h"
#include "tables.h"
#include "trinity.h"	// num_online_cpus

static unsigned int get_cpu(void)
{
	int i;
	i = rand() % 100;

	switch (i) {
	case 0: return -1;
	case 1: return rand() % 4096;
	case 2 ... 99:
		return rand() % num_online_cpus;
	}
	return 0;
}

static unsigned long handle_arg_address(int childno, unsigned int argnum)
{
	unsigned long addr = 0;

	if (argnum == 1)
		return (unsigned long) get_address();

	if (rand_bool())
		return (unsigned long) get_address();

	/* Half the time, we look to see if earlier args were also ARG_ADDRESS,
	 * and munge that instead of returning a new one from get_address() */

	addr = find_previous_arg_address(childno, argnum);

	switch (rand() % 4) {
	case 0:	break;	/* return unmodified */
	case 1:	addr++;
		break;
	case 2:	addr+= sizeof(int);
		break;
	case 3:	addr+= sizeof(long);
		break;
	}

	return addr;
}

static unsigned long handle_arg_range(struct syscallentry *entry, unsigned int argnum)
{
	unsigned long i;
	unsigned long low = 0, high = 0;

	switch (argnum) {
	case 1:	low = entry->low1range;
		high = entry->hi1range;
		break;
	case 2:	low = entry->low2range;
		high = entry->hi2range;
		break;
	case 3:	low = entry->low3range;
		high = entry->hi3range;
		break;
	case 4:	low = entry->low4range;
		high = entry->hi4range;
		break;
	case 5:	low = entry->low5range;
		high = entry->hi5range;
		break;
	case 6:	low = entry->low6range;
		high = entry->hi6range;
		break;
	}

	if (high == 0) {
		outputerr("%s forgets to set hirange!\n", entry->name);
		BUG("Fix syscall definition!\n");
	}

	i = (unsigned long) rand64() % high;
	if (i < low) {
		i += low;
		i &= high;
	}
	return i;
}

static unsigned long handle_arg_op(struct syscallentry *entry, unsigned int argnum)
{
	const unsigned long *values = NULL;
	unsigned int num = 0;
	unsigned long mask = 0;

	switch (argnum) {
	case 1:	num = entry->arg1list.num;
		values = entry->arg1list.values;
		break;
	case 2:	num = entry->arg2list.num;
		values = entry->arg2list.values;
		break;
	case 3:	num = entry->arg3list.num;
		values = entry->arg3list.values;
		break;
	case 4:	num = entry->arg4list.num;
		values = entry->arg4list.values;
		break;
	case 5:	num = entry->arg5list.num;
		values = entry->arg5list.values;
		break;
	case 6:	num = entry->arg6list.num;
		values = entry->arg6list.values;
		break;
	}

	if (num == 0)
		BUG("ARG_OP with 0 args. What?\n");

	if (values == NULL)
		BUG("ARG_OP with no values.\n");

	mask |= values[rand() % num];
	return mask;
}

unsigned long set_rand_bitmask(unsigned int num, const unsigned long *values)
{
	unsigned long i;
	unsigned long mask = 0;
	unsigned int bits;

	bits = rand_range(1, num);	/* num of bits to OR */
	for (i = 0; i < bits; i++)
		mask |= values[rand() % num];

	return mask;
}

static unsigned long handle_arg_list(struct syscallentry *entry, unsigned int argnum)
{
	unsigned long mask = 0;
	unsigned int num = 0;
	const unsigned long *values = NULL;

	switch (argnum) {
	case 1:	num = entry->arg1list.num;
		values = entry->arg1list.values;
		break;
	case 2:	num = entry->arg2list.num;
		values = entry->arg2list.values;
		break;
	case 3:	num = entry->arg3list.num;
		values = entry->arg3list.values;
		break;
	case 4:	num = entry->arg4list.num;
		values = entry->arg4list.values;
		break;
	case 5:	num = entry->arg5list.num;
		values = entry->arg5list.values;
		break;
	case 6:	num = entry->arg6list.num;
		values = entry->arg6list.values;
		break;
	}

	if (num == 0)
		BUG("ARG_LIST with 0 args. What?\n");

	if (values == NULL)
		BUG("ARG_LIST with no values.\n");

	mask = set_rand_bitmask(num, values);
	return mask;
}

static unsigned long handle_arg_randpage(void)
{
	return (unsigned long) page_rand;
}

static unsigned long handle_arg_iovec(struct syscallentry *entry, struct syscallrecord *rec, unsigned int argnum)
{
	unsigned long num_entries;

	num_entries = rand_range(1, 256);

	switch (argnum) {
	case 1:	if (entry->arg2type == ARG_IOVECLEN)
			rec->a2 = num_entries;
		break;
	case 2:	if (entry->arg3type == ARG_IOVECLEN)
			rec->a3 = num_entries;
		break;
	case 3:	if (entry->arg4type == ARG_IOVECLEN)
			rec->a4 = num_entries;
		break;
	case 4:	if (entry->arg5type == ARG_IOVECLEN)
			rec->a5 = num_entries;
		break;
	case 5:	if (entry->arg6type == ARG_IOVECLEN)
			rec->a6 = num_entries;
		break;
	}
	return (unsigned long) alloc_iovec(num_entries);
}

static unsigned long get_argval(int childno, unsigned int argnum)
{
	struct syscallrecord *rec;
	unsigned long val = 0;

	rec = &shm->children[childno]->syscall;

	switch (argnum) {
	case 1:	val = rec->a1;
		break;
	case 2:	val = rec->a2;
		break;
	case 3:	val = rec->a3;
		break;
	case 4:	val = rec->a4;
		break;
	case 5:	val = rec->a5;
		break;
	case 6:	val = rec->a6;
		break;
	}
	return val;
}


static unsigned long handle_arg_sockaddr(struct syscallentry *entry, struct syscallrecord *rec, unsigned int argnum)
{
	struct sockaddr *sockaddr = NULL;
	socklen_t sockaddrlen = 0;

	generate_sockaddr((struct sockaddr **)&sockaddr, &sockaddrlen, PF_NOHINT);

	switch (argnum) {
	case 1:	if (entry->arg2type == ARG_SOCKADDRLEN)
			rec->a2 = sockaddrlen;
		break;
	case 2:	if (entry->arg3type == ARG_SOCKADDRLEN)
			rec->a3 = sockaddrlen;
		break;
	case 3:	if (entry->arg4type == ARG_SOCKADDRLEN)
			rec->a4 = sockaddrlen;
		break;
	case 4:	if (entry->arg5type == ARG_SOCKADDRLEN)
			rec->a5 = sockaddrlen;
		break;
	case 5:	if (entry->arg6type == ARG_SOCKADDRLEN)
			rec->a6 = sockaddrlen;
		break;
	case 6:
		break;
	}
	return (unsigned long) sockaddr;
}

static unsigned long handle_arg_mode_t(void)
{
	unsigned int i, count;
	mode_t mode = 0;

	count = rand() % 9;

	for (i = 0; i < count; i++) {
		unsigned int j, bit;

		bit = rand() % 3;
		mode |= 1 << bit;
		j = rand() % 12;
		switch (j) {
		case 0: mode |= S_IRUSR; break;
		case 1: mode |= S_IWUSR; break;
		case 2: mode |= S_IXUSR; break;
		case 3: mode |= S_IRGRP; break;
		case 4: mode |= S_IWGRP; break;
		case 5: mode |= S_IXGRP; break;
		case 6: mode |= S_IROTH; break;
		case 7: mode |= S_IWOTH; break;
		case 8: mode |= S_IXOTH; break;
		case 9: mode |= S_ISUID; break;
		case 10: mode|= S_ISGID; break;
		case 11: mode|= S_ISVTX; break;
		}
	}
	return mode;
}

static enum argtype get_argtype(struct syscallentry *entry, unsigned int argnum)
{
	enum argtype argtype = 0;

	switch (argnum) {
	case 1:	argtype = entry->arg1type;
		break;
	case 2:	argtype = entry->arg2type;
		break;
	case 3:	argtype = entry->arg3type;
		break;
	case 4:	argtype = entry->arg4type;
		break;
	case 5:	argtype = entry->arg5type;
		break;
	case 6:	argtype = entry->arg6type;
		break;
	}

	return argtype;
}

static unsigned long fill_arg(int childno, unsigned int argnum)
{
	struct syscallrecord *rec;
	struct syscallentry *entry;
	unsigned int call;
	enum argtype argtype;

	rec = &shm->children[childno]->syscall;
	call = rec->nr;
	entry = syscalls[call].entry;

	if (argnum > entry->num_args)
		return 0;

	argtype = get_argtype(entry, argnum);

	switch (argtype) {
	case ARG_UNDEFINED:
	case ARG_RANDOM_LONG:
		return (unsigned long) rand64();

	case ARG_FD:
		return get_random_fd();

	case ARG_LEN:
		return (unsigned long) get_len();

	case ARG_ADDRESS:
		return handle_arg_address(childno, argnum);

	case ARG_NON_NULL_ADDRESS:
		return (unsigned long) get_non_null_address();

	case ARG_MMAP:
		return (unsigned long) get_map();

	case ARG_PID:
		return (unsigned long) get_pid();

	case ARG_RANGE:
		return handle_arg_range(entry, argnum);

	case ARG_OP:	/* Like ARG_LIST, but just a single value. */
		return handle_arg_op(entry, argnum);

	case ARG_LIST:
		return handle_arg_list(entry, argnum);

	case ARG_RANDPAGE:
		return handle_arg_randpage();

	case ARG_CPU:
		return (unsigned long) get_cpu();

	case ARG_PATHNAME:
		return (unsigned long) generate_pathname();

	case ARG_IOVEC:
		return handle_arg_iovec(entry, rec, argnum);

	case ARG_IOVECLEN:
	case ARG_SOCKADDRLEN:
		/* We already set the len in the ARG_IOVEC/ARG_SOCKADDR case
		 * So here we just return what we had set there. */
		return get_argval(childno, argnum);

	case ARG_SOCKADDR:
		return handle_arg_sockaddr(entry, rec, argnum);

	case ARG_MODE_T:
		return handle_arg_mode_t();
	}

	BUG("unreachable!\n");
}

void generic_sanitise(int childno)
{
	struct syscallrecord *rec;
	struct syscallentry *entry;
	unsigned int call;

	rec = &shm->children[childno]->syscall;
	call = rec->nr;
	entry = syscalls[call].entry;

	if (entry->arg1type != 0)
		rec->a1 = fill_arg(childno, 1);
	if (entry->arg2type != 0)
		rec->a2 = fill_arg(childno, 2);
	if (entry->arg3type != 0)
		rec->a3 = fill_arg(childno, 3);
	if (entry->arg4type != 0)
		rec->a4 = fill_arg(childno, 4);
	if (entry->arg5type != 0)
		rec->a5 = fill_arg(childno, 5);
	if (entry->arg6type != 0)
		rec->a6 = fill_arg(childno, 6);
}

void generic_free_arg(int childno)
{
	struct syscallentry *entry;
	unsigned int call = shm->children[childno]->syscall.nr;
	unsigned int i;

	entry = syscalls[call].entry;

	for (i = 1; i <= entry->num_args; i++) {
		enum argtype argtype;

		argtype = get_argtype(entry, i);

		if (argtype == ARG_IOVEC)
			free((void *) get_argval(childno, i));
	}
}
