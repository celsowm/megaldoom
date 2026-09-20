#!/usr/bin/env python3
"""Conservative per-subsector visibility for the flat BSP renderer.

Why this is sound for this engine specifically: every wall is one 128-unit
slab and bsp_draw_seg occludes in one dimension only (a sampled column is
closed by the first vis-solid seg that covers it, front-to-back). So a seg
that no line of sight from ANY point of the player's subsector can reach
through open space is never the front-most seg of any column, and removing
it from the walk cannot change one output byte. The runtime oracle
(BSP_VIS_ORACLE) is what actually proves that; this module only has to be
conservative.

Model:
  * Leaf regions are the TRUE convex subsector regions: the node cell clipped
    by the front half-plane of every original WAD seg of that subsector. That
    removes the void a node cell can extend into behind a one-sided wall,
    which would otherwise leak visibility around it.
  * A directed portal A->B is a shared boundary piece of A and B minus the
    vis-solid segs OF A covering it: bsp_draw_seg's facing test means a seg
    only blocks rays that reach it from its front, and A's segs face into A.
  * Vis-solid = the seg types whose draw path calls bsp_mark_sample_solid.
    DOOR (in any state), WINDOW and TRIGGER are always open, so door motion
    never invalidates the bake.
  * 2D portal flow with separator clipping (Quake vis), every clip made
    tolerant by EPS world units so the result is a superset.
"""

import math
import os
import struct
import sys
from collections import defaultdict

SEG_DOOR = 1
SEG_TRIGGER = 4
SEG_WINDOW = 5
# bsp_seg_is_open: a TRIGGER line is always open (never drawn, never blocks);
# a DOOR is open at some lift and a WINDOW draws an overlay without closing
# the column. None of them may block visibility in the bake.
NON_OCCLUDING_TYPES = {SEG_DOOR, SEG_WINDOW, SEG_TRIGGER}

CHILD_LEAF_BIT = 0x8000
DEFAULT_EPS = 1.0
MAP_BOUND = 40000.0


COLLINEAR_TOL = 2.0


def _edge_frame(p, q):
    """(origin, unit normal, unit direction, length) of the segment p->q."""
    dx = q[0] - p[0]
    dy = q[1] - p[1]
    n = math.hypot(dx, dy)
    d = (dx / n, dy / n)
    return (p, (-d[1], d[0]), d, n)


def _frame_param(frame, r):
    o, _, d, _ = frame
    return (r[0] - o[0]) * d[0] + (r[1] - o[1]) * d[1]


def _frame_dist(frame, r):
    o, nrm, _, _ = frame
    return (r[0] - o[0]) * nrm[0] + (r[1] - o[1]) * nrm[1]


def _frame_point(frame, t):
    o, _, d, _ = frame
    return (o[0] + d[0] * t, o[1] + d[1] * t)


def line_key(a, b, c):
    """Canonical integer key of the line a*x + b*y + c = 0 (orientation-free)."""
    g = math.gcd(math.gcd(abs(a), abs(b)), abs(c))
    if g > 1:
        a //= g
        b //= g
        c //= g
    if a < 0 or (a == 0 and b < 0):
        a, b, c = -a, -b, -c
    return (a, b, c)


def through(p, q):
    """Integer line through integer points p, q: returns (a, b, c)."""
    a = q[1] - p[1]
    b = p[0] - q[0]
    c = -(a * p[0] + b * p[1])
    return a, b, c


def clip_polygon(poly, keys, a, b, c, key, eps=1e-6):
    """Keep a*x+b*y+c >= 0 of a convex polygon, tracking each edge's line key."""
    n = len(poly)
    if n == 0:
        return poly, keys
    out = []
    for i in range(n):
        p = poly[i]
        q = poly[(i + 1) % n]
        dp = a * p[0] + b * p[1] + c
        dq = a * q[0] + b * q[1] + c
        p_in = dp >= -eps
        if p_in:
            out.append(p)
        if p_in != (dq >= -eps):
            t = dp / (dp - dq)
            out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
    # Edge i runs out[i] -> out[i+1]; recover which source line each lies on.
    return out, _rekey(out, poly, keys, (a, b, c, key))


def _rekey(out, poly, keys, clip):
    n = len(out)
    result = []
    a, b, c, key = clip
    norm = math.hypot(a, b) or 1.0
    for i in range(n):
        p = out[i]
        q = out[(i + 1) % n]
        m = ((p[0] + q[0]) / 2, (p[1] + q[1]) / 2)
        if abs(a * m[0] + b * m[1] + c) / norm < 1e-4:
            result.append(key)
            continue
        best = None
        best_d = None
        for j in range(len(poly)):
            u = poly[j]
            v = poly[(j + 1) % len(poly)]
            la = v[1] - u[1]
            lb = u[0] - v[0]
            ln = math.hypot(la, lb)
            if ln == 0:
                continue
            d = abs(la * (m[0] - u[0]) + lb * (m[1] - u[1])) / ln
            if best_d is None or d < best_d:
                best_d = d
                best = keys[j]
        result.append(best)
    return result


