/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* nvdrv: nvmap, nvhost-as-gpu, nvhost-ctrl, nvhost-ctrl-gpu, nvhost-gpu. enough for NVN and deko3d */
#include "rt.h"

/* ============================================================ nvmap */
typedef struct { u32 size, align, heap, flags; u8 kind; u64 addr; u32 id; int alive; } NvMapH;

static NvMapH nvh[8192]; static u32 nvh_next=1; static PMutex g_nvlock;

u64 nvmap_addr(u32 h){ if(h>=8192) return 0; return nvh[h].addr; }

u32 nvmap_size(u32 h){ if(h>=8192) return 0; return nvh[h].size; }

static u32 nvmap_from_id(u32 id){ for(u32 i=1;i<nvh_next;i++) if(nvh[i].alive && nvh[i].id==id) return i; return 0; }

u64 nvmap_addr_by_id(u32 id){ u32 h=nvmap_from_id(id); if(!h) h=id<8192? id : 0; return nvmap_addr(h); }

/* ============================================================ GPU VA space (40 bit) */

/* two level table, gva -> guest address, 4K granularity */
#define GPAGE_BITS 12
#define GL1 (1u<<16)     /* 40-12 = 28 bits, split 16 + 12 */
static u64** gmmu; /* gmmu[i] -> 4096 entries of (guest addr | 1) */
u64 gmmu_translate(u64 gva){
  u64 p=gva>>GPAGE_BITS; u32 i=(u32)(p>>12)&(GL1-1), j=(u32)p&0xFFF;
  u64* t=gmmu[i]; if(!t) return 0; u64 e=t[j]; if(!(e&1)) return 0; return (e&~1ull)+(gva&0xFFF); }

/* the longest run from gva (up to max) that maps to contiguous guest memory, and where it starts */
u64 gmmu_run(u64 gva, u64 max, u64* cpu){
  if(!max) return 0; u64 c=gmmu_translate(gva); if(!c) return 0; *cpu=c;
  u64 n=0x1000-(gva&0xFFF), want=(c&~0xFFFull)+0x1000;
  while(n<max){ u64 e=gmmu_translate(gva+n); if(!e || (e&~0xFFFull)!=want) break; n+=0x1000; want+=0x1000; }
  return n<max? n : max; }

extern void snxvk_map_begin(u64,u64), snxvk_map_end(u64,u64), snxvk_kick(void);
extern void snxvk_submit(const void*,u32,u32,u32,u32,u32,u32);
/* renderer=vulkan_native wants the mapped runs {gva, cpu, size} looked up from the guest side: the same memory can be
   mapped at several GPU addresses and a CPU write has to reach all of them. kept with the page table, under g_runs_lk */
typedef struct { u64 gva, cpu, size; } GRun;
static GRun* g_runs; static u32 g_nruns, g_capruns; static PMutex g_runs_lk;
static void runs_push(GRun r){ if(g_nruns==g_capruns){ g_capruns=g_capruns? g_capruns*2 : 256; g_runs=realloc(g_runs,g_capruns*sizeof *g_runs); } g_runs[g_nruns++]=r; }
static void runs_cut(u64 gva, u64 size){   /* drop [gva, gva+size), keeping what the cut runs have outside it */
  u64 e=gva+size;
  for(u32 i=0;i<g_nruns;){ GRun r=g_runs[i]; u64 re=r.gva+r.size;
    if(re<=gva || r.gva>=e){ i++; continue; }
    g_runs[i]=g_runs[--g_nruns];
    if(r.gva<gva) runs_push((GRun){r.gva,r.cpu,gva-r.gva});
    if(re>e) runs_push((GRun){e,r.cpu+(e-r.gva),re-e}); } }
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx){
  u64 e=cpu+size; if(!size) return;
  pm_lock(&g_runs_lk);
  for(u32 i=0;i<g_nruns;i++){ const GRun* r=&g_runs[i]; u64 lo= r->cpu>cpu? r->cpu : cpu, hi= r->cpu+r->size<e? r->cpu+r->size : e;
    if(lo<hi) fn(ctx,r->gva+(lo-r->cpu),hi-lo); }
  pm_unlock(&g_runs_lk); }
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx){
  pm_lock(&g_runs_lk);
  while(size){ u64 cpu=0, n=gmmu_run(gva,size,&cpu);
    if(n) fn(ctx,gva,cpu,n); else { n=0x1000-(gva&0xFFF); if(n>size) n=size; }
    gva+=n; size-=n; }
  pm_unlock(&g_runs_lk); }

