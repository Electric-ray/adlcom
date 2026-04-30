name adlcom

file adlcom
file cmdline
file res_opl2
file res_glue
file res_end

system dos

order
  clname CODE
    segment BEGTEXT
    segment RESIDENT
    segment RESEND
    segment _TEXT
    segment CODE
  clname FAR_DATA
  clname BEGDATA
  clname DATA
  clname BSS noemit
  clname STACK noemit

libpath C:\watcom19\lib286\dos
library clibs.lib

option map
option quiet
option start='_cstart_'
