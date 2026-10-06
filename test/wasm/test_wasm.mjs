// The shell as the browser runs it: build/roc.wasm, the same contracts the
// C tests check, through the page's imports. make wasm-test.
import { readdirSync, readFileSync } from "node:fs";
import { INDEX, boot, check, done, same } from "./harness.mjs";
import { escape, parse } from "../fixtures/oracle.mjs";

const enc = new TextEncoder();

async function statusAndData() {
  const t = await boot();
  t.put("st2.filo", "(exit-status 2)");
  t.put("copy.filo", "(out-write (in-read))");
  t.put("both.filo", '(out-write "out\\n") (err-write "err\\n")');
  check(t.status("filo ~/st2.filo") === 2, "explicit status");
  check(t.status("filo ~/copy.filo < /dev/null") === 0, "status 0");
  const all = new Uint8Array(768).map((_, i) => i & 255);
  t.put("all.bin", all);
  check(same(t.get("~/all.bin"), all), "upload and download keep the bytes");
  check(t.status("filo ~/copy.filo < ~/all.bin > ~/c1") === 0, "copy status");
  check(same(t.get("~/c1"), all), "< and > keep every byte");
  check(t.status("filo ~/copy.filo < ~/all.bin | filo ~/copy.filo > ~/c2") === 0, "pipe status");
  check(same(t.get("~/c2"), all), "| keeps every byte");
  check(t.status("filo ~/both.filo > ~/o 2> ~/e") === 0, "both status");
  check(same(t.get("~/o"), enc.encode("out\n")), "stdout apart");
  check(same(t.get("~/e"), enc.encode("err\n")), "stderr apart");
  check(t.status("printf 'a\\033b\\r\\n' > ~/pf") === 0, "printf status");
  check(same(t.get("~/pf"), enc.encode("a\x1bb\r\n")), "printf bytes");
  const out = t.type("printf 'x\\ny\\n'\r");
  check(out.includes("x\r\ny\r\n"), "terminal lines");
}

// the same cksum the C tests check, run by the freestanding build
const CKSUM = `
(def poly 79764919)
(def table
  (map (fn (i)
         (fold (fn (c k) (if (= (u32-and c 2147483648) 0) (u32-shl c 1) (u32-xor (u32-shl c 1) poly)))
               (u32-shl i 24) (range 0 8)))
       (range 0 256)))
(def step (fn (crc b) (u32-xor (u32-shl crc 8) (nth table (u32-xor (u32-shr crc 24) b)))))
(def with-len (fn (crc n) (if (= n 0) crc (with-len (step crc (u32-and n 255)) (floor (/ n 256))))))
(def total
  (iterate (fn (st)
             (let ((b (in-read 4096)))
               (if (is-nil b) (list) (list (fold step (nth st 0) (byte-list b)) (+ (nth st 1) (byte-len b))))))
           (list 0 0)))
(out-write (u32-not (with-len (nth total 0) (nth total 1))) " " (nth total 1) "\n")
`;

async function dataPackage() {
  const t = await boot();
  t.put("cksum.filo", CKSUM);
  const dec = new TextDecoder();
  check(t.status("filo ~/cksum.filo < /dev/null > ~/k0") === 0, "cksum empty status");
  check(dec.decode(t.get("~/k0")) === "4294967295 0\n", "cksum empty");
  check(t.status("printf 123456789 | filo ~/cksum.filo > ~/k1") === 0, "cksum status");
  check(dec.decode(t.get("~/k1")) === "930766865 9\n", "cksum 123456789");
  t.put(
    "re.filo",
    '(out-write (string (re-match (re-compile "(x|y)+z" "E") "aaxyxz")) " " (str-width "a中") " " (int-text 255 16))',
  );
  check(t.status("filo ~/re.filo > ~/r") === 0, "regex status");
  check(dec.decode(t.get("~/r")) === "(list (tuple 2 6) (tuple 4 5)) 3 ff", "regex, width, int-text");
}