static void gmmu_map(u64 gva, u64 cpu, u64 size){ snxvk_map_begin(gva,size);
  pm_lock(&g_runs_lk); runs_cut(gva,size); if(size) runs_push((GRun){gva,cpu,size});
  for(u64 o=0;o<size;o+=0x1000){ u64 p=(gva+o)>>GPAGE_BITS; u32 i=(u32)(p>>12)&(GL1-1), j=(u32)p&0xFFF;
    if(!gmmu[i]) gmmu[i]=calloc(4096,8); gmmu[i][j]=(cpu+o)|1; }
  pm_unlock(&g_runs_lk); snxvk_map_end(gva,size); }

static void gmmu_unmap(u64 gva, u64 size){ snxvk_map_begin(gva,size);
  pm_lock(&g_runs_lk); runs_cut(gva,size);
  for(u64 o=0;o<size;o+=0x1000){ u64 p=(gva+o)>>GPAGE_BITS; u32 i=(u32)(p>>12)&(GL1-1), j=(u32)p&0xFFF; if(gmmu[i]) gmmu[i][j]=0; }
  pm_unlock(&g_runs_lk); snxvk_map_end(gva,size); }

typedef struct { u64 gva, size; u32 h; u64 off; } GMap;

static GMap gmaps[16384]; static int ngmaps;

/* a mapping stops at the end of its nvmap handle. games map small buffers with a bigger size (12 KB as a 64 KB big
   page) and unmap the guest page right after the buffer as a guard. on the console the rest of the page is other
   memory, here it would point the GPU at that hole and crash the GPU thread */
static void gmmu_map_handle(u64 gva, u32 h, u64 off, u64 size){
  u64 hs=((u64)nvh[h].size+0xFFF)&~0xFFFull, n= off<hs? hs-off : 0; if(n>size) n=size;
  gmmu_map(gva,nvh[h].addr+off,n); if(n<size) gmmu_unmap(gva+n,size-n); }

static u64 g_va_next=0x0400000000ull;       /* dumb bump allocator for gva */
static u64 va_alloc(u64 size, u64 align){ if(align<0x10000) align=0x10000; u64 a=(g_va_next+align-1)&~(align-1); g_va_next=a+((size+0xFFFF)&~0xFFFFull); return a; }

/* ============================================================ syncpoints */
static u32 syncpt[256]; static u32 syncpt_max[256]; static PCond g_sync_cv; static PMutex g_sync_lk;

u32 syncpt_read(u32 id){ return __atomic_load_n(&syncpt[id&255],__ATOMIC_SEQ_CST); }

void syncpt_incr(u32 id){ pm_lock(&g_sync_lk); syncpt[id&255]++; pc_broadcast(&g_sync_cv); pm_unlock(&g_sync_lk); snxvk_kick();
  extern void nv_events_check(void); nv_events_check(); }

int syncpt_wait(u32 id, u32 thresh, i64 timeout){
  pm_lock(&g_sync_lk); u64 end=plat_ns()+(u64)(timeout<0?0:timeout);
  while((i32)(syncpt[id&255]-thresh)<0){
    if(timeout==0){ pm_unlock(&g_sync_lk); return 0; }
    i64 rem=-1; if(timeout>0){ u64 now=plat_ns(); if(now>=end){ pm_unlock(&g_sync_lk); return 0; } rem=(i64)(end-now); }
    pc_wait(&g_sync_cv,&g_sync_lk,rem); }
  pm_unlock(&g_sync_lk); return 1; }

u32 syncpt_alloc(void){ static u32 next=20; u32 id=next++; if(next>=192) next=20; return id; }   /* 20..191, the hw has 192. the video channels take theirs from here too */

/* nvhost-ctrl events = async syncpoint waits */
typedef struct { KEvent* ev; u32 id, thresh; volatile int armed; } NvEvent;

