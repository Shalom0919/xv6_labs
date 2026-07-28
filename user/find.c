#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"

static char *
basename(char *path)
{
  char *name = path + strlen(path);
  while(name > path && name[-1] != '/')
    name--;
  return name;
}

static void
find(char *path, char *target)
{
  int fd;
  struct stat st;

  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }
  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  if(st.type == T_FILE){
    if(strcmp(basename(path), target) == 0)
      printf("%s\n", path);
    close(fd);
    return;
  }
  if(st.type != T_DIR){
    close(fd);
    return;
  }

  char child[512];
  int length = strlen(path);
  if(length + 1 + DIRSIZ + 1 > sizeof(child)){
    fprintf(2, "find: path too long\n");
    close(fd);
    return;
  }
  strcpy(child, path);
  char *name = child + length;
  *name++ = '/';

  struct dirent de;
  while(read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    memmove(name, de.name, DIRSIZ);
    name[DIRSIZ] = 0;
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    find(child, target);
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    fprintf(2, "usage: find path name\n");
    exit(1);
  }
  find(argv[1], argv[2]);
  exit(0);
}
