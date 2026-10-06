# rocchetto

rocchetto, a POSIX shell with its utilities written in Filo. Here in the pager, Space and `b` turn the page, `j` and `k` move a line, `g` and `G` go to the start and the end, `/text` looks for text (`n` and `N` the next one and the one before), and `q` leaves. `help | grep word` works too.

## Files and directories

- `ls [-1ACFRSadfghklmoprstx] [path...]` — list directories, as POSIX's ls: columns at the terminal, one name a line into a `|`; `-l` long rows, `-a` dot files, `-F` marks, `-t` newest first, `-R` the directories inside too
- `cd [path]` — change directory; `cd` alone goes home, `cd -` back where you came from
- `pwd` — print working directory
- `cat <file>...` — print files, one after the other
- `less <file>` — read a file in the pager
- `tree [-a] [dir]` — the directory as a tree (a recursive Filo script)
- `find path... [-name pat] [-type f|d] [-newer f] [-exec cmd {} ;]` — walk the tree; `xargs [-n N] [-I {}] cmd` runs a command over what comes in (`find ~ -name '*.txt' | xargs grep -l todo`)
- `cp [-Rp] <file>... <file|dir>` — copy into your home, from the tree too
- `mv <file|dir>... <file|dir>` — rename or move what is yours
- `rm [-rf] <file>...` — remove files of yours (`-r` a directory with all in it)
- `mkdir [-p] <dir>...`, `rmdir [-p] <dir>...` — make and remove (empty) directories in your home
- `touch [-t time | -r file] <file>` — a file made, or its time set

`~` is your home. `*`, `?` and `[...]` stand for the names they match, sorted (`ls /pub/*.md`, `cat ~/notes-?.txt`); a pattern that matches nothing stays as typed, and a quoted one is only itself.

## Text

- `grep [-EFcilnqsvx] [-e pattern] pattern [file...]` — the lines a pattern matches
- `head`, `tail` (`-n N`, `-c N`, `tail -n +N`), `wc [-lwcm]` — the usual, over files or what a `|` hands over; a file the host serves is fetched while they run
- `sort [-bdfnru] [-k key] [-t c] [-o out] [file...]` — lines in order (up to 20000 lines, 1M)
- `uniq [-cdu] [-f N] [-s N]` — runs of the same line as one (`cat f | sort | uniq -c`)
- `sed <script> [<file>]` — edit lines as they pass (`sed 's/old/new/g'`, `sed -n '/re/p'`, `-i` for a file of yours)
- `diff [-u] <file> <file>` — what changed between two files, as diff writes it (`-u` the unified way); `-` is what a `|` hands over
- `tr`, `cut`, `tee`, `paste`, `join`, `comm`, `nl`, `fold`, `expand`, `unexpand`, `basename`, `dirname` — text for scripts and pipes
- `seq`, `cmp`, `od`, `cksum` — numbers one a line, two files compared, the bytes shown, a checksum
- `split [-l lines | -b bytes] [file [prefix]]` — a file cut into pieces xaa, xab...
- `pr [-columns] [-h header] [-l lines] [-tdn] [file]` — pages with a header, in columns too
- `tsort [file]` — words two by two, "a b" a before b, put in an order that keeps them all
- `expr` — the old way to count and match (`expr $n + 1`, `expr "$f" : '\(.*\)\.md'`)
- `date [-u] [-r seconds] [+format]` — now, or that moment (`date +%F`, `date '+%H:%M'`)

`grep` takes a regular expression, basic as POSIX's (`grep '^ab*c$'`, `grep 'x\{2\}'`), `-E` for an extended one (`grep -E 'a|b+'`), `-F` for plain text; `-i`, `-n`, `-v` (the lines that do not match), `-c` (how many), `-x` (the whole line), `-l` (the files), `-q` (only `$?`). `sed` reads the same ones: `s/re/new/g` with `&` and `\1` in the new text, `/re/d`, `1,5p` with `-n`, `y/abc/xyz/`, `a`/`i`/`c` text, and the hold space (`h g x`).

## Your home

