/*
 * Kernel Debugger Architecture Independent Main Code
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1999-2004 Silicon Graphics, Inc.  All Rights Reserved.
 * Copyright (C) 2000 Stephane Eranian <eranian@hpl.hp.com>
 */

#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/nmi.h>
#include <linux/ptrace.h>
#include <linux/cpu.h>
#include <linux/kdebug.h>

#include "lkmd.h"
#include "lkmd_private.h"

// #include <acpi/acpi_bus.h>
#include <linux/acpi.h>

// #include <asm/system.h>
#include <asm/kdebug.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>

/*
 * Kernel debugger state flags
 */
volatile int kdb_flags;

/*
 * kdb_lock protects updates to kdb_initial_cpu.  Used to
 * single thread processors through the kernel debugger.
 */
static DEFINE_SPINLOCK(kdb_lock);
volatile int kdb_initial_cpu = -1;		/* cpu number that owns kdb */
int kdb_seqno = 2;				/* how many times kdb has been entered */

volatile int kdb_nextline = 1;
static volatile int kdb_new_cpu;		/* Which cpu to switch to */

volatile int kdb_state[NR_CPUS];		/* Per cpu state */

const struct task_struct *lkmd_current_task;
struct pt_regs *kdb_current_regs;

int kdb_on = 1;				/* Default is on */

const char *kdb_diemsg;

#ifdef kdba_setjmp
	/*
	 * Must have a setjmp buffer per CPU.  Switching cpus will
	 * cause the jump buffer to be setup for the new cpu, and
	 * subsequent switches (and pager aborts) will use the
	 * appropriate per-processor values.
	 */
kdb_jmp_buf *kdbjmpbuf;
#endif	/* kdba_setjmp */

	/*
	 * kdb_commands describes the available commands.
	 */
static kdbtab_t *kdb_commands;
static int kdb_max_commands;

typedef struct _kdbmsg {
	int	km_diag;	/* kdb diagnostic */
	char	*km_msg;	/* Corresponding message text */
} kdbmsg_t;

#define KDBMSG(msgnum, text) \
	{ KDB_##msgnum, text }

static kdbmsg_t kdbmsgs[] = {
	KDBMSG(NOTFOUND,"Command Not Found"),
	KDBMSG(ARGCOUNT, "Improper argument count, see usage."),
	KDBMSG(BADWIDTH, "Illegal value for BYTESPERWORD use 1, 2, 4 or 8, 8 is only allowed on 64 bit systems"),
	KDBMSG(BADRADIX, "Illegal value for RADIX use 8, 10 or 16"),
	KDBMSG(NOTENV, "Cannot find environment variable"),
	KDBMSG(NOENVVALUE, "Environment variable should have value"),
	KDBMSG(NOTIMP, "Command not implemented"),
	KDBMSG(ENVFULL, "Environment full"),
	KDBMSG(ENVBUFFULL, "Environment buffer full"),
	KDBMSG(TOOMANYBPT, "Too many breakpoints defined"),
	KDBMSG(TOOMANYDBREGS, "More breakpoints than db registers defined"),
	KDBMSG(DUPBPT, "Duplicate breakpoint address"),
	KDBMSG(BPTNOTFOUND, "Breakpoint not found"),
	KDBMSG(BADMODE, "Invalid IDMODE"),
	KDBMSG(BADINT, "Illegal numeric value"),
	KDBMSG(INVADDRFMT, "Invalid symbolic address format"),
	KDBMSG(BADREG, "Invalid register name"),
	KDBMSG(BADCPUNUM, "Invalid cpu number"),
	KDBMSG(BADLENGTH, "Invalid length field"),
	KDBMSG(NOBP, "No Breakpoint exists"),
	KDBMSG(BADADDR, "Invalid address"),
};
#undef KDBMSG

static const int __nkdb_err = sizeof(kdbmsgs) / sizeof(kdbmsg_t);


/*
 * Initial environment.   This is all kept static and local to
 * this file.   We don't want to rely on the memory allocation
 * mechanisms in the kernel, so we use a very limited allocate-only
 * heap for new and altered environment variables.  The entire
 * environment is limited to a fixed number of entries (add more
 * to __env[] if required) and a fixed amount of heap (add more to
 * KDB_ENVBUFSIZE if required).
 */

static char *__env[] = {
#if defined(CONFIG_SMP)
 "PROMPT=[%d]LKMD> ",
 "MOREPROMPT=[%d]more> ",
#else
 "PROMPT=LKMD> ",
 "MOREPROMPT=more> ",
#endif
 "RADIX=16",
 "LINES=24",
 "COLUMNS=80",
 "MDCOUNT=8",			/* lines of md output */
 "BTARGS=9",			/* 9 possible args in bt */
 KDB_PLATFORM_ENV,
 "DTABCOUNT=30",
 "NOSECT=1",
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
 (char *)0,
};

static const int __nenv = (sizeof(__env) / sizeof(char *));

/* external commands: */
// int kdb_debuginfo_print(int argc, const char **argv);
// int kdb_pxhelp(int argc, const char **argv);
// int kdb_walkhelp(int argc, const char **argv);
// int kdb_walk(int argc, const char **argv);

struct task_struct *kdb_curr_task(int cpu)
{
	struct task_struct *p = lkmd_curr_task(cpu);
#ifdef	_TIF_MCA_INIT
	struct kdb_running_process *krp = kdb_running_process + cpu;
	if ((task_thread_info(p)->flags & _TIF_MCA_INIT) && krp->p)
		p = krp->p;
#endif
	return p;
}

/*
 * kdbgetenv
 *
 *	This function will return the character string value of
 *	an environment variable.
 *
 * Parameters:
 *	match	A character string representing an environment variable.
 * Outputs:
 *	None.
 * Returns:
 *	NULL	No environment variable matches 'match'
 *	char*	Pointer to string value of environment variable.
 * Locking:
 *	No locking considerations required.
 * Remarks:
 */
char *kdbgetenv(const char *match)
{
	char **ep = __env;
	int matchlen = strlen(match);
	int i;

	for(i = 0; i < __nenv; i++) {
		char *e = *ep++;

		if (!e) continue;

		if ((strncmp(match, e, matchlen) == 0) && ((e[matchlen] == '\0') ||(e[matchlen] == '='))) {
			char *cp = strchr(e, '=');
			return (cp ? ++cp :"");
		}
	}
	return NULL;
}

/*
 * kdballocenv
 *
 *	This function is used to allocate bytes for environment entries.
 *
 * Parameters:
 *	match	A character string representing a numeric value
 * Outputs:
 *	*value  the unsigned long represntation of the env variable 'match'
 * Returns:
 *	Zero on success, a kdb diagnostic on failure.
 * Locking:
 *	No locking considerations required.  Must be called with all
 *	processors halted.
 * Remarks:
 *	We use a static environment buffer (envbuffer) to hold the values
 *	of dynamically generated environment variables (see kdb_set).  Buffer
 *	space once allocated is never free'd, so over time, the amount of space
 *	(currently 512 bytes) will be exhausted if env variables are changed
 *	frequently.
 */
static char *kdballocenv(size_t bytes)
{
#define	KDB_ENVBUFSIZE	512
	static char envbuffer[KDB_ENVBUFSIZE];
	static int envbufsize;
	char *ep = NULL;

	if ((KDB_ENVBUFSIZE - envbufsize) >= bytes) {
		ep = &envbuffer[envbufsize];
		envbufsize += bytes;
	}
	return ep;
}

/*
 * kdbgetulenv
 *
 *	This function will return the value of an unsigned long-valued
 *	environment variable.
 *
 * Parameters:
 *	match	A character string representing a numeric value
 * Outputs:
 *	*value  the unsigned long represntation of the env variable 'match'
 * Returns:
 *	Zero on success, a kdb diagnostic on failure.
 * Locking:
 *	No locking considerations required.
 * Remarks:
 */

static int kdbgetulenv(const char *match, unsigned long *value)
{
	char *ep;

	ep = kdbgetenv(match);
	if (!ep) return KDB_NOTENV;
	if (strlen(ep) == 0) return KDB_NOENVVALUE;

	*value = simple_strtoul(ep, NULL, 0);

	return 0;
}

/*
 * kdbgetintenv
 *
 *	This function will return the value of an integer-valued
 *	environment variable.
 *
 * Parameters:
 *	match	A character string representing an integer-valued env variable
 * Outputs:
 *	*value  the integer representation of the environment variable 'match'
 * Returns:
 *	Zero on success, a kdb diagnostic on failure.
 * Locking:
 *	No locking considerations required.
 * Remarks:
 */

int kdbgetintenv(const char *match, int *value) {
	unsigned long val;
	int diag;

	diag = kdbgetulenv(match, &val);
	if (!diag) {
		*value = (int) val;
	}
	return diag;
}

/*
 * kdbgetularg
 *
 *	This function will convert a numeric string
 *	into an unsigned long value.
 *
 * Parameters:
 *	arg	A character string representing a numeric value
 * Outputs:
 *	*value  the unsigned long represntation of arg.
 * Returns:
 *	Zero on success, a kdb diagnostic on failure.
 * Locking:
 *	No locking considerations required.
 * Remarks:
 */

int kdbgetularg(const char *arg, unsigned long *value)
{
	char *endp;
	unsigned long val;

	val = simple_strtoul(arg, &endp, 0);

	if (endp == arg) {
		/*
		 * Try base 16, for us folks too lazy to type the
		 * leading 0x...
		 */
		val = simple_strtoul(arg, &endp, 16);
		if (endp == arg)
			return KDB_BADINT;
	}

	*value = val;

	return 0;
}

/*
 * kdb_set
 *
 *	This function implements the 'set' command.  Alter an existing
 *	environment variable or create a new one.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

static int kdb_set(int argc, const char **argv)
{
	int i;
	char *ep;
	size_t varlen, vallen;

	/*
	 * we can be invoked two ways:
	 *   set var=value    argv[1]="var", argv[2]="value"
	 *   set var = value  argv[1]="var", argv[2]="=", argv[3]="value"
	 * - if the latter, shift 'em down.
	 */
	if (argc == 3) {
		argv[2] = argv[3];
		argc--;
	}

	if (argc != 2)
		return KDB_ARGCOUNT;

	/*
	 * Check for internal variables
	 */
	if (strcmp(argv[1], "KDBDEBUG") == 0) {
		unsigned int debugflags;
		char *cp;

		debugflags = simple_strtoul(argv[2], &cp, 0);
		if (cp == argv[2] || debugflags & ~KDB_DEBUG_FLAG_MASK) {
			lkmd_printf("kdb: illegal debug flags '%s'\n",
				    argv[2]);
			return 0;
		}
		kdb_flags = (kdb_flags & ~(KDB_DEBUG_FLAG_MASK << KDB_DEBUG_FLAG_SHIFT))
			  | (debugflags << KDB_DEBUG_FLAG_SHIFT);

		return 0;
	}

	/*
	 * Tokenizer squashed the '=' sign.  argv[1] is variable
	 * name, argv[2] = value.
	 */
	varlen = strlen(argv[1]);
	vallen = strlen(argv[2]);
	ep = kdballocenv(varlen + vallen + 2);
	if (ep == (char *)0)
		return KDB_ENVBUFFULL;

	sprintf(ep, "%s=%s", argv[1], argv[2]);

	ep[varlen+vallen+1]='\0';

	for(i=0; i<__nenv; i++) {
		if (__env[i]
		 && ((strncmp(__env[i], argv[1], varlen)==0)
		   && ((__env[i][varlen] == '\0')
		    || (__env[i][varlen] == '=')))) {
			__env[i] = ep;
			return 0;
		}
	}

	/*
	 * Wasn't existing variable.  Fit into slot.
	 */
	for(i=0; i<__nenv-1; i++) {
		if (__env[i] == (char *)0) {
			__env[i] = ep;
			return 0;
		}
	}

	return KDB_ENVFULL;
}

static int kdb_check_regs(void)
{
	if (!kdb_current_regs) {
		lkmd_printf("No current kdb registers."
		           "  You may need to select another task\n");
		return KDB_BADREG;
	}
	return 0;
}

/*
 * kdbgetaddrarg
 *
 *	This function is responsible for parsing an
 *	address-expression and returning the value of
 *	the expression, symbol name, and offset to the caller.
 *
 *	The argument may consist of a numeric value (decimal or
 *	hexidecimal), a symbol name, a register name (preceeded
 *	by the percent sign), an environment variable with a numeric
 *	value (preceeded by a dollar sign) or a simple arithmetic
 *	expression consisting of a symbol name, +/-, and a numeric
 *	constant value (offset).
 *
 * Parameters:
 *	argc	- count of arguments in argv
 *	argv	- argument vector
 *	*nextarg - index to next unparsed argument in argv[]
 *	regs	- Register state at time of KDB entry
 * Outputs:
 *	*value	- receives the value of the address-expression
 *	*offset - receives the offset specified, if any
 *	*name   - receives the symbol name, if any
 *	*nextarg - index to next unparsed argument in argv[]
 *
 * Returns:
 *	zero is returned on success, a kdb diagnostic code is
 *      returned on error.
 *
 * Locking:
 *	No locking requirements.
 *
 * Remarks:
 *
 */

