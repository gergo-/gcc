/* { dg-additional-options "-O2" } */
/* { dg-additional-options "-fdump-tree-convert_oacc_kernels" } */
/* { dg-prune-output "will by default avoid offloading" } */

#define N 1024

unsigned int a[N];

int
main (void)
{
  int i;
  unsigned int sum = 1;

#pragma acc kernels copyin(a[0:N]) copy(sum)
  {
    /* converted to "oacc_kernels" */
    #pragma acc loop
    for (i = 0; i < N; ++i)
      sum += a[i];

    /* converted to "oacc_parallel_kernels_gang_single" */
    sum++;
    a[0]++;

    /* converted to "oacc_parallel_kernels_parallelized" */
    #pragma acc loop independent
    for (i = 0; i < N; ++i)
      sum += a[i];

    /* converted to "oacc_kernels" */
    if (sum > 10)
      { 
        #pragma acc loop
        for (i = 0; i < N; ++i)
          sum += a[i];
      }

    /* converted to "oacc_kernels" */
    #pragma acc loop auto
    for (i = 0; i < N; ++i)
      sum += a[i];
  }

  return 0;
}

/* Check that the kernels region is split into a data region and enclosed
   parallel regions.  */ 
/* { dg-final { scan-tree-dump-times "oacc_kernels_data" 1 "convert_oacc_kernels" } } */

/* As noted in the comments above, we get one gang-single serial region; one
   parallelized loop region; and three "old-style" kernel regions. */
/* { dg-final { scan-tree-dump-times "oacc_parallel_kernels_gang_single" 1 "convert_oacc_kernels" } } */
/* { dg-final { scan-tree-dump-times "oacc_parallel_kernels_parallelized" 1 "convert_oacc_kernels" } } */
/* { dg-final { scan-tree-dump-times "oacc_kernels " 3 "convert_oacc_kernels" } } */

/* Each of the parallel regions is async, and there is a final call to
   __builtin_GOACC_wait.  */
/* { dg-final { scan-tree-dump-times "oacc_.* async\\(-1\\)" 5 "convert_oacc_kernels" } } */
/* { dg-final { scan-tree-dump-times "__builtin_GOACC_wait" 1 "convert_oacc_kernels" } } */
