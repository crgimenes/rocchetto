// web/store.mjs against storage that works, refuses, is missing, holds
// what was edited by hand, or is shared by two tabs. No browser: a Map
// stands in for localStorage.
import { HOME_KEY, makeHomeStore } from "../../web/store.mjs";

let failures = 0;
const check = (ok, what) => {
  if (!ok) {
    failures++;
    console.log(`FAIL ${what}`);
  }
};

class Storage {
  constructor(cap = Infinity) {
    this.map = new Map();
    this.cap = cap;
  }
  getItem(k) {
    return this.map.has(k) ? this.map.get(k) : null;
  }
  setItem(k, v) {
    if (v.length > this.cap) {
      const e = new Error("quota");
      e.name = "QuotaExceededError";
      throw e;
    }
    this.map.set(k, v);
  }
}

const bytes = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0));
const same = (a, b) => a !== null && a.length === b.length && a.every((x, i) => x === b[i]);

const s = new Storage();
const tab = makeHomeStore(s);
check(tab.read() === null, "first visit: nothing kept");
const blob = bytes("msh-home 2\nf\tbin\t4\t-\n\x00\xff\x80\n\n");
check(tab.write(blob) === 0, "kept");
check(same(makeHomeStore(s).read(), blob), "binary bytes come back whole");

// quota: the save is refused and the home kept before it stays
const small = new Storage(64);
const st = makeHomeStore(small);
const old = bytes("msh-home 2\nf\ta\t1\t-\nx\n");
check(st.write(old) === 0, "small home kept");
check(st.write(new Uint8Array(200)) === 1, "over quota refused");
check(same(makeHomeStore(small).read(), old), "the old home survives a refused save");

// no storage at all (blocked site data): nothing kept, nothing thrown
const none = makeHomeStore(null);
check(none.read() === null, "no storage reads as empty");
check(none.write(old) === 1, "no storage refuses the save");

// a value edited by hand that is not base64 starts an empty home; one
// written before generations reads as generation 0
const bad = new Storage();
bad.map.set(HOME_KEY, "not base64 %%%");
check(makeHomeStore(bad).read() === null, "garbage reads as empty");
const legacy = new Storage();
legacy.map.set(HOME_KEY, btoa("msh-home 1\n"));
const lt = makeHomeStore(legacy);
check(same(lt.read(), bytes("msh-home 1\n")) && lt.write(old) === 0, "a home from before generations");

// two tabs: the second to save after the first is refused, until it
// claims (keep) or reads again (reload)
const shared = new Storage();
const a = makeHomeStore(shared);
const b = makeHomeStore(shared);
a.read();
b.read();
check(a.write(bytes("A")) === 0, "tab a keeps");
check(b.write(bytes("B")) === 2, "tab b is refused: a saved since b read");
check(same(makeHomeStore(shared).read(), bytes("A")), "a's home is not wiped");
b.claim();
check(b.write(bytes("B")) === 0, "b keeps after claiming");
check(a.write(bytes("A2")) === 2, "now a is refused");
check(same(a.read(), bytes("B")) && a.write(bytes("A3")) === 0, "a reads again, then keeps");

if (failures > 0) {
  console.log(`store: ${failures} failure(s)`);
  process.exit(1);
}
console.log("store: all tests passed");