int kdbgetaddrarg(int argc, const char **argv, int *nextarg,
	      kdb_machreg_t *value,  long *offset,
	      char **name)
{
	kdb_machreg_t addr;
	unsigned long off = 0;
	int positive;
	int diag;
	int found = 0;
	char *symname;
	char symbol = '\0';
	char *cp;
	kdb_symtab_t symtab;

	/*
	 * Process arguments which follow the following syntax:
	 *
	 *  symbol | numeric-address [+/- numeric-offset]
	 *  %register
	 *  $environment-variable
	 */

	if (*nextarg > argc) {
		return KDB_ARGCOUNT;
	}

	symname = (char *)argv[*nextarg];

	/*
	 * If there is no whitespace between the symbol
	 * or address and the '+' or '-' symbols, we
	 * remember the character and replace it with a
	 * null so the symbol/value can be properly parsed
	 */
	if ((cp = strpbrk(symname, "+-")) != NULL) {
		symbol = *cp;
		*cp++ = '\0';
	}

	if (symname[0] == '$') {
		diag = kdbgetulenv(&symname[1], &addr);
		if (diag)
			return diag;
	} else if (symname[0] == '%') {
		if ((diag = kdb_check_regs()))
			return diag;
		diag = kdba_getregcontents(&symname[1], kdb_current_regs, &addr);
		if (diag)
			return diag;
	} else {
		found = lkmd_get_sym_val(symname, &symtab);
		if (found) {
			addr = symtab.sym_start;
		} else {
			diag = kdbgetularg(argv[*nextarg], &addr);
			if (diag)
				return diag;
		}
	}

	if (!found)
		found = kdbnearsym(addr, &symtab);

	(*nextarg)++;

	if (name)
		*name = symname;
	if (value)
		*value = addr;
	if (offset && name && *name)
		*offset = addr - symtab.sym_start;

	if ((*nextarg > argc)
	 && (symbol == '\0'))
		return 0;

	/*
	 * check for +/- and offset
	 */

	if (symbol == '\0') {
		if ((argv[*nextarg][0] != '+')
		 && (argv[*nextarg][0] != '-')) {
			/*
			 * Not our argument.  Return.
			 */
			return 0;
		} else {
			positive = (argv[*nextarg][0] == '+');
			(*nextarg)++;
		}
	} else
		positive = (symbol == '+');

	/*
	 * Now there must be an offset!
	 */
	if ((*nextarg > argc)
	 && (symbol == '\0')) {
		return KDB_INVADDRFMT;
	}

	if (!symbol) {
		cp = (char *)argv[*nextarg];
		(*nextarg)++;
	}

	diag = kdbgetularg(cp, &off);
	if (diag)
		return diag;

	if (!positive)
		off = -off;

	if (offset)
		*offset += off;

	if (value)
		*value += off;

	return 0;
}

static void kdb_cmderror(int diag)
{
	int i;

	if (diag >= 0) {
		lkmd_printf("no error detected (diagnostic is %d)\n", diag);
		return;
	}

	for(i=0; i<__nkdb_err; i++) {
		if (kdbmsgs[i].km_diag == diag) {
			lkmd_printf("diag: %d: %s\n", diag, kdbmsgs[i].km_msg);
			return;
		}
	}

	lkmd_printf("Unknown diag %d\n", -diag);
}

#define CMD_BUFLEN		200	/* lkmd_printf: max printline size == 256 */
static char cmd_cur[CMD_BUFLEN];

/*
 * kdb_parse
 *
 *	Parse the command line, search the command table for a
 *	matching command and invoke the command function.
 *	This function may be called recursively, if it is, the second call
 *	will overwrite argv and cbuf.  It is the caller's responsibility to
 *	save their argv if they recursively call kdb_parse().
 *
 * Parameters:
 *      cmdstr	The input command line to be parsed.
 *	regs	The registers at the time kdb was entered.
 * Outputs:
 *	None.
 * Returns:
 *	Zero for success, a kdb diagnostic if failure.
 * Locking:
 * 	None.
 * Remarks:
 *	Limited to 20 tokens.
 *
 *	Real rudimentary tokenization. Basically only whitespace
 *	is considered a token delimeter (but special consideration
 *	is taken of the '=' sign as used by the 'set' command).
 *
 *	The algorithm used to tokenize the input string relies on
 *	there being at least one whitespace (or otherwise useless)
 *	character between tokens as the character immediately following
 *	the token is altered in-place to a null-byte to terminate the
 *	token string.
 */

#define MAXARGC	20

int kdb_parse(const char *cmdstr)
{
	static char *argv[MAXARGC];
	static int argc = 0;
	static char cbuf[CMD_BUFLEN+2];
	char *cp;
	char *cpp, quoted;
	kdbtab_t *tp;
	int i, escaped, ignore_errors = 0;

	/*
	 * First tokenize the command string.
	 */
	cp = (char *)cmdstr;

	if (KDB_FLAG(CMD_INTERRUPT)) {
		/* Previous command was interrupted, newline must not repeat the command */
		KDB_FLAG_CLEAR(CMD_INTERRUPT);
		argc = 0;	/* no repeat */
	}

	if (*cp != '\n' && *cp != '\0') {
		argc = 0;
		cpp = cbuf;
		while (*cp) {
			/* skip whitespace */
			while (isspace(*cp)) cp++;
			if ((*cp == '\0') || (*cp == '\n'))
				break;

			if (cpp >= cbuf + CMD_BUFLEN) {
				lkmd_printf("kdb_parse: command buffer overflow, command ignored\n%s\n", cmdstr);
				return KDB_NOTFOUND;
			}
			if (argc >= MAXARGC - 1) {
				lkmd_printf("kdb_parse: too many arguments, command ignored\n%s\n", cmdstr);
				return KDB_NOTFOUND;
			}
			argv[argc++] = cpp;
			escaped = 0;
			quoted = '\0';
			/* Copy to next unquoted and unescaped whitespace or '=' */
			while (*cp && *cp != '\n' && (escaped || quoted || !isspace(*cp))) {
				if (cpp >= cbuf + CMD_BUFLEN)
					break;
				if (escaped) {
					escaped = 0;
					*cpp++ = *cp++;
					continue;
				}
				if (*cp == '\\') {
					escaped = 1;
					++cp;
					continue;
				}
				if (*cp == quoted) {
					quoted = '\0';
				} else if (*cp == '\'' || *cp == '"') {
					quoted = *cp;
				}
				if ((*cpp = *cp++) == '=' && !quoted)
					break;
				++cpp;
			}
			*cpp++ = '\0';	/* Squash a ws or '=' character */
		}
	}
	if (!argc)
		return 0;
	if (argv[0][0] == '-' && argv[0][1] && (argv[0][1] < '0' || argv[0][1] > '9')) {
		ignore_errors = 1;
		++argv[0];
	}

	for(tp=kdb_commands, i=0; i < kdb_max_commands; i++,tp++) {
		if (tp->cmd_name) {
			/*
			 * If this command is allowed to be abbreviated,
			 * check to see if this is it.
			 */

			if (tp->cmd_minlen && (strlen(argv[0]) <= tp->cmd_minlen)) {
				if (strncmp(argv[0], tp->cmd_name, tp->cmd_minlen) == 0)
					break;
			}

			if (strcmp(argv[0], tp->cmd_name) == 0)
				break;
		}
	}

	/*
	 * If we don't find a command by this name, see if the first
	 * few characters of this match any of the known commands.
	 * e.g., md1c20 should match md.
	 */
	if (i == kdb_max_commands) {
		for(tp = kdb_commands, i = 0; i < kdb_max_commands; i++, tp++) {
			if (tp->cmd_name) {
				if (strncmp(argv[0], tp->cmd_name, strlen(tp->cmd_name))==0)
					break;
			}
		}
	}

	if (i < kdb_max_commands) {
		int result;
		KDB_STATE_SET(CMD);
		result = (*tp->cmd_func)(argc-1,
				       (const char**)argv);
		if (result && ignore_errors && result > KDB_CMD_GO)
			result = 0;
		KDB_STATE_CLEAR(CMD);
		switch (tp->cmd_repeat) {
		case KDB_REPEAT_NONE:
			argc = 0;
			if (argv[0])
				*(argv[0]) = '\0';
			break;
		case KDB_REPEAT_NO_ARGS:
			argc = 1;
			if (argv[1])
				*(argv[1]) = '\0';
			break;
		case KDB_REPEAT_WITH_ARGS:
			break;
		}
		return result;
	}

	/*
	 * If the input with which we were presented does not
	 * map to an existing command, attempt to parse it as an
	 * address argument and display the result.   Useful for
	 * obtaining the address of a variable, or the nearest symbol
	 * to an address contained in a register.
	 */
	{
		kdb_machreg_t value;
		char *name = NULL;
		long offset;
		int nextarg = 0;

		if (kdbgetaddrarg(0, (const char **)argv, &nextarg,
				  &value, &offset, &name)) {
			return KDB_NOTFOUND;
		}

		lkmd_printf("%s = ", argv[0]);
		kdb_symbol_print(value, NULL, KDB_SP_DEFAULT);
		lkmd_printf("\n");
		return 0;
	}
}

/*
 * kdb_reboot
 *
 *	This function implements the 'reboot' command.  Reboot the system
 *	immediately.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 *	Shouldn't return from this function.
 */

static int kdb_reboot(int argc, const char **argv)
{
	emergency_restart();
	lkmd_printf("Hmm, kdb_reboot did not reboot, spinning here\n");
	while (1) {};
	/* NOTREACHED */
	return 0;
}

static int kdb_quiet(int reason)
{
	return (reason == KDB_REASON_CPU_UP || reason == KDB_REASON_SILENT);
}

/*
 * kdb_local
 *
 *	The main code for kdb.  This routine is invoked on a specific
 *	processor, it is not global.  The main kdb() routine ensures
 *	that only one processor at a time is in this routine.  This
 *	code is called with the real reason code on the first entry
 *	to a kdb session, thereafter it is called with reason SWITCH,
 *	even if the user goes back to the original cpu.
 *
 * Inputs:
 *	reason		The reason KDB was invoked
 *	error		The hardware-defined error code
 *	regs		The exception frame at time of fault/breakpoint.  NULL
 *			for reason SILENT or CPU_UP, otherwise valid.
 *	db_result	Result code from the break or debug point.
 * Returns:
 *	0	KDB was invoked for an event which it wasn't responsible
 *	1	KDB handled the event for which it was invoked.
 *	KDB_CMD_GO	User typed 'go'.
 *	KDB_CMD_CPU	User switched to another cpu.
 *	KDB_CMD_SS	Single step.
 *	KDB_CMD_SSB	Single step until branch.
 * Locking:
 *	none
 * Remarks:
 *	none
 */

