#!/bin/sh
# Generates the .index TSV consumed by roc's VFS.
# usage: mkindex.sh <root-dir>
# Format: path<TAB>size<TAB>date<TAB>title  (dirs end with /, listed first)
set -eu

if [ $# -ne 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "usage: mkindex.sh <root-dir>"
    echo "Writes <root-dir>/.index describing dirs and *.md/*.txt files."
    exit 0
fi

root=$(cd "$1" && pwd)
out="$root/.index"
tmp="$out.tmp"

mtime() {
    if stat -f '%Sm' -t '%Y-%m-%d %H:%M' "$1" 2>/dev/null; then
        return
    fi
    stat -c '%y' "$1" 2>/dev/null | cut -c1-16
}

title_of() {
    # TOML: title = "..."  |  YAML: title: ...
    sed -n 's/^title[[:space:]]*[:=][[:space:]]*"\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$1" |
        head -1 | tr -d '\t'
}

: >"$tmp"

find "$root" -type d ! -path '*/.*' | sort | while read -r d; do
    rel="${d#"$root"}"
    if [ -z "$rel" ]; then
        continue
    fi
    printf '%s/\t0\t%s\t\n' "$rel" "$(mtime "$d")" >>"$tmp"
done

find "$root" -type f \( -name '*.md' -o -name '*.txt' \) ! -path '*/.*' | sort | while read -r f; do
    rel="${f#"$root"}"
    size=$(wc -c <"$f" | tr -d ' ')
    printf '%s\t%s\t%s\t%s\n' "$rel" "$size" "$(mtime "$f")" "$(title_of "$f")" >>"$tmp"
done

mv "$tmp" "$out"
echo "$(wc -l <"$out" | tr -d ' ') entries -> $out" >&2
