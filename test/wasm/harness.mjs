// The real build/roc.wasm under Node, with the browser's imports faked:
// requests wait in a queue that pump() serves in order (no timers, no
// network), the clock is fixed, the kept home lives in memory, and a
// download hands its bytes to the test. What the shell drew is in log.
// The site's files a script opens come through web/suspend.mjs, the
// page's own glue: siteHold keeps their fetches until releaseSite().
import { readFileSync } from "node:fs";
import { keysFor, makeCore, makeSiteFiles, oneChunk, pageTurn } from "../../web/suspend.mjs";
import { makeSpools } from "../../web/spool.mjs";
import { makeHomeStore } from "../../web/store.mjs";

const enc = new TextEncoder();
const dec = new TextDecoder();

export const INDEX =
  "/pub/\t0\t2026-08-28 10:00\t\n" +
  "/pub/kutta.md\t12\t2026-08-16 10:30\tKutta\n";

// storage: a localStorage stand-in shared by the tabs that boot on it (the
// kept home through web/store.mjs, as the page keeps it)
export async function boot({ site = {}, index = INDEX, home = null, storage = null } = {}) {
  const bytes = readFileSync(process.env.ROC_WASM ?? new URL("../../build/roc.wasm", import.meta.url));
  let mem = null;
  const t = {
    log: "",
    requests: [],
    downloads: [],
    kept: home,
    now: Date.UTC(2026, 9, 4, 12, 0, 0),
    site: { "/.index": enc.encode(index), ...site },
    hold: false, // when set, requests wait for serve() from the test
    siteHold: false, // when set, a script's fetch of a site file waits for releaseSite()
    siteHeld: [],
    siteDrip: 0, // bytes a piece of a site body; 0 whole
    dripHold: false,
    dripHeld: [],
    dripCancelled: 0,
    fetched: [],
    storeFails: false,
    spools: makeSpools(),
    yieldReal: false,
    homeStore: storage ? makeHomeStore(storage) : null,
  };
  const text = (ptr, len) => dec.decode(new Uint8Array(mem.buffer, ptr, len).slice());
  const env = {
    host_request(id, ptr, len) {
      t.requests.push({ id, path: text(ptr, len) });
    },
    host_stream_open() {},
    host_stream_close() {},
    host_term_resize() {},
    host_pick_file() {},
    host_store_put(ptr, len) {
      if (t.storeFails) {
        return 1; // the browser refused: quota, or a private window
      }
      if (t.homeStore) {
        return t.homeStore.write(new Uint8Array(mem.buffer, ptr, len));
      }
      t.kept = new Uint8Array(mem.buffer, ptr, len).slice();
      return 0;
    },
    host_store_claim() {
      t.homeStore?.claim();
    },
    host_now() {
      return t.now;
    },
    host_tz() {
      return 0;
    },
    // the page's turn only where a test asks for it (yieldReal): the sync
    // helpers (status, get) expect a line to run to its end
    host_yield() {
      return t.yieldReal ? t.core.wait(pageTurn) : 0;
    },
    host_ticks() {
      return performance.now();
    },
    host_spool_new() {
      return t.spools.create();
    },
    host_spool_write(id, ptr, n) {
      return t.spools.write(id, new Uint8Array(mem.buffer, ptr, n));
    },
    host_spool_read(id, off, ptr, n) {
      return t.spools.read(id, off, new Uint8Array(mem.buffer, ptr, n));
    },
    host_spool_free(id) {
      t.spools.free(id);
    },
    host_site_wait(ptr, len) {
      return t.core.wait(() => t.siteFiles.start(text(ptr, len)));
    },
    host_site_read(ptr, len, off, buf, n) {
      const path = text(ptr, len);
      const r = t.core.wait(() => t.siteFiles.ready(path, off));
      return r <= 0 ? r : t.siteFiles.copy(path, off, new Uint8Array(mem.buffer, buf, n));
    },
    host_site_drop(ptr, len) {
      t.siteFiles.drop(text(ptr, len));
    },
    host_download(np, nl, ptr, len) {
      t.downloads.push({ name: text(np, nl), bytes: new Uint8Array(mem.buffer, ptr, len).slice() });
    },
  };
  const { instance } = await WebAssembly.instantiate(bytes, { env });
  const w = instance.exports;
  mem = w.memory;
  t.wasm = w;

  const io = () => new Uint8Array(mem.buffer, w.roc_w_iobuf(), w.roc_w_iobuf_cap());
  t.drain = () => {
    for (;;) {
      const n = w.roc_w_out_read();
      if (n === 0) return;
      t.log += dec.decode(new Uint8Array(mem.buffer, w.roc_w_outbuf(), n).slice(), { stream: true });
    }
  };
  t.core = makeCore(w, { onStep: t.drain });
  // a body in pieces of siteDrip bytes; with dripHold each waits for releaseDrip()
  const reader = (data) => {
    if (!t.siteDrip) {
      return oneChunk(data);
    }
    let at = 0;
    return {
      read: () => {
        if (at >= data.length) {
          return Promise.resolve({ done: true });
        }
        const piece = data.subarray(at, at + t.siteDrip);
        at += piece.length;
        if (!t.dripHold) {
          return Promise.resolve({ value: piece, done: false });
        }
        return new Promise((resolve) => t.dripHeld.push(() => resolve({ value: piece, done: false })));
      },
      cancel() {
        t.dripCancelled++;
      },
    };
  };
  t.siteFiles = makeSiteFiles((path) => {
    t.fetched.push(path);
    const data = t.site[path] ?? null;
    const answer = () => (data == null ? null : reader(data));
    if (!t.siteHold) {
      return answer();
    }
    return new Promise((resolve) => t.siteHeld.push(() => resolve(answer())));
  });
  // the next held piece of a dripping body
  t.releaseDrip = async () => {
    const next = t.dripHeld.shift();
    if (next) {
      next();
    }
    await t.settle();
  };
  // the held fetches answered, in order; then what they let run, run
  t.releaseSite = async () => {
    const held = t.siteHeld.splice(0);
    held.forEach((answer) => answer());
    await t.settle();
  };
  // until the core is idle again (waits answered) and requests served
  t.settle = async () => {
    for (let i = 0; i < 1000; i++) {
      await new Promise((r) => setImmediate(r));
      t.pump();
      if (t.core.idle() && t.siteHeld.length === 0) {
        return;
      }
      if ((t.siteHold && t.siteHeld.length > 0) || (t.dripHold && t.dripHeld.length > 0)) {
        return; // the test answers those
      }
    }
  };
  // bytes into the core through iobuf, a chunk at a time, each chunk set
  // and handed over as one step of the queue
  const feed = (data, call) => {
    const cap = w.roc_w_iobuf_cap();
    for (let off = 0; off < data.length; off += cap) {
      const chunk = data.subarray(off, Math.min(off + cap, data.length));
      t.core.call(() => {
        io().set(chunk);
        call(chunk.length);
      });
    }
  };
  t.serve = (req) => {
    const path = req.path;
    const home = () => (t.homeStore ? t.homeStore.read() : t.kept);
    const data = path === "/.home" ? home() : t.site[path];
    if (data == null) {
      t.core.call(() => w.roc_w_feed_fail(req.id));
    } else {
      feed(data, (len) => w.roc_w_feed(req.id, len));
      t.core.call(() => w.roc_w_feed_eof(req.id));
    }
    t.drain();
  };
  t.pump = () => {
    t.drain();
    while (!t.hold && t.requests.length > 0) {
      t.serve(t.requests.shift());
    }
  };
  t.tick = (ms) => {
    t.core.call(() => w.roc_w_tick(ms));
    t.pump();
  };
  // a line typed at the shell; what it drew since
  t.type = (line) => {
    const from = t.log.length;
    feed(enc.encode(keysFor(t.core, line)), (len) => w.roc_w_input(len));
    t.pump();
    return t.log.slice(from);
  };
  // a file into the home, as an upload from the page
  t.put = (name, data, dest = "~") => {
    const b = typeof data === "string" ? enc.encode(data) : data;
    const n = enc.encode(name);
    const d = enc.encode(dest);
    t.core.call(() => {
      io().set(n);
      io().set(d, n.length);
      if (w.roc_w_upload_begin(n.length, d.length, b.length) === 0) {
        throw new Error(`upload of ${name} refused`);
      }
    });
    feed(b, (len) => w.roc_w_upload_data(len));
    t.core.call(() => w.roc_w_upload_end());
    t.pump();
  };
  // a file of the home, byte for byte, through download
  t.get = (path) => {
    const before = t.downloads.length;
    t.type(`download ${path}\r`);
    return t.downloads.length > before ? t.downloads[t.downloads.length - 1].bytes : null;
  };
  // $? of a line that may wait for the site
  t.statusAfterWait = async (line) => {
    t.type(`${line}; echo $? > ~/.st\r`);
    await t.settle();
    const b = t.get("~/.st");
    return b == null ? -1 : parseInt(dec.decode(b), 10);
  };
  // $? after a line, read back through a file
  t.status = (line) => {
    t.type(`${line}; echo $? > ~/.st\r`);
    const b = t.get("~/.st");
    return b == null ? -1 : parseInt(dec.decode(b), 10);
  };

  const ident = new TextEncoder().encode("shell.test\nhttps://shell.test");
  t.core.call(() => {
    new Uint8Array(w.memory.buffer, w.roc_w_iobuf(), ident.length).set(ident);
    w.roc_w_init(80, 24, 1, ident.length);
  });
  t.pump();
  t.type("s\r"); // the menu's way to the shell
  t.type("cd\r");
  return t;
}

let failures = 0;
export function check(ok, what) {
  if (!ok) {
    failures++;
    console.error(`FAIL ${what}`);
  }
}
export function done(name) {
  if (failures > 0) {
    console.error(`${name}: ${failures} failure(s)`);
    process.exit(1);
  }
  console.log(`${name}: all tests passed`);
}
export const same = (a, b) =>
  a != null && b != null && a.length === b.length && a.every((x, i) => x === b[i]);