// Etapa 3 in the browser: the in-memory home and the site's index answer
async function files() {
  const t = await boot();
  const dec = new TextDecoder();
  t.put("a.txt", "hello");
  t.type("mkdir ~/d\r");
  t.put("x", "1", "~/d");
  t.put(
    "meta.filo",
    `(def txt (fn (x) (cond ((= (type-of x) "number") (int-text x)) ((= (type-of x) "string") x) ((is-nil x) "nil") (else (string x)))))
(def st (fn (p) (let ((s (file-stat p))) (out-write (if (is-nil s) "nil" (letv (k z t o) s (str-join " " (map txt (list k z t o))))) "\n"))))
(st "~/a.txt") (st "~/d") (st "~/none") (st "/pub/kutta.md")
(letv (es next) (dir-read "~") (out-write (str-join "," (map (fn (e) (letv (n k) e n)) es)) " " (txt next)))`,
  );
  check(t.status("filo ~/meta.filo > ~/o") === 0, "meta status");
  const o = dec.decode(t.get("~/o"));
  check(
    o === "file 5 1791115200 home\ndir nil nil home\nnil\nfile 12 1786876200 site\n.history,a.txt,d,meta.filo,o nil",
    `meta output: ${JSON.stringify(o)}`,
  );
}

// Etapa 3b in the browser: a script runs another, words and input intact
async function calls() {
  const t = await boot();
  const dec = new TextDecoder();
  t.put("inner.filo", '(out-write (str-join "|" ARGS) ":" (in-read))');
  t.put(
    "outer.filo",
    `(out-write "a,")
(letv (st o e) (run (list "filo" "~/inner.filo" "x y" "it's") "in!") (out-write st "," o ","))
(out-write "b")`,
  );
  check(t.status("filo ~/outer.filo > ~/o") === 0, "outer status");
  const o = dec.decode(t.get("~/o"));
  check(o === "a,0,x y|it's:in!,b", `outer output: ${JSON.stringify(o)}`);
}

// Etapa 5 in the browser: a file written and read a piece at a time
async function handles() {
  const t = await boot();
  const dec = new TextDecoder();
  t.put(
    "w.filo",
    `(def h (file-open "~/w.txt" "w"))
(iterate (fn (i) (if (= i 500) (list) (do (file-write h (int-text i) "\n") (+ i 1)))) 0)
(file-close h)
(def r (file-open "~/w.txt"))
(out-write (nth (iterate (fn (st) (let ((l (file-line r))) (if (is-nil l) (list) (list (+ (nth st 0) 1) l)))) (list 0 "")) 0))`,
  );
  check(t.status("filo ~/w.filo > ~/o") === 0, "handles status");
  check(dec.decode(t.get("~/o")) === "500", "500 lines back");
  const w = dec.decode(t.get("~/w.txt"));
  check(w.startsWith("0\n1\n") && w.endsWith("499\n"), "the file the handle wrote");
}

// a script that runs long gives the page its turn: its output shows as it
// goes, keys wait their turn, a Ctrl-C stops it with 130
async function longScript() {
  const t = await boot();
  t.yieldReal = true;
  t.put("lines", "a line\n".repeat(5000));
  t.put("loop.filo", `(iterate (fn (i) (let ((h (file-open "~/lines"))) (do (out-write "pass " (int-text i) "\\n") (iterate (fn (k) (if (is-nil (file-line h)) (list) k)) 0) (file-close h) (+ i 1)))) 0)`);
  t.type("filo ~/loop.filo; echo after\r");
  await new Promise((r) => setTimeout(r, 400));
  check(!t.core.idle() && t.log.includes("pass 1"), "still running, its output out as it goes");
  t.type("echo queued > ~/q\r");
  t.type("\x03");
  await t.settle();
  check(t.core.idle() && t.log.includes("Interrupted") && !t.log.includes("\r\nafter"), "stopped, the line with it");
  t.yieldReal = false;
  check(t.status("true") === 0 && new TextDecoder().decode(t.get("~/q")) === "queued\n", "the queued line ran after");
}

