#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include "sig.h"
#include "strerr.h"
#include "error.h"
#include "fifo.h"
#include "fmt.h"
#include "open.h"
#include "lock.h"
#include "seek.h"
#include "wait.h"
#include "closeonexec.h"
#include "ndelay.h"
#include "env.h"
#include "iopause.h"
#include "taia.h"
#include "deepsleep.h"
#include "subreaper.h"
#include "stralloc.h"
#include "svpath.h"
#include "svstatus.h"

#define FATAL "supervise: fatal: "
#define WARNING "supervise: warning: "

struct svc
{
  int pid;
  enum svstatus flagstatus;
  int flagwant;
  int flagwantup;
  int flagpaused;
  struct taia when;
  int ranstop;
};

const char *dir;
int selfpipe[2];
int fdlock;
int fdcontrolwrite;
int fdcontrol;
int fdok;
int fdstatus;

int flagexit = 0;
int flagorphanage = 0;
int firstrun = 1;
const char *runscript = 0;

int logpipe[2] = {-1,-1};
struct svc svcmain = {0,svstatus_stopped,1,1};
struct svc svclog = {0,svstatus_stopped,1,0};

static int stat_isexec(const char *path)
{
  struct stat st;
  if (stat(path,&st) == -1)
    return errno == error_noent ? 0 : -1;
  return S_ISREG(st.st_mode) && (st.st_mode & 0100);
}

static int stat_exists(const char *path)
{
  struct stat st;
  return stat(path,&st) == 0 ? 1 :
    errno == error_noent ? 0 :
    -1;
}

static void die_nomem(void)
{
  strerr_die2sys(111,FATAL,"unable to allocate memory");
}

static void trigger(void)
{
  int ignored;
  ignored = write(selfpipe[1],"",1);
  (void)ignored;
}

static void terminate(void)
{
  int ignored;
  ignored = write(fdcontrolwrite,"dx",2);
  ignored = write(selfpipe[1],"",1);
  (void)ignored;
}

static void ttystop(void)
{
  int ignored;
  ignored = write(fdcontrolwrite,"p",1);
  ignored = write(selfpipe[1],"",1);
  (void)ignored;
}

static void resume(void)
{
  int ignored;
  ignored = write(fdcontrolwrite,"c",1);
  ignored = write(selfpipe[1],"",1);
  (void)ignored;
}

static int forkexecve(struct svc *svc,const char *argv[],int fd)
{
  int f;

  switch (f = fork()) {
    case -1:
      strerr_warn4sys(WARNING,"unable to fork for ",dir,", sleeping 60 seconds");
      deepsleep(60);
      trigger();
      return -1;
    case 0:
      sig_uncatch(sig_child);
      sig_unblock(sig_child);
      sig_uncatch(sig_int);
      sig_uncatch(sig_term);
      sig_uncatch(sig_ttystop);
      sig_uncatch(sig_cont);
      if (stat_exists("no-setsid") == 0)
	setsid();	       /* shouldn't fail; if it does, too bad */
      if (fd >= 0 && logpipe[0] >= 0) {
	dup2(logpipe[fd],fd);
	close(logpipe[0]);
	close(logpipe[1]);
      }
      execve(argv[0],(char*const*)argv,environ);
      strerr_die4sys(111,FATAL,"unable to start ",dir,argv[0]+1);
  }
  if (svc) {
    svc->pid = f;
    svc->flagpaused = 0;
  }
  return f;
}

static void make_status(const struct svc *svc,char status[20])
{
  unsigned long u;

  taia_pack(status,&svc->when);

  u = (unsigned long) svc->pid;
  status[12] = u; u >>= 8;
  status[13] = u; u >>= 8;
  status[14] = u; u >>= 8;
  status[15] = u;

  status[16] = (svc->pid ? svc->flagpaused : 0);
  status[17] = (svc->flagwant ? (svc->flagwantup ? 'u' : 'd') : 0);
  status[18] = svc->flagstatus;
  status[19] = 0;
}