def polygon_area(poly):
    s = 0.0
    for i in range(len(poly)):
        p = poly[i]
        q = poly[(i + 1) % len(poly)]
        s += p[0] * q[1] - q[0] * p[1]
    return s / 2


def centroid(poly):
    x = sum(p[0] for p in poly) / len(poly)
    y = sum(p[1] for p in poly) / len(poly)
    return x, y


class VisMap:
    def __init__(self, map_data, eps=DEFAULT_EPS):
        self.m = map_data
        self.eps = eps
        self.vertices = map_data.vertices
        self.nodes = map_data.nodes
        self.leaf_count = len(map_data.out_ssectors)
        self.root = len(self.nodes) - 1
        self.leaf_poly = [None] * self.leaf_count
        # The node cell BEFORE the true-region clip. bsp_find_subsector maps a
        # position to a subsector by the partitions alone, so the cell -- not
        # the clipped region -- is the exact set of positions that can report
        # this subsector, and it is what every claim about the PLAYER (a seg
        # never faces him, a partition's side is fixed) has to hold over. The
        # clipped region stays the basis for visibility.
        self.leaf_cell = [None] * self.leaf_count
        self.leaf_keys = [None] * self.leaf_count
        self.leaf_ancestors = [None] * self.leaf_count
        self._build_cells()
        self._clip_true_regions()
        self._build_portals()

    # -- regions ----------------------------------------------------------- #
    def _build_cells(self):
        box = [(-MAP_BOUND, -MAP_BOUND), (MAP_BOUND, -MAP_BOUND),
               (MAP_BOUND, MAP_BOUND), (-MAP_BOUND, MAP_BOUND)]
        keys = [None] * 4
        stack = [(self.root, box, keys, [])]
        while stack:
            child, poly, pkeys, anc = stack.pop()
            if child & CHILD_LEAF_BIT:
                leaf = child & 0x7FFF
                self.leaf_poly[leaf] = poly
                self.leaf_cell[leaf] = poly
                self.leaf_keys[leaf] = pkeys
                self.leaf_ancestors[leaf] = anc
                continue
            nd = self.nodes[child]
            # Runtime: cross = (px - x)*dy - (py - y)*dx >= 0 -> front.
            a = nd["dy"]
            b = -nd["dx"]
            c = -(nd["x"] * nd["dy"]) + nd["y"] * nd["dx"]
            key = line_key(a, b, c)
            fpoly, fkeys = clip_polygon(poly, pkeys, a, b, c, key)
            bpoly, bkeys = clip_polygon(poly, pkeys, -a, -b, -c, key)
            stack.append((nd["front"], fpoly, fkeys, anc + [(child, True)]))
            stack.append((nd["back"], bpoly, bkeys, anc + [(child, False)]))

    def _clip_true_regions(self):
        v = self.vertices
        self.leaf_empty = [False] * self.leaf_count
        for leaf, (count, first) in enumerate(self.m.source_ssectors):
            poly = self.leaf_poly[leaf]
            keys = self.leaf_keys[leaf]
            for k in range(count):
                v1, v2 = self.m.source_segs[first + k]
                p = v[v1]
                q = v[v2]
                if p == q:
                    continue
                # Doom keeps a subsector on its segs' right (y-up); after the
                # y flip that is the side where (q-p) x (r-p) > 0... verified
                # against the emitted out_seg normals in check_orientation().
                a, b, c = through(p, q)
                a, b, c = -a, -b, -c
                if self.seg_inside_sign < 0:
                    a, b, c = -a, -b, -c
                poly, keys = clip_polygon(poly, keys, a, b, c, line_key(a, b, c))
                if len(poly) < 3:
                    break
            if len(poly) < 3 or abs(polygon_area(poly)) < 1e-3:
                self.leaf_empty[leaf] = True
            self.leaf_poly[leaf] = poly
            self.leaf_keys[leaf] = keys
        self.empty_leaves = [leaf for leaf in range(self.leaf_count)
                             if self.leaf_empty[leaf]]

    seg_inside_sign = 1

    # -- portals ----------------------------------------------------------- #
    def _build_portals(self):
        """Directed portals between regions that share a boundary piece.

        Boundaries are matched by near-collinearity, NOT by exact line key: a
        node builder splits a linedef at a vertex rounded to integers, so the
        two halves of one wall can sit on integer lines that differ by a
        fraction of a unit (E1M2 leaves 130/133, 0.36 units). Exact matching
        found no portal there and sealed everything behind it. Accepting a
        near match can only add portals, which keeps the result a superset.
        """
        tol = COLLINEAR_TOL
        edges = []
        for leaf in range(self.leaf_count):
            if self.leaf_empty[leaf]:
                continue
            poly = self.leaf_poly[leaf]
            cx, cy = centroid(poly)
            for i in range(len(poly)):
                p = poly[i]
                q = poly[(i + 1) % len(poly)]
                if math.hypot(q[0] - p[0], q[1] - p[1]) < 1e-3:
                    continue
                edges.append((leaf, p, q, (cx, cy)))

        grid = defaultdict(list)
        cell = 64.0
        for index, (_, p, q, _) in enumerate(edges):
            x0 = int(math.floor((min(p[0], q[0]) - tol) / cell))
            x1 = int(math.floor((max(p[0], q[0]) + tol) / cell))
            y0 = int(math.floor((min(p[1], q[1]) - tol) / cell))
            y1 = int(math.floor((max(p[1], q[1]) + tol) / cell))
            for gx in range(x0, x1 + 1):
                for gy in range(y0, y1 + 1):
                    grid[(gx, gy)].append(index)
        candidates = set()
        for members in grid.values():
            for i in members:
                for j in members:
                    if edges[i][0] != edges[j][0]:
                        candidates.add((i, j))

        # Vis-solid out_segs per leaf, as endpoint pairs.
        solid = defaultdict(list)
        v = self.vertices
        for leaf, (first, count) in enumerate(self.m.out_ssectors):
            for k in range(first, first + count):
                seg = self.m.out_segs[k]
                if seg["type"] not in NON_OCCLUDING_TYPES:
                    solid[leaf].append((v[seg["v1"]], v[seg["v2"]]))

        self.portals = [[] for _ in range(self.leaf_count)]
        self.portal_count = 0
        for i, j in candidates:
            la, ap, aq, acen = edges[i]
            lb, bp, bq, bcen = edges[j]
            frame = _edge_frame(ap, aq)
            # B must lie on A's line within tol over their overlap.
            tb0 = _frame_param(frame, bp)
            tb1 = _frame_param(frame, bq)
            t0 = max(0.0, min(tb0, tb1))
            t1 = min(frame[3], max(tb0, tb1))
            if t1 - t0 < 1e-3:
                continue
            if abs(_frame_dist(frame, bp)) > tol or abs(_frame_dist(frame, bq)) > tol:
                continue
            # Opposite sides: A's centroid and B's centroid straddle the line.
            if (_frame_dist(frame, acen) > 0) == (_frame_dist(frame, bcen) > 0):
                continue
            blocks = []
            for sp, sq in solid[la]:
                if abs(_frame_dist(frame, sp)) > tol or abs(_frame_dist(frame, sq)) > tol:
                    continue
                s0 = _frame_param(frame, sp)
                s1 = _frame_param(frame, sq)
                blocks.append((min(s0, s1), max(s0, s1)))
            side = 1 if _frame_dist(frame, acen) > 0 else -1
            for u0, u1 in subtract_intervals((t0, t1), blocks, self.eps):
                self._add_portal(la, lb, frame, u0, u1, side)

    def _add_portal(self, src, dst, frame, t0, t1, src_side):
        # Widen by eps (and the collinearity tolerance) so rounding at portal
        # ends can only admit more.
        t0 -= self.eps + COLLINEAR_TOL
        t1 += self.eps + COLLINEAR_TOL
        p = _frame_point(frame, t0)
        q = _frame_point(frame, t1)
        nx, ny = frame[1]
        # Travel normal points from src into dst; >= 0 on the dst side.
        nx, ny = -src_side * nx, -src_side * ny
        line = (nx, ny, -(nx * p[0] + ny * p[1]))
        self.portals[src].append(Portal(src, dst, (p, q), line))
        self.portal_count += 1

    # -- flow -------------------------------------------------------------- #
    def leaf_pvs(self, source, budget=2000000):
        visible = {source}
        self._steps = 0
        self._budget = budget
        self._overflow = False
        for portal in self.portals[source]:
            visible.add(portal.dst)
            stack = {source, portal.dst}
            for nxt in self.portals[portal.dst]:
                if nxt.dst in stack:
                    continue
                c = clip_segment(nxt.seg, portal.line, self.eps)
                if c is None:
                    continue
                visible.add(nxt.dst)
                stack.add(nxt.dst)
                self._flow(nxt.dst, portal.seg, c, nxt.line, stack, visible)
                stack.discard(nxt.dst)
        # A zero-area region gets no portals, so flow can never reach it, yet a
        # node builder can still hang a sliver seg off one (E1M2 leaf 45). The
        # few there are count as visible from everywhere.
        visible.update(self.empty_leaves)
        return visible, self._overflow

    def _flow(self, leaf, src, win, win_line, stack, visible):
        self._steps += 1
        if self._steps > self._budget:
            # Out of budget: everything still reachable by adjacency counts as
            # visible. Conservative, and reported so it is never silent.
            self._overflow = True
            self._flood_rest(leaf, stack, visible)
            return
        for nxt in self.portals[leaf]:
            if nxt.dst in stack:
                continue
            c = clip_segment(nxt.seg, win_line, self.eps)
            if c is None:
                continue
            c = clip_by_separators(src, win, c, self.eps)
            if c is None:
                continue
            s = clip_by_separators(c, win, src, self.eps, reverse=True)
            if s is None:
                continue
            visible.add(nxt.dst)
            stack.add(nxt.dst)
            self._flow(nxt.dst, s, c, nxt.line, stack, visible)
            stack.discard(nxt.dst)

    def _flood_rest(self, leaf, stack, visible):
        seen = set(stack)
        todo = [leaf]
        while todo:
            cur = todo.pop()
            for p in self.portals[cur]:
                if p.dst not in seen:
                    seen.add(p.dst)
                    visible.add(p.dst)
                    todo.append(p.dst)

    # -- queries ----------------------------------------------------------- #
    def find_leaf(self, x, y):
        child = self.root
        while not (child & CHILD_LEAF_BIT):
            nd = self.nodes[child]
            cross = (x - nd["x"]) * nd["dy"] - (y - nd["y"]) * nd["dx"]
            child = nd["front"] if cross >= 0 else nd["back"]
        return child & 0x7FFF

    def check_orientation(self):
        """Every emitted seg's normal must point into its own leaf region."""
        bad = 0
        v = self.vertices
        for leaf, (first, count) in enumerate(self.m.out_ssectors):
            if self.leaf_empty[leaf]:
                continue
            cx, cy = centroid(self.leaf_poly[leaf])
            for s in range(first, first + count):
                seg = self.m.out_segs[s]
                ax, ay = v[seg["v1"]]
                if (cx - ax) * seg["nx"] + (cy - ay) * seg["ny"] <= 0:
                    bad += 1
        return bad


