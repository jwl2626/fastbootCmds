file fastbootCmds_dbg.exe
set print pretty on
break src/main.cpp:344
run getvar 1 3
bt 3
print command
print targets
print raw
quit
