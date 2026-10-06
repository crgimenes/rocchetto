# rocchetto

A POSIX shell whose utilities are [Filo](https://github.com/crgimenes/clang_filo)
programs, written in C with no C library under its core: the same shell
runs as a desktop binary, in the browser (wasm), on a 20x8 Cardputer and
inside the [fosforo](https://github.com/crgimenes/fosforo) terminal on iOS.

- **The shell**: pipes, redirects, `$(...)`, functions, `if`/`for`/`while`,
  history, Tab, a line editor; a home of your own over a read-only tree.
- **Its utilities**: `ls`, `grep`, `sed`, `sort`, `find`, `wc` and sixty
  more, each a Filo program in `commands/bin`, compiled into `/bin` and
  readable there (`less /bin/filo_api.md` says what a script may call).
- **Screens and apps**: Filo programs that paint, as
  [edt](https://github.com/crgimenes/edt) and
  [corewar](https://github.com/crgimenes/corewar)'s pick are; they run
  here and as programs of their own, the same bundle.
- **Filo itself**: `filo` runs, builds, dumps, decompiles, formats and
  debugs it; with no file, the REPL.

The terminal underneath — the canvas, the keys, the pager — is
[filo-term](https://github.com/crgimenes/filo-term).

## Hosts

| host | where | what it gives |
| --- | --- | --- |
| `host/posix` | a terminal | a directory as the tree (`rocchetto DIR`), `--home FILE` to keep the home |
| `host/wasm` | a browser | the page's files, the home in its storage, uploads and downloads |
| `host/esp32` | an M5Stack Cardputer | an SD card as the storage |
| `libroc.a` | another program (fosforo) | its own host, compiled against `build/libroc.cflags` |

A host may offer its programs builtins of its own (`filo_extend`); a
program that uses them does not load where they are not offered.

## A layer over it

A build that adds its own screens, commands and C (a BBS, say) sets `ROC`
to this repository and the hooks `TREE_ROOTS`, `COMMAND_ROOTS`,
`EXTEND_SRC`, `APP_BOARD`, `EXTRA_INC` and `TEST_SRC`, then includes this
Makefile. Its tests include `test/test_roc.c` (`ROC_TEST_NO_MAIN`) and call
`roc_tests()` before their own.

## Build

```
make               # ./rocchetto
make test          # under ASan/UBSan
make wasm          # build/roc.wasm: CLANG with the wasm32 target, LLD, wasm-opt
make wasm-test     # the wasm under Node, the page's imports faked
make esp32         # the Cardputer firmware (arduino-cli)
make qa            # all of the above but the firmware, clang-format, clang-tidy, cppcheck
```

The repositories it builds from live beside it: `FILO ?= ../clang_filo`,
`FILO_TERM ?= ../filo-term`, `CW ?= ../corewar`, `EDT ?= ../edt`. Tools
are named by `CC`, `CLANG`, `LLD`, `CLANG_FORMAT`, `CLANG_TIDY`,
`WASM_OPT`.

## License

MIT
