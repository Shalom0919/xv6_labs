#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int parent_to_child[2];
  int child_to_parent[2];
  char byte = 'x';

  if(argc != 1){
    fprintf(2, "usage: pingpong\n");
    exit(1);
  }
  if(pipe(parent_to_child) < 0 || pipe(child_to_parent) < 0){
    fprintf(2, "pingpong: pipe failed\n");
    exit(1);
  }

  int pid = fork();
  if(pid < 0){
    fprintf(2, "pingpong: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    close(parent_to_child[1]);
    close(child_to_parent[0]);
    if(read(parent_to_child[0], &byte, 1) != 1)
      exit(1);
    printf("%d: received ping\n", getpid());
    if(write(child_to_parent[1], &byte, 1) != 1)
      exit(1);
    close(parent_to_child[0]);
    close(child_to_parent[1]);
    exit(0);
  }

  close(parent_to_child[0]);
  close(child_to_parent[1]);
  if(write(parent_to_child[1], &byte, 1) != 1)
    exit(1);
  if(read(child_to_parent[0], &byte, 1) != 1)
    exit(1);
  printf("%d: received pong\n", getpid());
  close(parent_to_child[1]);
  close(child_to_parent[0]);
  wait(0);
  exit(0);
}
