#!/usr/bin/env python3
"""xctrace_audio: cycle statistics, events, window filtering, scheduling tables, bad input.

Synthetic exports only: a 480-frame cycle at 48 kHz has a 10 ms budget, so every
expected number below is a round one.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

TOOL = Path(__file__).resolve().parent / 'xctrace_audio.py'
MS = 1_000_000

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


def cell(tag, value, fmt=None):
    return '<%s fmt="%s">%s</%s>' % (tag, fmt if fmt is not None else value, value, tag)


def table(schema, cols, rows):
    head = ''.join('<col><mnemonic>%s</mnemonic></col>' % c for c in cols)
    body = ''.join('<row>%s</row>' % ''.join(r) for r in rows)
    return ('<?xml version="1.0"?><trace-query-result><node xpath="x"><schema name="%s">%s</schema>%s'
            '</node></trace-query-result>' % (schema, head, body))


def cycles(numbers, concepts=None):
    concepts = concepts or {}
    rows = [[cell('sample-time', (n - 1) * 10 * MS), cell('duration', MS // 2), cell('uint64', n),
             cell('uint32', 480), cell('event-concept', concepts.get(n, 'Normal'))] for n in numbers]
    return table('audio-hal-io-cycle', ['start', 'duration', 'cycle-number', 'frame-size', 'concept'], rows)


def ioproc(durations_ms):
    rows = [[cell('start-time', i * 10 * MS + 100_000), cell('duration', int(d * MS)),
             cell('process', 0, 'game (7)'), cell('event', 0, 'IOProc')] for i, d in enumerate(durations_ms)]
    return table('audio-hal-client-io-cycle-ioproc', ['start', 'duration', 'process', 'name'], rows)


def run(tmp, files, *args):
    paths = []
    for name, doc in files.items():
        p = Path(tmp) / name
        p.write_text(doc) if isinstance(doc, str) else p.write_bytes(doc)
        paths.append(str(p))
    r = subprocess.run([sys.executable, str(TOOL)] + paths + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


with tempfile.TemporaryDirectory() as tmp:
    # One overload in cycle 4: the HAL flags it and the client takes 12 ms of a 10 ms budget.
    files = {'c.xml': cycles(range(1, 7), {4: 'Overload'}), 'p.xml': ioproc([1, 1, 2, 12, 1, 1])}
    rc, out, err = run(tmp, files, '--json')
    check(rc == 0 and not err, 'overload run exits 0 quietly: rc=%s err=%r' % (rc, err))
    rep = json.loads(out)
    c = rep['cycles']
    check(c['n'] == 6 and c['first_cycle'] == 1 and c['last_cycle'] == 6, 'six cycles numbered 1..6')
    check(c['budget_ns']['p50'] == 10 * MS, 'budget is frame-size / rate = 10 ms')
    check(c['period_ns']['n'] == 5 and c['period_ns']['min'] == 10 * MS == c['period_ns']['max'], 'period 10 ms, five gaps')
    check(c['skipped_cycles'] == 0, 'no skipped cycle numbers')
    p = rep['clients']['audio-hal-client-io-cycle-ioproc']
    check(p['n'] == 6 and p['duration_ns']['max'] == 12 * MS and p['duration_ns']['p50'] == MS, 'ioproc duration percentiles')
    check(p['budget_use_pct']['max'] == 120.0 and p['over_budget'] == 1, 'ioproc budget use 120%, one over')
    check(rep['events_by_kind'] == {'client-over-budget': 1, 'concept': 1}, 'events by kind: %r' % rep['events_by_kind'])
    check(all(e['t'] == (30 * MS if e['kind'] == 'concept' else 30 * MS + 100_000) for e in rep['events']),
          'events carry the cycle timestamps')
    check(rep['worst_cycles'][0]['cycle'] == 4 and rep['worst_cycles'][0]['budget_use_pct'] == 120.0, 'worst cycle is 4')
    check(rep['clients_by_process'] == {'game (7)': {'n': 6, 'total_ns': 18 * MS, 'max_ns': 12 * MS}}, 'clients by process')
    check(rep['window']['from_ns'] == 0 and rep['window']['explicit'] is False, 'default window starts at the first cycle')

    rc2, out2, _ = run(tmp, files, '--json')
    check(out2 == out, 'output is deterministic')
    rc, txt, _ = run(tmp, files, '--wall-start', '2026-01-02T03:04:05+00:00')
    check(rc == 0 and 'overloads / glitches: 2 (client-over-budget 1, concept 1)' in txt, 'text report counts both events')
    check('0.030000 s (03:04:05.030)' in txt, 'text report maps trace time to wall clock')

    # Window: cycles 5 and 6 only, the overload is outside.
    rc, out, _ = run(tmp, files, '--json', '--from', '0.035', '--to', '0.06')
    rep = json.loads(out)
    check(rep['cycles']['n'] == 2 and rep['event_count'] == 0 and rep['window']['explicit'] is True,
          'window keeps cycles 5-6 and drops the overload: n=%s events=%s' % (rep['cycles']['n'], rep['event_count']))
    rc, out, _ = run(tmp, files, '--json', '--from', '0.03', '--to', '0.031')
    check(json.loads(out)['cycles']['n'] == 1 and json.loads(out)['event_count'] == 2, 'a window around cycle 4 keeps its two events')

    # A missing cycle number and a late cycle.
    rc, out, _ = run(tmp, {'c.xml': cycles([1, 2, 3, 5, 6])}, '--json')
    rep = json.loads(out)
    check(rep['cycles']['skipped_cycles'] == 1 and rep['events_by_kind'] == {'skipped-cycle': 1}, 'skipped cycle number is an event')

    # POI rows and zero-timestamp jitter.
    poi = table('audio-hal-poi', ['timestamp', 'description', 'concept', 'detail'], [
        [cell('event-time', 5 * MS), cell('narrative', 0, 'IO Overload on engine 3'), cell('event-concept', 'Normal'), '<sentinel/>'],
        [cell('event-time', 6 * MS), cell('narrative', 0, 'Zero Time Stamp'), cell('event-concept', 'Normal'), '<sentinel/>']])
    jit = table('audio-hal-zero-timestamp-jitter', ['timestamp', 'jitter', 'sample-rate'], [
        [cell('sample-time', 7 * MS), '<sentinel/>', cell('sampling-frequency', '48000.5', '48.000,5 Hz')],
        [cell('sample-time', 8 * MS), cell('duration', 500_000), cell('sampling-frequency', '48000.5', '48.000,5 Hz')],
        [cell('sample-time', 9 * MS), cell('duration', 90), cell('sampling-frequency', '48000.5', '48.000,5 Hz')]])
    rc, out, _ = run(tmp, {'c.xml': cycles([1, 2]), 'poi.xml': poi, 'j.xml': jit}, '--json')
    rep = json.loads(out)
    check(rep['events_by_kind'] == {'poi': 1, 'zero-timestamp-jitter': 1}, 'poi keyword + jitter above 100 us: %r' % rep['events_by_kind'])
    check(rep['poi'] == {'rows': 2, 'matching': 1} and rep['zero_timestamp_jitter']['n'] == 3, 'poi and jitter summaries')
    check(rep['cycles']['sample_rate_hz'] == 48000.5, 'sample rate comes from the jitter table')
    rc, out, _ = run(tmp, {'c.xml': cycles([1, 2]), 'poi.xml': table('audio-hal-poi', ['timestamp', 'description', 'concept', 'detail'], [])}, '--json')
    check(rc == 0 and json.loads(out)['poi'] == {'rows': 0, 'matching': 0}, 'an empty POI table is valid and means no events')

    # Thread states: clipped to the window; runnable latency from Runnable intervals.
    th = lambda name: cell('thread', 0, '%s (0x1) (proc, pid: 1)' % name)
    state = lambda t0, st, d: [cell('start-time', t0), th('mix'), cell('thread-state', st), cell('duration', d)]
    ts = table('thread-state', ['start', 'thread', 'state', 'duration'], [
        state(0, 'Runnable', 5_000), state(5_000, 'Running', 10_000), state(15_000, 'Blocked', 25_000_000),
        state(25_015_000, 'Runnable', 40_000), state(25_055_000, 'Running', 5_000), state(25_060_000, 'Blocked', 100_000_000)])
    rc, out, err = run(tmp, {'c.xml': cycles(range(1, 7)), 'ts.xml': ts}, '--json')
    rep = json.loads(out)['thread_state']
    t = rep['threads'][0]
    check(rc == 0 and t['thread'] == 'mix 0x1', 'thread label drops the process suffix: %r' % t['thread'])
    check((t['running_ns'], t['runnable_ns'], t['blocked_ns']) == (15_000, 45_000, 25_000_000 + 25_440_000),
          'state sums clipped to the HAL window: %r' % ((t['running_ns'], t['runnable_ns'], t['blocked_ns']),))
    check(t['runnable_latency_ns']['n'] == 2 and t['runnable_latency_ns']['max'] == 40_000, 'runnable latency')
    rc, out, _ = run(tmp, {'ts.xml': ts}, '--json', '--from', '0.03', '--to', '0.04')
    t = json.loads(out)['thread_state']['threads'][0]
    check(t['blocked_ns'] == 10 * MS and t['running_ns'] == 0 and t['runnable_latency_ns'] is None,
          'explicit window drops rows before it and clips the one across it: %r' % t)

    # Syscall cadence of an unnamed thread found by its stack.
    def call(t0, who, name, frames):
        bt = '<backtrace>%s</backtrace>' % ''.join('<frame name="%s"/>' % f for f in frames)
        return [cell('start-time', t0), cell('thread', 0, '%s (0x2) (proc, pid: 1)' % who), cell('syscall', 0, name),
                cell('duration', 6 * MS), bt]
    sc = table('syscall', ['start', 'thread', 'call', 'duration', 'backtrace'], [
        call(0, 'title', 'semwait_signal', ['__semwait_signal', 'usleep', 'audio_mix_thread_func']),
        call(6 * MS, 'title', 'semwait_signal', ['__semwait_signal']),
        call(12 * MS, 'title', 'semwait_signal', ['__semwait_signal']),
        call(0, 'other', 'semwait_signal', ['__semwait_signal', 'sleep'])])
    rc, out, err = run(tmp, {'sc.xml': sc}, '--json', '--stack', 'audio_mix', '--call', 'semwait_signal', '--expect-hz', '166.666667')
    rep = json.loads(out)
    rows = rep['syscalls']
    check(rc == 0 and len(rows) == 1 and rows[0]['thread'] == 'title 0x2', 'stack selects the mix thread only: %r' % rows)
    check(rows[0]['n'] == 3 and rows[0]['period_ns']['p50'] == 6 * MS and abs(rows[0]['rate_hz'] - 166.667) < 0.01,
          'cadence: three calls 6 ms apart')
    check(abs(rows[0]['drift_blocks']['min']) < 0.01 and abs(rows[0]['drift_blocks']['max']) < 0.01, 'no drift against the expected rate')
    check(rep['selected_by_stack'] == ['title 0x2'], 'selected_by_stack lists the thread')
    rc, out, err = run(tmp, {'c.xml': cycles([1])}, '--stack', 'x')
    check(rc == 2 and 'syscall' in err, 'stack without a syscall table is a usage error')

    # Bad input: a message on stderr, exit 2, never a traceback.
    good = cycles([1, 2])
    cases = {
        'truncated xml': {'c.xml': good[:len(good) // 2]},
        'not xml': {'c.xml': 'hello'},
        'no schema': {'c.xml': '<trace-query-result><node/></trace-query-result>'},
        'unsupported schema': {'c.xml': table('counters', ['start', 'x'], [[cell('t', 1), cell('t', 2)]])},
        'unsupported empty table': {'c.xml': table('counters', ['start', 'x'], [])},
        'missing column': {'c.xml': table('audio-hal-io-cycle', ['start', 'duration'], [[cell('t', 1), cell('t', 2)]])},
        'ref before id': {'c.xml': table('audio-hal-io-cycle', ['start', 'duration', 'cycle-number', 'frame-size', 'concept'],
                                         [['<sample-time ref="9"/>', cell('duration', 1), cell('uint64', 1), cell('uint32', 480),
                                           cell('event-concept', 'Normal')]])},
        'non-numeric value': {'c.xml': table('audio-hal-io-cycle', ['start', 'duration', 'cycle-number', 'frame-size', 'concept'],
                                             [[cell('sample-time', 'soon'), cell('duration', 1), cell('uint64', 1),
                                               cell('uint32', 480), cell('event-concept', 'Normal')]])},
        'client table without cycles': {'p.xml': ioproc([1])},
    }
    for what, files in cases.items():
        rc, out, err = run(tmp, files)
        check(rc == 2 and err.startswith('xctrace_audio:') and 'Traceback' not in err and out == '',
              '%s -> exit 2 with a message (rc=%s err=%r)' % (what, rc, err[:100]))
    missing = subprocess.run([sys.executable, str(TOOL), str(Path(tmp) / 'nope.xml')], capture_output=True, text=True)
    check(missing.returncode == 2 and 'Traceback' not in missing.stderr, 'missing file -> exit 2')
    for bad in ([], ['x.xml', '--from', '2', '--to', '1'], ['x.xml', '--wall-start', 'yesterday'], ['x.xml', '--expect-hz', '0']):
        r = subprocess.run([sys.executable, str(TOOL)] + bad, capture_output=True, text=True)
        check(r.returncode == 2 and 'Traceback' not in r.stderr, 'usage error %r -> exit 2' % bad)

    h = subprocess.run([sys.executable, str(TOOL), '--help'], capture_output=True, text=True)
    check(h.returncode == 0 and 'xcrun xctrace export' in h.stdout and '--from' in h.stdout and '--json' in h.stdout
          and 'audio-hal-io-cycle' in h.stdout, '--help documents the exports and options')

print('xctrace_audio: %s' % ('ok' if not failures else '%d failed' % failures))
sys.exit(1 if failures else 0)
