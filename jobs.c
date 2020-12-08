#include "shell.h"

typedef struct proc {
  pid_t pid;    /* process identifier */
  int state;    /* RUNNING or STOPPED or FINISHED */
  int exitcode; /* -1 if exit status not yet received */
} proc_t;

typedef struct job {
  pid_t pgid;            /* 0 if slot is free */
  proc_t *proc;          /* array of processes running in as a job */
  struct termios tmodes; /* saved terminal modes */
  int nproc;             /* number of processes */
  int state;             /* changes when live processes have same state */
  char *command;         /* textual representation of command line */
} job_t;

static job_t *jobs = NULL;          /* array of all jobs */
static int njobmax = 1;             /* number of slots in jobs array */
static int tty_fd = -1;             /* controlling terminal file descriptor */
static struct termios shell_tmodes; /* saved shell terminal modes */

static void sigchld_handler(int sig) {
  int old_errno = errno;
  pid_t pid;
  int status;
  /* TODO: Change state (FINISHED, RUNNING, STOPPED) of processes and jobs.
   * Bury all children that finished saving their status in jobs. */

  while ((pid = waitpid(-1, &status, WUNTRACED | WNOHANG | WCONTINUED)) > 0) {
    bool done = false;
    int numbJob = 0;
    for (int i = 0; i < njobmax; i++) {
      if (done)
        break;
      struct job *job = &jobs[i];
      for (int k = 0; k < job->nproc; k++) {
        if (done)
          break;
        struct proc *proc = &job->proc[k];
        if (proc->pid == pid) {
          numbJob = i;
          done = true;
          if (WIFSTOPPED(status)) {
            proc->state = STOPPED;
            proc->exitcode = status;
          } else if (WIFEXITED(status)) {
            proc->state = FINISHED;
            proc->exitcode = status;
          } else if (WIFSIGNALED(status)) {
            proc->state = FINISHED;
            proc->exitcode = status;
          } else if (WIFCONTINUED(status)) {
            proc->state = RUNNING;
            proc->exitcode = status;
          }
        }
      }
    }
    unsigned int mask = 0;
    job_t *job = &jobs[numbJob];
    for (int i = 0; i < job->nproc; i++) {
      switch (job->proc[i].state) {
        case FINISHED: {
          mask = mask | 1;
          break;
        }
        case STOPPED: {
          mask = mask | 2;
          break;
        }
        case RUNNING: {
          mask = mask | 4;
          break;
        }
      }
    }
    switch (mask) {
      case 1: {
        job->state = FINISHED;
        break;
      }
      case 2: {
        job->state = STOPPED;
        break;
      }
      case 4: {
        job->state = RUNNING;
        break;
      }
      default: {
        break;
      }
    }
  }
  errno = old_errno;
}

/* When pipeline is done, its exitcode is fetched from the last process. */
static int exitcode(job_t *job) {
  return job->proc[job->nproc - 1].exitcode;
}

static int allocjob(void) {
  /* Find empty slot for background job. */
  for (int j = BG; j < njobmax; j++)
    if (jobs[j].pgid == 0)
      return j;

  /* If none found, allocate new one. */
  jobs = realloc(jobs, sizeof(job_t) * (njobmax + 1));
  memset(&jobs[njobmax], 0, sizeof(job_t));
  return njobmax++;
}

static int allocproc(int j) {
  job_t *job = &jobs[j];
  job->proc = realloc(job->proc, sizeof(proc_t) * (job->nproc + 1));
  return job->nproc++;
}

int addjob(pid_t pgid, int bg) {
  int j = bg ? allocjob() : FG;
  job_t *job = &jobs[j];
  /* Initial state of a job. */
  job->pgid = pgid;
  job->state = RUNNING;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
  job->tmodes = shell_tmodes;
  return j;
}

static void deljob(job_t *job) {
  assert(job->state == FINISHED);
  free(job->command);
  free(job->proc);
  job->pgid = 0;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
}

static void movejob(int from, int to) {
  assert(jobs[to].pgid == 0);
  memcpy(&jobs[to], &jobs[from], sizeof(job_t));
  memset(&jobs[from], 0, sizeof(job_t));
}

static void mkcommand(char **cmdp, char **argv) {
  if (*cmdp)
    strapp(cmdp, " | ");

  for (strapp(cmdp, *argv++); *argv; argv++) {
    strapp(cmdp, " ");
    strapp(cmdp, *argv);
  }
}

void addproc(int j, pid_t pid, char **argv) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int p = allocproc(j);
  proc_t *proc = &job->proc[p];
  /* Initial state of a process. */
  proc->pid = pid;
  proc->state = RUNNING;
  proc->exitcode = -1;
  mkcommand(&job->command, argv);
}

/* Returns job's state.
 * If it's finished, delete it and return exitcode through statusp. */
int jobstate(int j, int *statusp) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int state = job->state;

  /* TODO: Handle case where job has finished. */
  if (state == FINISHED) {
    *statusp = exitcode(job);
    deljob(job);
  }

  return state;
}

