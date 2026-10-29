#!/usr/bin/env python3
"""Audio health report from `xcrun xctrace export` tables of an Audio System Trace.

  tools/xctrace_audio.py <export.xml>... [--from S] [--to S] [--thread SUBSTR]
                         [--stack SUBSTR] [--call NAME] [--expect-hz HZ] [--json]

Inputs: one XML per table (or several tables in one file), made with
  xcrun xctrace export --input audio.trace \\
      --xpath '/trace-toc/run[@number="1"]/data/table[@schema="<schema>"]' \\
      --output audio.<schema>.xml
Supported schemas:
  HAL cycles   audio-hal-io-cycle  audio-hal-client-io-cycle
               audio-hal-client-io-cycle-ioproc  audio-hal-io-cycle-client
               audio-hal-io-cycle-hardware  audio-hal-io-cycle-load
               audio-hal-client-io-cycle-load
  HAL events   audio-hal-poi  audio-hal-client-poi  audio-hal-zero-timestamp-jitter
  trace        time-info
  scheduling   thread-state  syscall  cpu-state   (large: tens of MB per second)
Only the HAL cycle tables are needed for the cycle report; the scheduling tables
add the title's thread states (running / runnable / preempted / blocked, runnable
latency), syscall cadence (for example a usleep-paced mix thread) and core load.

Output: a text report on stdout, or one JSON document with --json. All times are
seconds on the trace's own axis (what Instruments shows as 00:07.973.615);
--wall-start maps them to wall clock. Exit 0 on success, 2 on a usage error, an
unreadable or malformed export, an unsupported schema or a missing column.

Per IO cycle: the period to the next cycle, the budget (frame-size / sample rate),
the client IOProc time and its share of the budget. A cycle table row is an event
when its concept is not "Normal"; so is a POI row that mentions overload, underrun,
glitch, dropout, xrun or discontinuity, a client IOProc that takes the whole
budget, a skipped cycle number, a period more than --period-tol off the budget,
and a zero-timestamp jitter above --jitter-ns.

The window is --from/--to, else the span of the HAL cycle tables. Instruments
pads scheduling tables with placeholder rows before the recorded data (a
windowed recording keeps only its last seconds), so the default keeps them out.
"""
import argparse
import bisect
import collections
import datetime
import json
import math
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xctrace_xml import MalformedExport, iter_rows  # noqa: E402

CYCLE = 'audio-hal-io-cycle'
CLIENT_SCHEMAS = ('audio-hal-client-io-cycle-ioproc', 'audio-hal-client-io-cycle', 'audio-hal-io-cycle-client')
SCHEMAS = {
    CYCLE: ('start', 'duration', 'cycle-number', 'frame-size', 'concept'),
    'audio-hal-client-io-cycle': ('start', 'duration', 'process', 'concept'),
    'audio-hal-client-io-cycle-ioproc': ('start', 'duration', 'process'),
    'audio-hal-io-cycle-client': ('start', 'duration', 'client-process'),
    'audio-hal-io-cycle-hardware': ('start', 'duration', 'operation'),
    'audio-hal-io-cycle-load': ('start', 'duration', 'load', 'concept'),
    'audio-hal-client-io-cycle-load': ('start', 'duration', 'load', 'concept'),
    'audio-hal-poi': ('timestamp', 'description', 'concept', 'detail'),
    'audio-hal-client-poi': ('timestamp', 'description', 'concept', 'detail'),
    'audio-hal-zero-timestamp-jitter': ('timestamp', 'jitter', 'sample-rate'),
    'time-info': ('update-time', 'trace-start-time'),
    'thread-state': ('start', 'thread', 'state', 'duration'),
    'syscall': ('start', 'thread', 'call', 'duration', 'backtrace'),
    'cpu-state': ('start', 'cpu', 'state', 'duration', 'process'),
}
LOAD_SCHEMAS = ('audio-hal-io-cycle-load', 'audio-hal-client-io-cycle-load')
POI_SCHEMAS = ('audio-hal-poi', 'audio-hal-client-poi')
BIG = ('thread-state', 'syscall', 'cpu-state')
# Read after the HAL cycle tables, so the default window (their span) is known.
DEFERRED = BIG + LOAD_SCHEMAS + POI_SCHEMAS + ('audio-hal-zero-timestamp-jitter',)
TROUBLE = re.compile(r'overload|underrun|under-run|glitch|dropout|drop-out|xrun|discontinuit', re.I)
STATES = ('Running', 'Runnable', 'Preempted', 'Blocked', 'Interrupted')
FAR = 1 << 62


