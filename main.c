/* $begin shellmain */
#include <csapp/csapp.h>

#define MAXARGS   128
#define MAXJOBS    128

enum processStat{Running, Stopped};

typedef struct
{
    int gid;
    int pid;
    enum processStat stat;
    char *cmdline;
}Job;

typedef struct
{
    Job job[MAXJOBS];
    int minIndex;/* Min available index now */
    int maxIndex;/* Max index in history */
}Jobs;

/* Globle static variable */
static Jobs fgjobs;
static Jobs bgjobs;
static pthread_mutex_t mutex;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;

/* Function prototypes */
void init();
void eval(char *cmdline);
int parseline(char *buf, char **argv);
int builtin_command(char **argv);
int do_fgbg(int index, int fg);
void waitfg(int pgid);
void sigchld_handler(int signal);
void sigint_handler(int signal);
void sigtstp_handler(int signal);
void parseForFgAndBg(char **argv, int fg);
void bgFdRediction(char *tmpfile);
void *sigmgr_thread(void *);
void *sigmgr_chld_thread(void *);

void initjob(Job *job);
void initjobs(Jobs *jobs);
int addjob(Jobs *jobs, int gid, int pid, enum processStat stat, char *argv);
//int deletejob(Jobs *jobs, int pid);
char *getProcessStat(const Job *job) { return job->stat == Running ? "Running" : "Stopped"; }
int findInJobs(Jobs *jobs, int pid);
void showTheJob(Jobs *jobs, int index);
void showJobs(Jobs *jobs);

int unix_err_ret(char *msg);

int main()
{
    char cmdline[MAXLINE]; /* Command line */

    init();

    while (1) {
        /* Read */
        pthread_mutex_lock(&mutex);
        printf("> ");
        Fgets(cmdline, MAXLINE, stdin);
        if (feof(stdin))
            exit(0);
        pthread_mutex_unlock(&mutex);
        /* Evaluate */
        eval(cmdline);
    }
}
/* $end shellmain */

void init()
{
    initjobs(&fgjobs);
    initjobs(&bgjobs);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTSTP);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    pthread_mutex_init(&mutex, NULL);

    pthread_t ppid;
    pthread_create(&ppid, NULL, sigmgr_thread, NULL);
    pthread_create(&ppid, NULL, sigmgr_chld_thread, NULL);
}

/* $begin eval */
/* eval - Evaluate a command line */
void eval(char *cmdline)
{
    char *argv[MAXARGS]; /* Argument list execve() */
    char buf[MAXLINE];   /* Holds modified command line */
    int bg;              /* Should the job run in bg or fg? */
    pid_t pid;           /* Process id */

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL)
        return;   /* Ignore empty lines */

    if (!builtin_command(argv)) {
        if ((pid = Fork()) == 0) {/* Child runs user job */
            mkdir("./tmp", S_IXUSR | S_IWUSR);
            char tmpfile[] = "./tmp/XXXXXX";

            setpgid(0, 0);

            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGCHLD);
            sigaddset(&mask, SIGTSTP);
            sigaddset(&mask, SIGINT);
            sigprocmask(SIG_UNBLOCK, &mask, NULL);

            if(bg)
                bgFdRediction(tmpfile);

            if (execvp(argv[0], argv) < 0) {
                printf("%s: Command not found.\n", argv[0]);
                if(bg)
                    unlink(tmpfile);
                unlink("./tmp");
                exit(0);
            }
        }

        setpgid(pid, pid);/* Avoid competition */

        /* Parent waits for foreground job to terminate */
        if (!bg) {
            addjob(&fgjobs, pid, pid, Running, cmdline);
            waitfg(pid);
        }
        else{
            addjob(&bgjobs, pid, pid, Running, cmdline);
            int index = findInJobs(&bgjobs, pid);
            showTheJob(&bgjobs, index);
        }
    }
    pthread_cond_signal(&cond);

    return;
}

