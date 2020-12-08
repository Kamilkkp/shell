#include <readline/readline.h>
#include <readline/history.h>

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static sigjmp_buf loop_env;

static void sigint_handler(int sig) {
  siglongjmp(loop_env, sig);
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp) {
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++) {
    /* TODO: Handle tokens and open files as requested. */
    if (token[i] == T_INPUT) {
      if (!mode)
        n = i;
      mode = token[i];
      MaybeClose(inputp);
      *inputp = Open(token[i + 1], O_RDONLY, 0);
    } else if (token[i] == T_OUTPUT) {
      if (!mode)
        n = i;
      mode = token[i];
      MaybeClose(outputp);
      *outputp = Open(token[i + 1], O_WRONLY | O_CREAT,
                      S_IWUSR | S_IRUSR | S_IRGRP | S_IWGRP | S_IROTH);
    } else if (!mode)
      n++;
    else
      token[i] = T_NULL;
  }

  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if (!bg) {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */
  pid_t pid = fork();
  if (pid) {
    setpgid(pid, pid);
    MaybeClose(&input);
    MaybeClose(&output);
    int job = addjob(pid, bg);
    addproc(job, pid, token);
    if (!bg) {
      exitcode = monitorjob(&mask);
    } else {
      msg("[%d] running '%s'\n", job, jobcmd(job));
    }
  } else {
    Signal(SIGINT, SIG_DFL);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    Sigprocmask(SIG_SETMASK, &mask, NULL);
    setpgrp();
    if (input)
      dup2(input, STDIN_FILENO);
    if (output)
      dup2(output, STDOUT_FILENO);

    MaybeClose(&input);
    MaybeClose(&output);
    if ((exitcode = builtin_command(token)) >= 0)
      exit(exitcode);
    external_command(token);
  }

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens) {
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

  /* TODO: Start a subprocess and make sure it's moved to a process group. */
  pid_t pid = Fork();
  if (!pid) {
    if (!pgid)
      Setpgid(getpid(), getpid());
    else
      Setpgid(getpid(), pgid);
    Signal(SIGINT, SIG_DFL);
    Signal(SIGTSTP, SIG_DFL);
    Signal(SIGTTIN, SIG_DFL);
    Signal(SIGTTOU, SIG_DFL);
    Sigprocmask(SIG_SETMASK, mask, NULL);
    if (input)
      dup2(input, STDIN_FILENO);
    if (output)
      dup2(output, STDOUT_FILENO);
    MaybeClose(&input);
    MaybeClose(&output);
    int exitcode;
    if ((exitcode = builtin_command(token)) >= 0)
      exit(exitcode);
    external_command(token);
  }
  Setpgid(pid, pid);
  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */
  size_t length = 0;
  for (size_t i = 0; i < ntokens; i++) {
    if (token[i] == T_PIPE) {
      length = i;
      break;
    }
  }
  pid = do_stage(pgid, &mask, input, output, token, length);
  MaybeClose(&output);
  pgid = pid;
  job = addjob(pgid, bg);
  addproc(job, pid, token);
  token = &token[length + 1];
  ntokens -= (length + 1);
  input = next_input;
  while (true) {
    for (size_t i = 0; i < ntokens; i++) {
      if (token[i] == T_PIPE) {
        length = i;
        mkpipe(&next_input, &output);
        break;
      } else
        length = ntokens;
    }
    pid = do_stage(pgid, &mask, input, output, token, length);
    MaybeClose(&input);
    MaybeClose(&output);
    addproc(job, pid, token);
    input = next_input;
    ntokens -= (length + 1);
    if (ntokens > 0)
      token = &token[length + 1];
    else
      break;
  }

  if (!bg) {
    exitcode = monitorjob(&mask);
  } else {
    msg("[%d] running '%s'\n", job, jobcmd(job));
  }

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

int main(int argc, char *argv[]) {
  rl_initialize();

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  if (getsid(0) != getpgid(0))
    Setpgid(0, 0);

  initjobs();

  Signal(SIGINT, sigint_handler);
  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  char *line;
  while (true) {
    if (!sigsetjmp(loop_env, 1)) {
      line = readline("# ");
    } else {
      msg("\n");
      continue;
    }

    if (line == NULL)
      break;

    if (strlen(line)) {
      add_history(line);
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