static int kdb_local(kdb_reason_t reason, int error, struct pt_regs *regs, kdb_dbtrap_t db_result)
{
	char *cmdbuf;
	int diag;
	struct task_struct *kdb_current = kdb_curr_task(smp_processor_id());

	KDB_DEBUG_STATE("kdb_local 1", reason);

	if (kdb_quiet(reason)) {
		/* no message */
	} else if (reason == KDB_REASON_DEBUG) {
		/* special case below */
	} else {
		lkmd_printf("\nEntering LKMD (current=0x%p, pid %d) ", kdb_current, kdb_current->pid);
#if defined(CONFIG_SMP)
		lkmd_printf("on processor %d ", smp_processor_id());
#endif
	}

	switch (reason) {
	case KDB_REASON_DEBUG:
	{
		/* If re-entering kdb after a single step command, don't print the message. */
		switch(db_result) {
		case KDB_DB_BPT:
			lkmd_printf("\nEntering LKMD (0x%p, pid %d) ", kdb_current, kdb_current->pid);
#if defined(CONFIG_SMP)
			lkmd_printf("on processor %d ", smp_processor_id());
#endif
			lkmd_printf("due to Debug @ " kdb_machreg_fmt "\n", kdba_getpc(regs));
			break;
		case KDB_DB_SSB:
			/* In the midst of ssb command. Just return. */
			KDB_DEBUG_STATE("kdb_local 3", reason);
			return KDB_CMD_SSB;	/* Continue with SSB command */

			break;
		case KDB_DB_SS:
			break;
		case KDB_DB_SSBPT:
			KDB_DEBUG_STATE("kdb_local 4", reason);
			return 1;	/* kdba_db_trap did the work */
		default:
			lkmd_printf("kdb: Bad result from kdba_db_trap: %d\n", db_result);
			break;
		}
	}
		break;
	case KDB_REASON_ENTER:
		if (KDB_STATE(KEYBOARD))
			lkmd_printf("due to Keyboard Entry @ " kdb_machreg_fmt "\n", kdba_getpc(regs));
		else {
			lkmd_printf("due to KDB_ENTER() @ " kdb_machreg_fmt "\n", kdba_getpc(regs));
		}
		break;
	case KDB_REASON_KEYBOARD:
		KDB_STATE_SET(KEYBOARD);
		lkmd_printf("due to Keyboard Entry\n");
		break;
	case KDB_REASON_ENTER_SLAVE:	/* drop through, slaves only get released via cpu switch */
	case KDB_REASON_SWITCH:
		lkmd_printf("due to cpu switch\n");
		if (KDB_STATE(GO_SWITCH)) {
			KDB_STATE_CLEAR(GO_SWITCH);
			KDB_DEBUG_STATE("kdb_local 5", reason);
			return KDB_CMD_GO;
		}
		break;
	case KDB_REASON_OOPS:
		lkmd_printf("Oops: %s\n", kdb_diemsg);
		lkmd_printf("due to oops @ " kdb_machreg_fmt "\n", kdba_getpc(regs));
		kdba_dumpregs(regs, NULL, NULL);
		break;
	case KDB_REASON_NMI:
		lkmd_printf("due to NonMaskable Interrupt @ " kdb_machreg_fmt "\n",
			  kdba_getpc(regs));
		kdba_dumpregs(regs, NULL, NULL);
		break;
	case KDB_REASON_BREAK:
		lkmd_printf("due to Breakpoint @ " kdb_machreg_fmt "\n", kdba_getpc(regs));
		/* Determine if this breakpoint is one that we are interested in. */
		if (db_result != KDB_DB_BPT) {
			lkmd_printf("kdb: error return from kdba_bp_trap: %d\n", db_result);
			KDB_DEBUG_STATE("kdb_local 6", reason);
			return 0;	/* Not for us, dismiss it */
		}
		break;
	case KDB_REASON_RECURSE:
		lkmd_printf("due to Recursion @ " kdb_machreg_fmt "\n", kdba_getpc(regs));
		break;
	case KDB_REASON_CPU_UP:
	case KDB_REASON_SILENT:
		KDB_DEBUG_STATE("kdb_local 7", reason);
		if (reason == KDB_REASON_CPU_UP)
			kdba_cpu_up();
		return KDB_CMD_GO;	/* Silent entry, silent exit */
		break;
	default:
		lkmd_printf("kdb: unexpected reason code: %d\n", reason);
		KDB_DEBUG_STATE("kdb_local 8", reason);
		return 0;	/* Not for us, dismiss it */
	}

	kdba_set_current_task(kdb_current);

	while (1) {
		/* Initialize pager context. */
		kdb_nextline = 1;
		KDB_STATE_CLEAR(SUPPRESS);
#ifdef kdba_setjmp
		/*
		 * Use kdba_setjmp/kdba_longjmp to break out of
		 * the pager early and to attempt to recover from kdb errors.
		 */
		KDB_STATE_CLEAR(LONGJMP);
		if (kdbjmpbuf) {
			if (kdba_setjmp(&kdbjmpbuf[smp_processor_id()])) {
				/* Command aborted (usually in pager) */
				continue;
			}
			else
				KDB_STATE_SET(LONGJMP);
		}
#endif	/* kdba_setjmp */

		cmdbuf = cmd_cur;
		*cmdbuf = '\0';

#if defined(CONFIG_SMP)
		snprintf(kdb_prompt_str, CMD_BUFLEN, kdbgetenv("PROMPT"), smp_processor_id());
#else
		snprintf(kdb_prompt_str, CMD_BUFLEN, kdbgetenv("PROMPT"));
#endif
		/* Fetch command from keyboard */
		cmdbuf = kdb_getstr(cmdbuf, CMD_BUFLEN, kdb_prompt_str);
		diag = kdb_parse(cmdbuf);
		if (diag == KDB_NOTFOUND) {
			lkmd_printf("Unknown LKMD command: '%s'\n", cmdbuf);
			diag = 0;
		}
		if (diag == KDB_CMD_GO || diag == KDB_CMD_CPU || 
				diag == KDB_CMD_SS || diag == KDB_CMD_SSB)
			break;

		if (diag)
			kdb_cmderror(diag);
	}

	KDB_DEBUG_STATE("kdb_local 9", diag);
	return diag;
}


/*
 * kdb_print_state
 *
 *	Print the state data for the current processor for debugging.
 *
 * Inputs:
 *	text		Identifies the debug point
 *	value		Any integer value to be printed, e.g. reason code.
 * Returns:
 *	None.
 * Locking:
 *	none
 * Remarks:
 *	none
 */

void kdb_print_state(const char *text, int value)
{
	lkmd_printf("state: %s cpu %d value %d initial %d state %x\n",
		text, smp_processor_id(), value, kdb_initial_cpu, kdb_state[smp_processor_id()]);
}

/*
 * kdb_previous_event
 *
 *	Return a count of cpus that are leaving kdb, i.e. the number
 *	of processors that are still handling the previous kdb event.
 *
 * Inputs:
 *	None.
 * Returns:
 *	Count of cpus in previous event.
 * Locking:
 *	none
 * Remarks:
 *	none
 */

static int kdb_previous_event(void)
{
	int i, leaving = 0;
	for (i = 0; i < NR_CPUS; ++i) {
		if (KDB_STATE_CPU(LEAVING, i))
			++leaving;
	}
	return leaving;
}

/*
 * kdb_wait_for_cpus
 *
 * Invoked once at the start of a kdb event, from the controlling cpu.  Wait a
 * short period for the other cpus to enter kdb state.
 *
 * Inputs:
 *	none
 * Returns:
 *	none
 * Locking:
 *	none
 * Remarks:
 *	none
 */

int kdb_wait_for_cpus_secs;

static void kdb_wait_for_cpus(void)
{
#ifdef	CONFIG_SMP
	int online = 0, kdb_data = 0, prev_kdb_data = 0, c, time;
	
	mdelay(100);
	
	for (time = 0; time < kdb_wait_for_cpus_secs; ++time) {
		online = 0;
		kdb_data = 0;
		for_each_online_cpu(c) {
			++online;
			if (kdb_running_process[c].seqno >= kdb_seqno - 1)
				++kdb_data;
		}
		if (online == kdb_data)
			break;
		if (prev_kdb_data != kdb_data) {
			kdb_nextline = 0;	/* no prompt yet */
			lkmd_printf("  %d out of %d cpus in kdb, waiting for the rest, timeout in %d second(s)\n",
				kdb_data, online, kdb_wait_for_cpus_secs - time);
			prev_kdb_data = kdb_data;
		}
		
		touch_nmi_watchdog();
		mdelay(1000);

		/* Architectures may want to send a more forceful interrupt */
		if (time == min(kdb_wait_for_cpus_secs / 2, 5))
			kdba_wait_for_cpus();
		if (time % 4 == 0)
			lkmd_printf(".");
	}
	if (time) {
		int wait = online - kdb_data;
		if (wait == 0)
			lkmd_printf("All cpus are now in kdb\n");
		else
			lkmd_printf("%d cpu%s not in kdb, %s state is unknown\n",
					wait,
					wait == 1 ? " is" : "s are",
					wait == 1 ? "its" : "their");
	}
	/* give back the vector we took over in smp_kdb_stop */
	lkmda_giveback_vector();
#endif	/* CONFIG_SMP */
}

/*
 * kdb_main_loop
 *
 * The main kdb loop.  After initial setup and assignment of the controlling
 * cpu, all cpus are in this loop.  One cpu is in control and will issue the kdb
 * prompt, the others will spin until 'go' or cpu switch.
 *
 * To get a consistent view of the kernel stacks for all processes, this routine
 * is invoked from the main kdb code via an architecture specific routine.
 * kdba_main_loop is responsible for making the kernel stacks consistent for all
 * processes, there should be no difference between a blocked process and a
 * running process as far as kdb is concerned.
 *
 * Inputs:
 *	reason		The reason KDB was invoked
 *	error		The hardware-defined error code
 *	reason2		kdb's current reason code.  Initially error but can change
 *			acording to kdb state.
 *	db_result	Result code from break or debug point.
 *	regs		The exception frame at time of fault/breakpoint.  If reason
 *			is SILENT or CPU_UP then regs is NULL, otherwise it
 *			should always be valid.
 * Returns:
 *	0	KDB was invoked for an event which it wasn't responsible
 *	1	KDB handled the event for which it was invoked.
 * Locking:
 *	none
 * Remarks:
 *	none
 */

int kdb_main_loop(kdb_reason_t reason, kdb_reason_t reason2, int error,
	      kdb_dbtrap_t db_result, struct pt_regs *regs)
{
	int result = 1;

	/* Stay in kdb() until 'go', 'ss[b]' or an error */
	while (1) {
		/* All processors except the one that is in control will spin here. */
		KDB_DEBUG_STATE("kdb_main_loop 1", reason);
		while (KDB_STATE(HOLD_CPU)) {
			/* state KDB is turned off by kdb_cpu to see if the
			 * other cpus are still live, each cpu in this loop
			 * turns it back on.
			 */
			if (!KDB_STATE(KDB))
				KDB_STATE_SET(KDB);
		}

		KDB_STATE_CLEAR(SUPPRESS);
		KDB_DEBUG_STATE("kdb_main_loop 2", reason);
		if (KDB_STATE(LEAVING))
			break;	/* Another cpu said 'go' */

		if (!kdb_quiet(reason))
		 	kdb_wait_for_cpus();

		/* Still using kdb, this processor is in control */
		result = kdb_local(reason2, error, regs, db_result);
		KDB_DEBUG_STATE("kdb_main_loop 3", result);

		if (result == KDB_CMD_CPU) {
			/* Cpu switch, hold the current cpu, release the target one. */
			reason2 = KDB_REASON_SWITCH;
			KDB_STATE_SET(HOLD_CPU);
			KDB_STATE_CLEAR_CPU(HOLD_CPU, kdb_new_cpu);
			continue;
		}

		if (result == KDB_CMD_SS) {
			KDB_STATE_SET(DOING_SS);
			break;
		}

		if (result == KDB_CMD_SSB) {
			KDB_STATE_SET(DOING_SS);
			KDB_STATE_SET(DOING_SSB);
			break;
		}

		if (result && result != 1 && result != KDB_CMD_GO)
			lkmd_printf("\nUnexpected kdb_local return code %d\n", result);

		KDB_DEBUG_STATE("kdb_main_loop 4", reason);
		break;
	}
	if (KDB_STATE(DOING_SS))
		KDB_STATE_CLEAR(SSBPT);

	/* Clean up any keyboard devices before leaving */
	kdb_kbd_cleanup_state();
	return result;
}

