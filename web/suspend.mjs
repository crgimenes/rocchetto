// The core runs one call at a time, and a call may stop in the middle: a
// script waiting for one of the site's files (host_site_wait). Asyncify
// unwinds the core's stack back here; the page fetches, then calls the
// same thing again, which rewinds into the wait and gets the answer. What
// arrives meanwhile (keys, ticks, responses) waits its turn in order: the
// core is never entered twice, and the iobuf a stopped call still reads is
// never written over. A queued thunk sets its bytes and calls the export;
// run again on the rewind, it sets the same bytes.
export function makeCore(w, { onStep = () => {} } = {}) {
  if (typeof w.asyncify_get_state !== "function") {
    // a build without Asyncify: nothing can wait, a site file is not here
    return {
      call(thunk) {
        thunk();
        onStep();
      },
      wait: () => -1,
      waiting: () => false,
      idle: () => true,
      cancel() {},
    };
  }
  const data = w.roc_w_async_data();
  const size = w.roc_w_async_size();
  const queue = [];
  let busy = false;
  let pending = null; // { promise, cancel } of the wait in progress
  let answer = 0;

  // true when the thunk stopped in a wait (the core holds it until then)
  function enter(thunk) {
    thunk();
    onStep();
    if (w.asyncify_get_state() !== 1) {
      return false;
    }
    w.asyncify_stop_unwind();
    const wait = pending;
    wait.promise.then((x) => {
      pending = null;
      answer = x;
      w.asyncify_start_rewind(data);
      if (!enter(thunk)) {
        busy = false;
        run();
      }
    });
    return true;
  }

  function run() {
    while (queue.length > 0) {
      busy = true;
      if (enter(queue.shift())) {
        return;
      }
    }
    busy = false;
  }

  return {
    // every call that can change the core goes through here
    call(thunk) {
      queue.push(thunk);
      if (!busy) {
        run();
      }
    },
    // inside an import: start() gives the answer now (a number), or
    // { promise, cancel } to wait for it with the core's stack unwound
    wait(start) {
      if (w.asyncify_get_state() === 2) {
        w.asyncify_stop_rewind();
        return answer;
      }
      const r = start();
      if (typeof r === "number") {
        return r;
      }
      const head = new Uint32Array(w.memory.buffer, data, 2);
      head[0] = data + 8;
      head[1] = data + size;
      pending = r;
      w.asyncify_start_unwind(data);
      return 0;
    },
    waiting: () => pending !== null,
    idle: () => !busy,
    // Ctrl-C while a script waits: the wait answers -2 and the script ends
    cancel() {
      if (pending !== null) {
        pending.cancel();
      }
    },
  };
}

// The site's files a script opened, as the page receives them: open(path)
// gives a reader of the body a piece at a time ({ value, done } from
// read(), and cancel()), or null when the site has none. The script reads
// what has come and waits for the rest; the file is let go at its last
// close.
export function makeSiteFiles(open) {
  const files = new Map(); // path -> { buf, len, done, failed, waiters, refs, reader }
  const wake = (f) => {
    for (const w of [...f.waiters]) {
      w();
    }
  };
  const grow = (f, piece) => {
    if (f.len + piece.length > f.buf.length) {
      let cap = f.buf.length;
      while (cap < f.len + piece.length) {
        cap *= 2;
      }
      const bigger = new Uint8Array(cap);
      bigger.set(f.buf.subarray(0, f.len));
      f.buf = bigger;
    }
    f.buf.set(piece, f.len);
    f.len += piece.length;
  };
  const pump = async (f) => {
    try {
      for (;;) {
        const { value, done } = await f.reader.read();
        if (done) {
          break;
        }
        grow(f, value);
        wake(f);
      }
    } catch {
      f.failed = true;
    }
    f.done = true;
    wake(f);
  };
  // a promise settled once: the first answer counts (a cancel, or the site)
  const once = () => {
    let resolve;
    let settled = false;
    const promise = new Promise((r) => {
      resolve = r;
    });
    const settle = (x) => {
      if (!settled) {
        settled = true;
        resolve(x);
      }
    };
    return { promise, settle, settled: () => settled };
  };
  return {
    // -3 once the file is coming (its length known at its end), -1 none
    start(path) {
      const have = files.get(path);
      if (have) {
        have.refs++;
        return -3;
      }
      const w = once();
      Promise.resolve()
        .then(() => open(path))
        .then(
          (reader) => {
            if (w.settled()) {
              reader?.cancel?.();
              return;
            }
            if (reader == null) {
              w.settle(-1);
              return;
            }
            const f = { buf: new Uint8Array(4096), len: 0, done: false, failed: false, waiters: new Set(), refs: 1, reader };
            files.set(path, f);
            pump(f);
            w.settle(-3);
          },
          () => w.settle(-1),
        );
      return { promise: w.promise, cancel: () => w.settle(-2) };
    },
    // bytes there at off now (1 or more), 0 at the end, -1 an error, or a
    // wait for the next piece
    ready(path, off) {
      const f = files.get(path);
      if (!f) {
        return -1;
      }
      const now = () => (f.len > off ? f.len - off : f.done ? (f.failed ? -1 : 0) : null);
      if (now() !== null) {
        return now();
      }
      const w = once();
      const waiter = () => {
        if (now() !== null) {
          f.waiters.delete(waiter);
          w.settle(now());
        }
      };
      f.waiters.add(waiter);
      return {
        promise: w.promise,
        cancel: () => {
          f.waiters.delete(waiter);
          w.settle(-2);
        },
      };
    },
    copy(path, off, dst) {
      const f = files.get(path);
      if (!f || off >= f.len) {
        return 0;
      }
      const piece = f.buf.subarray(off, Math.min(f.len, off + dst.length));
      dst.set(piece);
      return piece.length;
    },
    drop(path) {
      const f = files.get(path);
      if (f && --f.refs <= 0) {
        files.delete(path);
        if (!f.done) {
          f.reader.cancel?.();
        }
      }
    },
    held: () => files.size,
  };
}

// A whole body as a reader of one piece (a test, or a fetch without body).
export function oneChunk(bytes) {
  let given = false;
  return {
    read: async () => {
      if (given) {
        return { done: true };
      }
      given = true;
      return { value: bytes, done: false };
    },
    cancel() {},
  };
}

// The page's turn in a long script: back once what waits has run (0), or
// at a Ctrl-C (-2), as a wait the core takes through core.wait.
export function pageTurn() {
  let resolve;
  let settled = false;
  const promise = new Promise((r) => {
    resolve = r;
  });
  const settle = (x) => {
    if (!settled) {
      settled = true;
      resolve(x);
    }
  };
  setTimeout(() => settle(0), 0);
  return { promise, cancel: () => settle(-2) };
}

// Keys typed while a script waits for the site: Ctrl-C stops the wait
// (and is spent on it); the rest go to the core when it is free again.
export function keysFor(core, text) {
  if (!core.waiting() || !text.includes("\x03")) {
    return text;
  }
  core.cancel();
  return text.replaceAll("\x03", "");
}
