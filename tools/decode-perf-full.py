import json,struct,sys

b=bytes.fromhex(json.load(open(sys.argv[1]))['perfMailbox'])

fields=[
    ('upload_dirty_tiles','u16'),('upload_tiles','u16'),('upload_runs','u16'),
    ('redraw_reasons','u16'),('overlay_restored_tiles','u16'),
    ('overlay_touched_tiles','u16'),('overlay_overlap_tiles','u16'),
    ('upload_full','b'),('upload_swap','b'),
    ('gameplay_subticks','u32'),('cast_subticks','u32'),('pack_subticks','u32'),
    ('projection_subticks','u32'),('billboard_subticks','u32'),
    ('weapon_subticks','u32'),('upload_prepare_subticks','u32'),
    ('dma_wait_subticks','u32'),('diagnostics_subticks','u32'),
    ('total_vblanks','u16'),('average_vblanks_x10','u16'),('p95_vblanks','u16'),
    ('max_vblanks','u16'),('missed_deadlines','u16'),
    ('deep_subticks','u32',6),('deep_units','u16',6),('deep_phase','u8'),
    ('asm_compare_tile','u16'),('asm_checked_tiles','u16'),('asm_mismatches','u16'),
    ('asm_canary_failures','u16'),('asm_cycles','u16'),
    ('columns_changed','u16'),('columns_reused','u16'),('hypothetical_tiles_uploaded','u16'),
    ('columns_changed_max','u16'),('columns_rebuild_frames','u16'),('columns_changed_sum','u32'),
    ('sparse_dyn_wall_last','u16'),('sparse_ceiling_last','u16'),('sparse_floor_last','u16'),
    ('sparse_overlay_last','u16'),('sparse_dyn_runs_last','u16'),('sparse_dma_bytes_last','u16'),
    ('sparse_dyn_wall_max','u16'),('sparse_ceiling_max','u16'),('sparse_floor_max','u16'),
    ('sparse_overlay_max','u16'),('sparse_dyn_runs_max','u16'),('sparse_dma_bytes_max','u16'),
    ('sparse_rebuild_frames','u16'),
    ('sparse_dyn_wall_sum','u32'),('sparse_ceiling_sum','u32'),('sparse_floor_sum','u32'),
    ('sparse_overlay_sum','u32'),('sparse_dyn_runs_sum','u32'),('sparse_dma_bytes_sum','u32'),
    ('cast_lut_max_invz','u16'),('cast_lut_fallback_hits','u16'),
    ('billboard_lut_max_forward','u16'),('billboard_lut_fallback_hits','u16'),
    ('visible_subsectors_last','u16'),('visible_subsector_objects_last','u16'),
    ('visible_subsectors_max','u16'),('visible_subsector_objects_max','u16'),
    ('visible_subsector_frames','u16'),('visible_subsectors_sum','u32'),
    ('visible_subsector_objects_sum','u32'),
    ('visible_subsector_safe_objects_last','u16'),
    ('visible_subsector_cullable_objects_last','u16'),
    ('visible_subsector_safe_objects_max','u16'),
    ('visible_subsector_cullable_objects_max','u16'),
    ('visible_subsector_safe_objects_sum','u32'),
    ('visible_subsector_cullable_objects_sum','u32'),
]

off=0; out={}
def align(o,a): return (o+a-1)//a*a
for f in fields:
    name=f[0]; t=f[1]; n=f[2] if len(f)>2 else 1
    a=2 if t in ('u16','u32') else 1
    off=align(off,a)
    for i in range(n):
        if t=='u16': v=struct.unpack_from('>H',b,off)[0]; off+=2
        elif t=='u32': v=struct.unpack_from('>I',b,off)[0]; off+=4
        else: v=b[off]; off+=1
        out[name+(f'[{i}]' if n>1 else '')]=v

d=json.load(open(sys.argv[1]))
print("route:", sys.argv[1], "checkpoints:", d.get('checkpoints'))
for k in ['average_vblanks_x10','p95_vblanks','max_vblanks','missed_deadlines','total_vblanks']:
    print(f"  {k:28} = {out[k]}")

stages = ['gameplay_subticks','cast_subticks','pack_subticks','projection_subticks',
          'billboard_subticks','weapon_subticks','upload_prepare_subticks',
          'dma_wait_subticks','diagnostics_subticks']
total = sum(out[s] for s in stages)
print(f"\n  SUBTICK BREAKDOWN (total={total}):")
for s in sorted(stages, key=lambda s: -out[s]):
    pct = 100*out[s]/total if total else 0
    print(f"    {s:26} = {out[s]:7d}  ({pct:5.1f}%)")

print(f"\n  upload: full={out['upload_full']} swap={out['upload_swap']} dirty_tiles={out['upload_dirty_tiles']} runs={out['upload_runs']}")
print(f"  overlay: restored={out['overlay_restored_tiles']} touched={out['overlay_touched_tiles']} overlap={out['overlay_overlap_tiles']}")
print(f"  asm: checked={out['asm_checked_tiles']} mismatches={out['asm_mismatches']} "
      f"canary_failures={out['asm_canary_failures']} cycles={out['asm_cycles']}")
print(f"  cast_lut: max_invz={out['cast_lut_max_invz']} fallback_hits={out['cast_lut_fallback_hits']}")
print(f"  billboard_lut: max_forward={out['billboard_lut_max_forward']} fallback_hits={out['billboard_lut_fallback_hits']}")
oracle_frames = out['visible_subsector_frames']
if oracle_frames:
    print("  visible-subsector oracle: "
          f"leaves={out['visible_subsectors_sum'] / oracle_frames:.1f} avg / "
          f"{out['visible_subsectors_max']} max; "
          f"point-owned objects={out['visible_subsector_objects_sum'] / oracle_frames:.1f} avg / "
          f"{out['visible_subsector_objects_max']} max")
    print(f"    footprint-safe={out['visible_subsector_safe_objects_sum'] / oracle_frames:.1f} avg / "
          f"{out['visible_subsector_safe_objects_max']} max; "
          f"safe-to-skip={out['visible_subsector_cullable_objects_sum'] / oracle_frames:.1f} avg / "
          f"{out['visible_subsector_cullable_objects_max']} max")
    print(f"    last: {out['visible_subsectors_last']} leaves, "
          f"{out['visible_subsector_objects_last']} point-owned, "
          f"{out['visible_subsector_safe_objects_last']} footprint-safe, "
          f"{out['visible_subsector_cullable_objects_last']} safe-to-skip")
