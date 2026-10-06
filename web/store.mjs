// The kept home lives in this browser, under one key, as base64 (storage
// holds strings, the blob is bytes) after a generation, "g<n>:": the
// number of saves so far. One setItem replaces the value whole or throws
// and leaves it as it was, so a save that fails (quota, a private window)
// never costs the home kept before it. Each tab remembers the generation
// it read or saved: when another tab saved since, this tab's save is
// refused (2) instead of wiping that one, until home keep (claim) or home
// reload (read again).
export const HOME_KEY = "roc.home";

function parse(value) {
  const m = /^g(\d+):(.*)$/s.exec(value);
  return m ? { gen: Number(m[1]), b64: m[2] } : { gen: 0, b64: value };
}

export function makeHomeStore(storage) {
  let mine = 0;
  const stored = () => {
    try {
      const v = storage.getItem(HOME_KEY);
      return v === null ? null : parse(v);
    } catch {
      return null;
    }
  };
  return {
    // the kept blob, or null: a first visit, storage that cannot be read,
    // or a value that is not base64 (edited by hand) start an empty home
    read() {
      const s = stored();
      mine = s ? s.gen : 0;
      if (!s) {
        return null;
      }
      try {
        return Uint8Array.from(atob(s.b64), (c) => c.charCodeAt(0));
      } catch {
        return null;
      }
    },
    // 0 kept, 1 the browser refused, 2 another tab saved since
    write(bytes) {
      const s = stored();
      const now = s ? s.gen : 0;
      if (now !== mine) {
        return 2;
      }
      let text = "";
      for (const b of bytes) {
        text += String.fromCharCode(b);
      }
      try {
        storage.setItem(HOME_KEY, `g${now + 1}:${btoa(text)}`);
      } catch {
        return 1;
      }
      mine = now + 1;
      return 0;
    },
    // home keep: this tab's next save goes over whatever is kept
    claim() {
      const s = stored();
      mine = s ? s.gen : 0;
    },
  };
}