void announce(void)
{
  int r;
  int w;
  char status[40];

  make_status(&svcmain,status);
  if (logpipe[0] < 0)
    w = 20;
  else {
    make_status(&svclog,status + 20);
    w = 40;
  }

  if (seek_begin(fdstatus)) {
    strerr_warn2sys(WARNING,"unable to seek in status");
    return;
  }
  r = write(fdstatus,status,w);
  if (r < w) {
    strerr_warn2(WARNING,"unable to write status: partial write",0);
    return;
  }
}

static void notify(const struct svc *svc,const char *notice,int code,int oldpid)
{
  char pidnum[FMT_ULONG];
  char codenum[FMT_ULONG];
  const char *argv[] = {
    "./notify",
    svc == &svclog ? "log" : runscript+2,
    notice,pidnum,codenum,0
  };

  pidnum[fmt_uint(pidnum,oldpid)] = 0;
  codenum[fmt_uint(codenum,code)] = 0;
  forkexecve(0,argv,-1);
}

void pidchange(struct svc *svc,const char *notice,int code,int oldpid)
{
  taia_now(&svc->when);

  if (notice != 0 && stat_isexec("notify") > 0)
    notify(svc,notice,code,oldpid);
  announce();
}

void trystart(struct svc *svc)
{
  const char *argv[] = { 0,0 };
  int f;
  int fd;

  if (svc == &svclog) {
    argv[0] = "./log";
    svclog.flagstatus = svstatus_running;
    fd = 0;
  }
  else {
    if (firstrun && stat_isexec("start") == 0)
      firstrun = 0;
    if (!firstrun && stat_exists("run") == 0) {
      svc->flagwant = 0;
      svc->flagstatus = svstatus_started;
      announce();
      return;
    }
    argv[0] = runscript = firstrun ? "./start" : "./run";
    svcmain.flagstatus = firstrun ? svstatus_starting : svstatus_running;
    fd = 1;
  }
  if ((f = forkexecve(svc,argv,fd)) < 0)
    return;
  pidchange(svc,"start",0,f);
  deepsleep(1);
}

void trystop(struct svc *svc)
{
  const char *argv[] = { "./stop",0 };
  int f;

  if (svc->ranstop
      || svc != &svcmain
      || stat_isexec("stop") != 1) {
    svc->ranstop = 1;
    svc->flagstatus = svstatus_stopped;
    announce();
    return;
  }
  runscript = argv[0];
  if ((f = forkexecve(svc,argv,1)) < 0)
    return;
  svc->ranstop = 1;
  pidchange(svc,"start",0,f);
  deepsleep(1);
}

static void killsvc(const struct svc *svc,int groupflag,int signo)
{
  if (svc->pid && (svc->flagstatus != svstatus_orphanage || groupflag < 0))
    kill(svc->pid*groupflag,signo);
}

static void stopsvc(struct svc *svc,int groupflag)
{
  killsvc(svc,groupflag,SIGTERM);
  killsvc(svc,groupflag,SIGCONT);
  svc->flagpaused = 0;
  svc->flagstatus = svstatus_stopping;
  svc->ranstop = 0;
}

