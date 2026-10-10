#!/usr/bin/env python3
"""Check src/shfolderproxy/d2d_arc.c (Direct2D arcs as quadratic curves) on random arcs.

Builds tests/test_arc.c for the host (with ASan/UBSan) and checks every arc against an
independent construction: the centre is found from the two unit circles through the end
points (in the ellipse's own axes), the one whose sweep in the asked direction is large or
small as asked. The curves must start and end on the ellipse at the right points, turn the
right way through the right angle in steps of at most 30 degrees, stay within 0.061% of the
radius of the ellipse and join without corners. Fixed cases cover the sweep direction
(clockwise means increasing angles with y pointing down, as in Direct2D), half circles as
Altium's schematic draws them, radii too small to reach the end point, and the arcs that
become straight lines (zero radius, end point = start point, NaN).

    tests/test_arc.py
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.environ.get("CC", "clang")
STEP_MAX = math.pi / 6
BULGE_MAX = (math.cos(STEP_MAX / 2) + 1 / math.cos(STEP_MAX / 2)) / 2 - 1  # 6.04e-4


def f32(x):
    """x rounded to a float, as the driver reads it."""
    return struct.unpack("f", struct.pack("f", x))[0]


def run(exe, cases):
    text = "".join(" ".join(repr(v) if isinstance(v, float) else str(v) for v in c) + "\n" for c in cases)
    out = subprocess.run([exe], input=text, capture_output=True, text=True, check=True).stdout.splitlines()
    assert len(out) == len(cases), (len(out), len(cases))
    curves = []
    for line in out:
        v = line.split()
        n = int(v[0])
        pts = [f32(float(x)) for x in v[1:]]
        curves.append([((pts[4 * i], pts[4 * i + 1]), (pts[4 * i + 2], pts[4 * i + 3])) for i in range(n)])
    return curves


def check(case, curves):
    """Problems with one arc's curves (empty if none)."""
    fx, fy, tx, ty, rx, ry, rot, cw, large = case
    fx, fy, tx, ty, rx, ry, rot = (f32(v) for v in (fx, fy, tx, ty, rx, ry, rot))
    rx, ry = abs(rx), abs(ry)
    c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    if (fx, fy) == (tx, ty):
        return [f"{len(curves)} curves for an arc that ends where it starts"] if curves else []

    def to_unit(p):  # ellipse axes, radii 1 (before scaling up)
        x, y = p
        return ((c * x + s * y) / rx, (-s * x + c * y) / ry)

    # the two unit circles through both end points (radii scaled up until there is one)
    q0, q1 = to_unit((fx, fy)), to_unit((tx, ty))
    mx, my = (q0[0] + q1[0]) / 2, (q0[1] + q1[1]) / 2
    hx, hy = (q1[0] - q0[0]) / 2, (q1[1] - q0[1]) / 2
    half = math.hypot(hx, hy)
    scale = max(1.0, half)
    h = math.sqrt(max(0.0, 1 - (half / scale) ** 2)) * scale
    px, py = -hy / half, hx / half
    preferred, borderline = [], []
    for sign in (1, -1):
        cx, cy = mx + sign * h * px, my + sign * h * py
        a0 = math.atan2(q0[1] - cy, q0[0] - cx)
        a1 = math.atan2(q1[1] - cy, q1[0] - cx)
        sweep = (a1 - a0) % (2 * math.pi)
        if not cw:
            sweep -= 2 * math.pi
        if (abs(sweep) > math.pi) == bool(large):
            preferred.append((cx, cy, a0, sweep))
        elif abs(abs(sweep) - math.pi) < 1e-3:  # a half ellipse, give or take float rounding: either
            borderline.append((cx, cy, a0, sweep))

    # float coordinates: allow a few float steps of the largest coordinate, in units of the radius
    span = max(abs(fx), abs(fy), abs(tx), abs(ty), rx * scale, ry * scale)
    tol = 2e-6 * span / (min(rx, ry) * scale) + 1e-9

    def problems_around(cx, cy, a0, sweep):
        def polar(p):
            u = to_unit(p)
            return math.hypot(u[0] - cx, u[1] - cy) / scale, math.atan2(u[1] - cy, u[0] - cx)

        problems = []
        if not 1 <= len(curves) <= 12:
            return [f"{len(curves)} curves"]
        if curves[-1][1] != (tx, ty):
            problems.append(f"ends at {curves[-1][1]}, not {(tx, ty)}")
        start = (fx, fy)
        turned, prev_angle = 0.0, a0
        for i, (ctrl, end) in enumerate(curves):
            r, angle = polar(end)
            if abs(r - 1) > tol:
                problems.append(f"curve {i} ends {r - 1:+.2e} radii off the ellipse")
            step = (angle - prev_angle + math.pi) % (2 * math.pi) - math.pi
            if (step > 0) != bool(cw) or abs(step) > STEP_MAX + tol:
                problems.append(f"curve {i} turns {math.degrees(step):.3f} degrees")
            turned += step
            prev_angle = angle
            for k in range(1, 16):
                t = k / 16
                p = tuple((1 - t) ** 2 * start[j] + 2 * t * (1 - t) * ctrl[j] + t * t * end[j] for j in (0, 1))
                r, _ = polar(p)
                if not -tol <= r - 1 <= BULGE_MAX + tol:
                    problems.append(f"curve {i} at t={t} is {r - 1:+.2e} radii off the ellipse")
                    break
            if i + 1 < len(curves):  # no corner: the controls on both sides are in line with the joint
                nxt = curves[i + 1][0]
                ax, ay = end[0] - ctrl[0], end[1] - ctrl[1]
                bx, by = nxt[0] - end[0], nxt[1] - end[1]
                la, lb = math.hypot(ax, ay), math.hypot(bx, by)
                if la and lb and (abs(ax * by - ay * bx) / (la * lb) > 50 * tol or ax * bx + ay * by <= 0):
                    problems.append(f"corner after curve {i}")
            start = end
        if abs(turned - sweep) > 1e-3 + 10 * tol:
            problems.append(f"turns {math.degrees(turned):.3f} degrees, not {math.degrees(sweep):.3f}")
        return problems

    found = [problems_around(*centre) for centre in preferred + borderline]
    return [] if [] in found else found[0]