/*
 * kdb
 *
 *	This function is the entry point for the kernel debugger.  It
 *	provides a command parser and associated support functions to
 *	allow examination and control of an active kernel.
 *
 *	The breakpoint trap code should invoke this function with
 *	one of KDB_REASON_BREAK (int 03) or KDB_REASON_DEBUG (debug register)
 *
 *	the die_if_kernel function should invoke this function with
 *	KDB_REASON_OOPS.
 *
 *	In single step mode, one cpu is released to run without
 *	breakpoints.   Interrupts and NMI are reset to their original values,
 *	the cpu is allowed to do one instruction which causes a trap
 *	into kdb with KDB_REASON_DEBUG.
 *
 * Inputs:
 *	reason		The reason KDB was invoked
 *	error		The hardware-defined error code
 *	regs		The exception frame at time of fault/breakpoint.  If reason
 *			is SILENT or CPU_UP then regs is NULL, otherwise it
 *			should always be valid.
 * Returns:
 *	0	KDB was invoked for an event which it wasn't responsible
 *	1	KDB handled the event for which it was invoked.
 * Locking:
 *	none
 * Remarks:
 *	No assumptions of system state.  This function may be invoked
 *	with arbitrary locks held.  It will stop all other processors
 *	in an SMP environment, disable all interrupts and does not use
 *	the operating systems keyboard driver.
 *
 *	This code is reentrant but only for cpu switch.  Any other
 *	reentrancy is an error, although kdb will attempt to recover.
 *
 *	At the start of a kdb session the initial processor is running
 *	kdb() and the other processors can be doing anything.  When the
 *	initial processor calls smp_kdb_stop() the other processors are
 *	driven through kdb_ipi which calls kdb() with reason SWITCH.
 *	That brings all processors into this routine, one with a "real"
 *	reason code, the other with SWITCH.
 *
 *	Because the other processors are driven via smp_kdb_stop(),
 *	they enter here from the NMI handler.  Until the other
 *	processors exit from here and exit from kdb_ipi, they will not
 *	take any more NMI requests.  The initial cpu will still take NMI.
 *
 *	Multiple race and reentrancy conditions, each with different
 *	advoidance mechanisms.
 *
 *	Two cpus hit debug points at the same time.
 *
 *	  kdb_lock and kdb_initial_cpu ensure that only one cpu gets
 *	  control of kdb.  The others spin on kdb_initial_cpu until
 *	  they are driven through NMI into kdb_ipi.  When the initial
 *	  cpu releases the others from NMI, they resume trying to get
 *	  kdb_initial_cpu to start a new event.
 *
 *	A cpu is released from kdb and starts a new event before the
 *	original event has completely ended.
 *
 *	  kdb_previous_event() prevents any cpu from entering
 *	  kdb_initial_cpu state until the previous event has completely
 *	  ended on all cpus.
 *
 *	An exception occurs inside kdb.
 *
 *	  kdb_initial_cpu detects recursive entry to kdb and attempts
 *	  to recover.  The recovery uses longjmp() which means that
 *	  recursive calls to kdb never return.  Beware of assumptions
 *	  like
 *
 *	    ++depth;
 *	    kdb();
 *	    --depth;
 *
 *	  If the kdb call is recursive then longjmp takes over and
 *	  --depth is never executed.
 *
 *	NMI handling.
 *
 *	  NMI handling is tricky.  The initial cpu is invoked by some kdb event,
 *	  this event could be NMI driven but usually is not.  The other cpus are
 *	  driven into kdb() via kdb_ipi which uses NMI so at the start the other
 *	  cpus will not accept NMI.  Some operations such as SS release one cpu
 *	  but hold all the others.  Releasing a cpu means it drops back to
 *	  whatever it was doing before the kdb event, this means it drops out of
 *	  kdb_ipi and hence out of NMI status.  But the software watchdog uses
 *	  NMI and we do not want spurious watchdog calls into kdb.  kdba_read()
 *	  resets the watchdog counters in its input polling loop, when a kdb
 *	  command is running it is subject to NMI watchdog events.
 *
 *	  Another problem with NMI handling is the NMI used to drive the other
 *	  cpus into kdb cannot be distinguished from the watchdog NMI.  State
 *	  flag WAIT_IPI indicates that a cpu is waiting for NMI via kdb_ipi,
 *	  if not set then software NMI is ignored by kdb_ipi.
 *
 *	Cpu switching.
 *
 *	  All cpus are in kdb (or they should be), all but one are
 *	  spinning on KDB_STATE(HOLD_CPU).  Only one cpu is not in
 *	  HOLD_CPU state, only that cpu can handle commands.
 *
 *	Go command entered.
 *
 *	  If necessary, go will switch to the initial cpu first.  If the event
 *	  was caused by a software breakpoint (assumed to be global) that
 *	  requires single-step to get over the breakpoint then only release the
 *	  initial cpu, after the initial cpu has single-stepped the breakpoint
 *	  then release the rest of the cpus.  If SSBPT is not required then
 *	  release all the cpus at once.
 */

int kdb(kdb_reason_t reason, int error, struct pt_regs *regs)
{
	kdb_intstate_t int_state;	/* Interrupt state */
	kdb_reason_t reason2 = reason;
	int result = 0;	/* Default is kdb did not handle it */
	int ss_event, old_regs_saved = 0;
	struct pt_regs *old_regs = NULL;
	kdb_dbtrap_t db_result = KDB_DB_NOBPT;
	preempt_disable();

	switch(reason) {
	case KDB_REASON_ENTER:
	case KDB_REASON_ENTER_SLAVE:
	case KDB_REASON_BREAK:
	case KDB_REASON_DEBUG:
	case KDB_REASON_OOPS:
	case KDB_REASON_SWITCH:
	case KDB_REASON_KEYBOARD:
	case KDB_REASON_NMI:
		if (regs && regs != get_irq_regs()) {
			old_regs = set_irq_regs(regs);
			old_regs_saved = 1;
		}
		break;
	default:
		break;
	}

	if (!kdb_on)
		goto out;

	KDB_DEBUG_STATE("kdb 1", reason);
	KDB_STATE_CLEAR(SUPPRESS);

	/* Filter out userspace breakpoints first, no point in doing all
	 * the kdb smp fiddling when it is really a gdb trap.
	 * Save the single step status first, kdba_db_trap clears ss status.
	 * kdba_b[dp]_trap sets SSBPT if required.
	 */
	ss_event = KDB_STATE(DOING_SS) || KDB_STATE(SSBPT);
	if (reason == KDB_REASON_BREAK)
		db_result = kdba_bp_trap(regs, error);	/* Only call this once */

	if (reason == KDB_REASON_DEBUG)
		db_result = kdba_db_trap(regs, error);	/* Only call this once */

	if (db_result == KDB_DB_NOBPT) {
		if (reason == KDB_REASON_DEBUG) {
			KDB_DEBUG_STATE("kdb 2", reason);
			goto out;	/* Not one of mine */
		}
		if (reason == KDB_REASON_BREAK)
			reason2 = reason = KDB_REASON_ENTER;
	}

	/* Turn off single step if it was being used */
	if (ss_event) {
		kdba_clearsinglestep(regs);
		/* Single step after a breakpoint removes the need for a delayed reinstall */
		if (reason == KDB_REASON_BREAK || reason == KDB_REASON_DEBUG)
			KDB_STATE_CLEAR(SSBPT);
	}

	/* kdb can validly reenter but only for certain well defined conditions */
	if (reason == KDB_REASON_DEBUG && !KDB_STATE(HOLD_CPU) && ss_event)
		KDB_STATE_SET(REENTRY);
	else
		KDB_STATE_CLEAR(REENTRY);

	/* Wait for previous kdb event to completely exit before starting a new event. */
	while (kdb_previous_event())
		;
	KDB_DEBUG_STATE("kdb 3", reason);

	/*
	 * If kdb is already active, print a message and try to recover.
	 * If recovery is not possible and recursion is allowed or
	 * forced recursion without recovery is set then try to recurse
	 * in kdb.  Not guaranteed to work but it makes an attempt at
	 * debugging the debugger.
	 */
	if (reason != KDB_REASON_SWITCH && reason != KDB_REASON_ENTER_SLAVE) {
		if (KDB_IS_RUNNING() && !KDB_STATE(REENTRY)) {
			int recover = 1;
			unsigned long recurse = 0;
			lkmd_printf("kdb: Debugger re-entered on cpu %d, new reason = %d\n", smp_processor_id(), reason);
			/* Should only re-enter from released cpu */

			if (KDB_STATE(HOLD_CPU)) {
				lkmd_printf("     Strange, cpu %d should not be running\n", smp_processor_id());
				recover = 0;
			}
			if (!KDB_STATE(CMD)) {
				lkmd_printf("     Not executing a kdb command\n");
				recover = 0;
			}
			if (!KDB_STATE(LONGJMP)) {
				lkmd_printf("     No longjmp available for recovery\n");
				recover = 0;
			}
			kdbgetulenv("RECURSE", &recurse);
			if (recurse > 1) {
				lkmd_printf("     Forced recursion is set\n");
				recover = 0;
			}
			if (recover) {
				lkmd_printf("     Attempting to abort command and recover\n");
#ifdef kdba_setjmp
				kdba_longjmp(&kdbjmpbuf[smp_processor_id()], 0);
#endif	/* kdba_setjmp */
			}
			if (recurse) {
				if (KDB_STATE(RECURSE)) {
					lkmd_printf("     Already in recursive mode\n");
				} else {
					lkmd_printf("     Attempting recursive mode\n");
					KDB_STATE_SET(RECURSE);
					KDB_STATE_SET(REENTRY);
					reason2 = KDB_REASON_RECURSE;
					recover = 1;
				}
			}
			if (!recover) {
				lkmd_printf("     Cannot recover, allowing event to proceed\n");
				/*temp*/
				while (KDB_IS_RUNNING())
					cpu_relax();
				goto out;
			}
		}
	} else if (reason == KDB_REASON_SWITCH && !KDB_IS_RUNNING()) {
		lkmd_printf("kdb: CPU switch without kdb running, I'm confused\n");
		goto out;
	}

	/*
	 * Disable interrupts, breakpoints etc. on this processor
	 * during kdb command processing
	 */
	KDB_STATE_SET(KDB);
	kdba_disableint(&int_state);
	if (!KDB_STATE(KDB_CONTROL)) {
		kdb_bp_remove_local();
		KDB_STATE_SET(KDB_CONTROL);
	}
	
	/*
	 * If not entering the debugger due to CPU switch or single step
	 * reentry, serialize access here.
	 * The processors may race getting to this point - if,
	 * for example, more than one processor hits a breakpoint
	 * at the same time.   We'll serialize access to kdb here -
	 * other processors will loop here, and the NMI from the stop
	 * IPI will take them into kdb as switch candidates.  Once
	 * the initial processor releases the debugger, the rest of
	 * the processors will race for it.
	 *
	 * The above describes the normal state of affairs, where two or more
	 * cpus that are entering kdb at the "same" time are assumed to be for
	 * separate events.  However some processes such as ia64 MCA/INIT will
	 * drive all the cpus into error processing at the same time.  For that
	 * case, all of the cpus entering kdb at the "same" time are really a
	 * single event.
	 *
	 * That case is handled by the use of KDB_ENTER by one cpu (the
	 * monarch) and KDB_ENTER_SLAVE on the other cpus (the slaves).
	 * KDB_ENTER_SLAVE maps to KDB_REASON_ENTER_SLAVE.  The slave events
	 * will be treated as if they had just responded to the kdb IPI, i.e.
	 * as if they were KDB_REASON_SWITCH.
	 *
	 * Because of races across multiple cpus, ENTER_SLAVE can occur before
	 * the main ENTER.   Hold up ENTER_SLAVE here until the main ENTER
	 * arrives.
	 */

	if (reason == KDB_REASON_ENTER_SLAVE) {
		spin_lock(&kdb_lock);
		while (!KDB_IS_RUNNING()) {
			spin_unlock(&kdb_lock);
			while (!KDB_IS_RUNNING())
				cpu_relax();
			spin_lock(&kdb_lock);
		}
		reason = KDB_REASON_SWITCH;
		KDB_STATE_SET(HOLD_CPU);
		spin_unlock(&kdb_lock);
	}

	if (reason == KDB_REASON_SWITCH || KDB_STATE(REENTRY))
		;	/* drop through */
	else {
		KDB_DEBUG_STATE("kdb 4", reason);
		spin_lock(&kdb_lock);
		while (KDB_IS_RUNNING() || kdb_previous_event()) {
			spin_unlock(&kdb_lock);
			while (KDB_IS_RUNNING() || kdb_previous_event())
				cpu_relax();
			spin_lock(&kdb_lock);
		}
		KDB_DEBUG_STATE("kdb 5", reason);

		kdb_initial_cpu = smp_processor_id();
		++kdb_seqno;
		spin_unlock(&kdb_lock);
//		if (!kdb_quiet(reason))
			//notify_die(DIE_KDEBUG_ENTER, "KDEBUG ENTER", regs, error, 0, 0);
	}

	if (smp_processor_id() == kdb_initial_cpu && !KDB_STATE(REENTRY)) {
		KDB_STATE_CLEAR(HOLD_CPU);
		KDB_STATE_CLEAR(WAIT_IPI);
		
		/*
		 * Remove the global breakpoints.  This is only done
		 * once from the initial processor on initial entry.
		 */
		if (!kdb_quiet(reason) || smp_processor_id() == 0)
			kdb_bp_remove_global();

		/*
		 * If SMP, stop other processors.  The other processors
		 * will enter kdb() with KDB_REASON_SWITCH and spin in
		 * kdb_main_loop().
		 */
		KDB_DEBUG_STATE("kdb 6", reason);
		if (NR_CPUS > 1 && !kdb_quiet(reason)) {
			int i;
			for (i = 0; i < NR_CPUS; ++i) {
				if (!cpu_online(i))
					continue;
				if (i != kdb_initial_cpu) {
					KDB_STATE_SET_CPU(HOLD_CPU, i);
					KDB_STATE_SET_CPU(WAIT_IPI, i);
				}
			}
			KDB_DEBUG_STATE("kdb 7", reason);
			smp_kdb_stop();
			KDB_DEBUG_STATE("kdb 8", reason);
		}
	}

	if (KDB_STATE(GO1)) {
		kdb_bp_remove_global();		/* They were set for single-step purposes */
		KDB_STATE_CLEAR(GO1);
		reason = KDB_REASON_SILENT;	/* Now silently go */
	}

	/* Set up a consistent set of process stacks before talking to the user */
	KDB_DEBUG_STATE("kdb 9", result);
	result = kdba_main_loop(reason, reason2, error, db_result, regs);
	reason = reason2;	/* back to original event type */
		
	KDB_DEBUG_STATE("kdb 10", result);
	kdba_adjust_ip(reason, error, regs);
	KDB_STATE_CLEAR(LONGJMP);
	KDB_DEBUG_STATE("kdb 11", result);

	/* go which requires single-step over a breakpoint must only release one cpu. */
	if (result == KDB_CMD_GO && KDB_STATE(SSBPT))
		KDB_STATE_SET(GO1);

	if (smp_processor_id() == kdb_initial_cpu && !KDB_STATE(DOING_SS) && !KDB_STATE(RECURSE)) {
		/*
		 * (Re)install the global breakpoints and cleanup the cached
		 * symbol table.  This is only done once from the initial
		 * processor on go.
		 */
		KDB_DEBUG_STATE("kdb 12", reason);
		if (!kdb_quiet(reason) || smp_processor_id() == 0) {
			kdb_bp_install_global(regs);
			kdbnearsym_cleanup();
			debug_kusage();
		}
		if (!KDB_STATE(GO1)) {
			/*
			 * Release all other cpus which will see KDB_STATE(LEAVING) is set.
			 */
			int i;
			for (i = 0; i < NR_CPUS; ++i) {
				if (KDB_STATE_CPU(KDB, i))
					KDB_STATE_SET_CPU(LEAVING, i);
				KDB_STATE_CLEAR_CPU(WAIT_IPI, i);
				KDB_STATE_CLEAR_CPU(HOLD_CPU, i);
			}
			/* Wait until all the other processors leave kdb */
			while (kdb_previous_event() != 1)
				;
			//if (!kdb_quiet(reason))
				//notify_die(DIE_KDEBUG_LEAVE, "KDEBUG LEAVE", regs, error, 0, 0);
			kdb_initial_cpu = -1;	/* release kdb control */
			KDB_DEBUG_STATE("kdb 13", reason);
		}
	}

	KDB_DEBUG_STATE("kdb 14", result);
	kdba_restoreint(&int_state);

	/* Only do this work if we are really leaving kdb */
	if (!(KDB_STATE(DOING_SS) || KDB_STATE(SSBPT) || KDB_STATE(RECURSE))) {
		KDB_DEBUG_STATE("kdb 15", result);
		kdb_bp_install_local(regs);
		if (old_regs_saved)
			set_irq_regs(old_regs);
		KDB_STATE_CLEAR(KDB_CONTROL);
	}

	KDB_DEBUG_STATE("kdb 16", result);
	KDB_STATE_CLEAR(IP_ADJUSTED);	/* Re-adjust ip next time in */
	KDB_STATE_CLEAR(KEYBOARD);
	KDB_STATE_CLEAR(KDB);		/* Main kdb state has been cleared */
	KDB_STATE_CLEAR(RECURSE);
	KDB_STATE_CLEAR(LEAVING);	/* No more kdb work after this */
	KDB_DEBUG_STATE("kdb 17", reason);
out:
	preempt_enable();
	return result != 0;
}