static NvEvent nvev[64];
/* arming (game thread) races with nv_events_check (GPU/video threads after an incr). I clear the event before
   arming and keep both under one lock, if not an incr in between signals it, the clear wipes that signal and the
   game never wakes up */
static volatile u32 g_nvev_lk;

static void nvev_lock(void){ while(__atomic_exchange_n(&g_nvev_lk,1,__ATOMIC_ACQUIRE)){ while(__atomic_load_n(&g_nvev_lk,__ATOMIC_RELAXED)) ; } }

static void nvev_unlock(void){ __atomic_store_n(&g_nvev_lk,0,__ATOMIC_RELEASE); }

void nv_events_check(void){
  nvev_lock();
  for(int i=0;i<64;i++) if(nvev[i].armed && (i32)(syncpt_read(nvev[i].id)-nvev[i].thresh)>=0){ nvev[i].armed=0; kevent_signal(nvev[i].ev); }
  nvev_unlock(); }

/* ============================================================ files (fds) */
enum { DEV_NONE, DEV_NVMAP, DEV_AS, DEV_CTRL, DEV_CTRLGPU, DEV_GPU, DEV_NVDEC, DEV_VIC, DEV_OTHER };

u32 host1x_ioctl(int is_vic, u32 cmd, u8* d, u32 n);
typedef struct { int dev; u32 syncpt; u64 user_data; u32 class_ids[8]; int nclass; KEvent* ev_sm, *ev_err; } NvFile;

static NvFile nvf[256];

static u32 ioctl_nvmap(u32 cmd, u8* d, u32 n){
  (void)n;
  switch(cmd&0xFF){
    case 0x01: { u32 size; memcpy(&size,d,4); pm_lock(&g_nvlock);
      /* reuse freed handles once the table is full instead of writing past it */
      u32 h=0; if(nvh_next<8192) h=nvh_next++; else for(u32 i=1;i<8192;i++) if(!nvh[i].alive){ h=i; break; }
      if(!h){ pm_unlock(&g_nvlock); return 1; }
      memset(&nvh[h],0,sizeof nvh[h]); nvh[h].size=size; nvh[h].id=h; nvh[h].alive=1; pm_unlock(&g_nvlock); memcpy(d+4,&h,4); return 0; }
    case 0x03: { u32 id; memcpy(&id,d,4); u32 h=nvmap_from_id(id); if(!h) return 1; memcpy(d+4,&h,4); return 0; }
    case 0x04: { u32 h; memcpy(&h,d,4); if(!h||h>=8192) return 1; NvMapH* m=&nvh[h];
      memcpy(&m->heap,d+4,4); memcpy(&m->flags,d+8,4); memcpy(&m->align,d+12,4); m->kind=d[16]; memcpy(&m->addr,d+24,8);
      LOGV("nvmap alloc h=%u size=%x addr=%llx\n",h,m->size,(unsigned long long)m->addr); return 0; }
    case 0x05: { u32 h; memcpy(&h,d,4); if(h&&h<8192){ u64 a=nvh[h].addr; u32 s=nvh[h].size, f=nvh[h].flags; memcpy(d+8,&a,8); memcpy(d+16,&s,4); memcpy(d+20,&f,4); nvh[h].alive=0; } return 0; }
    case 0x09: { u32 h, p, r=0; memcpy(&h,d,4); memcpy(&p,d+4,4); if(h>=8192) return 1; NvMapH* m=&nvh[h];
      switch(p){ case 1: r=m->size; break; case 2: r=m->align?m->align:0x1000; break; case 3: r=0; break; case 4: r=0x40000000; break; case 5: r=m->kind; break; case 6: r=0; break; }
      memcpy(d+8,&r,4); return 0; }
    case 0x0E: { u32 h; memcpy(&h,d+4,4); u32 id= h<8192? nvh[h].id : 0; memcpy(d,&id,4); return 0; }
  }
  WARN("nvmap ioctl %x\n",cmd); return 0; }

static u32 g_big_page_bits=16;     /* 64K unless asked otherwise */