class Portal:
    __slots__ = ("src", "dst", "seg", "line")

    def __init__(self, src, dst, seg, line):
        self.src = src
        self.dst = dst
        self.seg = seg
        self.line = line


def subtract_intervals(span, blocks, eps):
    """span minus the union of blocks, with blocks shrunk by eps at each end."""
    pieces = [span]
    for b0, b1 in blocks:
        b0 += eps
        b1 -= eps
        if b1 <= b0:
            continue
        nxt = []
        for p0, p1 in pieces:
            if b1 <= p0 or b0 >= p1:
                nxt.append((p0, p1))
                continue
            if p0 < b0:
                nxt.append((p0, b0))
            if b1 < p1:
                nxt.append((b1, p1))
        pieces = nxt
    return [(p0, p1) for p0, p1 in pieces if p1 - p0 > 1e-3]


def clip_segment(seg, line, eps):
    """Keep the part of seg with nx*x+ny*y+c >= -eps (line normalised)."""
    nx, ny, c = line
    p, q = seg
    dp = nx * p[0] + ny * p[1] + c
    dq = nx * q[0] + ny * q[1] + c
    if dp < -eps and dq < -eps:
        return None
    if dp >= -eps and dq >= -eps:
        return seg
    t = (-eps - dp) / (dq - dp)
    x = (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t)
    return (p, x) if dp >= -eps else (x, q)