class Bad(Exception):
    """A usage error or an input that cannot be analysed; reported, exit 2."""


def pct(v, p):
    """Nearest-rank percentile of the sorted list v."""
    return v[max(0, min(len(v) - 1, math.ceil(p / 100 * len(v)) - 1))]


def stats(vals):
    if not vals:
        return None
    v = sorted(vals)
    return {'n': len(v), 'min': v[0], 'p50': pct(v, 50), 'p90': pct(v, 90), 'p99': pct(v, 99),
            'max': v[-1], 'sum': sum(v)}


def num(node, what):
    """The integer value of a column, or None for the empty <sentinel/>."""
    if node.tag == 'sentinel':
        return None
    if node.text is None:
        raise ValueError('column %s holds no value' % what)
    try:
        return int(node.text)
    except ValueError:
        raise ValueError('column %s holds %r, not an integer' % (what, node.text)) from None


def peek_schema(path):
    """(name, mnemonics) of the first table in the file, rows or not."""
    for _, el in ET.iterparse(path, events=('end',)):
        if el.tag == 'schema':
            return el.get('name'), [c.findtext('mnemonic') for c in el.findall('col')]
    raise Bad('%s: no <schema> element: not an xctrace table export' % path)


def check_schema(path, name, cols):
    if name not in SCHEMAS:
        raise Bad("%s: unsupported schema '%s' (supported: %s)" % (path, name, ', '.join(sorted(SCHEMAS))))
    missing = [c for c in SCHEMAS[name] if c not in cols]
    if missing:
        raise Bad("%s: schema '%s' has no column %s (found: %s); export made by a different "
                  "Instruments version?" % (path, name, ', '.join(missing), ', '.join(cols)))


def label(fmt):
    """'name (0xtid) (proc, pid: N)' -> 'name (0xtid)'."""
    m = re.match(r'^(.*) \((0x[0-9a-fA-F]+)\) \(', fmt)
    return '%s %s' % (m.group(1), m.group(2)) if m else fmt


def core_short(name):
    """'CPU 4 (P Core)' -> 'P4'."""
    m = re.match(r'CPU (\d+) \((\w)', name)
    return '%s%s' % (m.group(2), m.group(1)) if m else name