/*
 * kdb_mdr
 *
 *	This function implements the guts of the 'mdr' command.
 *
 *	mdr  <addr arg>,<byte count>
 *
 * Inputs:
 *	addr	Start address
 *	count	Number of bytes
 * Outputs:
 *	None.
 * Returns:
 *	Always 0.  Any errors are detected and printed by kdb_getarea.
 * Locking:
 *	none.
 * Remarks:
 */

static int kdb_mdr(kdb_machreg_t addr, unsigned int count)
{
	unsigned char c;
	while (count--) {
		if (kdb_getarea(c, addr))
			return 0;
		lkmd_printf("%02x", c);
		addr++;
	}
	lkmd_printf("\n");
	return 0;
}

/*
 * kdb_md
 *
 *	This function implements the 'md', 'md1', 'md2', 'md4', 'md8'
 *	'mdr' and 'mds' commands.
 *
 *	md|mds  [<addr arg> [<line count> [<radix>]]]
 *	mdWcN	[<addr arg> [<line count> [<radix>]]]
 *		where W = is the width (1, 2, 4 or 8) and N is the count.
 *		for eg., md1c20 reads 20 bytes, 1 at a time.
 *	mdr  <addr arg>,<byte count>
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

static void kdb_md_line(const char *fmtstr, kdb_machreg_t addr,
	    int symbolic, int nosect, int bytesperword,
	    int num, int repeat, int phys)
{
	/* print just one line of data */
	kdb_symtab_t symtab;
	char cbuf[32];
	char *c = cbuf;
	int i;
	unsigned long word;

	memset(cbuf, '\0', sizeof(cbuf));
	if (phys)
		lkmd_printf("phys " kdb_machreg_fmt0 " ", addr);
	else
		lkmd_printf(kdb_machreg_fmt0 " ", addr);

	for (i = 0; i < num && repeat--; i++) {
		if (phys) {
			if (kdb_getphysword(&word, addr, bytesperword))
				break;
		} else if (kdb_getword(&word, addr, bytesperword))
			break;
		lkmd_printf(fmtstr, word);
		if (symbolic)
			kdbnearsym(word, &symtab);
		else
			memset(&symtab, 0, sizeof(symtab));
		if (symtab.sym_name) {
			kdb_symbol_print(word, &symtab, 0);
			if (!nosect) {
				lkmd_printf("\n");
				lkmd_printf("                       %s %s "
					   kdb_machreg_fmt " " kdb_machreg_fmt " " kdb_machreg_fmt,
					symtab.mod_name,
					symtab.sec_name,
					symtab.sec_start,
					symtab.sym_start,
					symtab.sym_end);
			}
			addr += bytesperword;
		} else {
			union {
				u64 word;
				unsigned char c[8];
			} wc;
			unsigned char *cp;
#ifdef	__BIG_ENDIAN
			cp = wc.c + 8 - bytesperword;
#else
			cp = wc.c;
#endif
			wc.word = word;
#define printable_char(c) ({unsigned char __c = c; isascii(__c) && isprint(__c) ? __c : '.';})
			switch (bytesperword) {
			case 8:
				*c++ = printable_char(*cp++);
				*c++ = printable_char(*cp++);
				*c++ = printable_char(*cp++);
				*c++ = printable_char(*cp++);
				addr += 4;
			case 4:
				*c++ = printable_char(*cp++);
				*c++ = printable_char(*cp++);
				addr += 2;
			case 2:
				*c++ = printable_char(*cp++);
				addr++;
			case 1:
				*c++ = printable_char(*cp++);
				addr++;
				break;
			}
#undef printable_char
		}
	}
	lkmd_printf("%*s %s\n", (int)((num-i)*(2*bytesperword + 1)+1), " ", cbuf);
}

static int kdb_md(int argc, const char **argv)
{
	static kdb_machreg_t last_addr;
	static int last_radix, last_bytesperword, last_repeat;
	int radix = 16, mdcount = 8, bytesperword = KDB_WORD_SIZE, repeat;
	int nosect = 0;
	char fmtchar, fmtstr[64];
	kdb_machreg_t addr;
	unsigned long word;
	long offset = 0;
	int symbolic = 0;
	int valid = 0;
	int phys = 0;

	kdbgetintenv("MDCOUNT", &mdcount);
	kdbgetintenv("RADIX", &radix);
	kdbgetintenv("BYTESPERWORD", &bytesperword);

	/* Assume 'md <addr>' and start with environment values */
	repeat = mdcount * 16 / bytesperword;

	if (strcmp(argv[0], "mdr") == 0) {
		if (argc != 2)
			return KDB_ARGCOUNT;
		valid = 1;
	} else if (isdigit(argv[0][2])) {
		bytesperword = (int)(argv[0][2] - '0');
		if (bytesperword == 0) {
			bytesperword = last_bytesperword;
			if (bytesperword == 0) {
				bytesperword = 4;
			}
		}
		last_bytesperword = bytesperword;
		repeat = mdcount * 16 / bytesperword;
		if (!argv[0][3])
			valid = 1;
		else if (argv[0][3] == 'c' && argv[0][4]) {
			char *p;
			repeat = simple_strtoul(argv[0]+4, &p, 10);
			mdcount = ((repeat * bytesperword) + 15) / 16;
			valid = !*p;
		}
		last_repeat = repeat;
	} else if (strcmp(argv[0], "md") == 0)
		valid = 1;
	else if (strcmp(argv[0], "mds") == 0)
		valid = 1;
	else if (strcmp(argv[0], "mdp") == 0) {
		phys = valid = 1;
	}
	if (!valid)
		return KDB_NOTFOUND;

	if (argc == 0) {
		if (last_addr == 0)
			return KDB_ARGCOUNT;
		addr = last_addr;
		radix = last_radix;
		bytesperword = last_bytesperword;
		repeat = last_repeat;
		mdcount = ((repeat * bytesperword) + 15) / 16;
	}

	if (argc) {
		kdb_machreg_t val;
		int diag, nextarg = 1;
		diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL);
		if (diag)
			return diag;
		if (argc > nextarg+2)
			return KDB_ARGCOUNT;

		if (argc >= nextarg) {
			diag = kdbgetularg(argv[nextarg], &val);
			if (!diag) {
				mdcount = (int) val;
				repeat = mdcount * 16 / bytesperword;
			}
		}
		if (argc >= nextarg+1) {
			diag = kdbgetularg(argv[nextarg+1], &val);
			if (!diag)
				radix = (int) val;
		}
	}

	if (strcmp(argv[0], "mdr") == 0) {
		return kdb_mdr(addr, mdcount);
	}

	switch (radix) {
	case 10:
		fmtchar = 'd';
		break;
	case 16:
		fmtchar = 'x';
		break;
	case 8:
		fmtchar = 'o';
		break;
	default:
		return KDB_BADRADIX;
	}

	last_radix = radix;

	if (bytesperword > KDB_WORD_SIZE)
		return KDB_BADWIDTH;

	switch (bytesperword) {
	case 8:
		sprintf(fmtstr, "%%16.16l%c ", fmtchar);
		break;
	case 4:
		sprintf(fmtstr, "%%8.8l%c ", fmtchar);
		break;
	case 2:
		sprintf(fmtstr, "%%4.4l%c ", fmtchar);
		break;
	case 1:
		sprintf(fmtstr, "%%2.2l%c ", fmtchar);
		break;
	default:
		return KDB_BADWIDTH;
	}

	last_repeat = repeat;
	last_bytesperword = bytesperword;

	if (strcmp(argv[0], "mds") == 0) {
		symbolic = 1;
		/* Do not save these changes as last_*, they are temporary mds
		 * overrides.
		 */
		bytesperword = KDB_WORD_SIZE;
		repeat = mdcount;
		kdbgetintenv("NOSECT", &nosect);
	}

	/* Round address down modulo BYTESPERWORD */

	addr &= ~(bytesperword-1);

	while (repeat > 0) {
		unsigned long a;
		int n, z, num = (symbolic ? 1 : (16 / bytesperword));

		for (a = addr, z = 0; z < repeat; a += bytesperword, ++z) {
			if (phys) {
				if (kdb_getphysword(&word, a, bytesperword)
						|| word)
					break;
			} else if (kdb_getword(&word, a, bytesperword) || word)
				break;
		}
		n = min(num, repeat);
		kdb_md_line(fmtstr, addr, symbolic, nosect, bytesperword, num, repeat, phys);
		addr += bytesperword * n;
		repeat -= n;
		z = (z + num - 1) / num;
		if (z > 2) {
			int s = num * (z-2);
			lkmd_printf(kdb_machreg_fmt0 "-" kdb_machreg_fmt0 " zero suppressed\n",
				addr, addr + bytesperword * s - 1);
			addr += bytesperword * s;
			repeat -= s;
		}
	}
	last_addr = addr;

	return 0;
}

/*
 * kdb_mm
 *
 *	This function implements the 'mm' command.
 *
 *	mm address-expression new-value
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 *	mm works on machine words, mmW works on bytes.
 */

