! Ensure that a non-scalar dummy arguments which are implicitly used inside
! offloaded regions are properly mapped using present_or_copy, or (default)
! present.

! { dg-do run }

program main
  implicit none

  integer,parameter :: size = 100
  integer :: array(size), i, n

  n = size

  !$acc data copy(array)

  call kernels(array, n)

  !$acc update host(array)

  do i = 1, n
     if (array(i) .ne. i) STOP 1
  end do

  call kernels_default_present(array, n)

  !$acc update host(array)

  do i = 1, n
     if (array(i) .ne. i+1) STOP 2
  end do

  call parallel(array, n)

  !$acc update host(array)

  do i = 1, n
     if (array(i) .ne. i+i) STOP 3
  end do

  call parallel_default_present(array, n)

  !$acc end data

  do i = 1, n
     if (array(i) .ne. i+i+1) STOP 4
  end do
end program main

subroutine kernels (array, n)
  integer, dimension (n) :: array
  integer :: n, i

  !$acc kernels ! { dg-bogus "OpenACC kernels construct will be executed sequentially; will by default avoid offloading to prevent data copy penalty" "PR80995" { xfail { openacc_nvidia_accel_selected && opt_levels_size } } }
  do i = 1, n
     array(i) = i
  end do
  !$acc end kernels
end subroutine kernels

subroutine kernels_default_present (array, n)
  integer, dimension (n) :: array
  integer :: n, i

  !$acc kernels default(present) ! { dg-bogus "OpenACC kernels construct will be executed sequentially; will by default avoid offloading to prevent data copy penalty" "PR80995" { xfail { openacc_nvidia_accel_selected && opt_levels_size } } }
  do i = 1, n
     array(i) = i+1
  end do
  !$acc end kernels
end subroutine kernels_default_present

subroutine parallel (array, n)
  integer, dimension (n) :: array
  integer :: n, i

  !$acc parallel
  do i = 1, n
     array(i) = i+i
  end do
  !$acc end parallel
end subroutine parallel

subroutine parallel_default_present (array, n)
  integer, dimension (n) :: array
  integer :: n, i

  !$acc parallel default(present)
  do i = 1, n
     array(i) = i+i+1
  end do
  !$acc end parallel
end subroutine parallel_default_present
