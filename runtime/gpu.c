/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* GM20B front end: GPFIFO, puller, semaphores/syncpoints, I2M, DMA, 2D and the parts of 3D that write memory
   (clears, reports). the drawing itself is the renderer's job */
#include "rt.h"
#include "gpu.h"

GpuState g_gpu;
static PMutex g_gpulock;

/* ============================================================ GPU memory */
extern volatile int g_cs_any; void cs_mem_need(u64 a, u64 n);

int gmem_read(u64 gva, void* dst, u64 size){
  u8* d=dst; if(g_cs_any) cs_mem_need(gva,size);
  while(size){ u64 cpu=gmmu_translate(gva); u64 n=0x1000-(gva&0xFFF); if(n>size) n=size;
    if(!cpu){ memset(d,0,n); } else memcpy(d,GP(cpu),n);
    d+=n; gva+=n; size-=n; }
  return 0; }

void (*g_ww_touch)(u64 cpu);   /* GPU wrote guest memory, record it for write tracking */
int gmem_write(u64 gva, const void* src, u64 size){
  const u8* s=src; u64 g0=gva, s0=size;
  while(size){ u64 cpu=gmmu_translate(gva); u64 n=0x1000-(gva&0xFFF); if(n>size) n=size;
    if(cpu){ memcpy(GP(cpu),s,n); if(g_ww_touch) g_ww_touch(cpu); } s+=n; gva+=n; size-=n; }
  gpu_mem_written(g0,s0);
  return 0; }

u8* gmem_ptr(u64 gva, u64 size){
  /* host pointer, only if the whole range is contiguous */
  if(g_cs_any) cs_mem_need(gva,size);
  u64 c0=gmmu_translate(gva); if(!c0) return NULL;
  for(u64 o=(0x1000-(gva&0xFFF));o<size;o+=0x1000){ if(gmmu_translate(gva+o)!=c0+o) return NULL; }
  return GP(c0); }

u32 gmem_r32(u64 gva){ u32 v=0; gmem_read(gva,&v,4); return v; }

void gmem_w32(u64 gva, u32 v){ gmem_write(gva,&v,4); }

void gmem_w64(u64 gva, u64 v){ gmem_write(gva,&v,8); }

/* ============================================================ block linear */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2){
  u32 gob_w=(width_bytes+63)/64; u32 bh=1u<<bh_log2, bd=1u<<bd_log2;
  u32 blocks_h=((height+7)/8+bh-1)/bh;
  u32 gx=x_bytes/64, gy=y/8;
  u32 block_y=gy/bh, gob_in=gy%bh; u32 block_z=z/bd, z_in=z%bd;
  u64 block_size=512ull*bh*bd;
  u64 off=((u64)block_z*blocks_h*gob_w + (u64)block_y*gob_w + gx)*block_size + ((u64)z_in*bh + gob_in)*512;
  u32 xb=x_bytes%64, yb=y%8;
  off+= ((xb%64)/32)*256 + (yb/2)*64 + ((xb%32)/16)*32 + (yb%2)*16 + (xb%16);
  return off; }

void deswizzle(u8* dst, u32 dst_pitch, const u8* src, u32 w_bytes, u32 h, u32 bh_log2){
  for(u32 y=0;y<h;y++){
    u8* d=dst+(u64)y*dst_pitch;
    for(u32 x=0;x<w_bytes;x+=16){
      u64 o=bl_offset(x,y,0,w_bytes,h,bh_log2,0); u32 n=w_bytes-x<16? w_bytes-x : 16; memcpy(d+x,src+o,n); } } }

void swizzle(u8* dst, const u8* src, u32 src_pitch, u32 w_bytes, u32 h, u32 bh_log2){
  for(u32 y=0;y<h;y++){ const u8* s=src+(u64)y*src_pitch;
    for(u32 x=0;x<w_bytes;x+=16){ u64 o=bl_offset(x,y,0,w_bytes,h,bh_log2,0); u32 n=w_bytes-x<16? w_bytes-x : 16; memcpy(dst+o,s+x,n); } } }

