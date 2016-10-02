/*
 * tsh - A tiny shell program with job control
 *
 * Simple shell that supports
 * builtin command: jobs, bg, fg, quit
 * execeve binary with input/output redirection.
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <zconf.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */


/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

typedef enum {
  BUILTIN_NONE,
  BUILTIN_QUIT,
  BUILTIN_JOBS,
  BUILTIN_BG,
  BUILTIN_FG
} builtins_t;

struct cmdline_tokens {
    int argc;               /* Number of arguments */
    char *argv[MAXARGS];    /* The arguments list */
    char *infile;           /* The input file */
    char *outfile;          /* The output file */
    builtins_t builtins;    /* Indicates if argv[0] is a builtin command */
};

/* End global variables */


/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list);
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid);
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

pid_t Fork(void);
void Kill(pid_t pid, int signum);
void Sigemptyset(sigset_t *set);
void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
void Sigaddset(sigset_t *set, int signum);
void Setpgid(pid_t pid, pid_t pgid);
void Execve(const char *filename, char *const argv[], char *const envp[]);
int Sigsuspend(const sigset_t *set);
int Open(const char *pathname, int flags, mode_t mode);
void Close(int fd);

void builtin_bgfg(int argc, char **argv);
void wait_fg(pid_t pid);
void redirect_io(char *infile, char *outfile);
void builtin_jobs(char *outfile);

/*
 * main - The shell's main routine
 */
int
main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];    /* cmdline for fgets */
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(job_list);


    /* Execute the shell's read/eval loop */
    while (1) {

        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) {
            /* End of file (ctrl-d) */
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }

        /* Remove the trailing newline */
        cmdline[strlen(cmdline)-1] = '\0';

        /* Evaluate the command line */
        eval(cmdline);

        fflush(stdout);
        fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */
void
eval(char *cmdline)
{
    int bg;              /* should the job run in bg or fg? */
    struct cmdline_tokens tok;

    /* Parse command line */
    bg = parseline(cmdline, &tok);

    if (bg == -1) /* parsing error */
        return;
    if (tok.argv[0] == NULL) /* ignore empty lines */
        return;

    /* Handle built-in commands */
    switch (tok.builtins) {
        case BUILTIN_QUIT:
            exit(0);
        case BUILTIN_JOBS:
            builtin_jobs(tok.outfile);
            return;
        case BUILTIN_BG:
        case BUILTIN_FG:
            builtin_bgfg(tok.argc, tok.argv);
            return;
        case BUILTIN_NONE:
            break;
    }

    pid_t pid;
    sigset_t mask, prev;
    Sigemptyset(&mask);
    Sigaddset(&mask, SIGCHLD);
    Sigaddset(&mask, SIGINT);
    Sigaddset(&mask, SIGTSTP);

    /* Block signals before fork */
    Sigprocmask(SIG_BLOCK, &mask, &prev);

    /* Execute binary */
    if ((pid = Fork()) == 0) { /* Child */
        Sigprocmask(SIG_UNBLOCK, &mask, NULL);
        redirect_io(tok.infile, tok.outfile);
        Setpgid(0, 0);  // set pgid the same as pid
        Execve(tok.argv[0], tok.argv, environ);
    }

    /* Parent */
    if (bg) {
        addjob(job_list, pid, BG, cmdline);
        fprintf(stdout, "[%d] (%d) %s\n", pid2jid(pid), pid, cmdline);
    } else {
        addjob(job_list, pid, FG, cmdline);
        wait_fg(pid);
    }

    /* Don't unblock too early */
    Sigprocmask(SIG_UNBLOCK, &mask, NULL);

    return;
}

/*
 * builtin_jobs - The builtin command of jobs
 *
 * Parameters:
 *   outfile:       Output file to be redirected. NULL for stdout.
 */
void builtin_jobs(char *outfile)
{
    if (outfile) {
        int fdout;
        if ((fdout = open(outfile, O_WRONLY | O_CREAT, S_IRWXU)) < 0) {
            fprintf(stderr, "Fail to open file: %s\n", outfile);
            return;
        }
        listjobs(job_list, fdout);
        close(fdout);
    } else {
        listjobs(job_list, STDOUT_FILENO);
    }
}

/*
 * redirect_io - Redirect stdio into files.
 *
 *   Call this after fork, and before execve.
 *
 * Parameters:
 *   infile:        filename of redirected input
 *   outfile:       filename of redirected output
 *
 */
void redirect_io(char *infile, char *outfile)
{
    /* Redirect input */
    int fdin = -1;
    if (infile) {
        fdin = Open(infile, O_RDONLY, S_IRUSR);
        dup2(fdin, STDIN_FILENO);
        /* Close duplicated fdin */
        Close(fdin);
    }

    /* Redirect output */
    int fdout = -1;
    if (outfile) {
        fdout = Open(outfile,
                     O_WRONLY | O_CREAT,
                     S_IRUSR | S_IWUSR);
        dup2(fdout, STDOUT_FILENO);
        /* Close duplicated fdout */
        Close(fdout);
    }
}

