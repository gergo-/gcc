/* Test OpenACC's support for manual deep copy, including the attach
   and detach clauses.  */

/* { dg-additional-options "-fdump-tree-omplower" } */

void
t1 ()
{
  struct foo {
    int *a, *b, c, d, *e;
  } s;

  int *a, *z;

#pragma acc enter data copyin(s)
  {
#pragma acc data copy(s.a[0:10]) copy(z[0:10])
    {
      s.e = z;
#pragma acc parallel loop attach(s.e)
      for (int i = 0; i < 10; i++)
        s.a[i] = s.e[i];


      a = s.e;
#pragma acc enter data attach(a)
#pragma acc exit data detach(a)
    }

#pragma acc enter data copyin(a)
#pragma acc acc enter data attach(s.e)
#pragma acc exit data detach(s.e)

#pragma acc data attach(s.e)
    {
    }
#pragma acc exit data delete(a)

#pragma acc exit data detach(a) finalize
#pragma acc exit data detach(s.a) finalize
  }
}

/* { dg-final { scan-tree-dump-times "pragma omp target oacc_enter_exit_data map.to:s .len: 32.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_data map.tofrom:.z .len: 40.. map.struct:s .len: 1.. map.alloc:s.a .len: 8.. map.tofrom:._1 .len: 40.. map.attach:s.a .len: 0.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_parallel map.force_present:s .len: 32.. map.attach:s.e .len: 8.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_enter_exit_data map.attach:a .len: 8.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_enter_exit_data map.detach:a .len: 8.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_enter_exit_data map.to:a .len: 8.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_enter_exit_data map.force_present:s .len: 32.. map.detach:s.e .len: 8.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_data map.force_present:s .len: 32.. map.attach:s.e .len: 8.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_enter_exit_data map.release:a .len: 8.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_enter_exit_data finalize map.force_detach:a .len: 8.." 1 "omplower" } } */
/* { dg-final { scan-tree-dump-times "pragma omp target oacc_enter_exit_data finalize map.force_present:s .len: 32.. map.force_detach:s.a .len: 8.." 1 "omplower" } } */
