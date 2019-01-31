/* Test that the compiler decides to "avoid offloading".  */

#include <openacc.h>

int main(void)
{
  int x, y;

#pragma acc data copyout(x, y)
#pragma acc kernels /* { dg-warning "OpenACC kernels construct will be executed sequentially; will by default avoid offloading to prevent data copy penalty" "" { target { openacc_nvidia_accel_selected && opt_levels_2_plus } } } */
  *((volatile int *) &x) = 33, y = acc_on_device (acc_device_host);

  if (x != 33)
    __builtin_abort();
#if defined ACC_DEVICE_TYPE_nvidia
# if !defined __OPTIMIZE__
  if (y != 0)
    __builtin_abort();
# else
  if (y != 1)
    __builtin_abort();
# endif
#elif defined ACC_DEVICE_TYPE_host
  if (y != 1)
    __builtin_abort();
#else
# error Not ported to this ACC_DEVICE_TYPE
#endif

  return 0;
}