// two tabs of the shell over one browser's storage: the second to save
// does not wipe the first, and says so; home keep and home reload settle it
async function twoTabs() {
  const dec = new TextDecoder();
  const storage = new Map();
  const ls = { getItem: (k) => (storage.has(k) ? storage.get(k) : null), setItem: (k, v) => storage.set(k, v) };
  const a = await boot({ storage: ls });
  const b = await boot({ storage: ls });
  a.type("echo from-a > ~/a.txt\r");
  const said = b.type("echo from-b > ~/b.txt\r");
  check(said.includes("another tab kept its home since this one read it"), "b is told, once");
  check(!b.type("echo more > ~/b2.txt\r").includes("another tab"), "and not at each change");
  check(b.type("home\r").includes("home keep (this tab's wins)"), "home says what to do");
  // a's home is what is kept: a new tab sees a.txt, not b.txt
  const c = await boot({ storage: ls });
  check(c.status("test -f ~/a.txt") === 0 && c.status("test -f ~/b.txt") === 1, "a's home kept, not b's");
  check(b.type("home reload\r").includes("To do it: home reload yes"), "reload asks first");
  b.type("home keep\r");
  const d = await boot({ storage: ls });
  check(d.status("test -f ~/b2.txt") === 0 && d.status("test -f ~/a.txt") === 1, "after keep, b's home is kept");
  // now a is behind: its change is refused, and reload brings b's home
  check(a.type("echo again > ~/a2.txt\r").includes("another tab kept"), "a refused in turn");
  a.type("home reload yes\r");
  check(a.log.includes("files back in /home/guest, the kept home."), "reload said");
  check(a.status("test -f ~/b2.txt") === 0 && a.status("test -f ~/a2.txt") === 1, "a has b's home now");
  check(a.status("echo after > ~/a3.txt") === 0 && !a.log.slice(-400).includes("another tab"), "and saves again");
  check(dec.decode(a.get("~/b2.txt")) === "more\n", "b's file whole in a");
}

// a | and a > past ROC_CAP_MAX: the | spooled in the page, the > into the
// store as it is written; no spool left after the line
async function bigPipes() {
  const dec = new TextDecoder();
  const t = await boot();
  check(t.status("seq 1 100000 | wc -l > ~/o") === 0, "spooled pipe status");
  check(dec.decode(t.get("~/o")) === "100000\n", "wc reads the spooled pipe whole");
  check(t.status("seq 1 100000 | grep -c 9 > ~/o") === 0 && dec.decode(t.get("~/o")) === "40951\n", "grep over it");
  check(t.status("seq 1 60000 > ~/big") === 0, "a > past the capture");
  check(t.status("wc -c ~/big > ~/o") === 0 && dec.decode(t.get("~/o")) === "348894 /home/guest/big\n", "written whole");
  check(t.spools.live() === 0, "no spool left");
}

