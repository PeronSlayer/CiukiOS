"""F4-36 deterministic single-CPU scheduling model (no OS priority privileges).

A higher-priority virtual thread owns 10 ms in every 20 ms. Pump wakeups
remain 2 ms and run at the next available CPU instant. Reader yields put it
at the tail of the normal run queue: the `peers` parameter gives the number of normal
10 ms quanta before its next dispatch. This is an explicit sensitivity
parameter, not a measurement of the T23 scheduler. Decoder CPU cost comes
from the real scalar benchmark, in microseconds per output PCM frame.
"""
# SPDX-License-Identifier: MIT
import argparse
import json


def run(*, legacy, cost_us, scale, peers=3, competitor=True, seconds=60):
    rate, ring, step = 48000, 131072, 100  # microseconds
    count, queued, decoded, audible = 4800, 0, 0, 0
    batch = 1024 if legacy else 4096
    pending, work, next_reader, next_pump = 0, 0., 0, 0
    writes = repaints = starves = empty = turns = gaps = 0
    in_gap = False
    buffering = False
    starve_start = last_starve = None
    was_starved = False
    next_ui = 0
    shown = None
    rows = []
    qmin, qmax, low, high, total, samples = 2048, 0, ring, 0, 0, 0
    previous = (0, 0, 0, 0)
    for now in range(0, seconds * 1000000, step):
        # The DAC clock continues while either virtual thread owns the CPU.
        used = min(queued, rate * step / 1000000)
        if audible and used < rate * step / 1000000:
            if not in_gap:
                gaps += 1
            in_gap = True
        elif used:
            in_gap = False
        queued -= used
        audible += used
        busy = competitor and now % 20000 < 10000
        if not busy:
            if now >= next_pump:
                turns += 1
                empty += count == 0
                starving = count == 0 and queued == 0
                if starving:
                    if not was_starved:
                        starves += 1
                        starve_start = now
                    last_starve = now
                    if legacy or now - starve_start >= 300000:
                        buffering = True
                else:
                    starve_start = None
                    if legacy or last_starve is None or now - last_starve >= 2000000:
                        buffering = False
                was_starved = starving
                qmin = min(qmin, int(queued))
                limit = 256 if legacy or not writes else 2048
                if queued < (256 if legacy else 1792):
                    n = min(count, int(limit - queued))
                    if n > 0:
                        count -= n
                        queued += n
                        writes += 1
                next_pump = now + 2000
                qmin, qmax = min(qmin, int(queued)), max(qmax, int(queued))
                low, high = min(low, count), max(high, count)
                total += count
                samples += 1
            # Decode work is interruptible at the virtual CPU step. A yield
            # forfeits dispatch opportunities, not decoder CPU instructions.
            if now >= next_reader and count < ring:
                if not pending:
                    pending = min(batch, ring - count)
                    work = pending * cost_us * scale
                work -= step
                if work <= 0:
                    count += pending
                    decoded += pending
                    pending = 0
                    if legacy or count > ring // 2:
                        next_reader = now + peers * 20000
            if now >= next_ui:
                # Legacy main polls at 10 Hz and dirties even identical views.
                displayed = (int(audible / rate), int(audible * 1000 / (rate * 180)), buffering)
                if legacy or displayed != shown:
                    repaints += 1
                    shown = displayed
                next_ui = now + (100000 if legacy or buffering else 500000)
        if (now + step) % 1000000 == 0:
            before = previous
            rows.append(dict(second=(now + step)//1000000, reservoir=int(count),
                             reader_fps=decoded-before[0], writes=writes-before[1],
                             queued_min=qmin, queued_max=qmax, repaints=repaints-before[2],
                             position=round(audible/rate, 4)))
            previous = (decoded, writes, repaints, starves)
            qmin, qmax = 2048, 0
    return dict(policy='before' if legacy else 'after', scale=scale,
                competitor_pct=50 if competitor else 0, normal_quanta=peers,
                starvations_per_minute=starves * 60 / seconds,
                device_gaps_per_minute=gaps * 60 / seconds,
                position_seconds=round(audible/rate, 4), wall_seconds=seconds,
                repaints_per_second=repaints/seconds,
                reader_fps=decoded/seconds, writes_per_second=writes/seconds,
                reservoir_min=low, reservoir_avg=total//max(samples, 1), reservoir_max=high,
                empty_pump_pct=round(100*empty/max(turns, 1), 2), per_second=rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cost-us', type=float, required=True)
    p.add_argument('--policy', choices=['before', 'after', 'both'], default='both')
    p.add_argument('--seconds', type=int, default=60)
    p.add_argument('--output')
    args = p.parse_args()
    results = [run(legacy=legacy, cost_us=args.cost_us, scale=scale,
                   peers=peers, competitor=competitor, seconds=args.seconds)
               for legacy in (True, False) if args.policy == 'both' or args.policy == ('before' if legacy else 'after')
               for scale in (10, 30)
               for competitor, peers in ((False, 1), (True, 1), (True, 3))]
    if args.output:
        with open(args.output, 'w') as f:
            json.dump(results, f, indent=2)
    for result in results:
        print(json.dumps({k: v for k, v in result.items() if k != 'per_second'}, sort_keys=True))


if __name__ == '__main__':
    main()
