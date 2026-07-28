#include "kernel/types.h"
#include "user/user.h"

static void
sieve(int input)
{
  int prime;
  if(read(input, &prime, sizeof(prime)) != sizeof(prime)){
    close(input);
    exit(0);
  }
  printf("prime %d\n", prime);

  int output[2];
  if(pipe(output) < 0){
    fprintf(2, "primes: pipe failed\n");
    exit(1);
  }

  int pid = fork();
  if(pid < 0){
    fprintf(2, "primes: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    close(input);
    close(output[1]);
    sieve(output[0]);
  }

  close(output[0]);
  int value;
  while(read(input, &value, sizeof(value)) == sizeof(value)){
    if(value % prime != 0)
      write(output[1], &value, sizeof(value));
  }
  close(input);
  close(output[1]);
  wait(0);
  exit(0);
}

int
main(int argc, char *argv[])
{
  if(argc != 1){
    fprintf(2, "usage: primes\n");
    exit(1);
  }

  int numbers[2];
  if(pipe(numbers) < 0){
    fprintf(2, "primes: pipe failed\n");
    exit(1);
  }

  int pid = fork();
  if(pid < 0){
    fprintf(2, "primes: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    close(numbers[1]);
    sieve(numbers[0]);
  }

  close(numbers[0]);
  for(int value = 2; value <= 35; value++)
    write(numbers[1], &value, sizeof(value));
  close(numbers[1]);
  wait(0);
  exit(0);
}