static int kdb_mm(int argc, const char **argv)
{
	int diag;
	kdb_machreg_t addr;
	long offset = 0;
	unsigned long contents;
	int nextarg;
	int width;

	if (argv[0][2] && !isdigit(argv[0][2]))
		return KDB_NOTFOUND;

	if (argc < 2) {
		return KDB_ARGCOUNT;
	}

	nextarg = 1;
	if ((diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL)))
		return diag;

	if (nextarg > argc)
		return KDB_ARGCOUNT;

	if ((diag = kdbgetaddrarg(argc, argv, &nextarg, &contents, NULL, NULL)))
		return diag;

	if (nextarg != argc + 1)
		return KDB_ARGCOUNT;

	width = argv[0][2] ? (argv[0][2] - '0') : (KDB_WORD_SIZE);
	if ((diag = kdb_putword(addr, contents, width)))
		return diag;

	lkmd_printf(kdb_machreg_fmt " = " kdb_machreg_fmt "\n", addr, contents);

	return 0;
}

/*
 * kdb_go
 *
 *	This function implements the 'go' command.
 *
 *	go [address-expression]
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	KDB_CMD_GO for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

static int kdb_go(int argc, const char **argv)
{
	kdb_machreg_t addr;
	int diag;
	int nextarg;
	long offset;
	struct pt_regs *regs = get_irq_regs();

	if (argc == 1) {
		if (smp_processor_id() != kdb_initial_cpu) {
			lkmd_printf("go <address> must be issued from the initial cpu, do cpu %d first\n", kdb_initial_cpu);
			return KDB_ARGCOUNT;
		}
		nextarg = 1;
		diag = kdbgetaddrarg(argc, argv, &nextarg,
				     &addr, &offset, NULL);
		if (diag)
			return diag;

		kdba_setpc(regs, addr);
	} else if (argc)
		return KDB_ARGCOUNT;

	diag = KDB_CMD_GO;
	if (smp_processor_id() != kdb_initial_cpu) {
		char buf[80];
		lkmd_printf("go was not issued from initial cpu, switching back to cpu %d\n", kdb_initial_cpu);
		sprintf(buf, "cpu %d\n", kdb_initial_cpu);
		/* Recursive use of kdb_parse, do not use argv after this point */
		argv = NULL;
		diag = kdb_parse(buf);
		if (diag == KDB_CMD_CPU)
			KDB_STATE_SET_CPU(GO_SWITCH, kdb_initial_cpu);
	}
	return diag;
}

/*
 * kdb_rd
 *
 *	This function implements the 'rd' command.
 *
 *	rd		display all general registers.
 *	rd  c		display all control registers.
 *	rd  d		display all debug registers.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

static int kdb_rd(int argc, const char **argv)
{
	int diag;
	if (argc == 0) {
		if ((diag = kdb_check_regs()))
			return diag;
		return kdba_dumpregs(kdb_current_regs, NULL, NULL);
	}

	if (argc > 2) {
		return KDB_ARGCOUNT;
	}

	if ((diag = kdb_check_regs()))
		return diag;
	return kdba_dumpregs(kdb_current_regs, argv[1], argc==2 ? argv[2]: NULL);
}

/*
 * kdb_rm
 *
 *	This function implements the 'rm' (register modify)  command.
 *
 *	rm register-name new-contents
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 *	Currently doesn't allow modification of control or
 *	debug registers.
 */

static int kdb_rm(int argc, const char **argv)
{
	int diag;
	int ind = 0;
	kdb_machreg_t contents;

	if (argc != 2) {
		return KDB_ARGCOUNT;
	}

	/*
	 * Allow presence or absence of leading '%' symbol.
	 */

	if (argv[1][0] == '%')
		ind = 1;

	diag = kdbgetularg(argv[2], &contents);
	if (diag)
		return diag;

	if ((diag = kdb_check_regs()))
		return diag;
	diag = kdba_setregcontents(&argv[1][ind], kdb_current_regs, contents);
	if (diag)
		return diag;

	return 0;
}

/*
 * kdb_ef
 *
 *	This function implements the 'regs' (display exception frame)
 *	command.  This command takes an address and expects to find
 *	an exception frame at that address, formats and prints it.
 *
 *	regs address-expression
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 *	Not done yet.
 */

static int kdb_ef(int argc, const char **argv)
{
	int diag;
	kdb_machreg_t addr;
	long offset;
	int nextarg;

	if (argc == 1) {
		nextarg = 1;
		diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL);
		if (diag)
			return diag;

		return kdba_dumpregs((struct pt_regs *)addr, NULL, NULL);
	}

	return KDB_ARGCOUNT;
}

#if defined(CONFIG_MODULES)
struct list_head *kdb_modules;
extern void free_module(struct module *);

/*
 * kdb_lsmod
 *
 *	This function implements the 'lsmod' command.  Lists currently
 *	loaded kernel modules.
 *
 *	Mostly taken from userland lsmod.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 *
 */

static int kdb_lsmod(int argc, const char **argv)
{
//	struct module *mod;

	if (argc != 0)
		return KDB_ARGCOUNT;

// 	lkmd_printf("Module                  Size  modstruct     Used by\n");
// 	list_for_each_entry(mod, kdb_modules, list) {

// 		lkmd_printf("%-20s%8u  0x%p ", mod->name,
// 			   mod->core_size, (void *)mod);
// #ifdef CONFIG_MODULE_UNLOAD
// 		lkmd_printf("%4lu ", module_refcount(mod));
// #endif
// 		if (mod->state == MODULE_STATE_GOING)
// 			lkmd_printf(" (Unloading)");
// 		else if (mod->state == MODULE_STATE_COMING)
// 			lkmd_printf(" (Loading)");
// 		else
// 			lkmd_printf(" (Live)");

//#ifdef CONFIG_MODULE_UNLOAD
		// {
		// 	struct module_use *use;
		// 	lkmd_printf(" [ ");
		// 	list_for_each_entry(use, &mod->source_list, source_list)
		// 		lkmd_printf("%s ", use->target->name);
		// 	lkmd_printf("]\n");
		// }
//#endif
//	}

	return 0;
}

#endif	/* CONFIG_MODULES */

/*
 * kdb_env
 *
 *	This function implements the 'env' command.  Display the current
 *	environment variables.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

static int kdb_env(int argc, const char **argv)
{
	int i;

	for(i=0; i<__nenv; i++) {
		if (__env[i]) {
			lkmd_printf("%s\n", __env[i]);
		}
	}

	if (KDB_DEBUG(MASK))
		lkmd_printf("KDBFLAGS=0x%x\n", kdb_flags);

	return 0;
}

/*
 * kdb_cpu
 *
 *	This function implements the 'cpu' command.
 *
 *	cpu	[<cpunum>]
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	KDB_CMD_CPU for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 *	All cpu's should be spinning in kdb().  However just in case
 *	a cpu did not take the smp_kdb_stop NMI, check that a cpu
 *	entered kdb() before passing control to it.
 */

static void kdb_cpu_status(void)
{
	int i, start_cpu, first_print = 1;
	char state, prev_state = '?';

	lkmd_printf("Currently on cpu %d\n", smp_processor_id());
	lkmd_printf("Available cpus: ");
	for (start_cpu = -1, i = 0; i < NR_CPUS; i++) {
		if (!cpu_online(i))
			state = 'F';	/* cpu is offline */
		else {
			struct kdb_running_process *krp = kdb_running_process+i;
			if (KDB_STATE_CPU(KDB, i)) {
				state = ' ';	/* cpu is responding to kdb */
				if (kdb_task_state_char(krp->p) == 'I')
					state = 'I';	/* running the idle task */
			} else if (krp->seqno && krp->p && krp->seqno >= kdb_seqno - 1)
				state = '+';	/* some kdb data, but not responding */
			else
				state = '*';	/* no kdb data */
		}
		if (state != prev_state) {
			if (prev_state != '?') {
				if (!first_print)
					lkmd_printf(", ");
				first_print = 0;
				lkmd_printf("%d", start_cpu);
				if (start_cpu < i-1)
					lkmd_printf("-%d", i-1);
				if (prev_state != ' ')
					lkmd_printf("(%c)", prev_state);
			}
			prev_state = state;
			start_cpu = i;
		}
	}
	/* print the trailing cpus, ignoring them if they are all offline */
	if (prev_state != 'F') {
		if (!first_print)
			lkmd_printf(", ");
		lkmd_printf("%d", start_cpu);
		if (start_cpu < i-1)
			lkmd_printf("-%d", i-1);
		if (prev_state != ' ')
			lkmd_printf("(%c)", prev_state);
	}
	lkmd_printf("\n");
}

static int kdb_cpu(int argc, const char **argv)
{
	unsigned long cpunum;
	int diag, i;

	/* ask the other cpus if they are still active */
	for (i=0; i<NR_CPUS; i++) {
		if (cpu_online(i))
			KDB_STATE_CLEAR_CPU(KDB, i);
	}
	KDB_STATE_SET(KDB);
	barrier();
	/* wait for the other cpus to notice and set state KDB again,
	 * see kdb_main_loop
	 */
	udelay(1000);

	if (argc == 0) {
		kdb_cpu_status();
		return 0;
	}

	if (argc != 1)
		return KDB_ARGCOUNT;

	diag = kdbgetularg(argv[1], &cpunum);
	if (diag)
		return diag;

	/*
	 * Validate cpunum
	 */
	if ((cpunum > NR_CPUS)
	 || !cpu_online(cpunum)
	 || !KDB_STATE_CPU(KDB, cpunum))
		return KDB_BADCPUNUM;

	kdb_new_cpu = cpunum;

	/*
	 * Switch to other cpu
	 */
	return KDB_CMD_CPU;
}

/* The user may not realize that ps/bta with no parameters does not print idle
 * or sleeping system daemon processes, so tell them how many were suppressed.
 */
void kdb_ps_suppressed(void)
{
	int idle = 0, daemon = 0;
	unsigned long mask_I = kdb_task_state_string("I"),
		      mask_M = kdb_task_state_string("M");
	unsigned long cpu;
	const struct task_struct *p, *g;
	for (cpu = 0; cpu < NR_CPUS; ++cpu) {
		if (!cpu_online(cpu))
			continue;
		p = kdb_curr_task(cpu);
		if (kdb_task_state(p, mask_I))
			++idle;
	}
	kdb_do_each_thread(g, p) {
		if (kdb_task_state(p, mask_M))
			++daemon;
	} kdb_while_each_thread(g, p);
	if (idle || daemon) {
		if (idle)
			lkmd_printf("%d idle process%s (state I)%s\n",
				   idle, idle == 1 ? "" : "es",
				   daemon ? " and " : "");
		if (daemon)
			lkmd_printf("%d sleeping system daemon (state M) process%s",
				   daemon, daemon == 1 ? "" : "es");
		lkmd_printf(" suppressed,\nuse 'ps A' to see all.\n");
	}
}

/*
 * kdb_ps
 *
 *	This function implements the 'ps' command which shows
 *	a list of the active processes.
 *
 *	ps [DRSTCZEUIMA]		All processes, optionally filtered by state
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

void kdb_ps1(const struct task_struct *p)
{
	struct kdb_running_process *krp = kdb_running_process + kdb_process_cpu(p);
	lkmd_printf("0x%p %8d %8d  %d %4d   %c  0x%p %c%s\n",
		   (void *)p, p->pid, p->parent->pid,
		   kdb_task_has_cpu(p), kdb_process_cpu(p),
		   kdb_task_state_char(p),
		   (void *)(&p->thread),
		   p == kdb_curr_task(smp_processor_id()) ? '*': ' ',
		   p->comm);
	if (kdb_task_has_cpu(p)) {
		if (!krp->seqno || !krp->p)
			lkmd_printf("  Error: no saved data for this cpu\n");
		else {
			if (krp->seqno < kdb_seqno - 1)
				lkmd_printf("  Warning: process state is stale\n");
			if (krp->p != p)
				lkmd_printf("  Error: does not match running process table (0x%p)\n", krp->p);
		}
	}
}

static int kdb_ps(int argc, const char **argv)
{
	struct task_struct *g, *p;
	unsigned long mask, cpu;

	if (argc == 0)
		kdb_ps_suppressed();
	lkmd_printf("%-*s      Pid   Parent [*] cpu State %-*s Command\n",
		(int)(2*sizeof(void *))+2, "Task Addr",
		(int)(2*sizeof(void *))+2, "Thread");
	mask = kdb_task_state_string(argc ? argv[1] : NULL);
	/* Run the active tasks first */
	for (cpu = 0; cpu < NR_CPUS; ++cpu) {
		if (!cpu_online(cpu))
			continue;
		p = kdb_curr_task(cpu);
		if (kdb_task_state(p, mask))
			kdb_ps1(p);
	}
	lkmd_printf("\n");
	/* Now the real tasks */
	kdb_do_each_thread(g, p) {
		if (kdb_task_state(p, mask))
			kdb_ps1(p);
	} kdb_while_each_thread(g, p);

	return 0;
}