static u32 ioctl_as(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra){
  (void)n;
  switch(cmd&0xFF){
    case 0x01: return 0;
    case 0x02: { u32 pages, psz, flags; u64 off; memcpy(&pages,d,4); memcpy(&psz,d+4,4); memcpy(&flags,d+8,4); memcpy(&off,d+16,8);
      u64 size=(u64)pages*psz; u64 a= (flags&1)? off : va_alloc(size,off>psz? off : psz); memcpy(d+16,&a,8);
      LOGV("as alloc_space %llx size %llx\n",(unsigned long long)a,(unsigned long long)size); return 0; }
    case 0x03: { u64 off; u32 pages, psz; memcpy(&off,d,8); memcpy(&pages,d+8,4); memcpy(&psz,d+12,4); gmmu_unmap(off,(u64)pages*psz); return 0; }
    case 0x05: { u64 off; memcpy(&off,d,8); for(int i=0;i<ngmaps;i++) if(gmaps[i].gva==off){ gmmu_unmap(off,gmaps[i].size); gmaps[i]=gmaps[--ngmaps]; break; } return 0; }
    case 0x06: { u32 flags, kind, h, psz; i64 boff; u64 msize, off; memcpy(&flags,d,4); memcpy(&kind,d+4,4); memcpy(&h,d+8,4); memcpy(&psz,d+12,4);
      memcpy(&boff,d+16,8); memcpy(&msize,d+24,8); memcpy(&off,d+32,8);
      if((flags&0x100) && h==0) return 0;   /* just updates kind on an existing map */
      if(h>=8192 || !nvh[h].alive){ WARN("as map: invalid nvmap handle %u flags=%x kind=%x psz=%x boff=%llx msize=%llx off=%llx n=%u\n",h,flags,kind,psz,(unsigned long long)boff,(unsigned long long)msize,(unsigned long long)off,n); return 1; }
      if(flags&0x100){ /* modify: remap inside an existing mapping */
        gmmu_map_handle(off,h,(u64)boff,msize?msize:nvh[h].size); return 0; }
      u64 size= msize? msize : nvh[h].size;
      u64 gva= (flags&1)? off : va_alloc(size,psz?psz:0x10000);
      gmmu_map_handle(gva,h,(u64)boff,size);
      if(ngmaps<16384){ gmaps[ngmaps].gva=gva; gmaps[ngmaps].size=size; gmaps[ngmaps].h=h; gmaps[ngmaps].off=(u64)boff; ngmaps++; }
      memcpy(d+32,&gva,8);
      LOGV("as map h=%u cpu=%llx gva=%llx size=%llx\n",h,(unsigned long long)(nvh[h].addr+boff),(unsigned long long)gva,(unsigned long long)size);
      return 0; }
    case 0x08: { u32 bufsz=2*24; memcpy(d+8,&bufsz,4); u64 r[6]={0x04000000,0x1000,0x3FFBFFF, 0x0400000000ull,0x10000,0x1BFFF};
      u8* rg=d+16; u64 o0=0x04000000; u32 ps0=0x1000; u64 np0=0x3FFBFFF; u64 o1=0x0400000000ull; u32 ps1=1u<<g_big_page_bits; u64 np1=0x1BFFF;
      memcpy(rg,&o0,8); memcpy(rg+8,&ps0,4); memset(rg+12,0,4); memcpy(rg+16,&np0,8);
      memcpy(rg+24,&o1,8); memcpy(rg+32,&ps1,4); memset(rg+36,0,4); memcpy(rg+40,&np1,8); (void)r;
      if(extra && nextra) memcpy(extra,rg,nextra<48? nextra : 48); return 0; }
    case 0x09: { /* ALLOC_AS_EX: {u32 flags, i32 as_fd, u32 big_page_size, u32 pad, u64 va_start, va_end, va_split}
                    big_page_size 0 means 128K (yuzu DEFAULT_BIG_PAGE_SIZE). REMAP uses big page units */
      u32 bps; memcpy(&bps,d+8,4); if(!bps) bps=0x20000;
      if(bps==0x10000 || bps==0x20000) g_big_page_bits= bps==0x20000? 17 : 16;
      LOG("nvdrv: GPU address space init (big page field %x) -> %u KB big pages\n",bps,(1u<<g_big_page_bits)>>10); return 0; }
    case 0x14: { /* REMAP entries: {u16 flags, u16 kind, u32 handle, u32 mem_off, u32 gpu_off, u32 pages}, offsets in big pages */
      u32 cnt=n/20; for(u32 i=0;i<cnt;i++){ u8* e=d+i*20; u32 h, moff, goff, pages; memcpy(&h,e+4,4); memcpy(&moff,e+8,4); memcpy(&goff,e+12,4); memcpy(&pages,e+16,4);
        /* big page size comes from ALLOC_AS_EX */
        u64 gva=(u64)goff<<g_big_page_bits, size=(u64)pages<<g_big_page_bits;
        if(!h) gmmu_unmap(gva,size); else if(h<8192) gmmu_map_handle(gva,h,(u64)moff<<g_big_page_bits,size);
        { static int said; if(said<4){ said++; LOG("nvdrv: remap handle %u: %llx bytes at gpu %llx (big page %u KB)\n",h,(unsigned long long)size,(unsigned long long)gva,(1u<<g_big_page_bits)>>10); } } }
      return 0; }
  }
  (void)extra; (void)nextra; WARN("as ioctl %x\n",cmd); return 0; }

