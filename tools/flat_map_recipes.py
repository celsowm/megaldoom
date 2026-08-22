#!/usr/bin/env python3
"""Curated offline simplifications for height features the flat runtime erases.

The runtime remains strictly 2D and only knows full-height walls. A material
transfer recipe therefore does not turn a Doom upper/lower portal band into a
new wall. It validates the erased source feature, then projects its recognisable
material onto existing solid perimeter linedefs. Geometry, collision and LOS
remain byte-for-byte equivalent to a conversion with recipes disabled.
"""

from dataclasses import dataclass


@dataclass(frozen=True)
class SourceFeatureSignature:
    endpoints: tuple
    right_heights: tuple
    left_heights: tuple
    texture_side: str
    texture_field: str
    texture_name: str
    linedef_hint: int


@dataclass(frozen=True)
class SolidTargetSignature:
    endpoints: tuple
    side: str
    sector_heights: tuple
    texture_field: str
    texture_name: str
    linedef_hint: int


@dataclass(frozen=True)
class FlatMaterialTransferRecipe:
    name: str
    source: SourceFeatureSignature
    targets: tuple


@dataclass(frozen=True)
class ResolvedFlatMaterialTransfer:
    recipe: FlatMaterialTransferRecipe
    source_linedef: int
    target_linedef: int
    target_side_id: int


# Engine coordinates are Doom x / negated Doom y, matching doom_map.MapData.
# E1M1's COMPUTE2 bank is an upper portal band, not a floor-to-ceiling wall.
# Since the flat runtime cannot draw that band, transfer the material to the
# already-solid north/west perimeter of the same room. These five linedefs are
# an L-shaped enclosing wall; no traversable line becomes solid.
FLAT_MATERIAL_TRANSFER_RECIPES = {
    "E1M1": (
        FlatMaterialTransferRecipe(
            name="start-room-computer-bank",
            source=SourceFeatureSignature(
                endpoints=((896, 3360), (896, 3104)),
                right_heights=(-8, 120),
                left_heights=(0, 72),
                texture_side="right",
                texture_field="upper",
                texture_name="COMPUTE2",
                linedef_hint=50,
            ),
            targets=(
                SolidTargetSignature(((704, 2944), (832, 2944)),
                                     "right", (0, 72), "middle", "STARTAN3", 40),
                SolidTargetSignature(((832, 2944), (968, 2880)),
                                     "right", (0, 72), "middle", "STARTAN3", 41),
                SolidTargetSignature(((968, 2880), (1216, 2880)),
                                     "right", (0, 72), "middle", "STARTAN3", 42),
                SolidTargetSignature(((704, 3552), (704, 3360)),
                                     "right", (0, 72), "middle", "STARTAN3", 52),
                SolidTargetSignature(((704, 3104), (704, 2944)),
                                     "right", (0, 72), "middle", "STARTAN3", 53),
            ),
        ),
    ),
}


PREVIEW_POSES = {
    "E1M1": (
        ("spawn", 1056, 3616, 192),
        ("approach", 1056, 3504, 192),
        ("entry", 1056, 3424, 192),
        ("beside", 1056, 3328, 192),
        ("west-room-turn", 800, 3264, 128),
    ),
}


def _line_endpoints(vertices, linedef):
    return frozenset((vertices[linedef["v1"]], vertices[linedef["v2"]]))


def _resolve_unique_line(mapn, role, signature, vertices, linedefs):
    wanted = frozenset(signature.endpoints)
    matches = [
        line_id for line_id, linedef in enumerate(linedefs)
        if _line_endpoints(vertices, linedef) == wanted
    ]
    if len(matches) != 1:
        raise ValueError(
            "Map %s %s expected one linedef at %s, found %d" %
            (mapn, role, signature.endpoints, len(matches)))
    return matches[0], linedefs[matches[0]]


def resolve_flat_material_transfers(mapn, vertices, linedefs, sidedefs,
                                    sectors, recipes=None):
    """Resolve material transfers as ``{target_linedef: resolved}``.

    Every source and destination is matched by geometry and sector/material
    signature. Linedef indices are retained only as diagnostics. Any drift is
    fatal so a recipe cannot silently repaint a different WAD feature.
    """
    selected = (FLAT_MATERIAL_TRANSFER_RECIPES.get(mapn, ())
                if recipes is None else recipes)
    resolved = {}
    for recipe in selected:
        source = recipe.source
        source_id, source_line = _resolve_unique_line(
            mapn, "material source %s" % recipe.name, source,
            vertices, linedefs)
        if source_line["right"] == 0xFFFF or source_line["left"] == 0xFFFF:
            raise ValueError("Map %s material source %s is no longer two-sided" %
                             (mapn, recipe.name))
        right_side = sidedefs[source_line["right"]]
        left_side = sidedefs[source_line["left"]]
        actual_right = tuple(sectors[right_side["sector"]][key]
                             for key in ("floor", "ceiling"))
        actual_left = tuple(sectors[left_side["sector"]][key]
                            for key in ("floor", "ceiling"))
        if (actual_right != source.right_heights or
                actual_left != source.left_heights):
            raise ValueError(
                "Map %s material source %s sector heights drifted: right=%s left=%s" %
                (mapn, recipe.name, actual_right, actual_left))
        source_side_id = source_line[source.texture_side]
        actual_texture = sidedefs[source_side_id][source.texture_field]
        if actual_texture != source.texture_name:
            raise ValueError(
                "Map %s material source %s expected %s.%s=%s, found %s" %
                (mapn, recipe.name, source.texture_side, source.texture_field,
                 source.texture_name, actual_texture))

        for target in recipe.targets:
            target_id, target_line = _resolve_unique_line(
                mapn, "material target %s" % recipe.name, target,
                vertices, linedefs)
            opposite = "left" if target.side == "right" else "right"
            if (target_line[target.side] == 0xFFFF or
                    target_line[opposite] != 0xFFFF):
                raise ValueError(
                    "Map %s material target %s at %s is no longer a one-sided %s wall" %
                    (mapn, recipe.name, target.endpoints, target.side))
            target_side_id = target_line[target.side]
            target_side = sidedefs[target_side_id]
            actual_heights = tuple(sectors[target_side["sector"]][key]
                                   for key in ("floor", "ceiling"))
            if actual_heights != target.sector_heights:
                raise ValueError(
                    "Map %s material target %s at %s sector heights drifted: %s" %
                    (mapn, recipe.name, target.endpoints, actual_heights))
            actual_target_texture = target_side[target.texture_field]
            if actual_target_texture != target.texture_name:
                raise ValueError(
                    "Map %s material target %s at %s expected %s.%s=%s, found %s" %
                    (mapn, recipe.name, target.endpoints, target.side,
                     target.texture_field, target.texture_name,
                     actual_target_texture))
            if target_id in resolved:
                raise ValueError("Map %s material target linedef %d is duplicated" %
                                 (mapn, target_id))
            resolved[target_id] = ResolvedFlatMaterialTransfer(
                recipe, source_id, target_id, target_side_id)
    return resolved
