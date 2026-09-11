import sys, struct, zlib
def read_png(p):
    d=open(p,'rb').read(); pos=8; w=h=0; idat=b''
    while pos<len(d):
        n=struct.unpack('>I',d[pos:pos+4])[0]; t=d[pos+4:pos+8]; c=d[pos+8:pos+8+n]; pos+=12+n
        if t==b'IHDR': w,h=struct.unpack('>II',c[:8])
        elif t==b'IDAT': idat+=c
    raw=zlib.decompress(idat); st=w*3
    return w,h,[raw[y*(st+1)+1:y*(st+1)+1+st] for y in range(h)]
pts=[(112+455,84+250,'wall face by lava'),(112+455,84+320,'wall face lower'),(112+150,84+450,'far grass')]
for f in sys.argv[1:]:
    w,h,rows=read_png(f); print(f.split('/')[-1])
    for x,y,n in pts: print('  %-16s'%n, tuple(rows[y][x*3:x*3+3]))