class Collector:
    def __init__(self, args):
        self.args = args
        self.lo = int(args.t_from * 1e9) if args.t_from is not None else None
        self.hi = int(args.t_to * 1e9) if args.t_to is not None else None
        self.files = []
        self.cycles = []
        self.clients = {s: [] for s in CLIENT_SCHEMAS}
        self.hw = []
        self.concepts = []            # (t, schema, concept text)
        self.load_buckets = collections.Counter()
        self.pois = []                # (t, schema, text, concept)
        self.jitter = []              # (t, jitter ns or None, rate Hz)
        self.time_info = None
        self.threads = collections.defaultdict(lambda: {
            'state_ns': collections.Counter(), 'runnable': [], 'preempted': []})
        self.poi_seen = False
        self.calls = collections.defaultdict(list)       # (thread, call) -> [(start, dur)]
        self.stack_threads = set()
        self.cores = collections.defaultdict(collections.Counter)
        self.procs = collections.Counter()
        self.bounds = [FAR, -FAR]     # first/last timestamp seen in rows of this window

    # -- window ---------------------------------------------------------
    def inside(self, t):
        return (self.lo is None or t >= self.lo) and (self.hi is None or t < self.hi)

    def clip(self, s, d):
        a = s if self.lo is None else max(s, self.lo)
        e = s + d if self.hi is None else min(s + d, self.hi)
        return max(0, e - a)

    def seen(self, t, end=None):
        self.bounds[0] = min(self.bounds[0], t)
        self.bounds[1] = max(self.bounds[1], end if end is not None else t)

    # -- rows -----------------------------------------------------------
    def feed(self, schema, r):
        if schema == 'time-info':
            self.time_info = r['trace-start-time'].fmt
            return
        if schema in POI_SCHEMAS:
            t = num(r['timestamp'], 'timestamp')
            if self.inside(t):
                self.seen(t)
                text = ' '.join(x for x in (r['description'].fmt, r['concept'].fmt, r['detail'].fmt) if x)
                self.pois.append((t, schema, text, r['concept'].fmt or ''))
            return
        if schema == 'audio-hal-zero-timestamp-jitter':
            t = num(r['timestamp'], 'timestamp')
            if self.inside(t):
                self.seen(t)
                rate = r['sample-rate'].text
                self.jitter.append((t, num(r['jitter'], 'jitter'), float(rate) if rate else None))
            return
        t = num(r['start' if 'start' in r else 'update-time'], 'start')
        d = (num(r['duration'], 'duration') if 'duration' in r else 0) or 0
        if schema in BIG:
            # A state or core interval counts when it overlaps the window.
            if (self.hi is not None and t >= self.hi) or (self.lo is not None and t + d <= self.lo):
                return
            self.feed_big(schema, r, t, d)
            return
        if not self.inside(t):
            return
        if schema not in LOAD_SCHEMAS:      # 100 ms buckets span the whole trace, not the recorded data
            self.seen(t, t + d)
        concept = r['concept'].fmt if 'concept' in r else None
        if concept and concept.lower() != 'normal':
            self.concepts.append((t, schema, concept))
        if schema == CYCLE:
            self.cycles.append({'start': t, 'dur': d, 'cycle': num(r['cycle-number'], 'cycle-number'),
                                'frame': num(r['frame-size'], 'frame-size')})
        elif schema in CLIENT_SCHEMAS:
            who = r['client-process' if schema == 'audio-hal-io-cycle-client' else 'process'].fmt
            self.clients[schema].append((t, d, who or 'Unknown'))
        elif schema == 'audio-hal-io-cycle-hardware':
            self.hw.append((t, d, r['operation'].fmt))
        elif schema in LOAD_SCHEMAS:
            self.load_buckets[(schema, num(r['load'], 'load'))] += 1

    def feed_big(self, schema, r, t, d):
        if schema == 'thread-state':
            c = self.clip(t, d)
            self.seen(t if self.lo is None else max(t, self.lo), t + d if self.hi is None else min(t + d, self.hi))
            ent = self.threads[r['thread'].fmt]
            ent['state_ns'][r['state'].fmt] += c
            if self.inside(t):
                if r['state'].fmt == 'Runnable':
                    ent['runnable'].append(d)
                elif r['state'].fmt == 'Preempted':
                    ent['preempted'].append(d)
        elif schema == 'syscall':
            if not self.inside(t):
                return
            self.seen(t, t + d)
            who = r['thread'].fmt
            self.calls[(who, r['call'].fmt)].append((t, d))
            if self.args.stack and who not in self.stack_threads:
                bt = r['backtrace']
                for f in bt.children:
                    if any(s in f.attrib.get('name', '') for s in self.args.stack):
                        self.stack_threads.add(who)
                        break
        else:
            c = self.clip(t, d)
            self.seen(t if self.lo is None else max(t, self.lo), t + d if self.hi is None else min(t + d, self.hi))
            self.cores[r['cpu'].fmt][r['state'].fmt] += c
            if r['state'].fmt == 'Running':
                self.procs[r['process'].fmt] += c


def read_file(col, path):
    n = 0
    checked = set()
    try:
        for schema, r in iter_rows(path):
            if schema not in checked:
                check_schema(path, schema, list(r))
                checked.add(schema)
            n += 1
            col.feed(schema, r)
        if not checked:
            name, cols = peek_schema(path)
            check_schema(path, name, cols)
            checked.add(name)
    except (ET.ParseError, MalformedExport, OSError, ValueError) as e:
        raise Bad('%s: %s' % (path, e)) from None
    col.poi_seen = col.poi_seen or bool(checked & set(POI_SCHEMAS))
    col.files.append({'path': str(path), 'schemas': sorted(checked), 'rows': n})


def first_schema(path):
    try:
        name, cols = peek_schema(path)
    except (ET.ParseError, OSError) as e:
        raise Bad('%s: %s' % (path, e)) from None
    check_schema(path, name, cols)
    return name


# -- analysis ---------------------------------------------------------------

