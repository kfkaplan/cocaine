# Run by /bin/sh from the Mac OS X system root: processes, pipes and signals.
# usage: procs.sh <scratch directory>
work=$1

x=$(/bin/echo sub | /usr/bin/tr a-z A-Z)
echo "substitution: $x"
(exit 7)
echo "subshell status: $?"
/usr/bin/false
echo "false: $?"
for i in 1 2 3; do /bin/echo "$i"; done | /usr/bin/wc -l | /usr/bin/tr -d ' '
/bin/sleep 1 &
wait $!
echo "background job: $?"
/bin/cat <<HERE | /usr/bin/sort -r | /usr/bin/head -2
alpha
gamma
beta
HERE

# A script with a "#!" line is run by the interpreter it names.
printf '#!/bin/sh\necho "script: $# arguments, first $1"\n' >"$work/script.sh"
/bin/chmod +x "$work/script.sh"
"$work/script.sh" x y
printf '#!/usr/bin/awk -f\nBEGIN { print "awk script:", 6 * 7 }\n' >"$work/script.awk"
/bin/chmod +x "$work/script.awk"
"$work/script.awk"
/nonexistent/program 2>/dev/null
echo "missing program: $?"

trap 'echo "caught USR1"' USR1
kill -USR1 $$
/bin/sleep 30 &
kill $!
wait $! 2>/dev/null
echo "killed child: $?"

# One guest starting another for the other CPU.
/usr/bin/arch -ppc /usr/bin/uname -p
/usr/bin/arch -i386 /usr/bin/uname -p
/usr/bin/perl -e 'print "perl: ", `/bin/echo backtick`; system("/bin/echo", "perl: system")'
trap 'echo "exit trap"' EXIT
