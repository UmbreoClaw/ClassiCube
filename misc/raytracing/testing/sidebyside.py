import sys, struct, zlib
def read_png(p):
    d=open(p,'rb').read(); pos=8; w=h=0; idat=b''
    while pos<len(d):
        n=struct.unpack('>I',d[pos:pos+4])[0]; t=d[pos+4:pos+8]; c=d[pos+8:pos+8+n]; pos+=12+n
        if t==b'IHDR': w,h=struct.unpack('>II',c[:8])
        elif t==b'IDAT': idat+=c
    raw=zlib.decompress(idat); st=w*3
    return w,h,[raw[y*(st+1)+1:y*(st+1)+1+st] for y in range(h)]
def write_png(p,w,h,rows):
    raw=b''.join(b'\x00'+bytes(r) for r in rows)
    def ch(t,dd): return struct.pack('>I',len(dd))+t+dd+struct.pack('>I',zlib.crc32(t+dd)&0xffffffff)
    open(p,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw,9))+ch(b'IEND',b''))
# args: out cropW cropH left.png right.png [more pairs]  -> crops top-left cropW x cropH of each (the game window) and puts them side by side
out, cw, chh = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
imgs = [read_png(p) for p in sys.argv[4:]]
gap = 8
rows = []
for y in range(chh):
    r = bytearray()
    for i,(w,h,rr) in enumerate(imgs):
        if i: r += b'\x20\x20\x20' * gap
        r += rr[y][:cw*3]
    rows.append(r)
write_png(out, cw*len(imgs) + gap*(len(imgs)-1), chh, rows); print('wrote', out)