def analyse_cycles(col, args, rate):
    cyc = sorted(col.cycles, key=lambda c: c['start'])
    if not cyc:
        return None, [], []
    events = []
    starts = [c['start'] for c in cyc]
    skipped = 0
    periods = []
    for c in cyc:
        c['budget'] = round(c['frame'] / rate * 1e9) if c['frame'] else None
        c['period'] = None
    for a, b in zip(cyc, cyc[1:]):
        gap = b['cycle'] - a['cycle'] - 1
        if gap > 0:
            skipped += gap
            events.append({'t': b['start'], 'kind': 'skipped-cycle', 'source': CYCLE,
                           'detail': '%d cycle number(s) missing before %d' % (gap, b['cycle'])})
        elif gap == 0:
            a['period'] = b['start'] - a['start']
            periods.append(a['period'])
            if a['budget'] and abs(a['period'] - a['budget']) > args.period_tol * a['budget']:
                events.append({'t': b['start'], 'kind': 'period-off-budget', 'source': CYCLE,
                               'detail': 'period %.3f ms vs budget %.3f ms' % (a['period'] / 1e6, a['budget'] / 1e6)})
    frames = collections.Counter(c['frame'] for c in cyc)
    out = {
        'n': len(cyc), 'first_cycle': cyc[0]['cycle'], 'last_cycle': cyc[-1]['cycle'],
        'frame_sizes': {str(k): v for k, v in sorted(frames.items())},
        'sample_rate_hz': rate,
        'budget_ns': stats([c['budget'] for c in cyc if c['budget']]),
        'period_ns': stats(periods), 'hal_duration_ns': stats([c['dur'] for c in cyc]),
        'skipped_cycles': skipped,
    }
    return out, events, starts


def analyse_clients(col, cyc, starts, args):
    out, events, per_proc = {}, [], {}
    primary = None
    for schema in CLIENT_SCHEMAS:
        rows = sorted(col.clients[schema])
        if not rows:
            continue
        uses, durs = [], []
        over = 0
        for t, d, who in rows:
            durs.append(d)
            i = bisect.bisect_right(starts, t) - 1
            budget = cyc[i]['budget'] if i >= 0 else None
            if budget:
                uses.append(d / budget * 100)
                if d >= budget:
                    over += 1
                    events.append({'t': t, 'kind': 'client-over-budget', 'source': schema,
                                   'detail': '%s took %.3f ms of a %.3f ms budget' % (who, d / 1e6, budget / 1e6)})
        out[schema] = {'n': len(rows), 'duration_ns': stats(durs), 'over_budget': over,
                       'budget_use_pct': ({'p50': round(pct(sorted(uses), 50), 3), 'p99': round(pct(sorted(uses), 99), 3),
                                           'max': round(max(uses), 3)} if uses else None)}
        if primary is None:
            primary = schema
            for t, d, who in rows:
                e = per_proc.setdefault(who, {'n': 0, 'total_ns': 0, 'max_ns': 0})
                e['n'] += 1
                e['total_ns'] += d
                e['max_ns'] = max(e['max_ns'], d)
    worst = []
    if primary:
        for t, d, who in col.clients[primary]:
            i = bisect.bisect_right(starts, t) - 1
            if i >= 0 and cyc[i]['budget']:
                worst.append((d / cyc[i]['budget'], t, d, cyc[i]))
        worst.sort(key=lambda w: (-w[0], w[1]))
        worst = [{'t': t, 'cycle': c['cycle'], 'client_ns': d, 'period_ns': c['period'], 'hal_ns': c['dur'],
                  'budget_use_pct': round(u * 100, 3)} for u, t, d, c in worst[:args.worst]]
    return out, events, per_proc, primary, worst


def pick_thread(fmt, args, stack_threads):
    if not args.thread and not args.stack:
        return True
    return fmt in stack_threads or any(s in fmt for s in args.thread)


def analyse_threads(col, args):
    if not col.threads:
        return None
    win = (col.hi if col.hi is not None else col.bounds[1]) - (col.lo if col.lo is not None else col.bounds[0])
    rows = []
    for fmt, ent in col.threads.items():
        if not pick_thread(fmt, args, col.stack_threads):
            continue
        s = ent['state_ns']
        rows.append({'thread': label(fmt), 'running_ns': s['Running'], 'runnable_ns': s['Runnable'],
                     'preempted_ns': s['Preempted'], 'blocked_ns': s['Blocked'],
                     'interrupted_ns': s['Interrupted'],
                     'other_ns': sum(v for k, v in s.items() if k not in STATES),
                     'runnable_latency_ns': stats(ent['runnable']), 'preempted_ns_stats': stats(ent['preempted'])})
    rows.sort(key=lambda r: (-r['running_ns'], r['thread']))
    if not args.thread and not args.stack:
        rows = rows[:args.top]
    alll = [d for e in col.threads.values() for d in e['runnable']]
    return {'window_ns': win, 'threads': rows, 'all_threads_runnable_latency_ns': stats(alll),
            'thread_count': len(col.threads)}