def _signed(p, q, r):
    """Normalised signed distance of r from the line p->q (None if degenerate)."""
    dx = q[0] - p[0]
    dy = q[1] - p[1]
    n = math.hypot(dx, dy)
    if n < 1e-6:
        return None
    return (dx * (r[1] - p[1]) - dy * (r[0] - p[0])) / n


def clip_by_separators(src, win, target, eps, reverse=False):
    """Clip target to the region lines through src and then win can reach.

    Separating lines pass through one endpoint of src and one of win, with the
    other src endpoint and the other win endpoint on opposite sides; the kept
    side is the one holding the other win endpoint. A degenerate or collinear
    candidate is skipped, which can only keep more.
    """
    for i in range(2):
        s = src[i]
        s_other = src[1 - i]
        for j in range(2):
            w = win[j]
            w_other = win[1 - j]
            d_s = _signed(s, w, s_other)
            d_w = _signed(s, w, w_other)
            if d_s is None or d_w is None:
                continue
            if abs(d_s) < 1e-6 or abs(d_w) < 1e-6:
                continue
            if (d_s > 0) == (d_w > 0):
                continue
            keep_positive = d_w > 0
            target = _clip_by_line(target, s, w, keep_positive, eps)
            if target is None:
                return None
    return target


def _clip_by_line(seg, p, q, keep_positive, eps):
    p0, p1 = seg
    d0 = _signed(p, q, p0)
    d1 = _signed(p, q, p1)
    if not keep_positive:
        d0, d1 = -d0, -d1
    if d0 < -eps and d1 < -eps:
        return None
    if d0 >= -eps and d1 >= -eps:
        return seg
    t = (-eps - d0) / (d1 - d0)
    x = (p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t)
    return (p0, x) if d0 >= -eps else (x, p1)