def main():
    random.seed(int(os.environ.get("SEED", "1")))
    cases = []
    for _ in range(20000):
        r = 10 ** random.uniform(-2, 3)
        rx, ry = r, r if random.random() < 0.5 else r * 10 ** random.uniform(-1, 1)
        cx, cy = random.uniform(-1000, 1000), random.uniform(-1000, 1000)
        rot = random.choice([0.0, 0.0, 90.0, random.uniform(-360, 360)])
        t0 = random.uniform(-math.pi, math.pi)
        t1 = t0 + random.choice([random.uniform(-2 * math.pi, 2 * math.pi), math.pi, -math.pi, 1e-3])
        cs, sn = math.cos(math.radians(rot)), math.sin(math.radians(rot))

        def on(t):
            x, y = rx * math.cos(t), ry * math.sin(t)
            return cx + cs * x - sn * y, cy + sn * x + cs * y

        (fx, fy), (tx, ty) = on(t0), on(t1)
        k = random.choice([1.0, 1.0, 1.0, 0.5, 0.1])  # radii too small: scaled up
        cases.append((fx, fy, tx, ty, rx * k, ry * k, rot, random.randint(0, 1), random.randint(0, 1)))

    fixed = [  # (case, check on the curves)
        ((10.0, 0.0, 10.0, 4.0, 2.0, 2.0, 0.0, 1, 0), lambda cs: max(p[0] for c in cs for p in c) > 11.99),
        ((10.0, 0.0, 10.0, 4.0, 2.0, 2.0, 0.0, 0, 0), lambda cs: min(p[0] for c in cs for p in c) < 8.01),
        # Altium: a 0-180 degree schematic arc, angles negated for y pointing down, counterclockwise
        ((5.0, 0.0, -5.0, 0.0, 5.0, 5.0, 0.0, 0, 1), lambda cs: min(p[1] for c in cs for p in c) < -4.99),
        ((0.0, 0.0, 100.0, 0.0, 1.0, 1.0, 0.0, 1, 0), lambda cs: min(p[1] for c in cs for p in c) < -49.9),
        ((0.0, 0.0, 0.0, 10.0, 0.0, 5.0, 0.0, 1, 0), lambda cs: cs == []),
        ((3.0, 4.0, 3.0, 4.0, 5.0, 5.0, 0.0, 1, 1), lambda cs: cs == []),
        ((0.0, 0.0, 1.0, 1.0, float("nan"), 5.0, 0.0, 1, 0), lambda cs: cs == []),
        ((0.0, 0.0, 1.0, 1.0, 5.0, 5.0, float("nan"), 1, 0), lambda cs: cs == []),
    ]

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_arc")
        subprocess.run([CC, "-O2", "-Wall", "-Wextra", "-fsanitize=address,undefined",
                        "-I", os.path.join(ROOT, "src", "shfolderproxy"),
                        os.path.join(ROOT, "tests", "test_arc.c"), "-o", exe, "-lm"], check=True)
        ok = fail = 0
        for case, curves in zip(cases, run(exe, cases)):
            problems = check(case, curves)
            if problems:
                fail += 1
                if fail <= 10:
                    print(f"{case}: {'; '.join(problems)}", file=sys.stderr)
            else:
                ok += 1
        for (case, test), curves in zip(fixed, run(exe, [c for c, _ in fixed])):
            if test(curves) and (not curves or not check(case, curves)):
                ok += 1
            else:
                fail += 1
                print(f"{case}: {curves}", file=sys.stderr)
        print(f"ok={ok} fail={fail}")
        return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