def analyse_calls(col, args):
    if not col.calls:
        return None
    rows = []
    for (who, call), ev in col.calls.items():
        if (args.thread or args.stack) and not pick_thread(who, args, col.stack_threads):
            continue
        if args.call and call not in args.call:
            continue
        ev.sort()
        starts = [s for s, _ in ev]
        wakes = [s + d for s, d in ev]
        row = {'thread': label(who), 'call': call, 'n': len(ev), 'total_ns': sum(d for _, d in ev),
               'duration_ns': stats([d for _, d in ev]),
               'period_ns': stats([b - a for a, b in zip(starts, starts[1:])])}
        if len(ev) > 1:
            row['rate_hz'] = round((len(ev) - 1) / ((wakes[-1] - wakes[0]) / 1e9), 3)
            if args.expect_hz:
                drift = [j - (w - wakes[0]) / 1e9 * args.expect_hz for j, w in enumerate(wakes)]
                row['drift_blocks'] = {'expect_hz': args.expect_hz, 'min': round(min(drift), 3),
                                       'max': round(max(drift), 3)}
        rows.append(row)
    rows.sort(key=lambda r: (-r['total_ns'], r['thread'], r['call']))
    if not (args.thread or args.stack or args.call):
        rows = rows[:args.top]
    return rows


def analyse_cpus(col, win):
    if not col.cores:
        return None
    cores = {}
    for name, c in col.cores.items():
        cores[name] = {'running_pct': round(100 * c['Running'] / win, 2) if win > 0 else None,
                       'interrupt_pct': round(100 * (c['Handling Interrupt']) / win, 2) if win > 0 else None}
    procs = [{'process': p, 'cores_busy': round(v / win, 3) if win > 0 else None}
             for p, v in sorted(col.procs.items(), key=lambda kv: (-kv[1], kv[0]))[:6]]

    def core_key(n):
        m = re.match(r'CPU (\d+)', n)
        return (int(m.group(1)) if m else 1 << 30, n)
    return {'cores': dict(sorted(cores.items(), key=lambda kv: core_key(kv[0]))), 'top_processes': procs}