/* ============================================================ puller */
static void puller_method(GpuChannel* ch, u32 m, u32 v){
  u32* r=ch->puller;
  r[m]=v;
  switch(m){
    case 0x00: break;
    case 0x07: { /* SemaphoreOperation */
      u64 addr=((u64)(r[4]&0xFF)<<32)|r[5]; u32 op=v&0x1F;
      { static int nl; if(nl<24){ nl++; LOG("gpu: channel semaphore op %u at %llx payload %x (word %x, now %x)\n",op,(unsigned long long)addr,r[6],v,gmem_r32(addr)); } }
      if(op==0x10){ /* reduction: an atomic op on the word instead of a write (fence counters) */
        extern void gpu_sem_reduce(u64 addr, u32 redop, u32 payload, int is_signed); extern int gpu_gl_hold_reduce(u64,u32,u32,int);
        if(!gpu_gl_hold_reduce(addr,(v>>27)&0xF,r[6],!((v>>31)&1))) gpu_sem_reduce(addr,(v>>27)&0xF,r[6],!((v>>31)&1));
      } else if(op==8){ /* acquire AND: wait until (mem & payload) != 0 */
        extern void gpu_gl_before_syncwait(void); gpu_gl_before_syncwait();
        u64 end=plat_ns()+2000000000ull;
        u32 bo=0; while(!(gmem_r32(addr)&r[6]) && plat_ns()<end) plat_backoff(&bo);
      } else
      if(op==2){ /* release */
        extern int gpu_gl_hold_write(u64,const void*,u32);   /* after compute results still on their way */
        if(v&(1u<<24)) { if(!gpu_gl_hold_write(addr,&r[6],4)) gmem_w32(addr,r[6]); }                     /* 4 bytes only */
        else { extern u64 gpu_ticks(void); u32 q[4]={r[6],0,0,0}; u64 t=gpu_ticks(); memcpy(&q[2],&t,8); if(!gpu_gl_hold_write(addr,q,16)) gmem_write(addr,q,16); }
      } else if(op==1 || op==4){ /* acquire, with a timeout so a bad value can't hang us */
        extern void gpu_gl_before_syncwait(void); gpu_gl_before_syncwait();
        u64 end=plat_ns()+2000000000ull;
        u32 bo=0; for(;;){ u32 cur=gmem_r32(addr); if(op==1? cur==r[6] : (i32)(cur-r[6])>=0) break; if(plat_ns()>end){ WARN("gpu: semaphore %llx waits for %x (has %x)\n",(unsigned long long)addr,r[6],cur); break; } plat_backoff(&bo); }
      }
      break; }
    case 0x1D: { /* Syncpoint */
      u32 id=(v>>8)&0xFFF; if(v&1){ extern void gpu_gl_stream_incr(u32); gpu_gl_stream_incr(id); } else { extern void gpu_gl_before_syncwait(void); gpu_gl_before_syncwait(); syncpt_wait(id,r[0x1C],2000000000); } break; }
    default: break;
  }
}

/* ============================================================ inline to memory */
void i2m_launch(GpuChannel* ch, u32* regs, int base){
  (void)ch;
  u32 line_len=regs[base+0], lines=regs[base+1]; u64 addr=((u64)regs[base+2]<<32)|regs[base+3];
  /* 64-bit: line_len*lines can wrap in 32 bits and leave a tiny buffer for a huge upload */
  u64 len=(u64)line_len*(lines?lines:1); if(len>(256ull<<20)) len=0;
  ch->i2m_len=(u32)len; ch->i2m_pos=0; ch->i2m_addr=addr; ch->i2m_linear=(regs[base+0xC]&1);
  ch->i2m_pitch=regs[base+4]; ch->i2m_w=line_len; ch->i2m_lines=lines;
  /* block linear dst. 0x65 = block dims (w 3:0, h 7:4, d 11:8), 0x66..0x68 size, 0x69 z, 0x6A x, 0x6B y. yes the
     width field is bits 3:0 and height is 7:4 */
  ch->i2m_bh=(regs[base+5]>>4)&0xF; ch->i2m_bd=(regs[base+5]>>8)&0xF;
  ch->i2m_dw=regs[base+6]; ch->i2m_dh=regs[base+7]; ch->i2m_z=regs[base+9]; ch->i2m_x=regs[base+0xA]; ch->i2m_y=regs[base+0xB];
  if(ch->i2m_buf_cap<ch->i2m_len){ u8* nb=realloc(ch->i2m_buf,(size_t)ch->i2m_len+4); if(!nb){ ch->i2m_len=0; return; } ch->i2m_buf=nb; ch->i2m_buf_cap=ch->i2m_len; }
}

