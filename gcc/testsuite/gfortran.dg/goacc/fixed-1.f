!$ACC ROUTINE(ABORT) SEQ
! { dg-bogus "invalid function name abort" "" { xfail *-*-* } .-1 }

      INTEGER :: ARGC
      ARGC = COMMAND_ARGUMENT_COUNT ()

!$OMP PARALLEL
!$ACC PARALLEL COPYIN(ARGC)
      IF (ARGC .NE. 0) THEN
         STOP 1
      END IF
!$ACC END PARALLEL
!$OMP END PARALLEL

      END