def subtree_leaves(nodes, child, out):
    if child & CHILD_LEAF_BIT:
        out.append(child & 0x7FFF)
        return out
    nd = nodes[child]
    subtree_leaves(nodes, nd["front"], out)
    subtree_leaves(nodes, nd["back"], out)
    return out


def pvs_rows(vm, pvs):
    """Per-leaf bit rows over child ids: bit i < N is node i, N + k is leaf k.

    A node bit is set when its subtree holds any PVS leaf, so the runtime can
    reject a whole child with one bit test before projecting its box.
    """
    n_nodes = len(vm.nodes)
    node_leaves = [set(subtree_leaves(vm.nodes, i, [])) for i in range(n_nodes)]
    width = (n_nodes + vm.leaf_count + 7) // 8
    rows = []
    for leaf in range(vm.leaf_count):
        row = bytearray(width)
        for i in range(n_nodes):
            if node_leaves[i] & pvs[leaf]:
                row[i >> 3] |= 1 << (i & 7)
        for k in pvs[leaf]:
            bit = n_nodes + k
            row[bit >> 3] |= 1 << (bit & 7)
        rows.append(bytes(row))
    return width, rows


# -- per-leaf draw programs (BSP_VIS_LIST) ----------------------------------- #
# One u16 word per record, walked by bsp_run_vis_program():
#   SEG     0b0Axxxxxxxxxxxxxx  draw seg x; A=1 when its front half-plane holds
#                                the whole leaf region, so the facing test is
#                                skipped.
#   BRANCH  0b10xxxxxxxxxxxxxx  node x's partition crosses the leaf region: the
#                                next two words are the front and back run
#                                lengths, then the front run, then the back run;
#                                the runtime side test picks which runs first.
#   GROUP   0b11xxxxxxxxxxxxxx  x = node * 2 + (1 for its back child). The next
#                                word is a run length; the runtime projects that
#                                child's box and skips the run when it is off
#                                screen or already covered, as
#                                bsp_render_boxed_child does. Emitted only
#                                where the box test is expected to pay for
#                                itself: see group_pays_off().
# Everything else is resolved offline: a partition the whole region lies on
# one side of has a fixed near/far order, subtrees with no PVS leaf vanish, and
# a seg whose front half-plane misses the region can never face the player.
PROGRAM_BRANCH = 0x8000
PROGRAM_GROUP = 0xC000
PROGRAM_ALWAYS_FACING = 0x4000
PROGRAM_INDEX_MASK = 0x3FFF
SIDE_MARGIN = 2.0


# Group choice (2026-09-18). A GROUP costs one box projection (~26 subticks,
# LOG 2026-07-30) every time the program reaches it and saves the run's seg
# tests only when the box is skipped. A fixed threshold (the old --group-min K)
# cannot see where that happens: K=8 lost to no groups at all at some poses and
# beat it by ~20% at others. So each candidate is priced per leaf: P_off is the
# fraction of (viewpoint over the leaf's region) x (16 headings) samples for
# which the child's box lies wholly outside the 90-degree frustum, and the group
# is kept iff P_off * segs * GROUP_SEG_COST > GROUP_BOX_COST. Occlusion skips are
# invisible offline, so P_off is a lower bound on the runtime skip rate. The
# costs are calibrated, not derived: over the 17 sweep poses GROUP_SEG_COST 5
# gave cast -3.5% against K=8 and -4.3% against no groups, with no pose worse
# than the better of the two by more than 2% (8 gave -1.3%, 3 gave -0.5%).
GROUP_BOX_COST = 26.0
GROUP_SEG_COST = 5.0
GROUP_MIN_SEGS = 2
GROUP_HEADINGS = 16
GROUP_NEAR = 16.0


def _group_sample_points(poly):
    cx, cy = centroid(poly)
    return (list(poly) + [(cx, cy)] +
            [((cx + x) / 2.0, (cy + y) / 2.0) for x, y in poly])


def _box_off_screen_share(box, points):
    x0, y0, x1, y1 = box
    corners = ((x0, y0), (x1, y0), (x0, y1), (x1, y1))
    off = 0
    total = 0
    for px, py in points:
        rel = [(cx - px, cy - py) for cx, cy in corners]
        for h in range(GROUP_HEADINGS):
            a = 2.0 * math.pi * h / GROUP_HEADINGS
            dx, dy = math.cos(a), math.sin(a)
            fs = [rx * dx + ry * dy for rx, ry in rel]
            ls = [-rx * dy + ry * dx for rx, ry in rel]
            total += 1
            if (all(f < GROUP_NEAR for f in fs) or
                    all(l > f for f, l in zip(fs, ls)) or
                    all(l < -f for f, l in zip(fs, ls))):
                off += 1
    return off / total if total else 0.0


