// Spools for a | larger than the core holds: bytes kept here, in the
// page's memory, written to their end and read back by position. The core
// names one by a number; 0 is none (it could not be made).
export function makeSpools() {
  const spools = new Map();
  let next = 1;
  return {
    create() {
      const id = next++;
      spools.set(id, { bytes: new Uint8Array(1 << 16), len: 0 });
      return id;
    },
    write(id, data) {
      const s = spools.get(id);
      if (!s) {
        return 0;
      }
      if (s.len + data.length > s.bytes.length) {
        let cap = s.bytes.length;
        while (cap < s.len + data.length) {
          cap *= 2;
        }
        const bigger = new Uint8Array(cap);
        bigger.set(s.bytes.subarray(0, s.len));
        s.bytes = bigger;
      }
      s.bytes.set(data, s.len);
      s.len += data.length;
      return 1;
    },
    read(id, off, dst) {
      const s = spools.get(id);
      if (!s || off >= s.len) {
        return 0;
      }
      const piece = s.bytes.subarray(off, Math.min(s.len, off + dst.length));
      dst.set(piece);
      return piece.length;
    },
    free(id) {
      spools.delete(id);
    },
    live: () => spools.size,
  };
}
