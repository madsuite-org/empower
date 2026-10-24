#!/bin/sh
# Rule: an opfsolve release never contains compiled Ipopt or HSL code.
# Ipopt and HSL are always downloaded by install.sh from their publisher.
# Fails if the archive has an Ipopt or HSL library file, or any binary in it
# defines an Ipopt or HSL symbol (calling Ipopt through dlsym is fine).
set -eu
archive=$1
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
tar xzf "$archive" -C "$t"

bad=0
files=$(find "$t" -type f | grep -iE '/[^/]*(ipopt|hsl|coinhsl)[^/]*\.(so|dylib|a|dll)([.0-9]*)$' || true)
if [ -n "$files" ]; then
    echo "Ipopt or HSL library files in $archive:"; echo "$files"; bad=1
fi

pattern='^_?(ma27|ma28|ma30|ma57|ma77|ma86|ma97|mc19|mc30|mc64|mc68|hsl_[a-z0-9_]*)[a-z_]*$|^_?(CreateIpoptProblem|IpoptSolve|FreeIpoptProblem)$|Ipopt[0-9]*[A-Z]'
find "$t" -type f > "$t.list"
while IFS= read -r f; do
    magic=$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')
    case "$magic" in
        7f454c46*) syms=$(nm -D --defined-only "$f" 2>/dev/null | awk '{print $NF}') ;;   # ELF
        cffaedfe* | cafebabe*) syms=$(nm -gU "$f" 2>/dev/null | awk '{print $NF}') ;;    # Mach-O
        4d5a*)                                                                            # PE: export table
            syms=$(objdump -p "$f" 2>/dev/null |
                awk '/\[Ordinal\/Name Pointer\] Table/ {on = 1; next} on && NF == 0 {on = 0} on {print $NF}') ;;
        *) continue ;;
    esac
    hit=$(printf '%s\n' "$syms" | grep -E "$pattern" | head -5 || true)
    if [ -n "$hit" ]; then
        echo "Ipopt or HSL symbols defined in ${f#$t/}:"; echo "$hit"; bad=1
    fi
done < "$t.list"
rm -f "$t.list"
[ $bad -eq 0 ] && echo "no compiled Ipopt or HSL in $archive"
exit $bad