void bgFdRediction(char *tmpfile)
{
    int fd0, fd1;

    fd0 = Open("/dev/null", O_RDWR, 0);
    fd1 = mkstemp(tmpfile);
    Dup2(fd0, STDIN_FILENO);
    Dup2(fd1, STDOUT_FILENO);
}

/* If first arg is a builtin command, run it and return true */
int builtin_command(char **argv)
{
    if (!strcmp(argv[0], "quit") || !strcmp(argv[0], "q")) /* quit command */
        exit(0);
    if (!strcmp(argv[0], "&"))/* Ignore singleton & */
        return 1;
    if (!strcmp(argv[0], "fg")) {
        parseForFgAndBg(argv, 1);
        return 1;
    }
    if(!strcmp(argv[0], "bg")){
        parseForFgAndBg(argv, 0);
        return 1;
    }
    if(!strcmp(argv[0], "jobs")) {
        showJobs(&bgjobs);
        return 1;
    }
    return 0;                     /* Not a builtin command */
}
/* $end eval */

void parseForFgAndBg(char **argv, int fg)
{
    char *pos, indexStr[MAXLINE];
    int index, pid;

    if((pos = strchr(argv[1],  '%')) == NULL){
        pid = atoi(argv[1]);
        if((index = findInJobs(&bgjobs, pid)) >= 0)
            do_fgbg(index, fg);
        else
            unix_err_ret("The process is not in bgjobs");
    }
    else{
        strcpy(indexStr, pos + 1);
        index = atoi(indexStr);
        if(bgjobs.job[index].pid != -1)
            do_fgbg(index, fg);
        else
            unix_err_ret("The item of the index of fgjobs is empty");
    }
}

/* $begin parseline */
/* parseline - Parse the command line and build the argv array */
int parseline(char *buf, char **argv)
{
    char *delim;         /* Points to first space delimiter */
    int argc;            /* Number of args */
    int bg;              /* Background job? */

    buf[strlen(buf)-1] = ' ';  /* Replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* Ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    while ((delim = strchr(buf, ' '))) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* Ignore spaces */
            buf++;
    }
    argv[argc] = NULL;

    if (argc == 0)  /* Ignore blank line */
        return 1;

    /* Should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0)
        argv[--argc] = NULL;

    return bg;
}
/* $end parseline */
void initjob(Job *job)
{
    job->gid = -1;
    job->pid = -1;
    job->cmdline = NULL;
}

void initjobs(Jobs *jobs)
{
    int i;
    for(i = 0; i < MAXJOBS; ++i){
        initjob(&jobs->job[i]);
        jobs->maxIndex = -1;
        jobs->minIndex = 0;
    }
}

int addjob(Jobs *jobs, int gid, int pid, enum processStat stat, char *argv)
{
    if(jobs->minIndex == MAXJOBS)
        unix_err_ret("The jobs is already full! ");

    int index = jobs->minIndex;

    jobs->job[index].gid = gid;
    jobs->job[index].pid = pid;
    jobs->job[index].stat = stat;
    jobs->job[index].cmdline = argv;

    jobs->minIndex++;
    jobs->maxIndex = jobs->maxIndex < index ? index : jobs->maxIndex;

}

//int deletejob(Jobs *jobs, int pid)
//{
//
//    int index;
//    if( (index = findInJobs(jobs, pid)) >= 0 ){
//        initjob(&jobs->job[index]);
//        jobs->minIndex = jobs->minIndex > index ? index : jobs->minIndex;
//        return 0;
//    }
//    else
//        unix_err_ret("The pid is not in the Jobs");
//}

int deletejobForIndex(Jobs *jobs, int index)
{
    if(index < 0 || index > jobs->maxIndex)
        unix_err_ret("deletejobForIndex error: the index out of range!");

    if( jobs->job[index].pid >= 0 ){
        initjob(&jobs->job[index]);
        jobs->minIndex = jobs->minIndex > index ? index : jobs->minIndex;
        return 0;
    }
    else
        unix_err_ret("The item of the index fo the job is empty");
}

int findInJobs(Jobs *jobs, int pid)
{
    int i;
    for(i = 0; i < jobs->maxIndex + 1; ++i)
        if(jobs->job[i].pid != -1 && jobs->job[i].pid == pid)
            return i;
    return -1;
}