static u32 ioctl_ctrl(u32 cmd, u8* d, u32 n){
  (void)n;
  switch(cmd&0xFF){
    case 0x14: { u32 id; memcpy(&id,d,4); u32 v=syncpt_read(id); memcpy(d+4,&v,4); return 0; }
    case 0x15: { u32 id; memcpy(&id,d,4); syncpt_incr(id); return 0; }
    case 0x16: case 0x19: { u32 id, th; i32 to; memcpy(&id,d,4); memcpy(&th,d+4,4); memcpy(&to,d+8,4);
      int ok=syncpt_wait(id,th,to<0? -1 : (i64)to*1000000); if(n>=16){ u32 v=syncpt_read(id); memcpy(d+12,&v,4); } return ok? 0 : 5; }
    case 0x1A: { u32 id; memcpy(&id,d,4); u32 v=syncpt_max[id&255]; memcpy(d+4,&v,4); return 0; }
    case 0x1B: { memset(d+0x41,0,0x41); return 0; }
    case 0x1C: { /* EVENT_SIGNAL: cancel the pending wait and wake the waiter. NVN does this before reusing an
                    event and the waiter stays blocked if I ignore it */
      u32 ev; memcpy(&ev,d,4); u32 slot=ev&63;
      nvev_lock(); if(nvev[slot].ev){ nvev[slot].armed=0; kevent_signal(nvev[slot].ev); } nvev_unlock();
      { static int said; if(said<2){ said++; LOG("nvdrv: event %u signaled by the game\n",slot); } }
      return 0; }
    case 0x1D: case 0x1E: { u32 id, th; i32 to; u32 ev; memcpy(&id,d,4); memcpy(&th,d+4,4); memcpy(&to,d+8,4); memcpy(&ev,d+12,4);
      if(syncpt_wait(id,th,0)){ u32 v=syncpt_read(id); memcpy(d+12,&v,4); return 0; }
      if((cmd&0xFF)==0x1D && to!=0){ int ok=syncpt_wait(id,th,to<0? -1 : (i64)to*1000000); if(ok){ u32 v=syncpt_read(id); memcpy(d+12,&v,4); return 0; } }
      u32 slot=ev&63; if(nvev[slot].ev){ nvev_lock(); kevent_clear(nvev[slot].ev); nvev[slot].id=id; nvev[slot].thresh=th; nvev[slot].armed=1; nvev_unlock(); nv_events_check(); }
      u32 r=0x10000000u|(slot)|(id<<16); memcpy(d+12,&r,4); return 5; }       /* timeout, caller will wait on the event */
    case 0x1F: { u32 ev; memcpy(&ev,d,4); ev&=63; if(!nvev[ev].ev){ nvev[ev].ev=kevent_new("nvhost event",0); kobj_ref(&nvev[ev].ev->o); } return 0; }
    case 0x20: case 0x21: return 0;
  }
  WARN("ctrl ioctl %x\n",cmd); return 0; }