def group_pays_off(box, points, segs, seg_cost=None):
    """Keep a GROUP around a run of `segs` seg tests behind `box`?"""
    if segs < GROUP_MIN_SEGS or points is None:
        return False
    cost = GROUP_SEG_COST if seg_cost is None else seg_cost
    return _box_off_screen_share(box, points) * segs * cost > GROUP_BOX_COST


def _classify(poly, f):
    lo = min(f(x, y) for x, y in poly)
    hi = max(f(x, y) for x, y in poly)
    return lo, hi


def leaf_program(vm, leaf, pvs, group_min=None, drop_every=None, group_seg_cost=None,
                 seg_keep=None):
    """Build one leaf program.

    ``seg_keep`` is an analysis hook: when supplied it is called as
    ``seg_keep(leaf, seg_index)`` and can remove a segment from a temporary
    experiment.  The shipped emitter leaves it unset, so the runtime bake is
    unchanged.  Keeping the hook here avoids copying this ordering/grouping
    logic into an offline cost model.
    """
    md = vm.m
    v = vm.vertices
    poly = vm.leaf_cell[leaf]
    if poly is None or len(poly) < 3:
        poly = None  # no usable cell: branch everywhere, test every facing
    region = vm.leaf_poly[leaf] or poly
    points = _group_sample_points(region) if region else None

    def seg_words(sub):
        first, count = md.out_ssectors[sub]
        words = []
        for k in range(first, first + count):
            seg = md.out_segs[k]
            if seg["type"] == SEG_TRIGGER:
                continue  # bsp_draw_seg returns at once: never drawn
            if drop_every and k % drop_every == 0:
                continue  # negative control only: a deliberately broken bake
            if seg_keep is not None and not seg_keep(leaf, k):
                continue  # analysis-only directional visibility filter
            flag = 0
            if poly is not None:
                ax, ay = v[seg["v1"]]
                lo, hi = _classify(poly, lambda x, y: (x - ax) * seg["nx"] +
                                   (y - ay) * seg["ny"])
                if hi <= -SIDE_MARGIN:
                    continue  # never facing from anywhere in the region
                if lo > SIDE_MARGIN:
                    flag = PROGRAM_ALWAYS_FACING
            assert k <= PROGRAM_INDEX_MASK
            words.append(k | flag)
        return words

    def seg_count(words):
        count = 0
        i = 0
        while i < len(words):
            word = words[i]
            if word & PROGRAM_BRANCH:
                i += 3 if (word & PROGRAM_GROUP) == PROGRAM_BRANCH else 2
                continue
            count += 1
            i += 1
        return count

    def grouped(node, back_side, words):
        if not words:
            return words
        if group_min is not None:
            if seg_count(words) < group_min:
                return words
        else:
            nd = vm.nodes[node]
            box = nd["back_box"] if back_side else nd["front_box"]
            if not group_pays_off(box, points, seg_count(words), group_seg_cost):
                return words
        ref = node * 2 + (1 if back_side else 0)
        assert ref <= PROGRAM_INDEX_MASK and len(words) <= 0xFFFF
        return [PROGRAM_GROUP | ref, len(words)] + words

    def build(child):
        if child & CHILD_LEAF_BIT:
            sub = child & 0x7FFF
            return seg_words(sub) if sub in pvs else []
        nd = vm.nodes[child]
        front = grouped(child, False, build(nd["front"]))
        back = grouped(child, True, build(nd["back"]))
        if not front and not back:
            return []
        if poly is not None:
            lo, hi = _classify(poly, lambda x, y: (x - nd["x"]) * nd["dy"] -
                               (y - nd["y"]) * nd["dx"])
            if lo >= SIDE_MARGIN:
                return front + back
            if hi <= -SIDE_MARGIN:
                return back + front
        if not front:
            return back
        if not back:
            return front
        assert child <= PROGRAM_INDEX_MASK
        assert len(front) <= 0xFFFF and len(back) <= 0xFFFF
        return [PROGRAM_BRANCH | child, len(front), len(back)] + front + back

    return build(vm.root)