// Etapa 4: a script reads the site's files in the browser; the core waits
// with its stack unwound (Asyncify), nothing it did is done twice, keys
// typed meanwhile wait their turn, Ctrl-C stops the wait
async function remote() {
  const dec = new TextDecoder();
  const enc = new TextEncoder();
  const index =
    "/pub/\t0\t2026-08-28 10:00\t\n" +
    "/pub/kutta.md\t12\t2026-08-16 10:30\tKutta\n" +
    "/pub/b.md\t3\t2026-08-16 10:30\tB\n" +
    "/pub/gone.md\t5\t2026-08-16 10:30\tGone\n" +
    "/pub/lines.md\t18\t2026-08-16 10:30\tLines\n";
  const site = {
    "/pub/kutta.md": enc.encode("Kutta text!\n"),
    "/pub/b.md": enc.encode("bb\n"),
    "/pub/lines.md": enc.encode("first line\nsecond\n"),
  };
  const count = (hay, needle) => hay.split(needle).length - 1;

  let t = await boot({ index, site });
  t.put(
    "r.filo",
    `(out-write "before\n")
(def h (file-open "/pub/kutta.md"))
(out-write (file-read h 100))
(file-close h)
(out-write "after\n")`,
  );
  // an answer that comes at once still goes through the wait
  t.type("filo ~/r.filo\r");
  await t.settle();
  check(count(t.log, "before") === 1 && count(t.log, "Kutta text!") === 1, "read once, said once");
  check(count(t.log, "after") === 1, "the script went on after the wait");
  check(t.siteFiles.held() === 0, "the file let go at the close");

  // a slow answer: the keys typed meanwhile run after the script
  t.siteHold = true;
  t.type("filo ~/r.filo > ~/o1\r");
  await t.settle();
  check(t.core.waiting(), "the script waits");
  t.type("echo queued > ~/o2\r");
  check(t.get("~/o2") === null, "nothing runs while the script waits");
  t.siteHold = false;
  await t.releaseSite();
  check(dec.decode(t.get("~/o1")) === "before\nKutta text!\nafter\n", "the script's output whole, once");
  check(dec.decode(t.get("~/o2")) === "queued\n", "the queued line ran after");

  // read-file, a missing file, a builtin calling Filo back (map), a nested
  // run, a pipe
  t.put(
    "m.filo",
    `(out-write (read-file "/pub/b.md"))
(out-write (file-open "/pub/gone.md") "\n")
(out-write (str-join "," (map (fn (p) (int-text (byte-len (read-file p)))) (list "/pub/kutta.md" "/pub/b.md"))) "\n")`,
  );
  t.type("filo ~/m.filo > ~/o3\r");
  await t.settle();
  check(dec.decode(t.get("~/o3")) === "bb\nNo such file or directory\n12,3\n", "read-file, missing, map");
  t.put("n.filo", `(letv (st o e) (run (list "filo" "~/m.filo")) (out-write o))`);
  t.type("filo ~/n.filo | wc -l > ~/o4\r");
  await t.settle();
  check(dec.decode(t.get("~/o4")).trim() === "3", "a nested run in a pipe waits too");

  // deep: a wait at the bottom of a recursion inside a map
  t.put(
    "d.filo",
    `(def down (fn (n) (if (= n 0) (byte-len (read-file "/pub/kutta.md")) (+ 0 (nth (map (fn (x) (down (- n 1))) (list 1)) 0)))))
(out-write (int-text (down 30)))`,
  );
  t.type("filo ~/d.filo > ~/o5\r");
  await t.settle();
  check(dec.decode(t.get("~/o5")) === "12", "a wait thirty calls deep");

  // a write before the wait is kept once
  t.put(
    "w.filo",
    `(def o (file-open "~/w.txt" "w"))
(file-write o "one\n")
(file-write o (read-file "/pub/b.md"))
(file-close o)`,
  );
  t.type("filo ~/w.filo\r");
  await t.settle();
  check(dec.decode(t.get("~/w.txt")) === "one\nbb\n", "the write around the wait, once");

  // the utilities in Filo over site files, waiting the same way
  t.type("grep -c t /pub/kutta.md /pub/b.md > ~/o6; wc -l /pub/kutta.md >> ~/o6\r");
  await t.settle();
  check(dec.decode(t.get("~/o6")) === "/pub/kutta.md:1\n/pub/b.md:0\n1 /pub/kutta.md\n", "grep and wc over the site");

  // cp of two site files at once: each read while the core waits
  t.type("mkdir ~/got; cp /pub/kutta.md /pub/b.md ~/got\r");
  await t.settle();
  check(dec.decode(t.get("~/got/kutta.md")) === "Kutta text!\n" && dec.decode(t.get("~/got/b.md")) === "bb\n", "cp of two site files");

  // a body that comes in pieces: the script reads what has come and waits
  // for the rest, its first line out before the file has all arrived
  t.siteDrip = 5;
  t.dripHold = true;
  t.put("s.filo", `(def h (file-open "/pub/lines.md"))
(out-write "got:" (file-line h))
(out-write "then:" (if (is-nil (file-line h)) "end" "more") "\\n")`);
  t.type("filo ~/s.filo\r");
  await t.settle();
  for (let i = 0; i < 2; i++) {
    await t.releaseDrip(); // "first", " line": the line not whole yet
  }
  check(!t.log.includes("got:"), "no line before its end came");
  await t.releaseDrip(); // "\nseco": the first line whole, the second coming
  check(t.log.includes("got:first line") && !t.log.includes("then:"), "the first line before the end of the file");
  await t.releaseDrip(); // "nd\n"
  check(t.log.includes("then:more"), "then the second line");
  check(t.siteFiles.held() === 0, "let go at the end");
  // read-file and cp of a body whose length is known only at its end
  t.dripHold = false;
  t.put("w.filo", `(out-write (int-text (byte-len (read-file "/pub/kutta.md"))))`);
  t.type("filo ~/w.filo > ~/o7; cp /pub/b.md ~/b2.md\r");
  await t.settle();
  check(dec.decode(t.get("~/o7")) === "12" && dec.decode(t.get("~/b2.md")) === "bb\n", "read whole, piece by piece");
  // Ctrl-C while it waits for the next piece
  t.dripHold = true;
  const before = t.dripCancelled;
  t.type("filo ~/s.filo\r");
  await t.settle();
  await t.releaseDrip();
  t.type("\x03");
  await t.settle();
  check(t.core.idle() && t.siteFiles.held() === 0 && t.dripCancelled === before + 1, "stopped mid-file, the fetch cancelled");
  t.dripHold = false;
  t.siteDrip = 0;
  t.dripHeld.splice(0);

  // Ctrl-C while it waits: the script ends, what it said stays, the fetch
  // that answers later keeps nothing
  t = await boot({ index, site });
  t.put("r.filo", `(out-write "start\n") (out-write (read-file "/pub/kutta.md")) (out-write "never\n")`);
  t.siteHold = true;
  t.type("filo ~/r.filo\r");
  await t.settle();
  check(t.core.waiting(), "waiting before the Ctrl-C");
  t.type("\x03");
  await t.settle();
  check(!t.core.waiting() && t.core.idle(), "the Ctrl-C ended the wait");
  check(t.log.includes("Interrupted") && count(t.log, "start") === 1 && !t.log.includes("never"), "interrupted, said once");
  await t.releaseSite();
  check(t.siteFiles.held() === 0, "the late answer keeps nothing");
  t.siteHold = false;
  check(t.status("echo ok > /dev/null") === 0, "the shell goes on after");
  check((await t.statusAfterWait("filo ~/r.filo > /dev/null")) === 0, "and the same script runs whole later");
  t.siteHold = true;
  check((await t.statusAfterWait("filo ~/r.filo > /dev/null; true")) === -1, "a waiting line has no status yet");
  t.type("\x03");
  await t.settle();
  check(t.status("filo ~/nothere.filo 2> /dev/null") !== 0 && t.status("true") === 0, "status after a Ctrl-C");
}

