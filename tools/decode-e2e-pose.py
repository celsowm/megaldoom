"""Print the player pose from a DebugE2EState mailbox dump (20 bytes, big-endian)."""
import json, struct, sys
b = bytes.fromhex(json.load(open(sys.argv[1]))["perfMailbox"])[:20]
ev, kc, kl, ku, sl, el, god, hits, deaths, us, ua, ut, uk = struct.unpack(">13B", b[:13])
x, y, ang = struct.unpack(">hhH", b[14:20])
print(f"pose x={x} y={y} angle={ang}  (events=0x{ev:02X} level={sl} deaths={deaths})")