static u32 ioctl_ctrlgpu(u32 cmd, u8* d, u32 n, u8* extra, u32 nextra){
  (void)n;
  switch(cmd&0xFF){
    case 0x01: { u32 v=0x28000; memcpy(d,&v,4); return 0; }
    case 0x02: { u32 z[10]={0x20,0x20,0x400,0x800,0x20,0x20,0xC0,0x20,0x40,0x10}; memcpy(d,z,40); return 0; }
    case 0x03: case 0x04: return 0;
    case 0x05: { u8 ch[0xA0]; memset(ch,0,sizeof ch); u32* w=(u32*)ch;
      w[0]=0x120; w[1]=0xB; w[2]=0xA1; w[3]=1; u64 l2=0x40000; memcpy(ch+0x10,&l2,8); w[8]=2; w[9]=0x20; w[10]=0x20000; w[11]=0x20000; w[12]=0x1B; w[13]=0x30000; w[14]=1;
      w[15]=0x503; w[16]=0x503; w[17]=0x80; w[18]=0x28; u64 fl=0x55; memcpy(ch+0x50,&fl,8); w[22]=0x902D; w[23]=0xB197; w[24]=0xB1C0; w[25]=0xB06F; w[26]=0xA140; w[27]=0xB0B5;
      w[28]=1; w[29]=0; w[30]=2; w[31]=1; w[32]=0; w[33]=1; w[34]=0x21D70; w[35]=0; u64 cn=0x6230326D67ull; memcpy(ch+0x90,&cn,8);
      u64 sz=0xA0, ad=0xdeadbeef; memcpy(d,&sz,8); memcpy(d+8,&ad,8); memcpy(d+16,ch,0xA0);
      if(extra && nextra) memcpy(extra,ch,nextra<0xA0? nextra : 0xA0);        /* Ioctl3 passes data in the extra buffer */
      return 0; }
    case 0x06: { u32 m=3; memcpy(d+16,&m,4); if(extra && nextra>=4) memcpy(extra,&m,4); return 0; }
    case 0x13: { /* NUM_VSMS: GM20B = 2 SMs (1 GPC x 2 TPC). Ioctl3 variant also returns {gpc,tpc} per SM */
      u32 z[2]={2,0}; memcpy(d,z,8); if(extra && nextra){ u8 mp[4]={0,0,0,1}; memcpy(extra,mp,nextra<4? nextra : 4); } return 0; }
    case 0x14: if((cmd>>30)==3){ /* VSMS_MAPPING: {u64 buf} -> {gpc,tpc} per SM */
        u64 a; memcpy(&a,d,8); u8 mp[4]={0,0,0,1}; if(extra && nextra) memcpy(extra,mp,nextra<4? nextra : 4); else if(a>=0x10000 && a<AS_SIZE-4) memcpy(GP(a),mp,4); return 0; }
      { u32 z[2]={7,1}; memcpy(d,z,8); return 0; }        /* ZBC_GET_ACTIVE_SLOT_MASK */
    case 0x15: { u32 z[2]={7,1}; memcpy(d,z,8); return 0; }
    case 0x1C: { extern u64 guest_ns(void); u64 t=guest_ns(); memcpy(d,&t,8); return 0; }   /* GET_GPU_TIME, same timebase as the system tick */
  }
  WARN("ctrl-gpu ioctl %x\n",cmd); return 0; }