// Etapa 6: the kept home, version 2 (empty directories, file times),
// version 1 still read, a refused save said and the old blob kept
async function persistence() {
  const dec = new TextDecoder();
  const enc = new TextEncoder();
  const v1 = enc.encode("msh-home 1\nw/imp.red\t9\nMOV 0, 1\n\n");
  let t = await boot({ home: v1 });
  check(t.log.includes("1 file back in /home/guest."), "v1 home restored");
  check(dec.decode(t.get("~/w/imp.red")) === "MOV 0, 1\n", "v1 file whole");

  t.type("mkdir -p ~/e/f\r");
  t.type("echo hi > ~/a.txt\r");
  t.type("touch -t 202601021530.45 ~/a.txt\r");
  const kept = dec.decode(t.kept);
  check(kept.startsWith("msh-home 2\n"), "saved as version 2");
  check(kept.includes("d\te\n") && kept.includes("d\te/f\n"), "empty directories kept");
  check(kept.includes("f\ta.txt\t3\t1767367845\nhi\n"), "the time touch gave, kept");
  check(kept.includes("f\tw/imp.red\t9\t-\n"), "a v1 file has no time");

  // a new visit from what was kept
  const blob = t.kept;
  t = await boot({ home: blob });
  t.put("mt.filo", '(letv (k z tm o) (file-stat "~/a.txt") (out-write (int-text tm)))');
  check(t.status("filo ~/mt.filo > ~/o") === 0, "time script");
  check(dec.decode(t.get("~/o")) === "1767367845", "the time came back");
  check(t.status("cd ~; find e > ~/o") === 0, "find e");
  check(dec.decode(t.get("~/o")) === "e\ne/f\n", "empty directories came back");

  // the browser refuses: said, and the blob kept before stays as it was
  const before = t.kept;
  t.storeFails = true;
  const said = t.type("echo more > ~/b.txt\r");
  check(said.includes("rocchetto: home: No space left on device; not kept"), "refused save said");
  check(t.kept === before, "old blob untouched");
  t.storeFails = false;

  // what the home can hold, declared
  const home = t.type("home\r");
  check(home.includes("Kept in this browser:") && home.includes("directories and file times too."), "home kept line");
  check(home.includes("In memory now:") && home.includes("of 64 files (512K each at most)."), "home memory line");
}

