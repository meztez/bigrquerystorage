## R CMD check results

Duration: 42s

❯ checking compilation flags used ... NOTE
  Compilation used the following non-portable flag(s):
    '-Werror=format-security' '-Wp,-D_GLIBCXX_ASSERTIONS'
    '-Wp,-U_FORTIFY_SOURCE,-D_FORTIFY_SOURCE=3' '-march=x86-64'
    '-mno-omit-leaf-frame-pointer' '-mtls-dialect=gnu2'

These flags come from the local Fedora toolchain's default hardening
settings, not from this package's Makevars, and are not expected to
appear on CRAN's build machines.

0 errors ✔ | 0 warnings ✔ | 1 note ✖

Tests :  [ FAIL 0 | WARN 0 | SKIP 0 | PASS 42 ]
Package checks : https://github.com/meztez/bigrquerystorage/actions