static void reaper(void)
{
  for (;;) {
    int wstat;
    int pid = 0;
    struct svc *svc;

    if (svcmain.flagstatus == svstatus_orphanage) {
      pid = waitpid(-svcmain.pid, &wstat, WNOHANG);
      if (pid > 0) continue;
    }

    if (!pid)
      pid = wait_nohang(&wstat);
    if (!pid) break;
    if (flagorphanage && pid == svcmain.pid) {
      svcmain.flagstatus = svstatus_orphanage;
      announce();
      continue;
    }
    if ((pid == svcmain.pid) || ((pid == -1) && (errno == ECHILD)))
      svc = &svcmain;
    else if (pid == svclog.pid)
      svc = &svclog;
    else if ((pid == -1) && (errno != error_intr))
      break;
    else
      continue;
    svc->pid = 0;
    if ((svc == &svcmain && svc->flagstatus == svstatus_starting && (wait_crashed(wstat) || wait_exitcode(wstat) != 0))
        || (!wait_crashed(wstat) && wait_exitcode(wstat) == 100)) {
      svc->flagwantup = 0;
      svc->flagstatus = svstatus_failed;
    }
    else if (svc == &svcmain && svc->flagstatus == svstatus_starting) {
    }
    else if (!svc->flagwant || !svc->flagwantup)
      svc->flagstatus = svstatus_stopped;
    pidchange(svc, wait_crashed(wstat) ? "killed" : "exit",
              wait_crashed(wstat) ? wait_stopsig(wstat) : wait_exitcode(wstat),
              pid);
    firstrun = 0;
    if ((svc->flagwant && svc->flagwantup) || (svc == &svcmain && svc->flagstatus == svstatus_starting)) {
      if (!flagexit)
        trystart(svc);
    }
    else if (svc->flagstatus != svstatus_failed)
      trystop(svc);
    break;
  }
}

static void controller(void)
{
  struct svc *svc = &svcmain;
  int killgroup = 1;
  char ch;

  while (read(fdcontrol,&ch,1) == 1)
    switch(ch) {
    case '+':
      killgroup = -1;
      break;
    case '=':
      killgroup = 1;
      break;
    case 'L':
      svc = &svclog;
      break;
    case 'l':
      svc = &svcmain;
      break;
    case 'd':
      svc->flagwant = 1;
      svc->flagwantup = 0;
      if (svc->pid)
        stopsvc(svc,killgroup);
      else
        trystop(svc);
      announce();
      break;
    case 'u':
      if (svc == &svcmain)
        firstrun = !svcmain.flagwantup;
      svc->flagwant = 1;
      svc->flagwantup = 1;
      if (!svc->pid)
        svc->flagstatus = svstatus_starting;
      announce();
      if (!svc->pid) trystart(svc);
      break;
    case 'o':
      svc->flagwant = 0;
      announce();
      if (!svc->pid) trystart(svc);
      break;
    case 'a': killsvc(svc,killgroup,SIGALRM); break;
    case 'h': killsvc(svc,killgroup,SIGHUP); break;
    case 'k': killsvc(svc,killgroup,SIGKILL); break;
    case 't': killsvc(svc,killgroup,SIGTERM); break;
    case 'i': killsvc(svc,killgroup,SIGINT); break;
    case 'q': killsvc(svc,killgroup,SIGQUIT); break;
    case '1': killsvc(svc,killgroup,SIGUSR1); break;
    case '2': killsvc(svc,killgroup,SIGUSR2); break;
    case 'w': killsvc(svc,killgroup,SIGWINCH); break;
    case 'p':
      svc->flagpaused = 1;
      announce();
      killsvc(svc,killgroup,SIGSTOP);
      break;
    case 'c':
      svc->flagpaused = 0;
      announce();
      killsvc(svc,killgroup,SIGCONT);
      break;
    case 'x':
      flagexit = 1;
      announce();
      break;
    }
}

void doit(void)
{
  iopause_fd x[2];
  struct taia deadline;
  struct taia stamp;
  struct stat st;
  int r;
  char ch;

  announce();

  for (;;) {
    if (flagexit && !svcmain.pid && !svclog.pid) return;

    sig_unblock(sig_child);

    x[0].fd = selfpipe[0];
    x[0].events = IOPAUSE_READ;
    x[1].fd = fdcontrol;
    x[1].events = IOPAUSE_READ;
    taia_now(&stamp);
    taia_uint(&deadline,10);
    taia_add(&deadline,&stamp,&deadline);
    iopause(x,2,&deadline,&stamp);

    sig_block(sig_child);

    while (read(selfpipe[0],&ch,1) == 1)
      ;

    reaper();

    r = fstat(fdcontrol,&st);
    if (r == 0 && st.st_nlink == 0) {
        if (svcmain.pid)
	    kill(-svcmain.pid,SIGKILL);
        if (svclog.pid)
	    kill(-svclog.pid,SIGKILL);
        flagexit = 1;
    }

    controller();

    if (flagexit
	&& svcmain.flagstatus == svstatus_stopped
	&& (svclog.flagstatus == svstatus_running || svclog.flagstatus == svstatus_started))
      stopsvc(&svclog,1);
  }
}