/*
 * builtin_bgfg - Do the builtin bg or fg job
 *
 * Parameters:
 *   tokens:    Parsed tokens
 */
void
builtin_bgfg(int argc, char **argv)
{
    /* Sanity check */
    if (argc != 2 ||
        (argv[1][0] != '%' &&
         (argv[1][0] > '9' ||
          argv[1][0] < '0'))
       ) {
        fprintf(stderr, "Usage: %s <job>\n", argv[0]);
        fprintf(stderr, "where <job> may either be pid or jobid\n");
        return;
    }

    /* Prepare pid and job */
    pid_t pid;
    int jid;
    struct job_t *job;
    if (argv[1][0] == '%') {
        jid = atoi(argv[1] + 1);
        job = getjobjid(job_list, jid);
        if (job == NULL) {
            fprintf(stderr, "No such job: %%%s!\n", argv[1]);
            return;
        }
        pid = job->pid;
    } else {
        pid = atoi(argv[1]);
        jid = pid2jid(pid);
        job = getjobjid(job_list, jid);
        if (job == NULL) {
            fprintf(stderr, "No such job: %%%s!\n", argv[1]);
            return;
        }
    }

    Kill(-pid, SIGCONT);
    if (!strcmp(argv[0], "bg")) {  /* bg */
        job->state = BG;
        fprintf(stdout, "[%d] (%d) %s", jid, pid, job->cmdline);
    } else {  /* fg */
        job->state = FG;
        wait_fg(pid);
    }
}

/*
 * wait_fg - Wait fg job with pid.
 *
 * Parameters:
 *   pid:       pid of the job to be waited
 */
