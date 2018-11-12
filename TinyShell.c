
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

extern char **environ;      
char prompt[] = "tsh> ";    
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              
    pid_t pid;              /* job PID */
    int jid;                
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  
};
struct job_t jobs[MAXJOBS]; 
/* Function prototypes */


void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

int Sigprocmask(int action, sigset_t* sigset, void* t);
int Sigaddset(sigset_t* sigset, int signal);
void Sigemptyset(sigset_t* sigset);


int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             // print help message 
            usage();
	    break;
        case 'v':             // emit additional diagnostic info
            verbose = 1;
	    break;
        case 'p':             // don't print a prompt 
            emit_prompt = 0;  // handy for automatic testing 
	    break;
	default:
            usage();
	}
    }

    
    Signal(SIGINT,  sigint_handler);   // ctrl-c 
    Signal(SIGTSTP, sigtstp_handler);  // ctrl-z 
    Signal(SIGCHLD, sigchld_handler);  // Terminated or stopped child 
    Signal(SIGQUIT, sigquit_handler); 

    initjobs(jobs);

    while (1) {

	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { // End of file (ctrl-d)
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  

void eval(char *cmdline) 
{
    char* argv[50];
    sigset_t mask;
    int parseresult;
    pid_t pid;
    if(cmdline!=NULL)  //if there is data in cmdline
    {
        parseresult = parseline(cmdline, argv); //parse cmdline and build the argv array
    }

    if(*argv!=NULL&&(builtin_cmd(argv)==0)) //if argv contains data and it is not a builtin command
    {
        Sigemptyset(&mask); //initializing signal set mask
        Sigaddset(&mask, SIGTSTP); //adding SIGTSTP signal to the mask
        Sigaddset(&mask, SIGCHLD); //adding SIGCHILD signal to the mask
        Sigaddset(&mask, SIGINT); //adding SIGINT signal to the mask
        Sigprocmask(SIG_BLOCK, &mask, NULL); //block the set
        if((pid=fork())==0) //if child
        {
			Sigprocmask(SIG_UNBLOCK, &mask, NULL); //unblock the signals
			setpgid(0,0); //set group id to pid
			int res = execve(argv[0], argv, environ); //execute new program
			if(res==-1) //if execute failed
			{
				printf("%s: Command not found\n", argv[0]);
				return;
			}
        }
		else{
		if(parseresult==0) //if foreground task
		{ 
			addjob(jobs, pid,FG, cmdline); //add job to jobs list
			Sigprocmask(SIG_UNBLOCK, &mask, NULL); //unblock signals
			waitfg(pid); //wait for foreground task to finish
		}
		else if(parseresult==1) //if background task
		{
			addjob(jobs, pid, BG, cmdline); //add job to jobs list
			Sigprocmask(SIG_UNBLOCK, &mask, NULL); //unblock signals
			struct job_t* job = getjobpid(jobs, pid); //get the job using the pid
			printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline); //print
		}

    }
    }
    
    return;
}


int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  //ignore blank line
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

int builtin_cmd(char **argv) 
{
    if(strcmp(argv[0],"quit")==0) //if user typed in 'quit'
		exit(0); //exit the program
	else if(strcmp(argv[0], "jobs")==0) //if user typed in 'jobs'
	{
		listjobs(jobs); //print the jobs current running
		return 1; 
	}
	else if(strcmp(argv[0],"fg")==0) //if user typed in 'fg'
	{
		do_bgfg(argv); //call the do_bgfg function, sending argv as parameter
		return 1;
	}
	else if(strcmp(argv[0],"bg")==0) //if user typed in 'bg'
	{
		do_bgfg(argv);
		return 1;
			
	}
    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    struct job_t* job;
    if(argv[1]==NULL)  //if only 1 argument in argv, we need to print error messages
    {
        if(strcmp(argv[0],"fg")==0) //if the user had typed in fg
        {
            printf("fg command requires PID or %%jobid argument\n");
            return;
        }
        else if(strcmp(argv[0],"bg")==0) //if the user had typed in bg
        {
            printf("bg command requires PID or %%jobid argument\n");
        }
    }
    else 
    {
        if(argv[1][0]=='%') //if received a JID
        {
            int temp = atoi(&argv[1][1]); //convert number to string
			if(temp==0) //if not a number, print the error messages
			{
				if(strcmp(argv[0],"fg")==0)
				{
					printf("fg: argument must be a PID or %%jobid\n");
					return;
				}
				else if(strcmp(argv[0],"bg")==0)
				{
					printf("bg: argument must be a PID or %%jobid\n");
					return;
				}
			}
			job = getjobjid(jobs, temp); //get the job from the pid
			kill(-(job->pid), SIGCONT); 
			if(strcmp(argv[0],"fg")==0) //if user typed in 'fg'
			{
				job->state = FG; //set state of job to FG
				waitfg(job->pid); //wait for foreground job to finish
			}
			else if(strcmp(argv[0],"bg")==0) //if user typed in 'bg'
			{
				job->state = BG; //set state of job to BG
				printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
			}
        }
        else //if received a PID
        {
            int temp = atoi(argv[1]); //convert string to a number
            if(temp==0) //if the string cannot be converted into a number, error
            {
                if(strcmp(argv[0],"fg")==0)
                {
                    printf("fg: argument must be a PID or %%jobid\n");
                    return;
                }
                else if(strcmp(argv[0],"bg")==0)
                {
                    printf("bg: argument must be a PID or %%jobid\n");
                    return;
                }
            }
            job = getjobpid(jobs, temp); //get the job from the pid
			kill(-(job->pid), SIGCONT);
            if(strcmp(argv[0],"fg")==0)  //if the user typed in 'fg'
            {
                job->state=FG; //set job state to FG
                waitfg(job->pid); //wait for FG to finish
            }
            else if(strcmp(argv[0],"bg")==0) //if the user typed in 'bg'
            {
                job->state = BG; //set job state to BG
                printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
            }
        }
    }

    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    struct job_t* job = getjobpid(jobs, pid); //get job from pid
    if(job==NULL) //if no such job exists
    {
            return;
    }
    while(job->state==FG) //while state of the job is FG
    {
        sleep(1); 
    }
    return;
}


void sigchld_handler(int sig) 
{
    int status;
	pid_t pid, pid_fg = fgpid(jobs);
	while((pid=waitpid(pid_fg,&status,WNOHANG|WUNTRACED))>0) //loop to check for all children who have terminated
	{
		struct job_t* job = getjobpid(jobs,pid); //get the job from the pid
		if(WIFEXITED(status)>0) //if exited normally
		{
			deletejob(jobs, pid); //delete the job 
		}
		if(WIFSIGNALED(status)>0) //if terminated
		{
			printf("Job [%d] (%d) terminated by signal %d\n", job->jid,job->pid, WTERMSIG(status));
			deletejob(jobs,pid);
		}
		if(WIFSTOPPED(status)>0) //if stopped
		{
			printf("Job [%d] (%d) stopped by signal %d\n", job->jid,job->pid, WSTOPSIG(status));
			job->state = ST;
		}
		
	}
    return;
}


void sigint_handler(int sig) 
{
    pid_t pid = fgpid(jobs); //get the pid of the current foreground job
	if(pid==0) //if no current foreground job
	{
		printf("There are no foreground jobs currently\n");
		return;
	}
	else //otherwise, kill the process
	{
		kill(-pid, SIGINT);
	}
    return;
}


void sigtstp_handler(int sig) 
{
    pid_t pid = fgpid(jobs);
	if(pid==0)
	{
		printf("There are no foreground jobs currently\n");
		return;
	}
	else
	{
		kill(-pid, SIGTSTP);
	}
    return;
}




void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}


void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}


void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}


handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}


void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



int Sigprocmask(int action, sigset_t* sigset, void* t)
{
    int status;
    if((status=sigprocmask(action, sigset, NULL))==-1)
    {
        unix_error("Fatal: Sigprocmask error");
    }
    return status;
}

int Sigaddset(sigset_t* sigset, int signal)
{
    int status;
    if((status=sigaddset(sigset, signal))==-1)
    {
        unix_error("Fatal: Sigaddset error");
    }
    return status;
}

void Sigemptyset(sigset_t *set)
{
    if (sigemptyset(set) < 0)
	unix_error("Sigemptyset error");
    return;
}
