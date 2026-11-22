#!/bin/sh
# sp16-check-glibc FILE...
# Fails if an ARM binary needs a glibc symbol version newer than 2.13 (the SP-16's C library),
# and prints the interpreter and the needed shared libraries.
set -eu
status=0
for file in "$@"; do
  echo "== $file"
  arm-linux-gnueabi-readelf -l "$file" | sed -n 's/.*Requesting program interpreter: \(.*\)]/interpreter: \1/p'
  arm-linux-gnueabi-readelf -d "$file" | sed -n 's/.*Shared library: \[\(.*\)\]/needs: \1/p'
  too_new=$(arm-linux-gnueabi-objdump -T "$file" 2>/dev/null \
    | grep -o 'GLIBC_[0-9][0-9.]*' | sort -u \
    | python3 -c '
import sys
for line in sys.stdin:
    v = line.strip()[6:]
    if tuple(int(x) for x in v.split(".")) > (2, 13):
        print(line.strip())')
  if [ -n "$too_new" ]; then
    echo "TOO NEW for glibc 2.13:"; echo "$too_new"; status=1
  else
    echo "glibc symbol versions: ok (<= 2.13)"
  fi
done
exit $status