int main(int argc,char **argv)
{
  struct stat st;
  const char *fntemp;

  dir = argv[1];
  if (!dir || argv[2])
    strerr_die1x(100,"supervise: usage: supervise dir");

  if (pipe(selfpipe) == -1)
    strerr_die3sys(111,FATAL,"unable to create pipe for ",dir);
  closeonexec(selfpipe[0]);
  closeonexec(selfpipe[1]);
  ndelay_on(selfpipe[0]);
  ndelay_on(selfpipe[1]);

  sig_block(sig_child);
  sig_catch(sig_child,trigger);
  sig_catch(sig_term,terminate);
  sig_catch(sig_int,terminate);
  sig_catch(sig_ttystop,ttystop);
  sig_catch(sig_cont,resume);

  if (chdir(dir) == -1)
    strerr_die3sys(111,FATAL,"unable to chdir to ",dir);
  if (!svpath_init())
    strerr_die3sys(111,FATAL,"unable to setup control path for ",dir);

  if ((fntemp = svpath_make("")) == 0) die_nomem();
  if (mkdir(fntemp,0700) != 0 && errno != error_exist)
    strerr_die3sys(111,FATAL,"unable to create ",fntemp);

  if ((fntemp = svpath_make("/lock")) == 0) die_nomem();
  fdlock = open_append(fntemp);
  if ((fdlock == -1) || (lock_exnb(fdlock) == -1))
    strerr_die3sys(111,FATAL,"unable to acquire ",fntemp);
  closeonexec(fdlock);

  if (stat_exists("orphanage") != 0) {
    flagorphanage = 1;
    if (set_subreaper())
      strerr_die2sys(111,FATAL,"could not set subreaper attribute");
  }
  if (stat_isexec("log") > 0) {
    if (pipe(logpipe) != 0)
      strerr_die3sys(111,FATAL,"unable to create pipe for ",dir);
    else if (flagorphanage)
      strerr_die2sys(111,FATAL,"orphanage and log are mutually exclusive");
    svclog.flagwantup = 1;
  }
  if (stat("down",&st) != -1) {
    svcmain.flagwantup = 0;
    svclog.flagwantup = 0;
  }
  else
    if (errno != error_noent)
      strerr_die4sys(111,FATAL,"unable to stat ",dir,"/down");

  if ((fntemp = svpath_make("/status")) == 0) die_nomem();
  fdstatus = open_trunc(fntemp);
  if (fdstatus == -1)
    strerr_die4sys(111,FATAL,"unable to open ",fntemp," for writing");
  closeonexec(fdstatus);

  if ((fntemp = svpath_make("/control")) == 0) die_nomem();
  fifo_make(fntemp,0600);
  fdcontrol = open_read(fntemp);
  if (fdcontrol == -1)
    strerr_die3sys(111,FATAL,"unable to read ",fntemp);
  closeonexec(fdcontrol);
  ndelay_on(fdcontrol); /* shouldn't be necessary */
  fdcontrolwrite = open_write(fntemp);
  if (fdcontrolwrite == -1)
    strerr_die3sys(111,FATAL,"unable to write ",fntemp);
  closeonexec(fdcontrolwrite);

  taia_now(&svclog.when);
  pidchange(&svcmain,0,0,0);

  if ((fntemp = svpath_make("/ok")) == 0) die_nomem();
  fifo_make(fntemp,0600);
  fdok = open_read(fntemp);
  if (fdok == -1)
    strerr_die3sys(111,FATAL,"unable to read ",fntemp);
  closeonexec(fdok);

  if (!svclog.flagwant || svclog.flagwantup) trystart(&svclog);
  if (!svcmain.flagwant || svcmain.flagwantup) trystart(&svcmain);

  doit();
  announce();
  _exit(0);
}
