# Filo in the shell

What a script run by `filo file [args]` (or, built, by its name) gets besides the
language itself: its words, its input and output, files, directories,
regular expressions, bytes and a few things of the machine. The utilities
in `/bin` use nothing else; their sources are rocchetto's `commands/bin`,
and a site may serve them too (at `/pub/filo/examples`).

Every example here runs as it is (a test checks them), and what it writes
follows it.

## Results

- **nil** is the empty list, `(list)`: the end of an input, nothing at a
  path, no match. Test it with `is-nil`.
- **A string where a value was expected says why not**: `file-open`,
  `file-write`, `file-close`, `file-touch`, `re-compile`. The words are
  the Unix ones: `No such file or directory`, `Is a directory`,
  `Permission denied`, `File too large`, `Disc quota exceeded`,
  `File table overflow`, `Device busy`, `Operation not supported`,
  `Input/output error`, `File name too long`. A utility says it and goes on.
- **An error stops the script**: a wrong argument, `read-file` of a file
  that is not there, memory or steps run out, Ctrl-C (`Interrupted`, `$?`
  130). The shell says where it happened.
- `=` compares values of one kind; `(= 1 "1")` is an error. Ask `type-of`
  first ("number", "string", "bool", "list", "tuple", "function").

```filo
(let ((h (file-open "~/nothing-here")))
  (out-write (if (= (type-of h) "string") h "opened") "\n"))
```
```
No such file or directory
```

## Words, input, output, status

- `ARGS`: the words after the script's name, a list of strings.
- `(in-read)` the rest of the input, `""` at its end; `(in-read n)` at most
  n bytes, nil at the end. `(in-line)` the next line with its `\n`, nil at
  the end. They read what a `|` or a `<` hands over, however large.
- `STDIN`: the old way, the whole input as one string; nil when it is too
  large to hold.
- `(out-write s ...)` the output as data, byte for byte (what `>` and `|`
  get); numbers are written as `int-text` spells them. `(err-write s ...)`
  goes where errors go (`2>`).
- `(exit-status n)` sets `$?` for when the script ends, 0 to 255; it does
  not end it. `(exit-status)` reads it.
- `echo` and `write` are for the terminal: `(write x)` spells any value.
- `(getopt ARGS "ab:")` reads options as POSIX's getopts does: the result
  is `(opts operands bad)`, opts a list of `(letter value)`.

```filo
(letv (opts words bad) (getopt (list "-n" "3" "-v" "file") "n:v")
  (out-write (str-join " " (map (fn (o) (letv (l v) o (str-concat l "=" v))) opts))
             " | " (str-join " " words) "\n"))
```
```
n=3 v= | file
```

## A loop over an input

The language has no loop and no tail calls. `(iterate f state)` calls
`(f state)` again and again, each result the next state, until f gives
nil; the last state is the value. Only the state outlives each turn, so a
walk over a million lines holds one state, not a million.

```filo
(def count (fn (text)
  (letv (at n)
        (iterate (fn (st) (letv (at n) st
                            (let ((i (byte-find text "\n" at)))
                              (if (< i 0) (list) (tuple (+ i 1) (+ n 1))))))
                 (tuple 0 0))
    n)))
(out-write (count "a\nb\nc\n") "\n")
```
```
3
```

Building text a piece at a time with `str-concat` (or a list with
`list-append`) copies all of it each time: write the pieces as they come
(`out-write`), or keep a short list of them and join it now and then.

## Bytes and characters

Positions are bytes in `byte-*`, `re-match` and the file functions;
characters (code points) in the language's `str-*`; columns in
`str-width` and `rune-width`. Text is UTF-8; a byte that is no character
is decoded as U+FFFD, one each, and never changes on the way through
`byte-*`. Sorting and comparing go byte by byte (as `LC_ALL=C`).

- `(byte-len s)`, `(byte-at s i)` (0..255, nil past the ends),
  `(byte-sub s from [to])`, `(byte-find s needle [from])` (-1 when not
  there), `(byte-cmp a b)` (-1, 0, 1), `(bytes 104 105)`, `(byte-list s)`.
- `(utf8-valid s)`, `(utf8-runes s)`, `(utf8-encode cp ...)`,
  `(str-width s)`, `(rune-width cp)`.
- `(int-text n [base])` a whole number's digits, exact up to 2^53 (base 8
  or 16 too); `(u32-add a b)`, `u32-mul`, `u32-and`, `u32-or`, `u32-xor`,
  `u32-not`, `u32-shl`, `u32-shr` wrap at 2^32.

```filo
(out-write (byte-len "café") " " (str-len "café") " " (int-text 255 16) " "
           (byte-sub "abcdef" 2 4) "\n")
```
```
5 4 ff cd
```

## Regular expressions

`(re-compile pattern)` reads a basic one, as POSIX's grep and sed do
(`\(\)`, `\{n,m\}`, and `?` and `+` are plain characters); `(re-compile
pattern "E")` an extended one, `"i"` folding ASCII case. The result is a
handle, or a string that says why the pattern does not read. 32 may be
held at once: compile outside a loop, and `(re-free re)` one no longer
used. `(re-match re s [from])` is nil, or a list of `(start end)` byte
offsets, the whole match first, then each group (nil for one that took no
part). A pattern that takes too many steps on a text is an error.

