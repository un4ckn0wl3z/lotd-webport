# rebase the database to argv[2] (hex), then run the normal export to argv[1]
import idc, ida_segment, ida_auto, idaapi
ida_auto.auto_wait()
new = int(idc.ARGV[2], 16)
cur = idaapi.get_imagebase()
if cur != new:
    r = ida_segment.rebase_program(new - cur, ida_segment.MSF_FIXONCE)
    print("rebase %x -> %x: %d" % (cur, new, r))
    ida_auto.auto_wait()
idc.ARGV = [idc.ARGV[0], idc.ARGV[1]]
exec(open("ida_export.py").read())
