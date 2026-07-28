#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

static void
run_line(char *line, int length, int base_argc, char *base_argv[])
{
  char *argv[MAXARG];
  int argc = 0;

  for(; argc < base_argc && argc < MAXARG - 1; argc++)
    argv[argc] = base_argv[argc];

  int i = 0;
  while(i < length){
    while(i < length && (line[i] == ' ' || line[i] == '\t'))
      i++;
    if(i == length)
      break;
    if(argc == MAXARG - 1){
      fprintf(2, "xargs: too many arguments\n");
      exit(1);
    }
    argv[argc++] = &line[i];
    while(i < length && line[i] != ' ' && line[i] != '\t')
      i++;
    line[i++] = 0;
  }
  argv[argc] = 0;

  if(argc == base_argc)
    return;

  int pid = fork();
  if(pid < 0){
    fprintf(2, "xargs: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    exec(argv[0], argv);
    fprintf(2, "xargs: exec %s failed\n", argv[0]);
    exit(1);
  }
  wait(0);
}

int
main(int argc, char *argv[])
{
  if(argc < 2){
    fprintf(2, "usage: xargs command [arguments ...]\n");
    exit(1);
  }

  char line[512];
  int length = 0;
  char ch;
  while(read(0, &ch, 1) == 1){
    if(ch == '\n'){
      run_line(line, length, argc - 1, argv + 1);
      length = 0;
    } else if(length < sizeof(line) - 1){
      line[length++] = ch;
    } else {
      fprintf(2, "xargs: input line too long\n");
      exit(1);
    }
  }
  if(length > 0)
    run_line(line, length, argc - 1, argv + 1);
  exit(0);
}
