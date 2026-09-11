import sys, struct, zlib
def read_png(p):
    d=open(p,'rb').read(); pos=8; w=h=0; idat=b''
    while pos<len(d):
        n=struct.unpack('>I',d[pos:pos+4])[0]; t=d[pos+4:pos+8]; c=d[pos+8:pos+8+n]; pos+=12+n
        if t==b'IHDR': w,h=struct.unpack('>II',c[:8])
        elif t==b'IDAT': idat+=c
    raw=zlib.decompress(idat); rows=[]; st=w*3
    for y in range(h): rows.append(bytearray(raw[y*(st+1)+1:y*(st+1)+1+st]))
    return w,h,rows
def write_png(p,w,h,rows):
    raw=b''.join(b'\x00'+bytes(r) for r in rows)
    def ch(t,dd): return struct.pack('>I',len(dd))+t+dd+struct.pack('>I',zlib.crc32(t+dd)&0xffffffff)
    open(p,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
src,dst,x0,y0,x1,y1,scale=sys.argv[1],sys.argv[2],*map(int,sys.argv[3:8])
w,h,rows=read_png(src); out=[]
for y in range(y0,y1):
    r=bytearray()
    for x in range(x0,x1):
        px=rows[y][x*3:x*3+3]
        for _ in range(scale): r+=px
    for _ in range(scale): out.append(r)
write_png(dst,(x1-x0)*scale,(y1-y0)*scale,out); print('ok')