```filo
(let ((re (re-compile "([a-z]+)=([0-9]+)" "E")))
  (letv (a b) (nth (re-match re "x: size=42") 2)
    (out-write (byte-sub "x: size=42" a b) "\n")))
```
```
42
```

## Files

Paths are read as the shell reads them (`~`, `.`, `..`); `(path-resolve
p)` gives the absolute one.

- `(file-open p)` to read, `(file-open p "w")` to write (the old file stays
  until `file-close`), `"a"` to add at the end: a handle, or why not. At
  most 8 open at once (4 in a script `run` by another); the handle works in
  its own run only.
- `(file-read h [n])` the next n bytes (4096), nil at the end;
  `(file-line h)` the next line, any length that fits the memory, nil at
  the end; `(file-seek h off)` for one open to read.
- `(file-write h s ...)` `#t` or why not; `(file-close h)` `#t` or why what
  was written could not be kept. What a run leaves open is closed when it
  ends: kept when it ended well, dropped when it failed.
- `(read-file p)` a whole file as a string (an error when it is not
  there), `(write-file p s)`, `(is-file p)`.
- `(file-stat p [#t])` nil, or `(kind size mtime from)`: kind "file",
  "dir" or "link" (`#t` follows the link), size in bytes, mtime in seconds
  since 1970 UTC (nil where the store does not keep it), from "home",
  "tree" or "site". `(file-id p)` what two names of one file share.
  `(file-touch p secs)`. `(file-runs p)` "program" or "script" when the
  shell runs the file by its first bytes, nil when not (nothing is
  fetched to tell: not the site's, not a file iCloud keeps away).
- `(dir-read d [from [count]])` `(entries next)`: entries `(name kind)` in
  byte order, dot files too, at most count (256, up to 1024), and next
  where the next page starts, nil after the last. `(dir-entries d)` the
  older whole list of `(name dir?)`.
- `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `cd`, `pwd`, `cat`, `less` as the
  shell's (`ls` is a program: `(run (list "ls" "-1" dir))`).

```filo
(let ((h (file-open "~/notes.txt" "w")))
  (do (file-write h "one\n" "two\n") (file-close h)))
(let ((h (file-open "~/notes.txt")))
  (do (out-write (file-line h)) (out-write (file-line h)) (out-write (if (is-nil (file-line h)) "the end" "more") "\n")
      (file-close h)))
(letv (kind size mtime from) (file-stat "~/notes.txt") (out-write kind " " size " " from "\n"))
```
```
one
two
the end
file 8 home
```

## Other commands, the environment, the machine

- `(run words [input [env]])` runs a command (a function, a builtin, a
  script of `/bin`) as the shell does, each word one word whatever it
  holds; the result is `(status out err)`. `env` is a list of `(name
  value)` for that command alone.
- `(env-get name)` an exported variable, nil when not; `(env-list)` all of
  them as `(name value)`, HOME, PWD and USER among them.
- `(now)` seconds since 1970 UTC, `(time-zone)` minutes east of UTC: nil
  on a machine with no clock.
- `USER`, `HOSTNAME` (as the prompt's `@` shows it, "" without one),
  `MACHINE` (wasm32, arm64, x86_64, xtensa, riscv), `VERSION`.
- `(store-info)` what the home can hold and keep, as `(name value)` pairs.
- `(list-sort items cmp [key [#t]])` a stable sort, cmp answering a number
  below 0 when the first goes first (as `byte-cmp`); key made once for each
  item; `#t` keeps one of each run of equal ones.
- `(glob-match pattern s)` the shell's `*`, `?` and `[...]`.
- `(out-terminal)` `(columns rows)` when out-write goes straight to the
  terminal, nil when a `|`, a `>` or a `$(...)` takes it.
- `(sgr 1 31)` and `(csi "2J")` escapes for the terminal; `pbcopy`,
  `pbpaste`, `upload`, `download`, `edit`, `edt`, `home-export`,
  `home-import`, `home-kept`, `home-keep`, `home-reload`.

```filo
(letv (status out err) (run (list "echo" "two words" "*"))
  (out-write status " " out))
```
```
0 two words *
```

## What each machine has

| | in a browser | Fosforo, on the iPad and iPhone |
| --- | --- | --- |
| home | in memory, 2M, 512K a file, 64 files; up to 64K kept between visits | the app's storage, as large as the device |
| files written at once | one (another says `Device busy`) | up to the 8 handles |
| a `\|` larger than 256K | kept in the page's memory | kept in a file of the app's |
| file times | kept for files, not for directories | as the disk keeps them |
| links | none | `~/iCloud`, seen as a link |
| `file-id` | nil | the disk's |
| the site's files | fetched while the script waits | none |

The ESP32 build runs smaller programs in smaller memory and may not run
the larger utilities.

## Limits

A run has 10M for what it makes and 1M for what it keeps (its program and
its globals): a value stored in a `def` goes to the 1M, so large data
lives in `let`s inside functions. A line has to fit the memory (up to
about 2M). `sort` holds its whole input: up to 20,000 lines and 1M,
and past that it says so (that much belongs in a database). A script with no input has a budget of steps; reading input buys
more. Past any of these the script stops with an error that says which;
nothing is cut short in silence.