def build(col, args):
    rate = None
    jit_rates = sorted(r for _, _, r in col.jitter if r)
    if jit_rates:
        rate = jit_rates[len(jit_rates) // 2]
    rate = rate or args.sample_rate
    rep = {'files': col.files}
    first, last = col.bounds
    rep['window'] = {
        'from_ns': col.lo if col.lo is not None else (first if first != FAR else None),
        'to_ns': col.hi if col.hi is not None else (last if last != -FAR else None),
        'data_first_ns': first if first != FAR else None, 'data_last_ns': last if last != -FAR else None,
        'explicit': args.t_from is not None or args.t_to is not None, 'trace_start': col.time_info,
        'wall_start': args.wall_start}
    cyc_out, events, starts = analyse_cycles(col, args, rate)
    rep['cycles'] = cyc_out
    cyc = sorted(col.cycles, key=lambda c: c['start'])
    if cyc_out:
        clients, cev, procs, primary, worst = analyse_clients(col, cyc, starts, args)
        events += cev
    else:
        clients, procs, primary, worst = {}, {}, None, []
        if any(col.clients.values()):
            raise Bad('client tables need audio-hal-io-cycle (the cycle table) for periods and budgets')
    rep['clients'] = clients
    rep['client_primary'] = primary
    rep['clients_by_process'] = dict(sorted(procs.items()))
    rep['worst_cycles'] = worst
    if col.hw:
        by = collections.defaultdict(list)
        for _, d, op in col.hw:
            by[op].append(d)
        rep['hardware'] = {op: stats(v) for op, v in sorted(by.items())}
    else:
        rep['hardware'] = None
    rep['poi'] = {'rows': len(col.pois), 'matching': sum(1 for p in col.pois if TROUBLE.search(p[2]))} \
        if col.poi_seen else None
    for t, schema, text, concept in col.pois:
        if TROUBLE.search(text) or (concept and concept.lower() != 'normal'):
            events.append({'t': t, 'kind': 'poi', 'source': schema, 'detail': text})
    for t, schema, concept in col.concepts:
        events.append({'t': t, 'kind': 'concept', 'source': schema, 'detail': concept})
    if col.jitter:
        js = [j for _, j, _ in col.jitter if j is not None]
        rep['zero_timestamp_jitter'] = {'n': len(col.jitter), 'jitter_ns': stats(js),
                                        'above_threshold_ns': args.jitter_ns}
        for t, j, _ in col.jitter:
            if j is not None and j > args.jitter_ns:
                events.append({'t': t, 'kind': 'zero-timestamp-jitter', 'source': 'audio-hal-zero-timestamp-jitter',
                               'detail': 'jitter %d ns' % j})
    else:
        rep['zero_timestamp_jitter'] = None
    rep['load_buckets'] = {'%s=%d' % k: v for k, v in sorted(col.load_buckets.items())} or None
    events.sort(key=lambda e: (e['t'], e['kind'], e['source'], e['detail']))
    rep['events'] = events
    rep['event_count'] = len(events)
    rep['events_by_kind'] = dict(sorted(collections.Counter(e['kind'] for e in events).items()))
    rep['thread_state'] = analyse_threads(col, args)
    rep['syscalls'] = analyse_calls(col, args)
    win = (rep['window']['to_ns'] or 0) - (rep['window']['from_ns'] or 0)
    rep['cpu'] = analyse_cpus(col, win)
    rep['selected_by_stack'] = sorted(label(t) for t in col.stack_threads) if args.stack else None
    return rep


# -- text -------------------------------------------------------------------

def ms(ns, d=3):
    return ('%.' + str(d) + 'f') % (ns / 1e6)


def us(ns):
    return '%.1f' % (ns / 1e3)


def secs(ns):
    return '%.6f' % (ns / 1e9)


def wall(ns, rep):
    if not rep['window']['wall_start']:
        return ''
    t = datetime.datetime.fromisoformat(rep['window']['wall_start']) + datetime.timedelta(microseconds=ns // 1000)
    return ' (%s)' % t.strftime('%H:%M:%S.%f')[:-3]


def dist(s, f=ms, unit='ms'):
    if not s:
        return 'none'
    return 'n=%d min %s p50 %s p90 %s p99 %s max %s %s' % (
        s['n'], f(s['min']), f(s['p50']), f(s['p90']), f(s['p99']), f(s['max']), unit)


def render(rep, args):
    o = []
    w = rep['window']
    o.append('files:')
    for f in rep['files']:
        o.append('  %s  %s  %d rows' % (f['path'], ','.join(f['schemas']), f['rows']))
    if w['from_ns'] is None:
        o.append('window: no rows in range')
    else:
        how = 'explicit' if w['explicit'] else 'span of the HAL cycle tables' if rep['cycles'] else 'span of the data'
        o.append('window: %s .. %s s, %.3f s, %s' % (
            secs(w['from_ns']), secs(w['to_ns']), (w['to_ns'] - w['from_ns']) / 1e9, how))
        if w['wall_start']:
            o.append('  wall clock: %s .. %s' % (wall(w['from_ns'], rep).strip(' ()'), wall(w['to_ns'], rep).strip(' ()')))
    if w['trace_start']:
        o.append('trace start (time-info, as Instruments formats it): %s' % w['trace_start'])

    c = rep['cycles']
    if c:
        o.append('')
        o.append('IO cycles: %d (numbers %d..%d), frame sizes %s, sample rate %.3f Hz' % (
            c['n'], c['first_cycle'], c['last_cycle'],
            ', '.join('%s x%d' % kv for kv in c['frame_sizes'].items()), c['sample_rate_hz']))
        o.append('  budget  %s' % dist(c['budget_ns']))
        o.append('  period  %s' % dist(c['period_ns']))
        o.append('  HAL cycle duration  %s' % dist(c['hal_duration_ns'], us, 'us'))
        o.append('  skipped cycle numbers: %d' % c['skipped_cycles'])
        for schema in CLIENT_SCHEMAS:
            s = rep['clients'].get(schema)
            if s:
                u = s['budget_use_pct']
                o.append('client %s: n=%d' % (schema, s['n']))
                o.append('  duration  %s' % dist(s['duration_ns'], us, 'us'))
                if u:
                    o.append('  budget use  p50 %.2f%%  p99 %.2f%%  max %.2f%%   over budget: %d' % (
                        u['p50'], u['p99'], u['max'], s['over_budget']))
        if rep['clients_by_process']:
            o.append('clients by process (%s):' % rep['client_primary'])
            for who, e in rep['clients_by_process'].items():
                o.append('  %-32s n=%d total %s ms max %s us' % (who, e['n'], ms(e['total_ns']), us(e['max_ns'])))
        if rep['worst_cycles']:
            o.append('busiest client cycles (cycle, at s, client us, period ms, HAL us, budget use):')
            for r in rep['worst_cycles']:
                o.append('  %d  %s%s  %s  %s  %s  %.2f%%' % (
                    r['cycle'], secs(r['t']), wall(r['t'], rep), us(r['client_ns']),
                    ms(r['period_ns']) if r['period_ns'] else '-', us(r['hal_ns']), r['budget_use_pct']))
    if rep['hardware']:
        for op, s in rep['hardware'].items():
            o.append('hardware %s: %s' % (op, dist(s, us, 'us')))
    if rep['poi']:
        o.append('POI rows: %d, mentioning overload/underrun/glitch/...: %d' % (rep['poi']['rows'], rep['poi']['matching']))
    if rep['zero_timestamp_jitter']:
        z = rep['zero_timestamp_jitter']
        o.append('zero timestamp events: %d, %d with a jitter value (the first has none); flagged above %d ns' % (
            z['n'], z['jitter_ns']['n'] if z['jitter_ns'] else 0, z['above_threshold_ns']))
        if z['jitter_ns']:
            o.append('  jitter  %s' % dist(z['jitter_ns'], lambda v: '%d' % v, 'ns'))
    if rep['load_buckets']:
        o.append('load buckets (100 ms; schema=cycles in the bucket:buckets): %s' % ', '.join(
            '%s:%d' % kv for kv in rep['load_buckets'].items()))

    o.append('')
    if rep['event_count']:
        o.append('overloads / glitches: %d (%s)' % (rep['event_count'], ', '.join(
            '%s %d' % kv for kv in rep['events_by_kind'].items())))
        for e in rep['events'][:args.max_events]:
            o.append('  %s s%s  %-22s %s' % (secs(e['t']), wall(e['t'], rep), e['kind'], e['detail']))
        if rep['event_count'] > args.max_events:
            o.append('  ... %d more (raise --max-events or use --json)' % (rep['event_count'] - args.max_events))
    else:
        o.append('overloads / glitches: 0')

    t = rep['thread_state']
    if t:
        o.append('')
        o.append('thread states over %.3f s (%d threads in the table%s):' % (
            t['window_ns'] / 1e9, t['thread_count'],
            '' if args.thread or args.stack else ', top %d by running time' % args.top))
        if rep['selected_by_stack']:
            o.append('  selected by stack %s: %s' % (args.stack, ', '.join(rep['selected_by_stack'])))
        o.append('  %-42s %9s %9s %9s %9s %9s | runnable latency us: n  p50  p99  max' % (
            'thread', 'run ms', 'rnbl ms', 'preempt', 'block ms', 'intr ms'))
        for r in t['threads']:
            rl = r['runnable_latency_ns']
            o.append('  %-42s %9s %9s %9s %9s %9s | %6d %5s %5s %6s' % (
                r['thread'][:42], ms(r['running_ns'], 1), ms(r['runnable_ns'], 2), ms(r['preempted_ns'], 2),
                ms(r['blocked_ns'], 1), ms(r['interrupted_ns'], 2),
                rl['n'] if rl else 0, us(rl['p50']) if rl else '-', us(rl['p99']) if rl else '-',
                us(rl['max']) if rl else '-'))
        al = t['all_threads_runnable_latency_ns']
        if al:
            o.append('  all %d threads, runnable->running latency  %s' % (t['thread_count'], dist(al, us, 'us')))

    s = rep['syscalls']
    if s:
        o.append('')
        o.append('syscalls (call cadence per thread):')
        for r in s:
            o.append('  %s  %s  n=%d total %s ms' % (r['thread'], r['call'], r['n'], ms(r['total_ns'])))
            o.append('    duration  %s' % dist(r['duration_ns'], us, 'us'))
            if r['period_ns']:
                o.append('    period    %s   rate %s Hz' % (dist(r['period_ns']), r.get('rate_hz')))
            if 'drift_blocks' in r:
                d = r['drift_blocks']
                o.append('    drift vs %.3f Hz: %+.2f .. %+.2f calls' % (d['expect_hz'], d['min'], d['max']))

    cpu = rep['cpu']
    if cpu:
        o.append('')
        o.append('cores (running / interrupt, % of the window):')
        o.append('  ' + '  '.join('%s %.0f/%.0f' % (core_short(n), v['running_pct'], v['interrupt_pct'])
                                  for n, v in cpu['cores'].items() if v['running_pct'] is not None))
        o.append('  busy cores by process: ' + ', '.join('%s %.2f' % (p['process'], p['cores_busy'])
                                                          for p in cpu['top_processes']))
    return '\n'.join(o)


# -- cli --------------------------------------------------------------------

def parse_args(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Needed for the cycle report: audio-hal-io-cycle plus at least one client table '
               '(audio-hal-client-io-cycle-ioproc is the most direct). Worth adding: '
               'audio-hal-io-cycle-client, audio-hal-client-io-cycle, audio-hal-poi, '
               'audio-hal-zero-timestamp-jitter, and thread-state / syscall / cpu-state for scheduling. '
               'Example: xcrun xctrace export --input audio.trace --xpath '
               '\'/trace-toc/run[@number="1"]/data/table[@schema="audio-hal-io-cycle"]\' '
               '--output audio.audio-hal-io-cycle.xml')
    ap.add_argument('exports', nargs='+', metavar='export.xml', help='xctrace table export(s)')
    ap.add_argument('--from', dest='t_from', type=float, metavar='S',
                    help='window start, seconds on the trace axis (default: first HAL cycle)')
    ap.add_argument('--to', dest='t_to', type=float, metavar='S',
                    help='window end, seconds on the trace axis (default: end of the last HAL cycle)')
    ap.add_argument('--wall-start', metavar='ISO8601',
                    help='wall-clock time of trace second 0 (for example 2026-10-29T15:45:10.238+01:00); '
                         'adds wall-clock times to the report')
    ap.add_argument('--sample-rate', type=float, default=48000.0, metavar='HZ',
                    help='sample rate for the budget when no zero-timestamp-jitter table gives it (default 48000)')
    ap.add_argument('--period-tol', type=float, default=0.2, metavar='F',
                    help='a cycle period this fraction off the budget is an event (default 0.2)')
    ap.add_argument('--jitter-ns', type=int, default=100000, metavar='NS',
                    help='zero-timestamp jitter above this is an event (default 100000)')
    ap.add_argument('--thread', action='append', default=[], metavar='SUBSTR',
                    help='thread-state/syscall rows: keep threads whose label contains SUBSTR (repeatable)')
    ap.add_argument('--stack', action='append', default=[], metavar='SUBSTR',
                    help='syscall table: also keep threads with a frame containing SUBSTR in any syscall '
                         'backtrace (finds an unnamed thread by its entry function, for example audio_mix_thread_func)')
    ap.add_argument('--call', action='append', default=[], metavar='NAME',
                    help='syscall table: keep this call only (for example semwait_signal, the usleep syscall; repeatable)')
    ap.add_argument('--expect-hz', type=float, metavar='HZ',
                    help='syscall cadence: expected calls per second; reports the drift of the call count against it')
    ap.add_argument('--top', type=int, default=8, help='rows per thread/syscall table without a filter (default 8)')
    ap.add_argument('--worst', type=int, default=5, help='busiest client cycles listed (default 5)')
    ap.add_argument('--max-events', type=int, default=40, help='events listed in text (default 40)')
    ap.add_argument('--json', action='store_true', help='one JSON document instead of text')
    args = ap.parse_args(argv)
    if args.t_from is not None and args.t_to is not None and args.t_to <= args.t_from:
        ap.error('--to must be greater than --from')
    if args.wall_start:
        try:
            datetime.datetime.fromisoformat(args.wall_start)
        except ValueError:
            ap.error('--wall-start is not ISO 8601: %s' % args.wall_start)
    if args.expect_hz is not None and args.expect_hz <= 0:
        ap.error('--expect-hz must be positive')
    return args


def run(args):
    col = Collector(args)
    firsts = [(first_schema(p), p) for p in args.exports]
    if (args.stack or args.call or args.expect_hz) and not any(s == 'syscall' for s, _ in firsts):
        raise Bad('--stack/--call/--expect-hz need a syscall table export')
    for name, path in firsts:
        if name not in DEFERRED:
            read_file(col, path)
    if col.lo is None and col.hi is None and col.bounds[0] != FAR:
        col.lo, col.hi = col.bounds
    for name, path in firsts:
        if name in DEFERRED:
            read_file(col, path)
    return build(col, args)


def main(argv=None):
    args = parse_args(argv)
    try:
        rep = run(args)
    except Bad as e:
        print('xctrace_audio: %s' % e, file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(rep, indent=1, sort_keys=True))
    else:
        print(render(rep, args))
    return 0


if __name__ == '__main__':
    sys.exit(main())
