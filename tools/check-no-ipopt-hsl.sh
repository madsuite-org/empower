#!/bin/sh
# Rule: an empower release never contains compiled Ipopt or HSL code.
# Ipopt is downloaded by the installer from conda-forge, and HSL is compiled on
# the user's machine from the user's own licensed copy.
#
# Fails if the archive has an Ipopt or HSL library file, or any binary or static
# library in it defines an Ipopt or HSL symbol (calling Ipopt through dlsym is
# fine). Fails too when it cannot inspect a binary, rather than passing it.
#
#   tools/check-no-ipopt-hsl.sh ARCHIVE
set -eu
archive=$1
for tool in nm objdump od; do
    command -v "$tool" >/dev/null || { echo "$tool is needed to inspect $archive" >&2; exit 1; }
done
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
tar xzf "$archive" -C "$t"

bad=0
files=$(find "$t" -type f | grep -iE '/[^/]*(ipopt|hsl|coinhsl)[^/]*\.(so|dylib|a|dll|lib)([.0-9]*)$' || true)
if [ -n "$files" ]; then
    echo "Ipopt or HSL library files in $archive:"; echo "$files"; bad=1
fi

pattern='^_?(ma27|ma28|ma30|ma57|ma77|ma86|ma97|mc19|mc30|mc64|mc68|hsl_[a-z0-9_]*)[a-z_]*$|^_?(CreateIpoptProblem|IpoptSolve|FreeIpoptProblem)$|Ipopt[0-9]*[A-Z]'
n=0
find "$t" -type f > "$t.list"
while IFS= read -r f; do
    magic=$(head -c 8 "$f" | od -An -tx1 | tr -d ' \n')
    case "$magic" in
        7f454c46*) kind=elf; syms=$(nm -D --defined-only "$f") || kind=error ;;
        cffaedfe* | cafebabe*) kind=macho; syms=$(nm -gU "$f") || kind=error ;;
        213c617263683e0a) kind=archive; syms=$(nm -g --defined-only "$f" 2>/dev/null || nm -gU "$f") || kind=error ;;   # !<arch>
        4d5a*)
            kind=pe
            syms=$(objdump -p "$f") || kind=error
            syms=$(printf '%s\n' "$syms" | awk '/\[Ordinal\/Name Pointer\] Table/ {on = 1; next} on && NF == 0 {on = 0} on {print $NF}') ;;
        *) continue ;;
    esac
    if [ "$kind" = error ]; then
        echo "could not read the symbols of ${f#"$t"/}"; bad=1; continue
    fi
    n=$((n + 1))
    hit=$(printf '%s\n' "$syms" | awk '{print $NF}' | grep -E "$pattern" | head -5 || true)
    if [ -n "$hit" ]; then
        echo "Ipopt or HSL symbols defined in ${f#"$t"/}:"; echo "$hit"; bad=1
    fi
done < "$t.list"
rm -f "$t.list"
[ $n -gt 0 ] || { echo "no binaries found in $archive; nothing was checked" >&2; exit 1; }
[ $bad -eq 0 ] && echo "no compiled Ipopt or HSL in $archive ($n binaries checked)"
exit $bad
