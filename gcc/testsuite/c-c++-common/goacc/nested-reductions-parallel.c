/* Test cases of nested reduction loops in parallel regions and routines
   that should compile cleanly.  See also corresponding tests in
   nested-reductions-kernels.c and nested-reductions-serial.c.  */

void acc_parallel (void)
{
  int i, j, k, sum, diff;

  #pragma acc parallel
  {
    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop collapse(2) reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop reduction(+:sum)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop collapse(2) reduction(+:sum)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop reduction(+:sum)
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum) reduction(-:diff)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum)
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop reduction(-:diff)
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }
  }
}

/* The same tests as above, but using a combined parallel loop construct.  */

void acc_parallel_loop (void)
{
  int i, j, k, l, sum, diff;

  #pragma acc parallel loop
  for (int h = 0; h < 10; ++h)
  {
    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop collapse(2) reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop reduction(+:sum)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop collapse(2) reduction(+:sum)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop reduction(+:sum) // { dg-warning "insufficient partitioning available to parallelize loop" }
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum) reduction(-:diff)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum) // { dg-warning "insufficient partitioning available to parallelize loop" }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop reduction(-:diff) // { dg-warning "insufficient partitioning available to parallelize loop" }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }
  }
}

/* The same tests as above, but now the outermost reduction clause is on
   the parallel region, not the outermost loop.  */

void acc_parallel_reduction (void)
{
  int i, j, k, sum, diff;

  #pragma acc parallel reduction(+:sum)
  {
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    for (i = 0; i < 10; i++)
      #pragma acc loop
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        #pragma acc loop
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop reduction(+:sum)
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum) reduction(-:diff)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum)
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop reduction(-:diff)
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum)
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop reduction(-:diff)
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum)
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }
  }
}

/* The same tests as above, but using a combined parallel loop construct, and
   the outermost reduction clause is on that one, not the outermost loop.  */
void acc_parallel_loop_reduction (void)
{
  int i, j, k, sum, diff;

  #pragma acc parallel loop reduction(+:sum)
  for (int h = 0; h < 10; ++h)
  {
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    for (i = 0; i < 10; i++)
      #pragma acc loop
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        #pragma acc loop
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop reduction(+:sum) // { dg-warning "insufficient partitioning available to parallelize loop" }
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum) reduction(-:diff)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum) // { dg-warning "insufficient partitioning available to parallelize loop" }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop reduction(-:diff) // { dg-warning "insufficient partitioning available to parallelize loop" }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum) // { dg-warning "insufficient partitioning available to parallelize loop" }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop reduction(-:diff) // { dg-warning "insufficient partitioning available to parallelize loop" }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum) // { dg-warning "insufficient partitioning available to parallelize loop" }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop // { dg-warning "insufficient partitioning available to parallelize loop" }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }
  }
}

/* The same tests as above, but inside a routine construct.  */
#pragma acc routine gang
void acc_routine (void) // { dg-bogus "region is gang partitioned but does not contain gang partitioned code" "TODO" { xfail *-*-* } }
{
  int i, j, k, sum, diff;

  {
    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop collapse(2) reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop reduction(+:sum)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop collapse(2) reduction(+:sum)
      for (j = 0; j < 10; j++)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum)
    for (i = 0; i < 10; i++)
      #pragma acc loop reduction(+:sum) // { dg-bogus "insufficient partitioning available to parallelize loop" "TODO" { xfail *-*-* } }
      for (j = 0; j < 10; j++)
        #pragma acc loop reduction(+:sum)
        for (k = 0; k < 10; k++)
          sum = 1;

    #pragma acc loop reduction(+:sum) reduction(-:diff)
    for (i = 0; i < 10; i++)
      {
        #pragma acc loop reduction(+:sum) // { dg-bogus "insufficient partitioning available to parallelize loop" "TODO" { xfail *-*-* } }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(+:sum)
          for (k = 0; k < 10; k++)
            sum = 1;

        #pragma acc loop reduction(-:diff) // { dg-bogus "insufficient partitioning available to parallelize loop" "TODO" { xfail *-*-* } }
        for (j = 0; j < 10; j++)
          #pragma acc loop reduction(-:diff)
          for (k = 0; k < 10; k++)
            diff = 1;
      }
  }
}