def emit_c(path, entries, group_min=None, drop_every=None):
    """entries: list of (map symbol prefix, VisMap, pvs). Rows are deduplicated."""
    out = ["// Generated by tools/bsp_vis.py -- do not edit.",
           "// Per-subsector potentially-visible child bits; see bsp_vis.py.",
           '#include "bsp_map.h"', "", "#if BSP_VIS_CULL", ""]
    cases = []
    for prefix, vm, pvs in entries:
        width, rows = pvs_rows(vm, pvs)
        unique = {}
        index = []
        for row in rows:
            index.append(unique.setdefault(row, len(unique)))
        out.append("// %s: %d leaves, %d unique rows x %d bytes = %d bytes" %
                   (prefix, len(rows), len(unique), width, len(unique) * width))
        out.append("static const u8 %s_bsp_vis_rows[%d] = {" %
                   (prefix, len(unique) * width))
        for row in unique:
            out.append("    " + ",".join(str(b) for b in row) + ",")
        out.append("};")
        out.append("static const u16 %s_bsp_vis_row_index[%d] = {" % (prefix, len(index)))
        for i in range(0, len(index), 16):
            out.append("    " + ",".join(str(x) for x in index[i:i + 16]) + ",")
        out.append("};")
        out.append("")
        cases.append((prefix, width, len(index)))
    out.append("const u8 *bsp_vis_row(const BspMapData *map, u16 subsector) {")
    for prefix, width, count in cases:
        out.append("    if (map == &g_%s_map && subsector < %d) {" % (prefix, count))
        out.append("        return &%s_bsp_vis_rows[%s_bsp_vis_row_index[subsector] * %d];"
                   % (prefix, prefix, width))
        out.append("    }")
    out.append("    return NULL;")
    out.append("}")
    out.append("")
    out.append("#endif")
    out.extend(["", "#if BSP_VIS_LIST", ""])
    # Programs are per-level data, so they live in that level's banked window
    # (tools/md_banked.ld, section .wallpackN) beside its wall pack, not in the
    # resident image. Only the offset/length tables stay resident; a program is
    # read exactly while its own level is mapped.
    base = os.path.splitext(path)[0]
    asm = [
        "/* Generated by tools/bsp_vis.py -- do not edit. Each level's draw",
        " * programs go in its banked window section beside its wall pack. */",
        "",
    ]
    list_cases = []
    for index, (prefix, vm, pvs) in enumerate(entries):
        words = []
        offsets = []
        lengths = []
        seen = {}
        for leaf in range(vm.leaf_count):
            program = tuple(leaf_program(vm, leaf, pvs[leaf], group_min, drop_every))
            if program not in seen:
                seen[program] = len(words)
                words.extend(program)
            offsets.append(seen[program])
            lengths.append(len(program))
        dat = "%s_%s.dat" % (base, prefix)
        with open(dat, "wb") as fh:
            fh.write(b"".join(struct.pack(">H", w) for w in words))
        label = "megaldoom_vis_program_%s" % prefix
        asm.extend([
            '    .section .wallpack%d,"a"' % index,
            "    .align  2",
            "    .globl  %s" % label,
            "%s: /* %d leaves, %d words, longest %d */" % (
                label, vm.leaf_count, len(words), max(lengths)),
            '    .incbin "src/bsp/%s"' % os.path.basename(dat),
            "",
        ])
        out.append("// %s: %d leaves, %d program words (%d banked bytes), "
                   "longest %d, group_min %s" %
                   (prefix, vm.leaf_count, len(words), 2 * len(words),
                    max(lengths), group_min))
        out.append("extern const u16 %s[];" % label)
        out.append("static const u32 %s_bsp_vis_program_offset[%d] = {" %
                   (prefix, vm.leaf_count))
        for i in range(0, vm.leaf_count, 8):
            out.append("    " + ",".join(str(x) for x in offsets[i:i + 8]) + ",")
        out.append("};")
        out.append("static const u16 %s_bsp_vis_program_length[%d] = {" %
                   (prefix, vm.leaf_count))
        for i in range(0, vm.leaf_count, 16):
            out.append("    " + ",".join(str(x) for x in lengths[i:i + 16]) + ",")
        out.append("};")
        out.append("")
        list_cases.append((prefix, label, vm.leaf_count))
    out.append("const u16 *bsp_vis_program(const BspMapData *map, u16 subsector, "
               "u16 *length) {")
    for prefix, label, count in list_cases:
        out.append("    if (map == &g_%s_map && subsector < %d) {" % (prefix, count))
        out.append("        *length = %s_bsp_vis_program_length[subsector];" % prefix)
        out.append("        return &%s[%s_bsp_vis_program_offset[subsector]];" %
                   (label, prefix))
        out.append("    }")
    out.append("    return NULL;")
    out.append("}")
    out.append("")
    out.append("#endif")
    with open(base + "_programs.s", "w", newline="\n") as fh:
        fh.write("\n".join(asm))
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def main():
    import argparse
    import os
    import time
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import doom_map
    from wad_reader import WadFile

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--wad", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "DOOM1.WAD"))
    ap.add_argument("--maps", nargs="+", default=["E1M1"])
    ap.add_argument("--eps", type=float, default=DEFAULT_EPS)
    ap.add_argument("--probe", nargs=2, type=int, action="append", default=[],
                    metavar=("X", "Y"), help="report the PVS of the leaf at X Y")
    ap.add_argument("--group-min", type=int, default=None,
                    help="A/B only: wrap every run of at least this many segs in a "
                         "box test (the pre-2026-09-18 rule; default prices each "
                         "group with group_pays_off)")
    ap.add_argument("--no-groups", action="store_true",
                    help="A/B only: emit no GROUP words at all")
    ap.add_argument("--negative-control-drop-every", type=int, default=None,
                    help="NEGATIVE CONTROL: omit every Nth seg from the programs "
                         "so tools/test-bsp-vis-oracle.ps1 must fail")
    ap.add_argument("--pvs-cache", default=None,
                    help="JSON file to reuse PVS sets from (keyed by map and tool source)")
    ap.add_argument("--emit-c", default=None,
                    help="write the per-leaf visibility rows for --maps to this C file")
    args = ap.parse_args()
    entries = []
    import hashlib
    import json
    with open(os.path.abspath(__file__), "rb") as fh:
        source_digest = hashlib.sha256(fh.read()).hexdigest()[:16]
    cache = {}
    if args.pvs_cache and os.path.exists(args.pvs_cache):
        with open(args.pvs_cache) as fh:
            cache = json.load(fh)

    wad = WadFile(args.wad)
    for mapn in args.maps:
        md = doom_map.load_map(wad, mapn)
        start = time.time()
        vm = VisMap(md, args.eps)
        bad = vm.check_orientation()
        if bad:
            # Wrong inside convention for the true-region clip: flip and redo.
            VisMap.seg_inside_sign = -VisMap.seg_inside_sign
            vm = VisMap(md, args.eps)
            bad = vm.check_orientation()
        empty = sum(vm.leaf_empty)
        print("%s: leaves=%d nodes=%d portals=%d empty_regions=%d "
              "misoriented_segs=%d build=%.1fs" %
              (mapn, vm.leaf_count, len(vm.nodes), vm.portal_count, empty, bad,
               time.time() - start))
        start = time.time()
        sizes = []
        overflow = 0
        pvs = []
        cache_key = "%s:%s" % (mapn, source_digest)
        cached = cache.get(cache_key)
        for leaf in range(vm.leaf_count):
            if cached is not None:
                vis, over = set(cached[leaf]), False
            else:
                # A zero-area region has no portals, so its flow would claim it
                # sees nothing; the point locator can still land in it on a
                # boundary.
                vis, over = ((set(range(vm.leaf_count)), False) if vm.leaf_empty[leaf]
                             else vm.leaf_pvs(leaf))
            pvs.append(vis)
            overflow += over
            sizes.append(len(vis))
        cache[cache_key] = [sorted(v) for v in pvs]
        n = vm.leaf_count
        sizes_sorted = sorted(sizes)
        print("  pvs: mean=%.1f%% median=%.1f%% max=%.1f%% overflow_leaves=%d flow=%.1fs" %
              (100.0 * sum(sizes) / (n * n), 100.0 * sizes_sorted[n // 2] / n,
               100.0 * sizes_sorted[-1] / n, overflow, time.time() - start))
        # Node pruning potential: nodes whose subtree holds any PVS leaf.
        node_leaves = [set(subtree_leaves(vm.nodes, i, [])) for i in range(len(vm.nodes))]
        kept = [sum(1 for i in range(len(vm.nodes)) if node_leaves[i] & pvs[leaf])
                for leaf in range(n)]
        print("  nodes kept: mean=%.1f%%" % (100.0 * sum(kept) / (n * len(vm.nodes))))
        entries.append((mapn.lower(), vm, pvs))
        order = sorted(range(n), key=lambda k: len(pvs[k]))
        for label, leaf in (("median", order[n // 2]), ("worst", order[-1])):
            cx, cy = centroid(vm.leaf_poly[leaf])
            print("  %s leaf %d at (%d,%d): pvs %d/%d, nodes kept %d" %
                  (label, leaf, cx, cy, len(pvs[leaf]), n, kept[leaf]))
        for x, y in args.probe:
            leaf = vm.find_leaf(x, y)
            print("  probe (%d,%d): leaf %d pvs %d/%d leaves (%.1f%%), nodes kept %d/%d" %
                  (x, y, leaf, len(pvs[leaf]), n, 100.0 * len(pvs[leaf]) / n,
                   kept[leaf], len(vm.nodes)))

    if args.pvs_cache:
        with open(args.pvs_cache, "w") as fh:
            json.dump(cache, fh)
    if args.emit_c:
        emit_c(args.emit_c, entries,
               (1 << 30) if args.no_groups else args.group_min,
               args.negative_control_drop_every)
        print("wrote %s" % args.emit_c)


if __name__ == "__main__":
    main()