static u32 ioctl_gpu(NvFile* f, u32 cmd, u8* d, u32 n, u8* extra, u32 nextra){
  switch(cmd&0xFF){
    case 0x01: case 0x03: case 0x0B: case 0x0C: case 0x0D: case 0x1D: case 0x0A: case 0x17: return 0;
    case 0x14: memcpy(&f->user_data,d,8); return 0;
    case 0x15: memcpy(d,&f->user_data,8); return 0;
    case 0x16: memset(d,0,n); return 0;
    case 0x09: { u32 cls; memcpy(&cls,d,4); if(f->nclass<8) f->class_ids[f->nclass++]=cls; u64 id=cls; memcpy(d+8,&id,8); return 0; }
    case 0x1A: { if(!f->syncpt) f->syncpt=syncpt_alloc(); u32 fence[2]={f->syncpt,syncpt_read(f->syncpt)}; memcpy(d+24,fence,8); return 0; }   /* the fence goes at +24. at +12 (yuzu's layout) mesa
                                                                                                                                                  reuses its pushbuffers before we read them */
    case 0x08: case 0x1B: {
      u32 num, flags; memcpy(&num,d+8,4); memcpy(&flags,d+12,4);
      u32 fid, fval; memcpy(&fid,d+16,4); memcpy(&fval,d+20,4);
      u64* entries = (cmd&0xFF)==0x08? (u64*)(d+24) : (u64*)extra;
      /* never read entries past what the guest sent */
      if((cmd&0xFF)==0x08 && (u64)num*8>(n>24? n-24 : 0)) num= n>24? (n-24)/8 : 0;
      if((cmd&0xFF)==0x1B && (u64)num*8>nextra) num=nextra/8;
      if(!f->syncpt) f->syncpt=syncpt_alloc();
      { extern int g_snxvk_on;
        if(g_snxvk_on){   /* renderer=vulkan_native: its GPU thread waits for the fence and increments the syncpoint */
          u32 host=(flags&2)? 2 : 0, incs=host+((flags&0x100)? fval : 0), wait_id=(flags&1)? fid : ~0u;
          pm_lock(&g_sync_lk); if((i32)(syncpt_max[f->syncpt]-syncpt[f->syncpt])<0) syncpt_max[f->syncpt]=syncpt[f->syncpt]; syncpt_max[f->syncpt]+=incs; u32 promised=syncpt_max[f->syncpt];
          snxvk_submit(entries,num,f->syncpt,host,wait_id,fval,promised); pm_unlock(&g_sync_lk);
          fid=f->syncpt; fval=promised; memcpy(d+16,&fid,4); memcpy(d+20,&fval,4); return 0; } }
      u32 incs=((flags&2)? 2 : 0)+((flags&0x100)? fval : 0);
      pm_lock(&g_sync_lk); if((i32)(syncpt_max[f->syncpt]-syncpt[f->syncpt])<0) syncpt_max[f->syncpt]=syncpt[f->syncpt]; syncpt_max[f->syncpt]+=incs; u32 promised=syncpt_max[f->syncpt]; pm_unlock(&g_sync_lk);
      /* the fence to wait for (flag 1) is waited for by the GPU thread before it runs these entries, like on hw.
         waiting here blocked the game thread, and the switch's GBA app increments that fence itself from another
         thread that was waiting for this one: 2 s per frame and a black screen */
      gpu_submit(entries,num,f->syncpt,incs,(flags&1)? fid : ~0u,fval);
      fid=f->syncpt; fval=promised;
      memcpy(d+16,&fid,4); memcpy(d+20,&fval,4);
      return 0; }
  }
  WARN("gpu channel ioctl %x\n",cmd); return 0; }

