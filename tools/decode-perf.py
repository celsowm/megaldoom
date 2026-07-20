import json,struct,sys
b=bytes.fromhex(json.load(open(sys.argv[1]))['perfMailbox'])
fields=[('upload_dirty_tiles','u16'),('upload_tiles','u16'),('upload_runs','u16'),('redraw_reasons','u16'),('overlay_restored_tiles','u16'),('overlay_touched_tiles','u16'),('overlay_overlap_tiles','u16'),('upload_full','b'),('upload_swap','b'),('gameplay_subticks','u32'),('cast_subticks','u32'),('pack_subticks','u32'),('projection_subticks','u32'),('billboard_subticks','u32'),('weapon_subticks','u32'),('upload_prepare_subticks','u32'),('dma_wait_subticks','u32'),('diagnostics_subticks','u32'),('total_vblanks','u16'),('average_vblanks_x10','u16'),('p95_vblanks','u16'),('max_vblanks','u16'),('missed_deadlines','u16'),('deep_subticks','u32',6),('deep_units','u16',6),('deep_phase','u8'),('asm_compare_tile','u16'),('asm_checked_tiles','u16'),('asm_mismatches','u16'),('asm_canary_failures','u16'),('asm_cycles','u16'),('columns_changed','u16'),('columns_reused','u16'),('hypothetical_tiles_uploaded','u16'),('columns_changed_max','u16'),('columns_rebuild_frames','u16'),('columns_changed_sum','u32')]
off=0;out={}
def align(o,a):return (o+a-1)//a*a
for f in fields:
    name=f[0];t=f[1];n=f[2] if len(f)>2 else 1;a=2 if t in('u16','u32') else 1;off=align(off,a)
    for i in range(n):
        if t=='u16':v=struct.unpack_from('>H',b,off)[0];off+=2
        elif t=='u32':v=struct.unpack_from('>I',b,off)[0];off+=4
        else:v=b[off];off+=1
        out[name+(f'[{i}]' if n>1 else '')]=v
d=json.load(open(sys.argv[1]))
print("checkpoints:",d['checkpoints'])
for k in ['average_vblanks_x10','max_vblanks','missed_deadlines','pack_subticks','upload_full','upload_dirty_tiles','asm_mismatches','columns_changed','columns_reused','hypothetical_tiles_uploaded','columns_changed_max','columns_rebuild_frames','columns_changed_sum']:
    print(f"  {k:28} = {out[k]}")
if out['columns_rebuild_frames']:
    avg=out['columns_changed_sum']/out['columns_rebuild_frames']
    print(f"  {'columns_changed_avg':28} = {avg:.2f}  ({out['columns_rebuild_frames']} rebuild frames)")
    print(f"  {'avg_tiles_per_rebuild':28} = {avg*15:.0f}  (vs 300 full; {avg*15/300*100:.0f}% of full DMA)")
