/* Public domain. */

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <grp.h>

int main()
{
  gid_t *x;
  long ngroups_max;
  
  ngroups_max = sysconf(_SC_NGROUPS_MAX) + 1;
  x = (gid_t *)malloc(ngroups_max *sizeof(gid_t));
  
  x[0] = x[1] = 1;
  if (getgroups(ngroups_max,x) == 0) if (setgroups(ngroups_max,x) == -1) _exit(1);
 
  if (getgroups(ngroups_max,x) == -1) _exit(1);
  if (x[1] != 1) _exit(1);
  x[1] = 2;
  if (getgroups(ngroups_max,x) == -1) _exit(1);
  if (x[1] != 2) _exit(1);
  _exit(0);
}
