import json,struct,sys

b=bytes.fromhex(json.load(open(sys.argv[1]))['perfMailbox'])

# Field layout MUST mirror RendererPerfSnapshot in src/renderer_perf.h.
# u16/u32 align to 2 bytes on 68000.
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
    # ColumnReuseOracle
    ('columns_changed','u16'),('columns_reused','u16'),('hypothetical_tiles_uploaded','u16'),
    ('columns_changed_max','u16'),('columns_rebuild_frames','u16'),('columns_changed_sum','u32'),
    # SparseTileOracle (last / max / sum per field)
    ('sparse_dyn_wall_last','u16'),('sparse_ceiling_last','u16'),('sparse_floor_last','u16'),
    ('sparse_overlay_last','u16'),('sparse_dyn_runs_last','u16'),('sparse_dma_bytes_last','u16'),
    ('sparse_dyn_wall_max','u16'),('sparse_ceiling_max','u16'),('sparse_floor_max','u16'),
    ('sparse_overlay_max','u16'),('sparse_dyn_runs_max','u16'),('sparse_dma_bytes_max','u16'),
    ('sparse_rebuild_frames','u16'),
    ('sparse_dyn_wall_sum','u32'),('sparse_ceiling_sum','u32'),('sparse_floor_sum','u32'),
    ('sparse_overlay_sum','u32'),('sparse_dyn_runs_sum','u32'),('sparse_dma_bytes_sum','u32'),
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
print("checkpoints:",d['checkpoints'])
for k in ['average_vblanks_x10','max_vblanks','missed_deadlines','pack_subticks',
          'upload_full','upload_dirty_tiles','asm_mismatches']:
    print(f"  {k:28} = {out[k]}")

# Column oracle
fr=out['columns_rebuild_frames']
if fr:
    avg=out['columns_changed_sum']/fr
    print(f"\n  COLUMN ORACLE ({fr} rebuild frames):")
    print(f"    avg columns changed   = {avg:.2f} / 20")
    print(f"    max columns changed   = {out['columns_changed_max']} / 20")
    print(f"    avg tiles/rebuild     = {avg*15:.0f}  ({avg*15/300*100:.0f}% of full DMA)")

# Sparse oracle
sf=out['sparse_rebuild_frames']
if sf:
    def avg(k): return out[k+'_sum']/sf
    print(f"\n  SPARSE TILE ORACLE ({sf} rebuild frames):")
    print(f"    avg dynamic wall tiles = {avg('sparse_dyn_wall'):.1f}  (max {out['sparse_dyn_wall_max']})")
    print(f"    avg ceiling tiles     = {avg('sparse_ceiling'):.1f}")
    print(f"    avg floor tiles       = {avg('sparse_floor'):.1f}")
    print(f"    avg overlay tiles     = {avg('sparse_overlay'):.1f}  (max {out['sparse_overlay_max']})")
    print(f"    avg dyn runs         = {avg('sparse_dyn_runs'):.1f}")
    dma=avg('sparse_dma_bytes')
    print(f"    avg est DMA bytes    = {dma:.0f}  (max {out['sparse_dma_bytes_max']})")
    # Gate: 120 safe, 121-150 tight, >150 two VBlanks. Compare dyn_wall (excl. 600 tilemap + overlay).
    dw=avg('sparse_dyn_wall')+avg('sparse_overlay')
    print(f"    P95-ish dyn tiles (avg) = {dw:.1f}  -> ", end='')
    if dw<=120: print("GO (fits in 1 VBlank)")
    elif dw<=150: print("MAYBE (tight; measure real VBlank budget)")
    else: print("NO-GO for true 60fps (needs 2 VBlanks)")