The shell opens at your home, `~`, and it is yours to fill; the rest of the tree is the host's, to read. The home holds 2M, 512K per file, for the session; a host that keeps it between sessions (a browser's storage, the file `rocchetto --home FILE` names, a card) keeps up to 64K of it (16K per file) — see `home`. Another machine starts empty: carry your home with `home export` and `home import`. A host with storage of its own (a card, the fosforo app) keeps your files there instead, whole.

- `upload [dir]` — put files of yours in your home, where the host has a way in (in a browser a picker opens, or drop them on the page); they go where you are, or to `~/uploads`
- `download <file>` — a file of yours (or the tree's) to your machine
- `edt [file]` — the editor, named after DEC's: `Esc` then the orange letter is its GOLD key; `^S` saves in your home, `^Q` leaves, `^L` shows the text alone, `Esc X` shows the bytes in hex; with no file the first save asks for a name
- `home` — what the host keeps of your home; `home export` downloads it as one file, `home import <file>` restores one you uploaded
- `pbcopy [text]`, `pbpaste` — the machine's clipboard: `echo hi | pbcopy`, `pbpaste | wc`; in Filo, `(pbcopy "hi")` and `(pbpaste)`. Only where the host has one (the fosforo app)

In a browser two tabs keep one home: when the other tab kept its own since this one read it, this tab says so and keeps nothing over it until `home keep` or `home reload`.

`~/.profile`, when you have one, runs as the session opens, in the shell itself (as `. ~/.profile`): the variables, aliases, functions and `PATH` it sets are there at the first prompt.

## Programs and scripts

A command is found in the directories of `PATH` (`/bin` to begin with: `PATH=~/bin:/bin` puts yours first). A file there runs when it is a program of this shell's (compiled Filo: `filo build -o hi hi.filo`) or a shell script (its first line `#!/bin/sh`), known by its first bytes, not by its name: `./hi` runs it from here. A source, or any other file, is not run that way (`filo hi.filo` is). Yours go over the shell's commands of the same name; the shell's own (`cd`, `export`, `exit`...) and your functions are looked at before `PATH`.

- `filo <file> [args]` — run a Filo script by its whole name (`filo hi.filo`), or a program
- `filo` — the language itself: a line is read and shown, `def` keeps, `exit` or Ctrl-D leaves
- `type <name>`, `command -v <name>` — what a name is: keyword, builtin, function, program
- `alias name='text'` — a shorter name for a command (`alias ll='ls -a'`), from the next line on; `unalias`
- `history` — the lines of this session (it is `cat ~/.history`)
- `sleep <seconds>` — the line waits (`sleep 0.5` too); Ctrl-C ends it

The commands in `/bin` are compiled Filo programs (`/bin/ls`); most of the simple ones above are, `grep`, `sort` and `find` among them. Their sources are rocchetto's `commands/bin`; a decompiled one reads well too (`filo decompile /bin/ver`). Scripts get the shell's commands as builtins and more: words, input and output, files, directories, regular expressions, bytes, other commands to run. `less /bin/filo_api.md` tells them all, each with an example that runs.

The shell carries its own files: the programs behind commands like this one live in `/bin`, and screens and documents under `/lib/roc` (`less /lib/roc/bin/filo_api.md`).

## The system

- `ver` — the version of this shell
- `whoami`, `hostname`, `uname [-a]`, `logname` — who and where you are, what system this is
- `clear` — clear screen
- `exit` — leave the shell
- `mars <a.red> <b.red> [more...] [-r N] [-F pos] [-b]` — Core War: Redcode warriors fight in the MARS (ICWS'94) on screen, and the score comes out when you leave; `-b` fights in text alone. Classics live in `/lib/roc/warriors`, yours in `~` (write them with `edt`)
- `corewar [<a.red> <b.red> ...]` — Core War, two warriors or up to eight. With no files it opens the screen that picks them: the classic warriors and your own `.red` files in `~`, which it also edits (`e N`) or starts new (`n name`); `h` there opens the Redcode manual, which the editor opens too, with `Esc` then `H`. Then the fight: the core as a map, a colour per warrior, the code each one is executing beside it, and what would end the round. Space pauses, + and - set the speed, M picks the core (standard, tiny or nano), C how many cycles a round lasts, N calls the round, R rematches

## Not here

`chmod`, `chown`, `chgrp`, `ln`, `kill`, `ps`, `mkfifo`, `nohup`, `nice`, `renice`, `crontab`, `at` and `stty` are POSIX's, but this shell cannot do what they do: each says so and fails (`$?` is 1).

## The line

Tab completes a command or a file (at the Filo REPL, a name of Filo). Up and Down walk the history of this session, matching what is left of the cursor, and Ctrl-R searches it backwards as you type (Ctrl-R again for an older one, Enter runs it, Esc keeps it to edit, Ctrl-G gives up). Ctrl-C aborts.

The line is read as sh reads it: words split at spaces, `'single quotes'` keep everything as typed, `"double quotes"` too but for `$` and `\` before `"` `\` `$`, a backslash outside quotes keeps the next character (`cd ~/my\ dir`), and `#` at the start of a word begins a comment. So `echo 'a b'` keeps its spaces, `grep 'one two' file` looks for both words, and a script gets `ARGS` word by word.

- `echo <text>` — say it back (`echo -n` leaves out the newline)
- `printf format args` — prints as C's does (`%s %d %5.2s %-8s %x %b`, the format again while arguments are left)
- `read name rest` — takes a line: typed at the prompt, or the next one of the `<` or `|` of the loop it is in (`while read l; do ...; done < ~/list`, `ls /pub | while read f; do ...; done`), the last name keeping the rest of the line
- `set`, `unset <name>` — list the variables, forget one
- `test`, `[ ]`, `true`, `false` — the questions `if` and `while` ask

## Output and input

`cmd > file` and `cmd >> file` put what a command prints into a file of yours, as plain text (`ls /pub > ~/list.txt`, `tree / > ~/map`); a `>` writes its file as the output comes, up to 512K a file in the home. `cmd < file` reads a file as the input (one here: a file the host serves wants a `cp` home first). `cmd 2> file` and `2>> file` send the refusals to a file, `2>&1` where the output goes (`ls /x 2>&1 | wc`), `>&2` the output where the refusals go (`echo "no such thing" >&2`), and `/dev/null` takes anything and keeps nothing (`ls /x 2> /dev/null`).

Pipes: `a | b` hands what a prints to b (`ls /pub | grep go`, `cat ~/notes.txt | grep -n todo | head -n 3`); what does not fit in 256K is kept aside and the utilities read it all, while `less` and `read` say it is too large for them. `cat` and `less` with no file, the utilities with no file (or `-`), and scripts (`(in-read)`, `(in-line)`, or the old `STDIN`) read it; the rest ignore it, as they would in sh.

A here-document gives a command lines of text as its input: `cat <<EOF`, then the lines, then a line that is only `EOF`. `$name`, `$(...)` and `$((...))` in them are expanded unless the word is quoted (`<<'EOF'`), and `<<-EOF` drops the tabs the lines start with. At the prompt the lines are asked for with `> `; in a script they follow the command.

## Variables

`name=value` sets one, `$name` or `${name}` reads it (split at spaces unless in double quotes: `"$name"`), `set` lists them and `unset name` forgets one. `$HOME`, `$USER` and `$PWD` are the session's, `$?` says how the last command ended (0 well, 1 refused, 2 misread, 127 not found), `$$` is this shell's number, and `~` at the start of a word is your home.

- `${name:-other}` — other when name is empty or unset (`${name-other}` only when unset)
- `${name:=other}` — the same, and sets it too
- `${name:+other}` — other only when it has a value
- `${name:?why}` — stops the line saying why
- `${#name}` — counts its characters
- `${name%.txt}`, `${name%%.*}`, `${name#*/}`, `${name##*/}` — cut the shortest or longest end or start a pattern matches (`${f##*/}` is the file's name, `${f%/*}` its directory)
- `$((expr))` — arithmetic as C does, in 64 bits (`i=$((i + 1))`, `$((n % 2 == 0 ? 1 : 0))`); `$((i += 2))` and the other assignment operators set the variable
- `$(command)` — what the command prints, in place (`cd $(cat ~/where)`, `n=$(ls /pub | wc)`); it runs there and then, so one that waits (a file the host fetches, a screen) cannot be one; they nest four deep. `` `command` `` is the old way to write `$(command)`

`readonly name` or `readonly name=value` keeps it as it is; `export name` gives it to the commands this shell runs (`export -p` lists them, `env` shows them), `NAME=value command` gives it to that one command alone, and `env [n=v] cmd` runs with variables set. A `NAME=value` before a builtin is set only while it runs. `IFS` says where `$x` and `read` split (`IFS=: read a b`, `IFS= read -r line` to keep the spaces).

## Several commands

`a; b` runs both, `a && b` runs b only when a went well (`$?` is 0), `a || b` only when it did not (`cd ~/w || mkdir ~/w`), and `! command` turns its `$?` (0 becomes 1, anything else 0). The whole line is read first, so a mistake anywhere runs none of it; a command that waits (a file the host fetches, the editor, the REPL) has the rest wait too, and Ctrl-C while it waits drops the rest. A line that stops too soon (a quote left open, a `&&` or `\` at the end, an `if` with no `fi`) asks for the rest with `> `; Ctrl-C drops it all. `cmd &` runs `cmd` then and there (nothing runs in the background here): `$!` names it and `wait $!` gives its `$?`.

- `if a; then b; elif c; then d; else e; fi`
- `while a; do b; done`, `until a; do b; done`
- `for f in ~/*.txt; do wc $f; done`
- `case $x in a|b) ...;; *.md) ...;; *) ...;; esac` — the first branch whose pattern matches
- `break` and `continue` — with a number, that many loops out
- `{ a; b; }` — groups commands, for a `|` or a `>` after them
- `( a; b )` — groups them in a subshell: what it sets, the `cd` it makes, the functions it defines stay in it, and `exit` leaves only it (`(cd /pub; ls) | wc`); `$(...)` is a subshell too

They nest and go over several lines, as in sh. What a whole block writes goes on with `|`, `>` or `>>` after its `fi` or `done` (`for f in ~/*.txt; do head -n 1 $f; done | sort > ~/firsts`), and a block, a function or a script takes `2> file`, `2>> file` and `2>&1` like a command (`for f in a b; do ls $f; done 2> ~/errors`). A loop that goes on and on still hears Ctrl-C.

`test` and `[ ]` ask: `-n s`, `-z s`, `a = b`, `a != b`, `-eq -ne -lt -le -gt -ge` for numbers, `-e -f -d` for paths (and `-s -r -w -x`), `!` before any of them, and `-a`, `-o`, `!` and `( )` to join questions; `true`, `false` and `:` answer alone.

## Functions and scripts

`name() { ...; }` makes a function for the session: it gets its words as `$1`, `$2`... (`${10}`), `$#` counts them, `"$@"` is all of them one word each, `shift` drops the first, `return n` leaves it with that status, `local name` gives the variable back as it was when it returns, and `unset -f name` forgets it. `command name` runs name past any function of that name.

A shell script is a file of these lines: `sh file args` runs it with its words as `$1`... and `$0` its name, leaving your variables and directory as they were (`exit n` ends only it); `./file` does the same when its first line is `#!/bin/sh`, and `. file` runs it in this shell, so what it sets stays (a library of functions, say). `sh -c 'text' name args` runs a line the same way, and `eval words` reads the words as a line here (`eval "w=\$$name"` for the variable a variable names).

- `set -e` — ends the line (or the script) at the first command that fails where nothing looks at it (not an `if` or `while` condition, not before `&&` or `||`)
- `set -u` — a variable not set is an error
- `set -x` — shows each command, expanded, with `+ ` before it
- `+e`, `+u`, `+x` turn them off and `$-` says which are on; `set -- a b` sets `$1`, `$2`...
- `trap 'commands' EXIT` — runs them when the script ends (or at `exit`); `trap 'commands' INT` after a Ctrl-C stops a line (a script running long stops too, with `$?` 130); `trap` lists them and `trap - EXIT` forgets one
- `getopts ab:c opt` — reads `-a -b value -c` one at a time (`OPTARG`, `OPTIND`), for a script's own flags
