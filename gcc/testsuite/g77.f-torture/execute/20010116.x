set torture_eval_before_execute {
  set compiler_conditional_xfail_data {
    "" "i?86-*-*" { "-O[23s]" } { "" }
    }
}
return 0