static int nvdrv_disp(SvcObj* o, IpcCtx* c){
  switch(c->cmd){
    case 0: { u64 a,s; buf_in(c,0,&a,&s); char name[64]={0}; memcpy(name,GP(a),s<63?s:63);
      int dev= !strcmp(name,"/dev/nvmap")? DEV_NVMAP : !strcmp(name,"/dev/nvhost-as-gpu")? DEV_AS : !strcmp(name,"/dev/nvhost-ctrl")? DEV_CTRL :
               !strcmp(name,"/dev/nvhost-ctrl-gpu")? DEV_CTRLGPU : !strcmp(name,"/dev/nvhost-gpu")? DEV_GPU :
               !strcmp(name,"/dev/nvhost-nvdec")? DEV_NVDEC : !strcmp(name,"/dev/nvhost-vic")? DEV_VIC : DEV_OTHER;
      int fd=-1; pm_lock(&g_nvlock); for(int i=1;i<256;i++) if(!nvf[i].dev){ fd=i; memset(&nvf[i],0,sizeof nvf[i]); nvf[i].dev=dev; break; } pm_unlock(&g_nvlock);
      LOG("nvdrv: open %s -> %d\n",name,fd);
      out32(c,(u32)fd); out32(c,0); return 0; }
    case 1: case 11: case 12: {
      u32 fd=in32(c,0), cmd=in32(c,4);
      u64 ia=0,is=0,oa=0,os=0,ea=0,es=0; buf_in(c,0,&ia,&is); buf_out(c,0,&oa,&os);
      if(c->cmd==11){ buf_in(c,1,&ea,&es); }
      if(c->cmd==12){ buf_out(c,1,&ea,&es); }
      u32 size=(cmd>>16)&0x3FFF;
      u64 isz=is, osz=os;
      u32 nb= size? size : (u32)(isz>osz? isz : osz);
      if(fd>=256 || !nvf[fd].dev){ out32(c,8); return 0; }
      /* handlers get the guest's size so the buffer has to hold all of it (a big REMAP batch is bigger than the 16
         KB that fits on the stack) */
      u64 need= isz>osz? isz : osz; if(need<nb) need=nb; if(need>(64u<<20)) need=64u<<20;
      u8 stk[0x4000]; u8* tmp= need>sizeof stk? calloc(1,(size_t)need) : stk; if(!tmp){ out32(c,6); return 0; }   /* NvResult InsufficientMemory */
      if(tmp==stk){ memset(stk,0,sizeof stk); need=sizeof stk; }
      if(ia) memcpy(tmp,GP(ia),isz>need? need : isz);
      if(isz>need) isz=need;
      if(nb>need) nb=(u32)need;
      NvFile* f=&nvf[fd]; u32 r=0;
      { static u32 seen[256]; static int nseen; int k; for(k=0;k<nseen;k++) if(seen[k]==(cmd^((u32)f->dev<<28))) break;
        if(k==nseen && nseen<256){ seen[nseen++]=cmd^((u32)f->dev<<28); LOG("nvdrv: ioctl %08x (disp %d, ipc %u, in %llu out %llu extra %llu)\n",cmd,f->dev,c->cmd,(unsigned long long)is,(unsigned long long)os,(unsigned long long)es); } }
      switch(f->dev){
        case DEV_NVMAP: r=ioctl_nvmap(cmd,tmp,nb); break;
        case DEV_AS: r=ioctl_as(cmd,tmp,(u32)(isz>nb?isz:nb),(c->cmd==12&&ea)? GP(ea):NULL,(u32)es); break;
        case DEV_CTRL: r=ioctl_ctrl(cmd,tmp,nb); break;
        case DEV_CTRLGPU: r=ioctl_ctrlgpu(cmd,tmp,nb,(c->cmd==12&&ea)? GP(ea):NULL,(u32)es); break;
        case DEV_GPU: r=ioctl_gpu(f,cmd,tmp,(u32)(isz>nb?isz:nb),ea? GP(ea):NULL,(u32)es); break;
        case DEV_NVDEC: r=host1x_ioctl(0,cmd,tmp,(u32)(isz>nb?isz:nb)); break;
        case DEV_VIC: r=host1x_ioctl(1,cmd,tmp,(u32)(isz>nb?isz:nb)); break;
        default: WARN("nvdrv: ioctl %x to unknown device\n",cmd); break;
      }
      if(oa) memcpy(GP(oa),tmp,osz>need? need : osz);
      if(tmp!=stk) free(tmp);
      out32(c,r); return 0; }
    case 2: { u32 fd=in32(c,0); if(fd<256) nvf[fd].dev=0; out32(c,0); return 0; }
    case 3: out32(c,0); return 0;
    case 4: { u32 fd=in32(c,0), id=in32(c,4); (void)fd;
      u32 slot=id&63; if(((id>>28)&0xF)==1 || id<64){ if(!nvev[slot].ev){ nvev[slot].ev=kevent_new("nv event",0); kobj_ref(&nvev[slot].ev->o); } out_copy_h(c,h_new(&nvev[slot].ev->o)); }
      else { KEvent* e=kevent_new("nv query",0); out_copy_h(c,h_new(&e->o)); }
      out32(c,0); return 0; }
    case 5: case 6: case 8: case 9: case 10: case 13: out32(c,0); return 0;
  }
  return stub_ok(o,c); }

static SvcObj* new_nvdrv(void){ return svcobj_new("nvdrv",nvdrv_disp,0); }

void nv_init(void){
  pm_init(&g_nvlock); pm_init(&g_sync_lk); pm_init(&g_runs_lk); pc_init(&g_sync_cv);
  gmmu=calloc(GL1,sizeof(u64*));
  svc_register("nvdrv",new_nvdrv); svc_register("nvdrv:a",new_nvdrv); svc_register("nvdrv:s",new_nvdrv); svc_register("nvdrv:t",new_nvdrv);
}