/*
 * kdb_pid
 *
 *	This function implements the 'pid' command which switches
 *	the currently active process.
 *
 *	pid [<pid> | R]
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */


static int kdb_pid(int argc, const char **argv)
{
	struct task_struct *p;
	unsigned long val;
	int diag;

	if (argc > 1)
		return KDB_ARGCOUNT;

	if (argc) {
		if (strcmp(argv[1], "R") == 0) {
			p = KDB_RUNNING_PROCESS_ORIGINAL[kdb_initial_cpu].p;
		} else {
			diag = kdbgetularg(argv[1], &val);
			if (diag)
				return KDB_BADINT;

			p = lkmd_find_task_by_pid_ns((pid_t)val, &init_pid_ns);
			if (!p) {
				lkmd_printf("No task with pid=%d\n", (pid_t)val);
				return 0;
			}
		}

		kdba_set_current_task(p);
	}

	lkmd_printf("KDB current process is %s(pid=%d)\n", lkmd_current_task->comm,
		   lkmd_current_task->pid);

	return 0;
}

/*
 * kdb_ll
 *
 *	This function implements the 'll' command which follows a linked
 *	list and executes an arbitrary command for each element.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

static int kdb_ll(int argc, const char **argv)
{
	int diag;
	kdb_machreg_t addr;
	long offset = 0;
	kdb_machreg_t va;
	unsigned long linkoffset;
	int nextarg;
	const char *command;

	if (argc != 3) {
		return KDB_ARGCOUNT;
	}

	nextarg = 1;
	diag = kdbgetaddrarg(argc, argv, &nextarg, &addr, &offset, NULL);
	if (diag)
		return diag;

	diag = kdbgetularg(argv[2], &linkoffset);
	if (diag)
		return diag;

	/*
	 * Using the starting address as
	 * the first element in the list, and assuming that
	 * the list ends with a null pointer.
	 */

	va = addr;
	if (!(command = kdb_strdup(argv[3], GFP_KDB))) {
		lkmd_printf("%s: cannot duplicate command\n", __FUNCTION__);
		return 0;
	}
	/* Recursive use of kdb_parse, do not use argv after this point */
	argv = NULL;

	while (va) {
		char buf[80];

		if (KDB_FLAG(CMD_INTERRUPT))
			return 0;

		sprintf(buf, "%s " kdb_machreg_fmt "\n", command, va);
		diag = kdb_parse(buf);
		if (diag)
			return diag;

		addr = va + linkoffset;
		if (kdb_getword(&va, addr, sizeof(va)))
			return 0;
	}
	kfree(command);

	return 0;
}

/*
 * kdb_help
 *
 *	This function implements the 'help' and '?' commands.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

static int kdb_help(int argc, const char **argv)
{
	kdbtab_t *kt;
	int i;

	lkmd_printf("%-15.15s %-20.20s %s\n", "Command", "Usage", "Description");
	lkmd_printf("----------------------------------------------------------\n");
	for(i=0, kt=kdb_commands; i<kdb_max_commands; i++, kt++) {
		if (kt->cmd_name)
			lkmd_printf("%-15.15s %-20.20s %s\n", kt->cmd_name,
				   kt->cmd_usage, kt->cmd_help);
	}
	return 0;
}

extern int kdb_wake_up_process(struct task_struct * p);

// /*
//  * kdb_kill
//  *
//  *	This function implements the 'kill' commands.
//  *
//  * Inputs:
//  *	argc	argument count
//  *	argv	argument vector
//  * Outputs:
//  *	None.
//  * Returns:
//  *	zero for success, a kdb diagnostic if error
//  * Locking:
//  *	none.
//  * Remarks:
//  */

// static int kdb_kill(int argc, const char **argv)
// {
// 	long sig, pid;
// 	char *endp;
// 	struct task_struct *p;
// 	struct siginfo info;

// 	if (argc!=2)
// 		return KDB_ARGCOUNT;

// 	sig = simple_strtol(argv[1], &endp, 0);
// 	if (*endp)
// 		return KDB_BADINT;
// 	if (sig >= 0 ) {
// 		lkmd_printf("Invalid signal parameter.<-signal>\n");
// 		return 0;
// 	}
// 	sig=-sig;

// 	pid = simple_strtol(argv[2], &endp, 0);
// 	if (*endp)
// 		return KDB_BADINT;
// 	if (pid <=0 ) {
// 		lkmd_printf("Process ID must be large than 0.\n");
// 		return 0;
// 	}

// 	/* Find the process. */
// 	if (!(p = lkmd_find_task_by_pid_ns(pid, &init_pid_ns))) {
// 		lkmd_printf("The specified process isn't found.\n");
// 		return 0;
// 	}
// 	p = p->group_leader;
// 	info.si_signo = sig;
// 	info.si_errno = 0;
// 	info.si_code = SI_USER;
// 	info.si_pid = pid;	/* use same capabilities as process being signalled */
// 	info.si_uid = 0;	/* kdb has root authority */
// 	kdb_send_sig_info(p, &info, kdb_seqno);
// 	return 0;
// }

// struct kdb_tm {
// 	int tm_sec;	/* seconds */
// 	int tm_min;	/* minutes */
// 	int tm_hour;	/* hours */
// 	int tm_mday;	/* day of the month */
// 	int tm_mon;	/* month */
// 	int tm_year;	/* year */
// };
// 
// static void
// kdb_gmtime(struct timespec *tv, struct kdb_tm *tm)
// {
// 	/* This will work from 1970-2099, 2100 is not a leap year */
// 	static int mon_day[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
// 	memset(tm, 0, sizeof(*tm));
// 	tm->tm_sec  = tv->tv_sec % (24 * 60 * 60);
// 	tm->tm_mday = tv->tv_sec / (24 * 60 * 60) + (2 * 365 + 1); /* shift base from 1970 to 1968 */
// 	tm->tm_min =  tm->tm_sec / 60 % 60;
// 	tm->tm_hour = tm->tm_sec / 60 / 60;
// 	tm->tm_sec =  tm->tm_sec % 60;
// 	tm->tm_year = 68 + 4*(tm->tm_mday / (4*365+1));
// 	tm->tm_mday %= (4*365+1);
// 	mon_day[1] = 29;
// 	while (tm->tm_mday >= mon_day[tm->tm_mon]) {
// 		tm->tm_mday -= mon_day[tm->tm_mon];
// 		if (++tm->tm_mon == 12) {
// 			tm->tm_mon = 0;
// 			++tm->tm_year;
// 			mon_day[1] = 28;
// 		}
// 	}
// 	++tm->tm_mday;
// }

/*
 * Most of this code has been lifted from kernel/timer.c::sys_sysinfo().
 * I cannot call that code directly from kdb, it has an unconditional
 * cli()/sti() and calls routines that take locks which can stop the debugger.
 */

// static void kdb_sysinfo(struct sysinfo *val)
// {   	
// 	struct timespec uptime;
// 	ktime_get_ts(&uptime);
// 	memset(val, 0, sizeof(*val));
// 	val->uptime = uptime.tv_sec;
// 	val->loads[0] = avenrun[0];
// 	val->loads[1] = avenrun[1];
// 	val->loads[2] = avenrun[2];
// 	val->procs = nr_threads-1;
// 	si_meminfo(val);
// 	// kdb_si_swapinfo(val);

// 	return;
// }

/*
 * kdb_summary
 *
 *	This function implements the 'summary' command.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

// static int kdb_summary(int argc, const char **argv)
// {
// 	struct sysinfo val;

// 	if (argc)
// 		return KDB_ARGCOUNT;

// 	lkmd_printf("sysname    %s\n", init_uts_ns.name.sysname);
// 	lkmd_printf("release    %s\n", init_uts_ns.name.release);
// 	lkmd_printf("version    %s\n", init_uts_ns.name.version);
// 	lkmd_printf("machine    %s\n", init_uts_ns.name.machine);
// 	lkmd_printf("nodename   %s\n", init_uts_ns.name.nodename);
// 	lkmd_printf("domainname %s\n", init_uts_ns.name.domainname);
// 	lkmd_printf("ccversion  %s\n", __stringify(CCVERSION));

// 	kdb_sysinfo(&val);
// 	lkmd_printf("uptime     ");
// 	if (val.uptime > (24*60*60)) {
// 		int days = val.uptime / (24*60*60);
// 		val.uptime %= (24*60*60);
// 		lkmd_printf("%d day%s ", days, days == 1 ? "" : "s");
// 	}
// 	lkmd_printf("%02ld:%02ld\n", val.uptime/(60*60), (val.uptime/60)%60);

// 	/* lifted from fs/proc/proc_misc.c::loadavg_read_proc() */

// #define LOAD_INT(x) ((x) >> FSHIFT)
// #define LOAD_FRAC(x) LOAD_INT(((x) & (FIXED_1-1)) * 100)
// 	lkmd_printf("load avg   %ld.%02ld %ld.%02ld %ld.%02ld\n",
// 		LOAD_INT(val.loads[0]), LOAD_FRAC(val.loads[0]),
// 		LOAD_INT(val.loads[1]), LOAD_FRAC(val.loads[1]),
// 		LOAD_INT(val.loads[2]), LOAD_FRAC(val.loads[2]));
// 	lkmd_printf("\n");
// #undef LOAD_INT
// #undef LOAD_FRAC

// 	return 0;
// }

/*
 * kdb_per_cpu
 *
 *	This function implements the 'per_cpu' command.
 *
 * Inputs:
 *	argc	argument count
 *	argv	argument vector
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, a kdb diagnostic if error
 * Locking:
 *	none.
 * Remarks:
 */

static int kdb_per_cpu(int argc, const char **argv)
{
// 	char buf[256], fmtstr[64];
// 	kdb_symtab_t symtab;
// 	cpumask_t suppress;
// 	int cpu, diag;
// 	unsigned long addr, val, bytesperword = 0, whichcpu = ~0UL;

// 	if (argc < 1 || argc > 3)
// 		return KDB_ARGCOUNT;

// 	cpus_clear(suppress);
// 	snprintf(buf, sizeof(buf), "per_cpu__%s", argv[1]);
// 	if (!lkmd_get_sym_val(buf, &symtab)) {
// 		lkmd_printf("%s is not a per_cpu variable\n", argv[1]);
// 		return KDB_BADADDR;
// 	}
// 	if (argc >=2 && (diag = kdbgetularg(argv[2], &bytesperword)))
// 		return diag;
// 	if (!bytesperword)
// 		bytesperword = KDB_WORD_SIZE;
// 	else if (bytesperword > KDB_WORD_SIZE)
// 		return KDB_BADWIDTH;
// 	sprintf(fmtstr, "%%0%dlx ", (int)(2*bytesperword));
// 	if (argc >= 3) {
// 		if ((diag = kdbgetularg(argv[3], &whichcpu)))
// 			return diag;
// 		if (!cpu_online(whichcpu)) {
// 			lkmd_printf("cpu %ld is not online\n", whichcpu);
// 			return KDB_BADCPUNUM;
// 		}
// 	}

// 	/* Most architectures use __per_cpu_offset[cpu], some use
// 	 * __per_cpu_offset(cpu), smp has no __per_cpu_offset.
// 	 */
// #ifdef	__per_cpu_offset
// #define KDB_PCU(cpu) __per_cpu_offset(cpu)
// #else
// #ifdef	CONFIG_SMP
// #define KDB_PCU(cpu) __per_cpu_offset[cpu]
// #else
// #define KDB_PCU(cpu) 0
// #endif
// #endif

// 	for_each_online_cpu(cpu) {
// 		if (whichcpu != ~0UL && whichcpu != cpu)
// 			continue;
// 		addr = symtab.sym_start + KDB_PCU(cpu);
// 		if ((diag = kdb_getword(&val, addr, bytesperword))) {
// 			lkmd_printf("%5d " kdb_bfd_vma_fmt0 " - unable to read, diag=%d\n",
// 				cpu, addr, diag);
// 			continue;
// 		}
// #ifdef	CONFIG_SMP
// 		if (!val) {
// 			cpu_set(cpu, suppress);
// 			continue;
// 		}
// #endif	/* CONFIG_SMP */
// 		lkmd_printf("%5d ", cpu);
// 		kdb_md_line(fmtstr, addr,
// 			bytesperword == KDB_WORD_SIZE,
// 			1, bytesperword, 1, 1, 0);
// 	}
// 	if (cpus_weight(suppress) == 0)
// 		return 0;
// 	lkmd_printf("Zero suppressed cpu(s):");
// 	for_each_cpu_mask(cpu, suppress) {
// 		lkmd_printf(" %d", cpu);
// 		if (cpu == NR_CPUS-1 || next_cpu(cpu, suppress) != cpu + 1)
// 			continue;
// 		while (cpu < NR_CPUS && next_cpu(cpu, suppress) == cpu + 1)
// 			++cpu;
// 		lkmd_printf("-%d", cpu);
// 	}
// 	lkmd_printf("\n");

// #undef KDB_PCU

	return 0;
}

