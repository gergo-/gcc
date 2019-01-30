! { dg-do run }
! { dg-additional-options "-fopenacc-kernels=parloops" } as this is
! specifically testing "parloops" handling.

program main
  implicit none
  integer, parameter         :: n = 1024
  integer, dimension (0:n-1) :: a, b, c
  integer                    :: i, ii

  !$acc data copyout (a(0:n-1), b(0:n-1), c(0:n-1))

  !$acc kernels present (a(0:n-1))
  do i = 0, n - 1 ! { dg-bogus "OpenACC kernels construct will be executed sequentially; will by default avoid offloading to prevent data copy penalty" "PR80995" { xfail { openacc_nvidia_accel_selected && opt_levels_size } } }
     a(i) = i * 2
  end do
  !$acc end kernels

  !$acc kernels present (b(0:n-1))
  do i = 0, n -1 ! { dg-bogus "OpenACC kernels construct will be executed sequentially; will by default avoid offloading to prevent data copy penalty" "PR80995" { xfail { openacc_nvidia_accel_selected && opt_levels_size } } }
     b(i) = i * 4
  end do
  !$acc end kernels

  !$acc kernels present (a(0:n-1), b(0:n-1), c(0:n-1))
  do ii = 0, n - 1 ! { dg-bogus "OpenACC kernels construct will be executed sequentially; will by default avoid offloading to prevent data copy penalty" "PR80995" { xfail { openacc_nvidia_accel_selected && opt_levels_size } } }
     c(ii) = a(ii) + b(ii)
  end do
  !$acc end kernels

  !$acc end data

  do i = 0, n - 1
     if (c(i) .ne. a(i) + b(i)) STOP 1
  end do

end program main