// Etapa 7/8: find, xargs and env, the utilities that run others
async function composition() {
  const t = await boot();
  const dec = new TextDecoder();
  check(t.status("mkdir -p ~/t/a/b") === 0, "mkdir -p");
  t.put("one.txt", "1\n", "~/t/a");
  t.put("two.txt", "22\n", "~/t/a/b");
  t.put("skip.log", "x\n", "~/t/a/b");
  check(t.status("cd ~/t; find . -name '*.txt' > ~/o") === 0, "find status");
  check(dec.decode(t.get("~/o")) === "./a/b/two.txt\n./a/one.txt\n", "find in byte order");
  check(t.status("cd ~/t; find a -type f -name '*.txt' | xargs cksum > ~/o") === 0, "find | xargs");
  const sums = dec.decode(t.get("~/o")).split("\n");
  check(sums.length === 3 && sums[0].endsWith(" 3 a/b/two.txt") && sums[1].endsWith(" 2 a/one.txt"), "cksum of both");
  check(t.status("cd ~/t; find a -name '*.log' -exec echo got {} + > ~/o") === 0, "find -exec +");
  check(dec.decode(t.get("~/o")) === "got a/b/skip.log\n", "exec + output");
  check(t.status("env W=7 env > ~/o") === 0, "env runs env");
  check(dec.decode(t.get("~/o")).includes("W=7\n"), "env passes the variable");
  check(t.status("printf 'x y z' | xargs -n 2 > ~/o") === 0, "xargs -n");
  check(dec.decode(t.get("~/o")) === "x y\nz\n", "xargs batches");
}

// test/fixtures/utilities.txt, as test_roc.c runs it: each case in an empty
// ~/fx, by /bin and by the sources, fetched from the site's /pub/filo/examples
async function fixtures() {
  const cases = parse(readFileSync(new URL("../fixtures/utilities.txt", import.meta.url), "utf8"));
  // the sources as the site serves them: /pub/filo/examples in its index, fetched
  const site = {};
  const dir = "/pub/filo/examples/";
  let index = INDEX + `/pub/filo/\t0\t\t\n${dir}\t0\t\t\n`;
  for (const f of readdirSync(new URL("../../commands/bin/", import.meta.url))) {
    if (!f.endsWith(".filo")) continue;
    const body = readFileSync(new URL(`../../commands/bin/${f}`, import.meta.url));
    site[dir + f] = body;
    index += `${dir}${f}\t${body.length}\t\t\n`;
  }
  for (const source of [false, true]) {
    const t = await boot(source ? { index, site } : {});
    if (source) {
      for (const f of Object.keys(site)) {
        const name = f.slice(dir.length, -5);
        t.type(`alias ${name}='filo ${f}'\r`);
      }
    }
    let failed = 0;
    for (const c of cases) {
      t.type("cd; rm -rf ~/fx; mkdir ~/fx; cd ~/fx\r");
      const got = t.status(`{ ${c.line}; } > ~/.fx.out 2> ~/.fx.err`);
      const out = t.get("~/.fx.out");
      const err = t.get("~/.fx.err");
      const okStatus = c.status === "+" ? got > 0 : got === Number(c.status);
      const okErr = (err != null && err.length > 0) === (c.err === "+");
      const shown = out == null ? "(none)" : escape(out);
      if (!okStatus || !okErr || shown !== c.out) {
        failed++;
        console.error(`fixture (${source ? "source" : "bytecode"}): ${c.line}\t${got}\t${okErr ? c.err : "?"}\t${shown}`);
      }
    }
    check(failed === 0, `fixtures by ${source ? "source" : "bytecode"}`);
  }
}

await statusAndData();
await fixtures();
await dataPackage();
await files();
await calls();
await handles();
await composition();
await persistence();
await remote();
await bigPipes();
await twoTabs();
await longScript();
done("wasm");
