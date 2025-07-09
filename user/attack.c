#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define SECRET_OFFSET 32

int
main(int argc, char *argv [])
{

// your code here. you should write the secret to fd 2 using write
// (e.g., write(2, secret, 8)
char *end = sbrk(17*PGSIZE);
end += 16 * PGSIZE;
write(2,end+32,8);
exit(1);
}