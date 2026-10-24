set pagination off
set confirm off
target remote :31234
break main
break init
break failureMode
break HardFault_Handler
tbreak taskSystemLoad
cont
cont
cont
cont
cont
printf "== first taskSystemLoad, virtual uptime (ms): %u\n", *(unsigned*)&'sysTickUptime.lto_priv.0'
break taskSystemLoad
ignore 6 49
cont
printf "== 50th taskSystemLoad call after that, virtual uptime (ms): %u\n", *(unsigned*)&'sysTickUptime.lto_priv.0'
info breakpoints
bt 3
kill