int unix_err_ret(char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    return -1;
}

int do_fgbg(int index, int fg)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    Sigprocmask(SIG_BLOCK, &mask, NULL);

    if(fg){/* fg commmand */
        Kill(-bgjobs.job[index].gid, SIGCONT);
        addjob(&fgjobs, bgjobs.job[index].gid,
               bgjobs.job[index].pid, Running, bgjobs.job[index].cmdline);
        deletejobForIndex(&bgjobs, index);
        waitfg(fgjobs.job[index].gid);
    }
    else {
        Kill(-bgjobs.job[index].gid, SIGCONT);
        bgjobs.job[index].stat = Running;
    }
 }

void showTheJob(Jobs *jobs, int index)
{
    if(jobs->job[index].pid != -1)
        printf("[%d]  (%d)  %s\t%s\n",
               index, bgjobs.job[index].pid,
               getProcessStat(&bgjobs.job[index]), bgjobs.job[index].cmdline);
}

void showJobs(Jobs *jobs)
{
    int i;
    for(i = 0; i < jobs->maxIndex + 1; ++i)
        showTheJob(jobs, i);
}

void waitfg(int pgid)
{
    int pid, index, status;
    while((pid =  waitpid(-pgid, &status, WUNTRACED)) > 0) {
        if ((index = findInJobs(&fgjobs, pid)) >= 0)
            if (WIFSTOPPED(status)) {
                printf("\nJob [%d] (%d) stopped by signal %d\n",
                       index, fgjobs.job[index].pid, WSTOPSIG(status));
                addjob(&bgjobs, fgjobs.job[index].gid, fgjobs.job[index].pid,
                       Stopped, fgjobs.job[index].cmdline);
                deletejobForIndex(&fgjobs, index);
                return;
            } else
                deletejobForIndex(&fgjobs, index);
    }
}

void sigchld_handler(int signal)
{
    int pid, index;
    while((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        if ((index = findInJobs(&bgjobs, pid)) >= 0){
            printf("[%d]  (%d)  Done\t%s\n",
                   index, bgjobs.job[index].pid, bgjobs.job[index].cmdline);
            deletejobForIndex(&bgjobs, index);
        }
        else if ((index = findInJobs(&fgjobs, pid)) >= 0)
            deletejobForIndex(&fgjobs, index);
        else
            unix_err_ret("sigchld_handler error");
    }

    pthread_mutex_unlock(&mutex);
}

void sigtstp_handler(int signal)
{
    int i;
    for(i = 0; i < fgjobs.maxIndex + 1; ++i)
        if(fgjobs.job[i].pid != -1) {
            fgjobs.job[i].stat = Stopped;
            Kill(-fgjobs.job[i].gid, SIGTSTP);
        }
}

void sigint_handler(int signal)
{
    int i;
    for(i = 0; i < fgjobs.maxIndex + 1; ++i)
        if(fgjobs.job[i].pid != -1)
            Kill(-fgjobs.job[i].gid, SIGINT);
    printf("\n");
}

void *sigmgr_thread(void *argv)
{
    siginfo_t info;
    sigset_t mask;

    pthread_detach(pthread_self());

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTSTP);

    while (1){
        if( sigwaitinfo(&mask, &info) != -1){
           if(info.si_signo == SIGINT) {
               sigint_handler(SIGINT);
           }
            else if(info.si_signo == SIGTSTP){
                sigtstp_handler(SIGTSTP);
           }
        }
    }
}

void *sigmgr_chld_thread(void *argv) {
    sigset_t mask;
    int sig;

    pthread_detach(pthread_self());

    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    while (1) {
        pthread_mutex_lock(&mutex);
        pthread_cond_wait(&cond, &mutex);
        pthread_mutex_unlock(&mutex);
        if( sigwait(&mask, &sig) != -1){
            if(sig == SIGCHLD) {
                pthread_mutex_lock(&mutex);
                sigchld_handler(SIGCHLD);
            }
        }
    }
}