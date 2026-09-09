#!/usr/bin/env python3
"""Curated texture aliasing: fold rare wall materials onto generic common ones.

The shared atlas costs a flat 32768 bytes per texture (4 shade levels x 64 x 128),
and a texture that also appears on a door face costs that twice, because door
faces are baked a second time with their frame and safety stripe. Texture usage
across the shipped campaign is heavily concentrated -- the ten most-used names
cover about two thirds of all SEGs, while roughly thirty names appear five times
or fewer. Aliasing that tail onto the head is therefore the cheapest ROM lever
available: it costs no runtime work at all, unlike deduplicating the atlas, and
it is what lets a third map fit under the 4 MB cartridge cap.

This is a cosmetic transform with one hard exception. A texture that carries
game meaning must never be folded into a plain wall: the coloured key doors and
the exit door keep their own art, or the player loses the language that tells
them where to go. Switches are the interesting middle case -- a switch that
looks like the wall around it is unplayable, but every switch may share ONE
generic switch face and still read as a switch, so the eight SW1* materials
collapse to a single entry.

Aliasing is applied in doom_map.py at the point a SEG's texture name is
resolved, so texture_usage, door_texture_names, the world palette and the
emitted SEG texture ids all see canonical names and nothing downstream needs to
know this module exists.
"""

# Names that may never be aliased away: losing their art loses game meaning.
PROTECTED_TEXTURES = frozenset({
    "DOORBLU", "DOORRED", "DOORYEL",  # coloured key doors
    "EXITDOOR",                       # the level exit
    # The exit control. It is the one switch that must NOT look like the
    # others: world_assets curates it as the sole MIXED_RAMP_MATERIAL so its
    # grey housing and red indicator survive the earth ramp (see
    # tools/test-wall-quality.py). Folding it into the generic switch face
    # would throw that curation away and make the exit read as any other wall
    # switch.
    "SW1STRTN",
})

# source -> canonical. Flat by construction: no value is itself a key.
TEXTURE_ALIASES = {
    # Decorative switches -> one generic switch face. They still read as
    # switches, which is the only property that matters for them. The exit
    # control SW1STRTN is deliberately NOT here; see PROTECTED_TEXTURES.
    "SW1PIPE": "SW1COMP",
    "SW1STON1": "SW1COMP",
    "SW1SLAD": "SW1COMP",
    "SW1BRCOM": "SW1COMP",
    "SW1BRNGN": "SW1COMP",
    "SW1DIRT": "SW1COMP",

    # Computer panels -> COMPTALL. COMPUTE2 is the long-standing problem case:
    # an oversized semantic panel that curation and filtering could not repair
    # (see flat_map_recipes' start-room-computer-bank and wall_bake_preview's
    # COMPUTE2_OBLIQUE_POSE). Replacing it outright is the untried option.
    "COMPUTE1": "COMPTALL",
    "COMPUTE2": "COMPTALL",
    "COMPUTE3": "COMPTALL",
    "COMP2": "COMPTALL",
    "COMPSTA1": "COMPTALL",
    "COMPSTA2": "COMPTALL",
    "COMPTILE": "COMPTALL",

    # Nukage surrounds -> SLADWALL.
    "SLADRIP2": "SLADWALL",
    "SLADPOIS": "SLADWALL",
    "NUKESLAD": "SLADWALL",
    "NUKEDGE1": "SLADWALL",

    # Brown variants. AASTINKY is a door face and BROWN1 is already a door
    # texture, so the door table does not grow (see assert_alias_table_sound).
    "BRNSMALL": "BROWN96",
    "BRNSMALR": "BROWN96",
    "BRNSMALC": "BROWN96",
    "BRNBIGL": "BROWN1",
    "BRNBIGR": "BROWN1",
    "BRNBIGC": "BROWN1",
    "BRNPOIS2": "BROWN1",
    "AASTINKY": "BROWN1",

    # Greys -> STONE2. GRAY4 and STONE3 are door faces; STONE2 already is one.
    "GRAY4": "STONE2",
    "GRAY5": "STONE2",
    "GRAYTALL": "STONE2",
    "STEP1": "STONE2",
    "STONE3": "STONE2",

    # Lights, tech and star panels.
    "LITE4": "LITE3",
    "LITE5": "LITE3",
    "LITEBLU4": "LITEBLU3",
    "TEKWALL2": "TEKWALL1",
    "STARG1": "STARG3",
    "STONPOIS": "STONE",
    "DOOR3": "DOOR1",
}


def resolve_texture_alias(name):
    """Canonical name for one source texture. Never chains: the table is flat."""
    return TEXTURE_ALIASES.get(name, name)


def assert_alias_table_sound(door_texture_names=None, known_texture_names=None):
    """Validate the table. Called by the generator before anything is emitted.

    `door_texture_names` is the POST-alias door set, so the classic worry -- a
    door face aliased onto a non-door texture falling back to the wall table and
    losing its frame -- cannot happen: door_texture_names is derived from the
    already-aliased SEGs, so whatever a door face ends up carrying is in the
    door table by construction. The real hazard is the opposite one, and it is
    a silent ROM regression rather than a visual bug: aliasing a door face onto
    a texture that was NOT previously a door face ADDS a 32768-byte door entry
    instead of saving one. That is what the door check below catches.
    """
    for source, target in TEXTURE_ALIASES.items():
        if source in PROTECTED_TEXTURES:
            raise SystemExit(
                "texture alias %s -> %s targets a protected material; key doors "
                "and the exit must keep their own art" % (source, target))
        if target in TEXTURE_ALIASES:
            raise SystemExit(
                "texture alias %s -> %s chains through another alias; the table "
                "must be flat so one lookup is enough" % (source, target))
        if source == target:
            raise SystemExit("texture alias %s is a self-loop" % source)

    if known_texture_names is not None:
        # `known_texture_names` is POST-alias usage, so it already contains
        # every target that actually fired -- world_assets bakes exactly the
        # names it is handed. Requiring all targets to be present would instead
        # be wrong: the table is written against the whole campaign, and a
        # target whose source is unused in the maps being built (STONPOIS ->
        # STONE, both E1M3-only) legitimately appears in neither set.
        # The invariant worth asserting is the opposite one -- that no aliased
        # source SURVIVED, which would mean a substitution silently did not
        # apply and the atlas is paying for a texture we meant to fold away.
        survivors = sorted(set(known_texture_names) & set(TEXTURE_ALIASES))
        if survivors:
            raise SystemExit(
                "aliased textures still present after substitution: %s. The "
                "alias was not applied on every path that names a texture." %
                ", ".join(survivors))

    if door_texture_names is not None:
        doors = set(door_texture_names)
        # A door-carried alias whose target is not itself a door face grows the
        # door table by a full texture. Report it rather than paying it blind.
        grew = sorted(target for source, target in TEXTURE_ALIASES.items()
                      if source in doors and target not in doors)
        if grew:
            raise SystemExit(
                "texture aliases add door-table entries instead of removing "
                "them: %s. Retarget them at materials that already appear on a "
                "door face." % ", ".join(grew))