void wait_fg(pid_t pid) {
    struct job_t *job;
    sigset_t mask;
    Sigemptyset(&mask);

    while (1) {
        job = getjobpid(job_list, pid);

        /* No longer need to wait */
        if (job == NULL || job->state == ST ||
            job->state == BG) {
            break;
        }


        /* Wait till reaped or changed status.
         * As unblock/wait is atomic in sigsuspend,
         * it won't lead to race as pause does.
         */
        Sigsuspend(&mask);
    }
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. Characters
 *             enclosed in single or double quotes are treated as a single
 *             argument.
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job
 *  -1:        if cmdline is incorrectly formatted
 *
 * Note:       The string elements of tok (e.g., argv[], infile, outfile)
 *             are statically allocated inside parseline() and will be
 *             overwritten the next time this function is invoked.
 */
int
parseline(const char *cmdline, struct cmdline_tokens *tok)
{

    static char array[MAXLINE];          /* holds local copy of command line */
    const char delims[10] = " \t\r\n";   /* argument delimiters (white-space) */
    char *buf = array;                   /* ptr that traverses command line */
    char *next;                          /* ptr to the end of the current arg */
    char *endbuf;                        /* ptr to end of cmdline string */
    int is_bg;                           /* background job? */

    int parsing_state;                   /* indicates if the next token is the
                                            input or output file */

    if (cmdline == NULL) {
        (void) fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void) strncpy(buf, cmdline, MAXLINE);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf) {
        /* Skip the white-spaces */
        buf += strspn (buf, delims);
        if (buf >= endbuf) break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<') {
            if (tok->infile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>') {
            if (tok->outfile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf ++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"') {
            /* Detect quoted tokens */
            buf++;
            next = strchr (buf, *(buf-1));
        } else {
            /* Find next delimiter */
            next = buf + strcspn (buf, delims);
        }

        if (next == NULL) {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* Record the token as either the next argument or the i/o file */
        switch (parsing_state) {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS-1) break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL) {
        (void) fprintf(stderr,
                       "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0)  /* ignore blank line */
        return 1;

    if (!strcmp(tok->argv[0], "quit")) {                 /* quit command */
        tok->builtins = BUILTIN_QUIT;
    } else if (!strcmp(tok->argv[0], "jobs")) {          /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    } else if (!strcmp(tok->argv[0], "bg")) {            /* bg command */
        tok->builtins = BUILTIN_BG;
    } else if (!strcmp(tok->argv[0], "fg")) {            /* fg command */
        tok->builtins = BUILTIN_FG;
    } else {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The
 *     handler reaps all available zombie children, but doesn't wait
 *     for any other currently running children to terminate.
 */
void
sigchld_handler(int sig)
{
    int olderrno = errno;
    pid_t pid;
    int status;

    if (verbose) {
        fprintf(stdout, "Enter sigchld_handler\n");
    }

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        if (WIFEXITED(status)) {
            /* Child has exited */
            if (!deletejob(job_list, pid)) {
                continue;
            }
            if (verbose) {
                fprintf(stdout, "sigchld_handler: %d exited, status=%d\n",
                        pid, WEXITSTATUS(status));
            }
        } else if (WIFSIGNALED(status)) {
            /* Child is cancelled by some signal */
            int jid = pid2jid(pid);
            if (!deletejob(job_list, pid)) {
                continue;
            }
            fprintf(stdout, "Job [%d] (%d) terminated by signal %d\n",
                    jid, pid, WTERMSIG(status));
        } else if (WIFSTOPPED(status)) {
            /* Child is stopped by some signal */
            struct job_t *job = getjobpid(job_list, pid);
            if (!job) {
                fprintf(stderr, "sigchld_handler: Lost track of %d\n", pid);
                continue;
            }
            job->state = ST;
            fprintf(stdout, "Job [%d] (%d) stopped by signal %d\n",
                    job->jid, pid, WSTOPSIG(status));
        } else if (WIFCONTINUED(status)) {
            /* Child continued */
            struct job_t *job = getjobpid(job_list, pid);
            if (!job) {
                fprintf(stderr, "sigchld_handler: Lost track of %d\n", pid);
                continue;
            }
            if (verbose) {
                fprintf(stdout, "sigchld_handler: %d continued\n", pid);
            }
        }
    }

    errno = olderrno;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void
sigint_handler(int sig)
{
    int olderrno = errno;
    pid_t pid = fgpid(job_list);
    if (pid == 0) {
        return;
    }
    Kill(-pid, SIGINT);
    if (verbose) {
        fprintf(stdout, "sigint_handler: %d killed\n.", pid);
    }
    errno = olderrno;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void
sigtstp_handler(int sig)
{
    int olderrno = errno;
    pid_t pid = fgpid(job_list);
    if (pid == 0) {
        return;
    }
    Kill(-pid, SIGTSTP);
    if (verbose) {
        fprintf(stdout, "sigtstp_handler: %d stopped\n.", pid);
    }
    errno = olderrno;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void
clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void
initjobs(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int
maxjid(struct job_t *job_list)
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int
addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(job_list[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n",
                       job_list[i].jid,
                       job_list[i].pid,
                       job_list[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int
deletejob(struct job_t *job_list, pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t
fgpid(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t
*getjobpid(struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid)
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int
pid2jid(pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void
listjobs(struct job_t *job_list, int output_fd)
{
    int i;
    char buf[MAXLINE];

    for (i = 0; i < MAXJOBS; i++) {
        memset(buf, '\0', MAXLINE);
        if (job_list[i].pid != 0) {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            switch (job_list[i].state) {
            case BG:
                sprintf(buf, "Running    ");
                break;
            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void
usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void
unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void
app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t
*Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void
sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

/*
 * Fork - wrapper for fork
 */
pid_t Fork(void)
{
    pid_t pid;

    if ((pid = fork()) < 0)
        unix_error("Fork error");
    return pid;
}

/*
 * Kill - wrapper for kill
 */
void Kill(pid_t pid, int signum)
{
    int rc;

    if ((rc = kill(pid, signum)) < 0)
        unix_error("Kill error");
}

/*
 * Sigemptyset - wrapper for sigemptyset
 */
void Sigemptyset(sigset_t *set)
{
    if (sigemptyset(set) < 0)
        unix_error("Sigemptyset error");
    return;
}

/*
 * Sigprocmask - wrapper for sigprocmask
 */
void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    if (sigprocmask(how, set, oldset) < 0)
        unix_error("Sigprocmask error");
    return;
}

/*
 * Sigaddset - wrapper for sigaddset
 */
void Sigaddset(sigset_t *set, int signum)
{
    if (sigaddset(set, signum) < 0)
        unix_error("Sigaddset error");
    return;
}

/*
 * Setpgid - wrapper for setpgid
 */
void Setpgid(pid_t pid, pid_t pgid)
{
    int rc;

    if ((rc = setpgid(pid, pgid)) < 0)
        unix_error("Setpgid error");
    return;
}

/*
 * Exeve - wrapper for execve
 */
void Execve(const char *filename, char *const argv[], char *const envp[])
{
    if (execve(filename, argv, envp) < 0) {
        unix_error("Execve error");
        exit(1);
    }
}

/*
 * Sigsuspend - wrapper for sigsuspend
 */
int Sigsuspend(const sigset_t *set)
{
    int rc = sigsuspend(set); /* always returns -1 */
    if (errno != EINTR)
        unix_error("Sigsuspend error");
    return rc;
}

/*
 * Open - wrapper for open
 */
int Open(const char *pathname, int flags, mode_t mode)
{
    int rc;

    if ((rc = open(pathname, flags, mode))  < 0)
        unix_error("Open error");
    return rc;
}

/*
 * Close - wrapper for close
 */
void Close(int fd)
{
    int rc;

    if ((rc = close(fd)) < 0)
        unix_error("Close error");
}