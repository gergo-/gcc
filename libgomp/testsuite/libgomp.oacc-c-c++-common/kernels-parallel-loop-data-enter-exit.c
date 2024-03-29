/* FIXME: OpenACC kernels stopped working with the firstprivate subarray
   changes.  */
/* { dg-prune-output "OpenACC kernels construct will be executed sequentially" } */

#include <stdlib.h>

#define N (1024 * 512)
#define COUNTERTYPE unsigned int

int
main (void)
{
  unsigned int *__restrict a;
  unsigned int *__restrict b;
  unsigned int *__restrict c;

  a = (unsigned int *__restrict)malloc (N * sizeof (unsigned int));
  b = (unsigned int *__restrict)malloc (N * sizeof (unsigned int));
  c = (unsigned int *__restrict)malloc (N * sizeof (unsigned int));

#pragma acc enter data create (a[0:N], b[0:N], c[0:N])

#pragma acc kernels present (a[0:N])
  {
    for (COUNTERTYPE i = 0; i < N; i++)
      a[i] = i * 2;
  }

#pragma acc parallel present (b[0:N])
  {
#pragma acc loop
    for (COUNTERTYPE i = 0; i < N; i++)
      b[i] = i * 4;
  }

#pragma acc kernels present (a[0:N], b[0:N], c[0:N])
  {
    for (COUNTERTYPE ii = 0; ii < N; ii++)
      c[ii] = a[ii] + b[ii];
  }

#pragma acc exit data copyout (a[0:N], b[0:N], c[0:N])

  for (COUNTERTYPE i = 0; i < N; i++)
    if (c[i] != a[i] + b[i])
      abort ();

  free (a);
  free (b);
  free (c);

  return 0;
}