void i2m_data(GpuChannel* ch, u32 v){
  if(ch->i2m_pos>=ch->i2m_len) return;
  u32 n=ch->i2m_len-ch->i2m_pos; if(n>4) n=4; memcpy(ch->i2m_buf+ch->i2m_pos,&v,n); ch->i2m_pos+=n;
  if(ch->i2m_pos>=ch->i2m_len){
    if(ch->i2m_linear || (ch->i2m_lines<=1 && !ch->i2m_dw)){   /* single line, no surface: plain copy */
      if(ch->i2m_lines<=1 || !ch->i2m_pitch || ch->i2m_pitch==ch->i2m_w) gmem_write(ch->i2m_addr,ch->i2m_buf,ch->i2m_len);
      else for(u32 y=0;y<ch->i2m_lines;y++) gmem_write(ch->i2m_addr+(u64)y*ch->i2m_pitch,ch->i2m_buf+(u64)y*ch->i2m_w,ch->i2m_w);
      gpu_mem_written(ch->i2m_addr,ch->i2m_lines>1 && ch->i2m_pitch? (u64)ch->i2m_pitch*ch->i2m_lines : ch->i2m_len); }
    else { /* bl dst */
      u32 w=ch->i2m_w, h=ch->i2m_lines? ch->i2m_lines : 1; u32 wb=ch->i2m_dw? ch->i2m_dw : w, hh=ch->i2m_dh? ch->i2m_dh : h;
      u64 hi=0;
      for(u32 y=0;y<h;y++) for(u32 x=0;x<w;){ u32 xb=ch->i2m_x+x, n=16-(xb&15); if(n>w-x) n=w-x;
        u64 o=bl_offset(xb,ch->i2m_y+y,ch->i2m_z,wb,hh,ch->i2m_bh,ch->i2m_bd); gmem_write(ch->i2m_addr+o,ch->i2m_buf+(u64)y*w+x,n); if(o+n>hi) hi=o+n; x+=n; }
      gpu_mem_written(ch->i2m_addr,hi); }
  }
}

/* ============================================================ dispatch */
extern void r3d_pending_flush(void); extern int r3d_pending_on(void);

static void call_method(GpuChannel* ch, u32 sub, u32 m, u32 v, int last){
  if(m<0x40){ if(r3d_pending_on()) r3d_pending_flush(); puller_method(ch,m,v); if(m==0){ ch->sub_class[sub]=v&0xFFFF; } return; }
  u32 cls=ch->sub_class[sub];
  if(r3d_pending_on() && !(cls==0xB197 || (sub==0 && cls!=0xB1C0 && cls!=0xA140 && cls!=0x902D && cls!=0xB0B5))) r3d_pending_flush();   /* switching engines: flush pending draws */
  switch(cls){
    case 0xB197: maxwell3d_method(ch,m,v,last); break;
    case 0xB1C0: compute_method(ch,m,v,last); break;
    case 0xA140: kepler_i2m_method(ch,m,v); break;
    case 0x902D: fermi2d_method(ch,m,v); break;
    case 0xB0B5: dma_method(ch,m,v); break;
    default:
      if(sub==0) maxwell3d_method(ch,m,v,last);
      else if(sub==1) compute_method(ch,m,v,last);
      else if(sub==2) kepler_i2m_method(ch,m,v);
      else if(sub==3) fermi2d_method(ch,m,v);
      else if(sub==4) dma_method(ch,m,v);
      break;
  }
}

static void run_pushbuffer(GpuChannel* ch, const u32* w, u32 n){
  /* method batches can spill into the next GPFIFO entry (hw does this, yuzu's DmaPusher too). keep the batch state
     across entries or the leftover data gets parsed as headers */
  u32 i=0;
  for(;;){
    if(ch->pb_k<ch->pb_cnt){
      u32 mode=ch->pb_mode, sub=ch->pb_sub, m=ch->pb_m;
      while(ch->pb_k<ch->pb_cnt && i<n){ u32 k=ch->pb_k++; u32 mm= mode==1? m+k : mode==5? (k==0? m : m+1) : m;
        call_method(ch,sub,mm,w[i++],ch->pb_k==ch->pb_cnt); }
      if(ch->pb_k<ch->pb_cnt){ static int said; if(said<6){ said++; LOG("gpu: method batch continues in the next GPFIFO entry (sub %u method %x mode %u, %u of %u words done)\n",ch->pb_sub,ch->pb_m,ch->pb_mode,ch->pb_k,ch->pb_cnt); } return; }    /* rest comes in the next entry */
    }
    if(i>=n) return;
    u32 h=w[i++];
    u32 mode=h>>29, sub=(h>>13)&7, m=h&0x1FFF, cnt=(h>>16)&0x1FFF;
    if(h && (mode==0 || mode==2 || mode>=6)){ /* shouldn't happen in a sane stream, log it */
      static int said; if(said<6){ said++; LOG("gpu: odd command header %08x (mode %u) at word %u of %u in channel %u; words around:",h,mode,i-1,n,ch->id);
        for(u32 j= i>5? i-5 : 0; j<i+6 && j<n; j++) LOG(" %s%08x",j==i-1? "*" : "",w[j]); LOG("\n"); } }
    switch(mode){
      case 1: case 3: case 5: ch->pb_mode=mode; ch->pb_sub=sub; ch->pb_m=m; ch->pb_cnt=cnt; ch->pb_k=0; break;
      /* modes 0/2 (fermi), 6 and 7 carry no data words (same as DmaPusher). don't treat them as batches, a stray
         one would eat the methods after it, syncpoint incrs included */
      case 4: call_method(ch,sub,m,cnt,1); break;
      default: break;
    }
  }
}