char *jobcmd(int j) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  return job->command;
}

/* Continues a job that has been stopped. If move to foreground was requested,
 * then move the job to foreground and start monitoring it. */
bool resumejob(int j, int bg, sigset_t *mask) {
  if (j < 0) {
    for (j = njobmax - 1; j > 0 && jobs[j].state == FINISHED; j--)
      continue;
  }

  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

  /* TODO: Continue stopped job. Possibly move job to foreground slot. */
  if (Tcgetpgrp(tty_fd) != getpgrp() || jobs[0].pgid != 0) {
    msg("No task control!\n");
    return false;
  }
  if (!bg) {
    movejob(j, 0);
    Tcsetpgrp(tty_fd, jobs[0].pgid);
    Tcsetattr(tty_fd, 0, &jobs[0].tmodes);
    if (jobs[0].state == STOPPED) {
      Kill(-jobs[0].pgid, SIGCONT);
      while (jobs[0].state == STOPPED) {
        sigsuspend(mask);
      }
    }
    msg("[%d] continue '%s'\n", j, jobcmd(0));
    monitorjob(mask);
  } else {
    if (jobs[j].state == STOPPED) {
      Kill(-jobs[j].pgid, SIGCONT);
      msg("[%d] continue '%s'\n", j, jobcmd(j));
    }
  }
  return true;
}

/* Kill the job by sending it a SIGTERM. */
bool killjob(int j) {
  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;
  debug("[%d] killing '%s'\n", j, jobs[j].command);

  /* TODO: I love the smell of napalm in the morning. */
  kill(-jobs[j].pgid, SIGTERM);
  if (jobs[j].state == STOPPED)
    kill(-jobs[j].pgid, SIGCONT);
  return true;
}

/* Report state of requested background jobs. Clean up finished jobs. */
void watchjobs(int which) {
  for (int j = BG; j < njobmax; j++) {
    if (jobs[j].pgid == 0)
      continue;

    /* TODO: Report job number, state, command and exit code or signal. */
    int status;
    char *cmd = strdup(jobcmd(j));
    int jobstat = jobstate(j, &status);
    switch (which) {
      case ALL: {
        if (jobstat == FINISHED)
          WIFEXITED(status)
        ? printf("[%d] exited '%s', status=%d\n", j, cmd, WEXITSTATUS(status))
        : printf("[%d] killed '%s' by signal %d\n", j, cmd, WTERMSIG(status));
        else if (jobstat == STOPPED) printf("[%d] suspended '%s'\n", j, cmd);
        else printf("[%d] running '%s'\n", j, cmd);
        break;
      }
      case FINISHED: {
        if (jobstat == FINISHED) {
          WIFEXITED(status)
          ? printf("[%d] exited '%s', status=%d\n", j, cmd, WEXITSTATUS(status))
          : printf("[%d] killed '%s' by signal %d\n", j, cmd, WTERMSIG(status));
          break;
        }
      }
    }
    free(cmd);
  }
}

/* Monitor job execution. If it gets stopped move it to background.
 * When a job has finished or has been stopped move shell to foreground. */
int monitorjob(sigset_t *mask) {
  int exitcode = 0, state;

  /* TODO: Following code requires use of Tcsetpgrp of tty_fd. */
  Tcsetpgrp(tty_fd, jobs[0].pgid);
  while ((state = jobstate(0, &exitcode)) == RUNNING) {
    sigsuspend(mask);
  }
  if (state == STOPPED) {
    Tcgetattr(tty_fd, &jobs[0].tmodes);
    int to = allocjob();
    movejob(0, to);
    msg("[%d] suspended '%s'\n", to, jobcmd(to));
  }
  if (state != RUNNING) {
    Tcsetattr(tty_fd, 0, &shell_tmodes);
    Tcsetpgrp(tty_fd, getpgrp());
  }
  return exitcode;
}

/* Called just at the beginning of shell's life. */
void initjobs(void) {
  Signal(SIGCHLD, sigchld_handler);
  jobs = calloc(sizeof(job_t), 1);

  /* Assume we're running in interactive mode, so move us to foreground.
   * Duplicate terminal fd, but do not leak it to subprocesses that execve. */
  assert(isatty(STDIN_FILENO));
  tty_fd = Dup(STDIN_FILENO);
  fcntl(tty_fd, F_SETFD, FD_CLOEXEC);

  /* Take control of the terminal. */
  Tcsetpgrp(tty_fd, getpgrp());

  /* Save default terminal attributes for the shell. */
  Tcgetattr(tty_fd, &shell_tmodes);
}

/* Called just before the shell finishes. */
void shutdownjobs(void) {
  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Kill remaining jobs and wait for them to finish. */
  for (int i = 1; i < njobmax; i++)
    if (killjob(i))
      while (jobs[i].state != FINISHED)
        sigsuspend(&mask);

  watchjobs(FINISHED);
  Sigprocmask(SIG_SETMASK, &mask, NULL);

  Close(tty_fd);
}
