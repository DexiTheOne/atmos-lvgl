"""Rebuild the checked-in 1-bit DejaVu Sans Bold LVGL subset (Pillow required)."""
from pathlib import Path
import sys
from PIL import Image, ImageDraw, ImageFont
SYMMETRIC='0O8o'  # mirror-symmetric in DejaVu Sans Bold
def mirror(bits,w,h):
 # Hinting can leave a symmetric glyph a pixel wider on one side. Make it
 # symmetric about its box: OR or AND with its mirror, whichever changes less.
 g=[bits[r*w:(r+1)*w] for r in range(h)]
 m=[[a|b for a,b in zip(l,l[::-1])] for l in g]; n=[[a&b for a,b in zip(l,l[::-1])] for l in g]
 diff=lambda x:sum(a!=b for l,k in zip(x,g) for a,b in zip(l,k))
 return [b for l in min((m,n),key=diff) for b in l]
def straighten(bits,w,h):
 # Hinting leaves straight diagonals (the neck of the 2, the 7, the 4) with
 # uneven steps. Follow each left and right edge from row to row; where it moves
 # the same way for at least MIN_ROWS rows and lies within half a pixel of a
 # straight line, put it exactly on that line so the steps are even. Curves do
 # not fit a line that closely and are left alone.
 g=[bits[r*w:(r+1)*w] for r in range(h)]
 runs=[]
 for l in g:
  rr=[]; c=0
  while c<w:
   if l[c]:
    a=c
    while c<w and l[c]: c+=1
    rr.append([a,c-1])
   else: c+=1
  runs.append(rr)
 def chains(side):
  used=set(); out=[]
  for r in range(h):
   for i in range(len(runs[r])):
    if (r,i) in used: continue
    ch=[(r,i)]; used.add((r,i)); rr,ii=r,i
    while rr+1<h:
     a,b=runs[rr][ii]
     nx=[j for j,(c,d) in enumerate(runs[rr+1]) if c<=b+1 and d>=a-1]
     if len(nx)!=1 or (rr+1,nx[0]) in used: break
     prev=[j for j,(c,d) in enumerate(runs[rr]) if c<=runs[rr+1][nx[0]][1]+1 and d>=runs[rr+1][nx[0]][0]-1]
     if len(prev)!=1: break
     rr,ii=rr+1,nx[0]; ch.append((rr,ii)); used.add((rr,ii))
    out.append(ch)
  return out
 fitted={0:{},1:{}}
 for side in (0,1):
  for ch in chains(side):
   xs=[runs[r][i][side] for r,i in ch]
   k=0
   while k<len(xs)-1:
    d=xs[k+1]-xs[k]
    if d==0: k+=1; continue
    sg=1 if d>0 else -1; e=k+1
    wid=lambda j:runs[ch[j][0]][ch[j][1]][1]-runs[ch[j][0]][ch[j][1]][0]
    # stop at junctions (the 2's base bar), where the run's width jumps
    while e+1<len(xs) and (xs[e+1]-xs[e])*sg>=0 and abs(xs[e+1]-xs[e])<=3 and abs(wid(e+1)-wid(e))<=2: e+=1
    seg=list(range(k,e+1))
    def linefit(seg):
     n=len(seg); mx=sum(seg)/n; my=sum(xs[j] for j in seg)/n
     sxx=sum((j-mx)**2 for j in seg); m=sum((j-mx)*(xs[j]-my) for j in seg)/sxx
     return m,[my+m*(j-mx) for j in seg]
    # Trim curved ends (the neck of the 2 bends into the arch) until the rest
    # is straight.
    while len(seg)>=MIN_ROWS:
     m,fit=linefit(seg); err=[abs(xs[j]-f) for j,f in zip(seg,fit)]
     if max(err)<=0.75: break
     seg=seg[1:] if err[0]>=err[-1] else seg[:-1]
    if len(seg)>=MIN_ROWS and abs(xs[seg[-1]]-xs[seg[0]])>=3:
     m,fit=linefit(seg)
     if True:
      for j,f in zip(seg,fit):
       r,i=ch[j]; v=int(f+0.5) if f>=0 else int(f-0.5)
       a,b=runs[r][i]
       if side==0 and v<=b: runs[r][i][0]=v
       if side==1 and v>=a: runs[r][i][1]=v
       fitted[side][(r,i)]=m
    k=e
 # A straight stroke whose two edges step on different rows changes width by a
 # pixel at each step and looks jagged. Where both edges follow parallel lines,
 # give the stroke one width, so both edges step together.
 both=[k for k in fitted[0] if k in fitted[1] and abs(fitted[0][k]-fitted[1][k])<0.4]
 groups={}
 for r,i in both:
  key=round(fitted[0][(r,i)],3)
  groups.setdefault(key,[]).append((r,i))
 for ks in groups.values():
  if len(ks)<MIN_ROWS: continue
  ws=sorted(runs[r][i][1]-runs[r][i][0] for r,i in ks); W=ws[len(ws)//2]
  for r,i in ks: runs[r][i][1]=runs[r][i][0]+W
 out=[]
 for r in range(h):
  l=[0]*w
  for a,b in runs[r]:
   for c in range(max(0,a),min(w,b+1)): l[c]=1
  out.append(l)
 return [b for l in out for b in l]
MIN_ROWS=5
# Hand-drawn glyphs where hinting leaves too few pixels to clean up. Same box
# size as the rendered glyph (checked), so offsets and advance are unchanged.
OVERRIDES={
 (16,'3'):['...#####...',
           '.########..',
           '.#....####.',
           '......####.',
           '...######..',
           '...######..',
           '......####.',
           '......####.',
           '......####.',
           '.#....####.',
           '.########..',
           '...#####...'],
}
def counters(g,h,w):
 # Paper components not reachable from outside: list of pixel sets, top first.
 seen=[[0]*w for _ in range(h)]; out=[[0]*(w+2) for _ in range(h+2)]; out[0][0]=1; todo=[(0,0)]
 while todo:
  r,c=todo.pop()
  for rr,cc in ((r+1,c),(r-1,c),(r,c+1),(r,c-1)):
   if 0<=rr<h+2 and 0<=cc<w+2 and not out[rr][cc] and not (1<=rr<=h and 1<=cc<=w and g[rr-1][cc-1]):
    out[rr][cc]=1; todo.append((rr,cc))
 comps=[]
 for r in range(h):
  for c in range(w):
   if not g[r][c] and not out[r+1][c+1] and not seen[r][c]:
    comp=[]; todo=[(r,c)]; seen[r][c]=1
    while todo:
     a,b=todo.pop(); comp.append((a,b))
     for aa,bb in ((a+1,b),(a-1,b),(a,b+1),(a,b-1)):
      if 0<=aa<h and 0<=bb<w and not g[aa][bb] and not seen[aa][bb]: seen[aa][bb]=1; todo.append((aa,bb))
    comps.append(comp)
 return sorted(comps,key=lambda k:sum(a for a,_ in k)/len(k))
def smooth_counters(bits,w,h,big,ch):
 # Hinting leaves holes (0, 6, 8, 9, O) lopsided and stepped. Redraw each hole,
 # inside its hinted bounding box (so stroke weights stay), from the unhinted
 # outline at 8x size, made mirror-symmetric left to right when the outline's
 # hole is (so the 4's triangle keeps its slope).
 g=[bits[r*w:(r+1)*w] for r in range(h)]
 holes=counters(g,h,w)
 if not holes: return bits
 x0,y0,x1,y1=big.getbbox(ch,anchor='ls'); W,H=x1-x0,y1-y0
 im=Image.new('L',(W,H)); ImageDraw.Draw(im).text((-x0,-y0),ch,font=big,fill=255,anchor='ls')
 D=[int(v>=128) for v in im.getdata()]; B=[D[i*W:(i+1)*W] for i in range(H)]
 bholes=counters(B,H,W)
 if len(bholes)!=len(holes): return bits
 for hp,bp in zip(holes,bholes):
  r0=min(a for a,_ in hp); r1=max(a for a,_ in hp)+1; c0=min(b for _,b in hp); c1=max(b for _,b in hp)+1
  R0=min(a for a,_ in bp); R1=max(a for a,_ in bp)+1; C0=min(b for _,b in bp); C1=max(b for _,b in bp)+1
  bs=set(bp)
  def cov(r,c):
   ya=R0+(r-r0)*(R1-R0)/(r1-r0); yb=R0+(r+1-r0)*(R1-R0)/(r1-r0)
   xa=C0+(c-c0)*(C1-C0)/(c1-c0); xb=C0+(c+1-c0)*(C1-C0)/(c1-c0)
   pts=[(int(ya+(yb-ya)*(i+.5)/4),int(xa+(xb-xa)*(j+.5)/4)) for i in range(4) for j in range(4)]
   return sum(p in bs for p in pts)/16
  mine=set(hp)
  # Mirror only holes that are symmetric in the outline (not the 4's triangle).
  mir=sum((a,C0+C1-1-b) in bs for a,b in bp)/len(bp)>=0.9
  for r in range(r0,r1):
   for c in range(c0,c1):
    if (r,c) in mine or g[r][c]:
     v=(cov(r,c)+cov(r,c0+c1-1-c))/2 if mir else cov(r,c)
     g[r][c]=0 if v>=0.5 else 1
 return [b for l in g for b in l]
def left_mirror(bits,w,h):
 # At 12 and 16 px hinting leaves both the outline and the holes of 0, 8, O and
 # o lopsided, and there are too few pixels to clean them. Use the left half,
 # mirrored about the centre between the leftmost ink column and the most common
 # right edge.
 g=[bits[r*w:(r+1)*w] for r in range(h)]
 rows=[l for l in g if 1 in l]
 L=min(l.index(1) for l in rows)
 rs=sorted(w-1-l[::-1].index(1) for l,k in zip(rows,rows) if l.index(1)==L); R=rs[len(rs)//2]
 out=[]
 for l in g:
  n=[0]*w
  for c in range(w):
   m=L+R-c
   if 2*c<=L+R: n[c]=l[c]
   elif 0<=m<w: n[c]=l[m]
  out.append(n)
 return [b for l in out for b in l]
def keep_counters(orig,new,w,h):
 # The clean-up squared off the holes in 0, 6, 8 and 9. Counters (paper not
 # reachable from outside the glyph) and the ink bordering them keep the
 # hinted rendering; only outer edges are cleaned.
 out=[[0]*(w+2) for _ in range(h+2)]; out[0][0]=1; todo=[(0,0)]
 while todo:
  r,c=todo.pop()
  for rr,cc in ((r+1,c),(r-1,c),(r,c+1),(r,c-1)):
   if 0<=rr<h+2 and 0<=cc<w+2 and not out[rr][cc] and not (1<=rr<=h and 1<=cc<=w and orig[(rr-1)*w+cc-1]):
    out[rr][cc]=1; todo.append((rr,cc))
 hole=lambda r,c:0<=r<h and 0<=c<w and not orig[r*w+c] and not out[r+1][c+1]
 res=new[:]
 for r in range(h):
  for c in range(w):
   if any(hole(r+i,c+j) for i in (-1,0,1) for j in (-1,0,1)): res[r*w+c]=orig[r*w+c]
 return res
def denub(bits,w,h,size):
 # Thresholding the hinted glyph leaves stray steps on curves: a pixel or two
 # sticking out of an otherwise even edge (the 40 px 6, the bowl of a 5, one side
 # of an O) or missing from it. Even such steps out, on outer edges and counters,
 # rows and columns. Runs of one pixel from 16 px up, of two from 24 px up; the
 # glyph's first and last rows and columns never change, so heights, baselines
 # and widths stay. Then remove one-pixel spikes at the end of a wider edge (the
 # tail tips of the 5 at 32 px) from 24 px up.
 g=[bits[r*w:(r+1)*w] for r in range(h)]
 def even(g,H,W,lmax):
  at=lambda r,c:g[r][c] if 0<=r<H and 0<=c<W else 0
  for L in range(1,lmax+1):
   for r in range(1,H-L):
    for c in range(1,W-1):
     for d in (-1,1):
      run=range(r,r+L)
      if (all(g[k][c] and not at(k,c+d) for k in run) and not at(r-1,c) and not at(r+L,c)
          and at(r-1,c-d) and at(r+L,c-d)):
       for k in run: g[k][c]=0
      elif (all(not g[k][c] and at(k,c-d) and not at(k,c+d) for k in run) and at(r-1,c) and at(r+L,c)
          and not at(r-1,c+d) and not at(r+L,c+d)):
       for k in run: g[k][c]=1
 if size>=16:
  lmax=2 if size>=24 else 1
  even(g,h,w,lmax)
  t=[list(x) for x in zip(*g)]; even(t,w,h,lmax); g=[list(x) for x in zip(*t)]
 at=lambda r,c:g[r][c] if 0<=r<h and 0<=c<w else 0
 if size>=24:
  for r in range(1,h-1):
   for c in range(w):
    for d in (-1,1):
     if g[r][c] and not at(r+d,c) and not at(r,c-1) and not at(r,c+1) and at(r-d,c) and (at(r-d,c+1) and at(r-d,c+2) or at(r-d,c-1) and at(r-d,c-2)): g[r][c]=0
 return [b for l in g for b in l]
source=Path(sys.argv[1])
chars=list(range(32,127))+[176,179,181]
out=['/* Generated by generate_ui_fonts.py; see fonts/LICENSE_DEJAVU. */','#include "ui_fonts.h"']
for size in [12,16,24,28,32,40,48,60,64,72,80,96,112]:
 f=ImageFont.truetype(str(source),size); big=ImageFont.truetype(str(source),size*8); asc,desc=f.getmetrics(); data=[]; ds=['{0}']
 for code in chars:
  ch=chr(code); x0,y0,x1,y1=f.getbbox(ch,anchor='ls'); w,h=x1-x0,y1-y0
  im=Image.new('L',(max(1,w),max(1,h)));ImageDraw.Draw(im).text((-x0,-y0),ch,font=f,fill=255,anchor='ls')
  bits=[int(v>=128) for v in im.getdata()] if w*h else []
  # Skip 12 px (one pixel there is crossbar or stem detail) and glyphs with real points.
  if (size,ch) in OVERRIDES:
   o=OVERRIDES[size,ch]; assert (len(o[0]),len(o))==(w,h),(size,ch,w,h)
   bits=[int(v=='#') for l in o for v in l]
  elif bits and size==12 and ch.isdigit(): bits=denub(left_mirror(bits,w,h) if ch in SYMMETRIC else bits,w,h,16)
  elif bits and size<=16 and ch in SYMMETRIC: bits=left_mirror(bits,w,h)
  elif bits and size>=16 and ch not in '*<>{}~$': bits=keep_counters(bits,denub(straighten(mirror(bits,w,h) if ch in SYMMETRIC else bits,w,h) if size>=24 else (mirror(bits,w,h) if ch in SYMMETRIC else bits),w,h,size),w,h)
  if bits: bits=smooth_counters(bits,w,h,big,ch)
  packed=[sum((bits[j+k] if j+k<len(bits) else 0)<<(7-k) for k in range(8)) for j in range(0,len(bits),8)]
  ds.append('{.bitmap_index=%d,.adv_w=%d,.box_w=%d,.box_h=%d,.ofs_x=%d,.ofs_y=%d}'%(len(data),round(f.getlength(ch)*16),w,h,x0,-y1));data+=packed
 out+=['static const uint8_t bitmap_%d[]={%s};'%(size,','.join(map(str,data))), 'static const lv_font_fmt_txt_glyph_dsc_t glyph_%d[]={%s};'%(size,','.join(ds))]
 out+=['static const uint16_t unicode_%d[]={%s};'%(size,','.join(str(c-32) for c in chars)), 'static const lv_font_fmt_txt_cmap_t map_%d[]={ {.range_start=32,.range_length=150,.glyph_id_start=1,.unicode_list=unicode_%d,.list_length=%d,.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY} };'%(size,size,len(chars))]
 out+=['static const lv_font_fmt_txt_dsc_t dsc_%d={.glyph_bitmap=bitmap_%d,.glyph_dsc=glyph_%d,.cmaps=map_%d,.cmap_num=1,.bpp=1};'%(size,size,size,size), 'const lv_font_t ui_bold_%d={.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height=%d,.base_line=%d,.dsc=&dsc_%d};'%(size,asc+desc,desc,size)]
Path(__file__).with_name('ui_fonts.c').write_text('\n'.join(out)+'\n')
Path(__file__).with_name('ui_fonts.h').write_text('#pragma once\n#include "lvgl.h"\n'+''.join('LV_FONT_DECLARE(ui_bold_%d);\n'%s for s in [12,16,24,28,32,40,48,60,64,72,80,96,112]))