static GpuChannel* chan_get(u32 id){
  for(int i=0;i<g_gpu.nchan;i++) if(g_gpu.chan[i]->id==id) return g_gpu.chan[i];
  if(g_gpu.nchan>=64){ static int said; if(!said){ said=1; WARN("gpu: more than 64 channels, sharing the last one\n"); } return g_gpu.chan[63]; }
  GpuChannel* c=calloc(1,sizeof *c); c->id=id; c->sub_class[0]=0xB197; c->sub_class[1]=0xB1C0; c->sub_class[2]=0xA140; c->sub_class[3]=0x902D; c->sub_class[4]=0xB0B5;
  /* hw reset values that NVN/deko3d rely on and never write. sRGB writes are on by default, if not sRGB targets
     get stored linear and look too dark */
  c->r3d[0x4B8]=1;
  /* gba classics leaves the color masks at reset. zero masks discarded every sprite, including the loading UI.
     initialize once; explicit masks from the game still replace these values */
  for(int i=0;i<8;i++) c->r3d[0x680+i]=0x1111;
  c->r3d[0x4E]=1; c->r3d[0x556]=1; /* render-enable reset: true */
  g_gpu.chan[g_gpu.nchan++]=c; return c; }

void gpu_submit_now(u64* e, u32 n, u32 channel){
  pm_lock(&g_gpulock);
  GpuChannel* ch=chan_get(channel);
  for(u32 k=0;k<n;k++){
    u64 ent=e[k]; u64 va=ent&0xFFFFFFFFFCull; u32 words=(u32)((ent>>42)&0x1FFFFF);
    if(!words) continue;
    u32* buf=(u32*)gmem_ptr(va,(u64)words*4);
    if(buf) run_pushbuffer(ch,buf,words);
    else { u32* tmp=malloc((u64)words*4); gmem_read(va,tmp,(u64)words*4); run_pushbuffer(ch,tmp,words); free(tmp); }
  }
  r3d_pending_flush();
  gpu_flush(ch);
  pm_unlock(&g_gpulock);
}

void gpu_init(void){ pm_init(&g_gpulock); gpu3d_init(); }

/* with GL, submissions go to the GPU thread */
volatile u64 g_gpu_submits;

void gpu_submit(u64* e, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val){ g_gpu_submits++;
  extern int gpu_async_submit(u64* e, u32 n, u32 syncpt, u32 incs, u32 wait_id, u32 wait_val);
  if(gpu_async_submit(e,n,channel,incs,wait_id,wait_val)) return;
  if(wait_id!=~0u) syncpt_wait(wait_id,wait_val,2000000000);
  gpu_submit_now(e,n,channel);
  for(u32 i=0;i<incs;i++) syncpt_incr(channel);
}

/* semaphore reductions, the GPFIFO encoding: MIN MAX XOR AND OR ADD INC DEC (INC wraps to 0 past the payload,
   DEC wraps to the payload at 0, like the hw) */
void gpu_sem_reduce(u64 addr, u32 redop, u32 payload, int is_signed){
  u32 cur=gmem_r32(addr), nv=cur;
  switch(redop){
    case 0: nv= is_signed? (u32)((i32)cur<(i32)payload? (i32)cur : (i32)payload) : (cur<payload? cur : payload); break;
    case 1: nv= is_signed? (u32)((i32)cur>(i32)payload? (i32)cur : (i32)payload) : (cur>payload? cur : payload); break;
    case 2: nv=cur^payload; break;
    case 3: nv=cur&payload; break;
    case 4: nv=cur|payload; break;
    case 5: nv=cur+payload; break;
    case 6: nv= cur>=payload? 0 : cur+1; break;
    case 7: nv= (cur==0 || cur>payload)? payload : cur-1; break;
    default: break; }
  gmem_w32(addr,nv); }
