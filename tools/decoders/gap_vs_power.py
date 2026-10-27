"""Tabulate the exact-zero sample fraction against local signal power.

Read-only screening tool. Blocks are 10 microseconds at 40 MSps; power excludes
zero samples and is reported relative to the median block. If the zero fraction
changes with RF content, the zeros originate upstream of SRAM/DMA transport.
Usage: gap_vs_power.py CAPTURE.cs16
"""
import sys, numpy as np
path=sys.argv[1]
raw=np.memmap(path,dtype='<i2',mode='r').reshape(-1,2)
B=400  # 10 us blocks at 40 MSps
n=(len(raw)//B)*B
pw=[];zr=[]
for base in range(0,n,4_000_000):
    x=np.asarray(raw[base:min(base+4_000_000,n)],dtype=np.float32)
    z=np.all(x==0,axis=1)
    p=(x[:,0]**2+x[:,1]**2)
    zb=z.reshape(-1,B); pb=p.reshape(-1,B)
    nz=(~zb).sum(1)
    pw.append(np.where(nz>0,(pb*(~zb)).sum(1)/np.maximum(nz,1),0)); zr.append(zb.sum(1))
pw=np.concatenate(pw); zr=np.concatenate(zr)
db=10*np.log10(pw+1e-9)
floor=np.median(db)
print('blocks',len(db),'median block power dB',round(floor,2))
edges=[-99,1,3,6,10,15,20,99]
for a,b in zip(edges[:-1],edges[1:]):
    m=(db-floor>=a)&(db-floor<b)
    if m.sum()==0: continue
    print(f'{a:>4}..{b:<3} dB over floor: blocks {m.sum():>7}  zero-sample fraction {zr[m].sum()/(m.sum()*B)*100:6.3f}%')
