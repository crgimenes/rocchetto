// The system's utilities as the oracle for utilities.txt: each case runs in
// /bin/sh, LC_ALL=C and TZ=UTC, in a directory of its own.
//   node test/fixtures/oracle.mjs check utilities.txt   the cases that differ
//   node test/fixtures/oracle.mjs record lines.txt      a case per line
// A case is "line<TAB>status<TAB>stderr<TAB>stdout": status a number or +
// (any but 0), stderr - (none) or + (some), stdout with \n \t \\ \xHH.
// A line starting with ! is the roc's own (the reason in a # line above
// it): check leaves it alone.
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

export function escape(bytes) {
    let s = '';
    for (const b of bytes) {
        if (b === 10) s += '\\n';
        else if (b === 9) s += '\\t';
        else if (b === 92) s += '\\\\';
        else if (b < 32 || b > 126) s += '\\x' + b.toString(16).padStart(2, '0');
        else s += String.fromCharCode(b);
    }
    return s;
}

export function parse(text) {
    const cases = [];
    for (const raw of text.split('\n')) {
        if (raw === '' || raw.startsWith('#')) continue;
        const f = raw.split('\t');
        if (f.length !== 4) throw new Error(`not a case: ${raw}`);
        const own = f[0].startsWith('!');
        cases.push({ line: own ? f[0].slice(1) : f[0], own, status: f[1], err: f[2], out: f[3] });
    }
    return cases;
}

function run(line) {
    const dir = mkdtempSync(join(tmpdir(), 'oracle-'));
    const r = spawnSync('/bin/sh', ['-c', line], {
        cwd: dir,
        env: { PATH: '/usr/bin:/bin', LC_ALL: 'C', TZ: 'UTC', HOME: dir },
        input: '',
    });
    rmSync(dir, { recursive: true, force: true });
    return { status: String(r.status), err: r.stderr.length > 0 ? '+' : '-', out: escape(r.stdout) };
}

const [mode, file] = process.argv.slice(2);
if (mode === 'record') {
    for (const line of readFileSync(file, 'utf8').split('\n')) {
        if (line === '' || line.startsWith('#')) {
            console.log(line);
            continue;
        }
        const r = run(line);
        console.log([line, r.status, r.err, r.out].join('\t'));
    }
} else if (mode === 'check') {
    let differ = 0;
    for (const c of parse(readFileSync(file, 'utf8'))) {
        if (c.own) continue;
        const r = run(c.line);
        const statusOk = c.status === '+' ? r.status !== '0' : r.status === c.status;
        if (!statusOk || r.err !== c.err || r.out !== c.out) {
            differ++;
            console.log(`${c.line}\n  want ${c.status} ${c.err} ${c.out}\n  got  ${r.status} ${r.err} ${r.out}`);
        }
    }
    console.log(`${differ} differ`);
    process.exitCode = differ > 0 ? 1 : 0;
} else if (mode !== undefined) {
    console.error('usage: node test/fixtures/oracle.mjs check|record file');
    process.exitCode = 2;
}