/*
 * lkmd_register_repeat
 *
 *	This function is used to register a kernel debugger command.
 *
 * Inputs:
 *	cmd	Command name
 *	func	Function to execute the command
 *	usage	A simple usage string showing arguments
 *	help	A simple help string describing command
 *	repeat	Does the command auto repeat on enter?
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, one if a duplicate command.
 * Locking:
 *	none.
 * Remarks:
 *
 */

#define kdb_command_extend 50	/* arbitrary */
int lkmd_register_repeat(char *cmd, kdb_func_t func,
		char *usage, char *help, short minlen, kdb_repeat_t repeat)
{
	int i;
	kdbtab_t *kp;

	/*
	 *  Brute force method to determine duplicates
	 */
	for (i=0, kp=kdb_commands; i<kdb_max_commands; i++, kp++) {
		if (kp->cmd_name && (strcmp(kp->cmd_name, cmd)==0)) {
			lkmd_printf("Duplicate kdb command registered: "
				"%s, func %p help %s\n", cmd, func, help);
			return 1;
		}
	}

	/*
	 * Insert command into first available location in table
	 */
	for (i=0, kp=kdb_commands; i<kdb_max_commands; i++, kp++) {
		if (kp->cmd_name == NULL) {
			break;
		}
	}

	if (i >= kdb_max_commands) {
		kdbtab_t *new = kmalloc((kdb_max_commands + kdb_command_extend) * sizeof(*new), GFP_KDB);
		if (!new) {
			lkmd_printf("Could not allocate new kdb_command table\n");
			return 1;
		}
		if (kdb_commands) {
			memcpy(new, kdb_commands, kdb_max_commands * sizeof(*new));
			kfree(kdb_commands);
		}
		memset(new + kdb_max_commands, 0, kdb_command_extend * sizeof(*new));
		kdb_commands = new;
		kp = kdb_commands + kdb_max_commands;
		kdb_max_commands += kdb_command_extend;
	}

	kp->cmd_name   = cmd;
	kp->cmd_func   = func;
	kp->cmd_usage  = usage;
	kp->cmd_help   = help;
	kp->cmd_flags  = 0;
	kp->cmd_minlen = minlen;
	kp->cmd_repeat = repeat;

	return 0;
}

/*
 * lkmd_register
 *
 *	Compatibility register function for commands that do not need to
 *	specify a repeat state.  Equivalent to lkmd_register_repeat with
 *	KDB_REPEAT_NONE.
 *
 * Inputs:
 *	cmd	Command name
 *	func	Function to execute the command
 *	usage	A simple usage string showing arguments
 *	help	A simple help string describing command
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, one if a duplicate command.
 * Locking:
 *	none.
 * Remarks:
 *
 */

int lkmd_register(char *cmd, kdb_func_t func, char *usage, char *help, short minlen)
{
	return lkmd_register_repeat(cmd, func, usage, help, minlen, KDB_REPEAT_NONE);
}

/*
 * lkmd_unregister
 *
 *	This function is used to unregister a kernel debugger command.
 *	It is generally called when a module which implements kdb
 *	commands is unloaded.
 *
 * Inputs:
 *	cmd	Command name
 * Outputs:
 *	None.
 * Returns:
 *	zero for success, one command not registered.
 * Locking:
 *	none.
 * Remarks:
 *
 */

int lkmd_unregister(char *cmd)
{
	int i;
	kdbtab_t *kp;

	/*
	 *  find the command.
	 */
	for (i = 0, kp = kdb_commands; i < kdb_max_commands; i++, kp++) {
		if (kp->cmd_name && (strcmp(kp->cmd_name, cmd)==0)) {
			kp->cmd_name = NULL;
			return 0;
		}
	}

	/*
	 * Couldn't find it.
	 */
	return 1;
}

/*
 * kdb_inittab
 *
 *	This function is called by the kdb_init function to initialize
 *	the kdb command table.   It must be called prior to any other
 *	call to lkmd_register_repeat.
 *
 * Inputs:
 *	None.
 * Outputs:
 *	None.
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 *
 */

static void __init kdb_inittab(void)
{
	int i;
	kdbtab_t *kp;

	for(i = 0, kp = kdb_commands; i < kdb_max_commands; i++,kp++)
		kp->cmd_name = NULL;

	lkmd_register_repeat("md", kdb_md, "<vaddr>",   "Display Memory Contents, also mdWcN, e.g. md8c1", 1, KDB_REPEAT_NO_ARGS);
	lkmd_register_repeat("mdr", kdb_md, "<vaddr> <bytes>", 	"Display Raw Memory", 0, KDB_REPEAT_NO_ARGS);
	lkmd_register_repeat("mdp", kdb_md, "<paddr> <bytes>", 	"Display Physical Memory", 0, KDB_REPEAT_NO_ARGS);
	lkmd_register_repeat("mds", kdb_md, "<vaddr>", 	"Display Memory Symbolically", 0, KDB_REPEAT_NO_ARGS);
	lkmd_register_repeat("mm", kdb_mm, "<vaddr> <contents>",   "Modify Memory Contents", 0, KDB_REPEAT_NO_ARGS);
	lkmd_register_repeat("id", kdb_id, "<vaddr>",   "Display Instructions", 1, KDB_REPEAT_NO_ARGS);
	lkmd_register_repeat("go", kdb_go, "[<vaddr>]", "Continue Execution", 1, KDB_REPEAT_NONE);
	lkmd_register_repeat("rd", kdb_rd, "",		"Display Registers", 1, KDB_REPEAT_NONE);
	lkmd_register_repeat("rm", kdb_rm, "<reg> <contents>", "Modify Registers", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("ef", kdb_ef, "<vaddr>",   "Display exception frame", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("bt", kdb_bt, "[<vaddr>]", "Stack traceback", 1, KDB_REPEAT_NONE);
	// lkmd_register_repeat("btp", kdb_bt, "<pid>", 	"Display stack for process <pid>", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("bta", kdb_bt, "[DRSTCZEUIMA]", 	"Display stack all processes", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("btc", kdb_bt, "", 	"Backtrace current process on each cpu", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("btt", kdb_bt, "<vaddr>", 	"Backtrace process given its struct task address", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("ll", kdb_ll, "<first-element> <linkoffset> <cmd>", "Execute cmd for each element in linked list", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("env", kdb_env, "", 	"Show environment variables", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("set", kdb_set, "", 	"Set environment variables", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("help", kdb_help, "", 	"Display Help Message", 1, KDB_REPEAT_NONE);
	lkmd_register_repeat("?", kdb_help, "",         "Display Help Message", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("cpu", kdb_cpu, "<cpunum>","Switch to new cpu", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("ps", kdb_ps, "[<flags>|A]", "Display active task list", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("pid", kdb_pid, "<pidnum>",	"Switch to another task", 0, KDB_REPEAT_NONE);
	lkmd_register_repeat("reboot", kdb_reboot, "",  "Reboot the machine immediately", 0, KDB_REPEAT_NONE);
#if defined(CONFIG_MODULES)
	lkmd_register_repeat("lsmod", kdb_lsmod, "",	"List loaded kernel modules", 0, KDB_REPEAT_NONE);
#endif
	// lkmd_register_repeat("kill", kdb_kill, "<-signal> <pid>", "Send a signal to a process", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("summary", kdb_summary, "", "Summarize the system", 4, KDB_REPEAT_NONE);
	lkmd_register_repeat("per_cpu", kdb_per_cpu, "", "Display per_cpu variables", 3, KDB_REPEAT_NONE);
	// lkmd_register_repeat("print", kdb_debuginfo_print, "<expression>",
	// 	"Type casting, as in lcrash",  0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("px", kdb_debuginfo_print, "<expression>",
	//    "Print in hex (type casting) (see 'pxhelp')",  0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("pxhelp", kdb_pxhelp, "",
	// 	"Display help for the px command", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("pd", kdb_debuginfo_print, "<expression>",
	// 	"Print in decimal (type casting)", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("whatis", kdb_debuginfo_print,"<type or symbol>",
	// "Display the type, or the address for a symbol", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("sizeof", kdb_debuginfo_print, "<type>",
	// "Display the size of a structure, typedef, etc.", 0, KDB_REPEAT_NONE);
    //     lkmd_register_repeat("walk", kdb_walk, "",
	// 	"Walk a linked list (see 'walkhelp')", 0, KDB_REPEAT_NONE);
	// lkmd_register_repeat("walkhelp", kdb_walkhelp, "",
	// 	"Display help for the walk command", 0, KDB_REPEAT_NONE);
}

/*
 * kdb_panic
 *
 *	Invoked via the panic_notifier_list.
 *
 * Inputs:
 *	None.
 * Outputs:
 *	None.
 * Returns:
 *	Zero.
 * Locking:
 *	None.
 * Remarks:
 *	When this function is called from panic(), the other cpus have already
 *	been stopped.
 *
 */

static int kdb_panic(struct notifier_block *self, unsigned long command, void *ptr)
{
	KDB_ENTER();
	return 0;
}

static struct notifier_block kdb_block = { kdb_panic, NULL, 0 };

// static int kdb_cpu_callback(struct notifier_block *nfb, unsigned long action, void *hcpu)
// {
// 	if (action == CPU_ONLINE) {
// 		int cpu =(unsigned long)hcpu;
// 		cpumask_t save_cpus_allowed = current->cpus_allowed;
// 		set_cpus_allowed_ptr(current, &cpumask_of_cpu(cpu));
// 		kdb(KDB_REASON_CPU_UP, 0, NULL); /* do kdb setup on this cpu */
// 		set_cpus_allowed_ptr(current, &save_cpus_allowed);
// 	}
// 	return NOTIFY_OK;
// }

// static struct notifier_block kdb_cpu_nfb = {
// 	.notifier_call = kdb_cpu_callback
// };

/*
 * kdb_init
 *
 * 	Initialize the kernel debugger environment.
 *
 * Parameters:
 *	None.
 * Returns:
 *	None.
 * Locking:
 *	None.
 * Remarks:
 *	None.
 */

int __init lkmd_init(void)
{
	kdb_initial_cpu = smp_processor_id();

	if (lkmd_kernsym_init()) {
		printk(KERN_ERR "Get kernel symbol error\n");
 		return -EFAULT;
    }

	/*
	 * This must be called before any calls to lkmd_printf.
	 */
	if (kdb_io_init()) {
		printk(KERN_ERR "Init io error\n");
		return -EFAULT;
    }
	
	lkmd_printf("LKMD Version %d.%d%s, elemeta <elemeta47@gmail.com>\n",
		KDB_MAJOR_VERSION, KDB_MINOR_VERSION, KDB_TEST_VERSION);

	kdb_inittab();		/* Initialize Command Table */
	kdb_initbptab();	/* Initialize Breakpoint Table */
	kdb_id_init();		/* Initialize Disassembler */
	lkmda_init();		/* Architecture Dependent Initialization */

	kdb_initial_cpu = -1;	/* Avoid recursion problems */
	kdb(KDB_REASON_CPU_UP, 0, NULL);	/* do kdb setup on boot cpu */
	kdb_initial_cpu = smp_processor_id();
	//atomic_notifier_chain_register(&panic_notifier_list, &kdb_block);
	//register_cpu_notifier(&kdb_cpu_nfb);

#ifdef kdba_setjmp
	kdbjmpbuf = vmalloc(NR_CPUS * sizeof(*kdbjmpbuf));
	if (!kdbjmpbuf) {
		lkmd_printf("Cannot allocate kdbjmpbuf, no kdb recovery will be possible\n");
        return -ENOMEM;
    }
#endif	/* kdba_setjmp */

	kdb_initial_cpu = -1;
	kdb_wait_for_cpus_secs = 2 * num_online_cpus();
	kdb_wait_for_cpus_secs = max(kdb_wait_for_cpus_secs, 10);

	/* for lsmod command, must set init function */
#if defined(CONFIG_MODULES)
	kdb_modules = THIS_MODULE->list.prev;
#endif

    return 0;
}

void __exit lkmd_exit(void)
{
	lkmd_printf("LKMD Exited!\n");
	
	kdb_initial_cpu = -1;

#ifdef kdba_setjmp
    if (kdbjmpbuf)
	    vfree(kdbjmpbuf);
#endif
}

module_init(lkmd_init);
module_exit(lkmd_exit);

MODULE_AUTHOR("elemeta <elemeta@foxmail.com>");
MODULE_DESCRIPTION("LKMD - Linux Kernel Module Debugger");
MODULE_LICENSE("GPL");

