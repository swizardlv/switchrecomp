/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#define _GNU_SOURCE

/* platform glue: threads, timers, VM, files. win32 + posix */
#include "rt.h"
#include <time.h>

__thread void* tls_thread;
int g_verbose = 1;
static FILE* g_logf;
static PMutex g_logm; static int g_logm_ok;

__thread FILE* g_log_redirect;   /* redirect for the threads.txt dumps so they don't end up in the log, it's per
                                    thread so the other threads keep logging like normal */
void logf_(int lvl, const char* fmt, ...){
  if(!g_logm_ok){ pm_init(&g_logm); g_logm_ok=1; }
  char buf[2048]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
  { FILE* rf=g_log_redirect; if(rf){ fputs(buf,rf); return; } }
  static u64 t0; u64 now=plat_ns(); if(!t0) t0=now;
  char tb[24]; snprintf(tb,sizeof tb,"[%7.2f] ",(double)(now-t0)/1e9);
  pm_lock(&g_logm);
  if(!g_logf){ char p[1200]; snprintf(p,sizeof p,"%s%s.log",g_exedir,g_exename[0]?g_exename:"switchrecomp"); g_logf=fopen(p,"w"); }
  if(g_logf){ static int bol=1; if(bol) fputs(tb,g_logf); fputs(buf,g_logf); size_t L=strlen(buf); bol= L && buf[L-1]=='\n'; fflush(g_logf); }
  if(lvl<=1 || g_verbose>=2) fputs(buf,stderr);
  pm_unlock(&g_logm);
}

volatile int g_exit_logged;   /* the log already says why the process ends (exit_tls_cb) */
void fatal(const char* fmt, ...){
  char buf[4096]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
  logf_(0,"FATAL: %s\n",buf); g_exit_logged=1;
#if defined(_WIN32)
  { extern void win_fatal_box(const char*); win_fatal_box(buf); }
#else
  { char m[4400]; snprintf(m,sizeof m,"%s\n\nDetails are in %s.log",buf,g_exename); plat_message(m,1,1); }
#endif
  fflush(NULL);
  _exit(1);
}

#if defined(_WIN32)
/* ------------------------------------------------------------------ Windows */
#include <process.h>
#include <io.h>        /* _get_osfhandle */
void pm_init(PMutex* m){ InitializeSRWLock(&m->l); }

void pm_lock(PMutex* m){ AcquireSRWLockExclusive(&m->l); }

void pm_unlock(PMutex* m){ ReleaseSRWLockExclusive(&m->l); }

void pc_init(PCond* c){ InitializeConditionVariable(&c->c); }

void pc_signal(PCond* c){ WakeConditionVariable(&c->c); }

void pc_broadcast(PCond* c){ WakeAllConditionVariable(&c->c); }

int pc_wait(PCond* c, PMutex* m, i64 ns){
  DWORD ms= ns<0? INFINITE : (DWORD)((ns+999999)/1000000);
  if(!SleepConditionVariableSRW(&c->c,&m->l,ms,0)) return GetLastError()==ERROR_TIMEOUT? 1 : 0;
  return 0; }

typedef struct { PThreadFn fn; void* arg; } TArg;

/* 64 KB kept free past the guard page: a stack overflow can still write its line in the log */
static unsigned __stdcall tstart(void* p){ TArg a=*(TArg*)p; free(p); { ULONG g=64*1024; SetThreadStackGuarantee(&g); } a.fn(a.arg); return 0; }

int plat_thread(PThreadFn fn, void* arg, size_t stack){
  TArg* a=malloc(sizeof *a); a->fn=fn; a->arg=arg;
  /* at least 8 MB reserved (only what's used gets committed): the video driver compiles shaders on the calling thread,
     and the vulkan renderer's pipeline threads ask for 1 MB. suspected cause of Tomodachi Life on intel closing at its
     first 3D draw with no line in the log (our GL shader threads already use 8 MB) */
  if(stack<(8u<<20)) stack=8u<<20;
  uintptr_t h=_beginthreadex(NULL,(unsigned)stack,tstart,a,STACK_SIZE_PARAM_IS_A_RESERVATION,NULL);
  if(!h) return -1;
  { extern void plat_prof_add_thread(HANDLE); plat_prof_add_thread((HANDLE)h); }
  CloseHandle((HANDLE)h); return 0; }

void plat_thread_exit(void){ _endthreadex(0); }

static LONG g_hires;

void plat_sleep_ns(i64 ns){
  if(!g_hires){ typedef UINT (WINAPI*TBP)(UINT); HMODULE w=LoadLibraryA("winmm.dll"); if(w){ TBP f=(TBP)GetProcAddress(w,"timeBeginPeriod"); if(f) f(1);} g_hires=1; }
  if(ns<=0){ SwitchToThread(); return; }
  /* use the high-res waitable timer when the OS has it */
  static __thread HANDLE tmr; if(!tmr){ tmr=CreateWaitableTimerExW(NULL,NULL,0x00000002/*HIGH_RESOLUTION*/,TIMER_ALL_ACCESS); if(!tmr) tmr=CreateWaitableTimerW(NULL,TRUE,NULL); }
  if(tmr){ LARGE_INTEGER due; due.QuadPart=-(ns/100); if(!due.QuadPart) due.QuadPart=-1; SetWaitableTimer(tmr,&due,0,NULL,NULL,FALSE); WaitForSingleObject(tmr,INFINITE); }
  else Sleep((DWORD)(ns/1000000)); }

void plat_yield(void){ SwitchToThread(); }

/* on 2 core CPUs the shader compiler (300-400 ms per shader on a celeron) ate more than a core in busy scenes, at
   the lowest priority it only gets what the game leaves free */
void plat_thread_low_priority(void){ extern int plat_ncpu(void); SetThreadPriority(GetCurrentThread(), plat_ncpu()<=2? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_BELOW_NORMAL); }

void plat_thread_high_priority(void){ SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL); }

u64 plat_thread_cpu_ns(void){ FILETIME c,e,k,u; if(!GetThreadTimes(GetCurrentThread(),&c,&e,&k,&u)) return 0; return ((((u64)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((u64)u.dwHighDateTime<<32)|u.dwLowDateTime))*100; }

u64 plat_process_cpu_ns(void){ FILETIME c,e,k,u; if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)) return 0; return ((((u64)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((u64)u.dwHighDateTime<<32)|u.dwLowDateTime))*100; }

u64 plat_ns(void){ static LARGE_INTEGER f; LARGE_INTEGER t; if(!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
  return (u64)((double)t.QuadPart*1e9/(double)f.QuadPart); }

i64 plat_unix_time(void){ return (i64)time(NULL); }

/* UTC offset in seconds, DST included */
i64 plat_tz_offset(void){ TIME_ZONE_INFORMATION z; DWORD r=GetTimeZoneInformation(&z); if(r==TIME_ZONE_ID_INVALID) return 0;
  long b=z.Bias+(r==TIME_ZONE_ID_DAYLIGHT? z.DaylightBias : r==TIME_ZONE_ID_STANDARD? z.StandardBias : 0); return -(i64)b*60; }

int g_ww_ok;   /* MEM_WRITE_WATCH: Windows tracks written pages, so unchanged buffers don't need a reread */
u8* plat_vm_reserve(u64 size){ void* p=!getenv("SWR_WW")? NULL : VirtualAlloc(NULL,size,MEM_RESERVE|MEM_WRITE_WATCH,PAGE_NOACCESS);
  if(p){ g_ww_ok=1; return (u8*)p; } return (u8*)VirtualAlloc(NULL,size,MEM_RESERVE,PAGE_NOACCESS); }

int plat_ww_collect(u8* base, u64 size, void (*cb)(u64)){
  static PVOID a[16384]; if(!g_ww_ok) return -1;
  for(int it=0;it<64;it++){ ULONG_PTR n=16384; ULONG gran=0;
    if(GetWriteWatch(WRITE_WATCH_FLAG_RESET,base,(SIZE_T)size,a,&n,&gran)) return -1;
    for(ULONG_PTR i=0;i<n;i++) cb((u64)((u8*)a[i]-base));
    if(n<16384) break; }
  return 0; }

int plat_vm_commit(u8* p, u64 size){ return VirtualAlloc(p,size,MEM_COMMIT,PAGE_READWRITE)? 0 : -1; }

void plat_vm_readonly(u8* p, u64 size){ DWORD o; VirtualProtect(p,size,PAGE_READONLY,&o); }
void plat_vm_decommit(u8* p, u64 size){ if(size) VirtualFree(p,size,MEM_DECOMMIT); }   /* size 0 at the base would decommit the whole reservation */

void* plat_dl_open(const char* path){ return (void*)LoadLibraryA(path); }

void* plat_dl_sym(void* h, const char* n){ return (void*)GetProcAddress((HMODULE)h,n); }

int plat_mkdir(const char* p){ return CreateDirectoryA(p,NULL)? 0 : -1; }

int plat_is_dir(const char* p){ DWORD a=GetFileAttributesA(p); return a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_DIRECTORY); }

int plat_dir_open(PDir* d, const char* path){ char p[1100]; snprintf(p,sizeof p,"%s\\*",path); WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA(p,&fd);
  if(h==INVALID_HANDLE_VALUE) return -1; d->h=h; d->first=1; strncpy(d->name,fd.cFileName,511); d->is_dir=(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
  d->size=((u64)fd.nFileSizeHigh<<32)|fd.nFileSizeLow; return 0; }

int plat_dir_next(PDir* d){
  for(;;){
    if(d->first){ d->first=0; }
    else { WIN32_FIND_DATAA fd; if(!FindNextFileA((HANDLE)d->h,&fd)) return 0; strncpy(d->name,fd.cFileName,511); d->is_dir=(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0; d->size=((u64)fd.nFileSizeHigh<<32)|fd.nFileSizeLow; }
    if(strcmp(d->name,".") && strcmp(d->name,"..")) return 1; } }

void plat_dir_close(PDir* d){ FindClose((HANDLE)d->h); }

u64 plat_file_size(const char* p){ WIN32_FILE_ATTRIBUTE_DATA a; if(!GetFileAttributesExA(p,GetFileExInfoStandard,&a)) return 0; return ((u64)a.nFileSizeHigh<<32)|a.nFileSizeLow; }

int plat_pread(FILE* f, void* buf, u64 size, u64 off){
  HANDLE h=(HANDLE)_get_osfhandle(_fileno(f)); OVERLAPPED ov; memset(&ov,0,sizeof ov); ov.Offset=(DWORD)off; ov.OffsetHigh=(DWORD)(off>>32);
  DWORD got=0; if(!ReadFile(h,buf,(DWORD)size,&got,&ov)) return -1; return (int)got; }

int plat_ncpu(void){ SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }

void plat_battery(int* pct, int* chg){ SYSTEM_POWER_STATUS s; *pct=100; *chg=1;
  if(!GetSystemPowerStatus(&s) || (s.BatteryFlag&128) || s.BatteryLifePercent>100) return;
  *pct=s.BatteryLifePercent; *chg= s.ACLineStatus==1; }

/* a crash inside a system DLL (memcpy in msvcrt, the video driver) doesn't say anything by itself, so we log the
   return addresses into the runtime that are on the stack, they show what code called it */
static void log_runtime_callers(CONTEXT* c){
  extern char __ImageBase; u64 b=(u64)(size_t)&__ImageBase;
  IMAGE_NT_HEADERS* nt=(IMAGE_NT_HEADERS*)(b+(u64)((IMAGE_DOS_HEADER*)b)->e_lfanew); u64 end=b+nt->OptionalHeader.SizeOfImage;
  if(c->Rip>=b && c->Rip<end) return;
  u64* sp=(u64*)(size_t)c->Rsp; u64* top=(u64*)((NT_TIB*)NtCurrentTeb())->StackBase;
  int n=0; for(; sp<top && n<8; sp++) if(*sp>b+0x1000 && *sp<end){ if(!n) logf_(0,"  called from the runtime at"); logf_(0," +%llx",(unsigned long long)(*sp-b)); n++; }
  if(n) logf_(0," (exe offsets)\n"); }

static DWORD g_guard_tls=TLS_OUT_OF_INDEXES;

void plat_guard_enter(void* rec){ if(g_guard_tls==TLS_OUT_OF_INDEXES) return; TlsSetValue(g_guard_tls,rec); }

void* plat_guard_get(void){ return g_guard_tls==TLS_OUT_OF_INDEXES? NULL : TlsGetValue(g_guard_tls); }

volatile int g_driver_recovered;   /* times we jumped out of a driver crash */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e){
  plat_jmpbuf* b=(plat_jmpbuf*)plat_guard_get();
  if(b){ /* the video driver crashed in a guarded call (shader compile): drop that shader, keep playing */
    { extern void gpu_crash_note(void); gpu_crash_note(); }
    plat_guard_enter(NULL); g_driver_recovered++;
    logf_(0,"  (exception %08lx at %p inside the video driver, recovered: only that effect is missing)\n",(unsigned long)e->ExceptionRecord->ExceptionCode,(void*)e->ContextRecord->Rip);
    __builtin_longjmp(*b,1); }
  extern void crash_report(void* host_pc, void* fault_addr, int sig);
  log_runtime_callers(e->ContextRecord);
  { extern void gpu_crash_note(void); gpu_crash_note(); }
  g_exit_logged=1;
  crash_report((void*)e->ContextRecord->Rip,(void*)e->ExceptionRecord->ExceptionInformation[1],(int)e->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER; }

static LONG WINAPI veh_fatal(EXCEPTION_POINTERS* e){
  DWORD c=e->ExceptionRecord->ExceptionCode;
  if(c==EXCEPTION_STACK_OVERFLOW || c==0xC0000409 /* STACK_BUFFER_OVERRUN / fastfail */ || c==0xC0000374 /* heap corruption */){
    static volatile LONG once; if(!InterlockedExchange(&once,1)){ logf_(0,"\n*** Shutdown: exception %08lx at %p (stack/heap) ***\n",(unsigned long)c,(void*)e->ContextRecord->Rip); g_exit_logged=1; } }
  return EXCEPTION_CONTINUE_SEARCH; }

#include <signal.h>

static void on_abort(int s){ logf_(0,"\n*** Shutdown: abort() (signal %d), probably the video driver or out of memory ***\n",s); }

static void on_exit_log(void){ g_exit_logged=1; logf_(0,"program shutdown\n"); }

/* someone called ExitProcess without going through exit() (a driver, a library): the TLS callback still runs then, so
   the log says the process ended that way. a crash with no line at all means TerminateProcess or a fast fail (the
   Windows event log has its code). written with WriteFile: the thread that held the log lock is gone by now */
static void NTAPI exit_tls_cb(PVOID h, DWORD reason, PVOID r){ (void)h; (void)r;
  if(reason!=DLL_PROCESS_DETACH || g_exit_logged || !g_logf) return;
  char p[1200]; snprintf(p,sizeof p,"%s%s.log",g_exedir,g_exename[0]?g_exename:"switchrecomp");
  HANDLE f=CreateFileA(p,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL); if(f==INVALID_HANDLE_VALUE) return;
  static const char m[]="\n*** Shutdown: ExitProcess was called outside the game's own exit (a driver or library ended the process) ***\n";
  DWORD w; WriteFile(f,m,sizeof m-1,&w,NULL); CloseHandle(f); }
extern const IMAGE_TLS_DIRECTORY _tls_used;
PIMAGE_TLS_CALLBACK __attribute__((section(".CRT$XLB"), used)) g_exit_tls=exit_tls_cb;
const void* const g_exit_tls_ref=&_tls_used;   /* keeps the TLS directory linked */

static void inv_param(const wchar_t* e, const wchar_t* f, const wchar_t* fi, unsigned l, uintptr_t r){ (void)e;(void)f;(void)fi;(void)l;(void)r; logf_(0,"warning: invalid parameter in the C library\n"); }

void plat_install_crash_handler(void){ g_guard_tls=TlsAlloc(); SetUnhandledExceptionFilter(crash_filter);
  AddVectoredExceptionHandler(0,veh_fatal); signal(SIGABRT,on_abort); atexit(on_exit_log); _set_invalid_parameter_handler(inv_param);
  /* keep the display/system awake while playing */
  SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED); }

/* ------------------------------------------------------------------ sampling profiler (profile=1) ~1 ms tick:
   suspend each thread, grab rip, map it to a game function / exe offset / module. profile.txt gets rewritten every
   10 s */
#include "rt.h"
static HANDLE g_pth[256]; static volatile LONG g_npth;

void plat_prof_add_thread(HANDLE h){ HANDLE d=NULL; if(!DuplicateHandle(GetCurrentProcess(),h,GetCurrentProcess(),&d,0,FALSE,DUPLICATE_SAME_ACCESS)) return;
  LONG i=InterlockedIncrement(&g_npth)-1; if(i<256) g_pth[i]=d; else CloseHandle(d); }

typedef struct { u64 host; u32 idx; } PEnt;

static PEnt* g_pe; static u32 g_npe; static u64 g_plo, g_phi; static u32* g_pcnt;
#define PMOD 32
static struct { u64 base; char name[64]; u32 n; } g_pmod[PMOD]; static int g_npmod;

#define PEXE 4096
static struct { u32 off; u32 n; } g_pexe[PEXE];

static u64 g_ptot, g_pguest, g_pexe_n;

static int pe_cmp(const void* a, const void* b){ u64 x=((const PEnt*)a)->host, y=((const PEnt*)b)->host; return x<y? -1 : x>y; }

static void prof_note(u64 rip){
  g_ptot++;
  if(rip>=g_plo && rip<g_phi+0x4000){ u32 lo=0, hi=g_npe; while(hi-lo>1){ u32 m=(lo+hi)/2; if(g_pe[m].host<=rip) lo=m; else hi=m; } g_pcnt[lo]++; g_pguest++; return; }
  HMODULE hm=NULL; if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)rip,&hm) || !hm){ hm=NULL; }
  u64 base=(u64)(uintptr_t)hm;
  if(hm==GetModuleHandleA(NULL)){ u32 off=(u32)((rip-base)&~0x3Full); u32 k=(off*2654435761u)>>20; for(int t=0;t<PEXE;t++){ u32 s=(k+t)&(PEXE-1); if(!g_pexe[s].n || g_pexe[s].off==off){ g_pexe[s].off=off; g_pexe[s].n++; break; } } g_pexe_n++; }
  int i; for(i=0;i<g_npmod;i++) if(g_pmod[i].base==base) break;
  if(i==g_npmod && g_npmod<PMOD){ g_pmod[i].base=base; g_pmod[i].n=0; if(!hm) strcpy(g_pmod[i].name,"(no module: generated/JIT code)"); else { char pth[MAX_PATH]; GetModuleFileNameA(hm,pth,MAX_PATH); const char* s=strrchr(pth,'\\'); snprintf(g_pmod[i].name,64,"%s",s? s+1 : pth); } g_npmod++; }
  if(i<g_npmod) g_pmod[i].n++; }

static const char* g_pfile="profile.txt"; static int g_pboot;

static void prof_write(void){
  FILE* f=fopen(g_pfile,"w"); if(!f) return;
  /* idle threads sit in ntdll/kernelbase/win32u, so percentages are of non-idle samples */
  u64 idle=0; for(int i=0;i<g_npmod;i++){ const char* n=g_pmod[i].name; if(!_stricmp(n,"ntdll.dll")||!_stricmp(n,"kernelbase.dll")||!_stricmp(n,"win32u.dll")||!_stricmp(n,"kernel32.dll")) idle+=g_pmod[i].n; }
  u64 act= g_ptot>idle? g_ptot-idle : 1;
  fprintf(f,"SwitchRecomp profile: %llu samples, %llu active (excluding waits). %% of active time\n\n",(unsigned long long)g_ptot,(unsigned long long)act);
  fprintf(f,"modules:\n"); for(int i=0;i<g_npmod;i++) { const char* n=g_pmod[i].name; int w=!_stricmp(n,"ntdll.dll")||!_stricmp(n,"kernelbase.dll")||!_stricmp(n,"win32u.dll")||!_stricmp(n,"kernel32.dll"); if(w) fprintf(f,"   (waits) %s: %llu samples\n",n,(unsigned long long)g_pmod[i].n); else fprintf(f,"  %6.2f%%  %s\n",100.0*g_pmod[i].n/act,n); }
  fprintf(f,"  %6.2f%%  game code (functions below)\n",100.0*g_pguest/act); fprintf(f,"\ngame functions (guest address):\n");
  u64 top[6]; int ntop=0;
  for(int r=0;r<60;r++){ u32 best=~0u, bn=0; for(u32 i=0;i<g_npe;i++) if(!(g_pcnt[i]&0x80000000u) && g_pcnt[i]>bn){ bn=g_pcnt[i]; best=i; }
    if(best==~0u || !bn || bn*2000ull<act) break; fprintf(f,"  %6.2f%%  %llx\n",100.0*bn/act,(unsigned long long)g_mod->entry_addr[g_pe[best].idx]);
    if(r<6) top[ntop++]=g_mod->entry_addr[g_pe[best].idx]; g_pcnt[best]|=0x80000000u; }
  for(u32 i=0;i<g_npe;i++) g_pcnt[i]&=0x7FFFFFFFu;
  /* raw A64 words of the top functions, for disassembling later */
  fprintf(f,"\ncode of the hottest game functions (first 96 instructions):\n");
  for(int t=0;t<ntop;t++){ fprintf(f,"  %llx:",(unsigned long long)top[t]); const u32* w=(const u32*)GP(top[t]);
    for(int k=0;k<96;k++){ if(k%16==0) fprintf(f,"\n   "); fprintf(f," %08x",w[k]); } fprintf(f,"\n"); }
  fprintf(f,"\nruntime (offset in the exe):\n");
  for(int r=0;r<40;r++){ int best=-1; u32 bn=0; for(int s=0;s<PEXE;s++) if(g_pexe[s].n>bn && !(g_pexe[s].n&0x80000000u)){ bn=g_pexe[s].n; best=s; }
    if(best<0 || bn*2000ull<act) break; fprintf(f,"  %6.2f%%  +%x\n",100.0*bn/act,g_pexe[best].off); g_pexe[best].n|=0x80000000u; }
  for(int s=0;s<PEXE;s++) g_pexe[s].n&=0x7FFFFFFFu;
  fclose(f); }

static unsigned __stdcall prof_thread(void* a){ (void)a;
  u64 last=GetTickCount64(); DWORD me=GetCurrentThreadId();
  u64 start=last;
  for(;;){ Sleep(g_pboot? 2 : 1);
    LONG n=g_npth; if(n>256) n=256;
    for(LONG i=0;i<n;i++){ HANDLE h=g_pth[i]; if(!h || GetThreadId(h)==me) continue;
      if(SuspendThread(h)==(DWORD)-1) continue; CONTEXT cx; memset(&cx,0,sizeof cx); cx.ContextFlags=CONTEXT_CONTROL;
      if(GetThreadContext(h,&cx)){ ResumeThread(h); prof_note(cx.Rip); } else ResumeThread(h); }
    if(GetTickCount64()-last>=(g_pboot? 5000u : 10000u)){ last=GetTickCount64(); prof_write(); }
    if(g_pboot && GetTickCount64()-start>=90000){ prof_write(); break; } }
  return 0; }

void plat_prof_start(int boot){
  if(!g_mod || !g_mod->nentries) return;
  g_pboot=boot; if(boot) g_pfile="startup.txt";
  g_npe=g_mod->nentries; g_pe=malloc(sizeof(PEnt)*g_npe); g_pcnt=calloc(g_npe,4); g_plo=~0ull; g_phi=0;
  for(u32 i=0;i<g_npe;i++){ g_pe[i].host=(u64)(uintptr_t)g_mod->entry_fn[i]; g_pe[i].idx=i; if(g_pe[i].host<g_plo) g_plo=g_pe[i].host; if(g_pe[i].host>g_phi) g_phi=g_pe[i].host; }
  qsort(g_pe,g_npe,sizeof(PEnt),pe_cmp);
  { HANDLE d=NULL; DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&d,0,FALSE,DUPLICATE_SAME_ACCESS); plat_prof_add_thread(d); CloseHandle(d); }
  uintptr_t h=_beginthreadex(NULL,0,prof_thread,NULL,0,NULL); if(h){ SetThreadPriority((HANDLE)h,THREAD_PRIORITY_TIME_CRITICAL); CloseHandle((HANDLE)h); } }

/* stall dump: where a game thread that runs without SVCs really is. a handle to the calling thread, closed on exit */
void* plat_thread_self(void){ HANDLE d=NULL;
  if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&d,THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,0)) return NULL;
  return d; }

void plat_thread_forget(void* h){ if(h) CloseHandle((HANDLE)h); }

/* its host pc. nothing between suspend and resume (no allocation, no log), it could be holding any lock */
int plat_thread_pc(void* h, u64* pc){ if(!h || SuspendThread((HANDLE)h)==(DWORD)-1) return 0;
  CONTEXT cx; memset(&cx,0,sizeof cx); cx.ContextFlags=CONTEXT_CONTROL; int ok=GetThreadContext((HANDLE)h,&cx)!=0; ResumeThread((HANDLE)h);
  if(ok) *pc=cx.Rip; return ok; }

/* module name + offset of a host address */
int plat_addr_module(u64 a, char* name, int n, u64* off){ HMODULE hm=NULL;
  if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)(uintptr_t)a,&hm) || !hm) return 0;
  char full[MAX_PATH]; DWORD k=GetModuleFileNameA(hm,full,sizeof full); if(!k) return 0;
  const char* b=full; for(DWORD i=0;i<k;i++) if(full[i]=='\\' || full[i]=='/') b=full+i+1;
  snprintf(name,n,"%s",hm==GetModuleHandleA(NULL)? "runtime" : b); *off=a-(u64)(uintptr_t)hm; return 1; }
/* text input for the on-screen keyboard (swkbd), drawn like the SwitchRecomp tool's dialogs (dark panel over the
   dimmed game, Hind font, cyan buttons). typing uses the PC keyboard, any layout. Enter = OK, Esc = keep the
   default text. returns the length in UTF-16 units, -1 = keep the default */
#include "swkbd_font.h"
static struct { wchar_t txt[1024]; int n, max, all, res, hover, tick; float s; HFONT fb, fh; } KB;
#define KB_W 680
#define KB_H 268
#define KB_BAR 84
static COLORREF kb_pulse(void){ float t=(sinf(KB.tick*0.12f)+1)*0.5f;   /* the tool's selection frame: cyan that breathes */
  return RGB((int)(0+t*90),(int)(200+t*40),(int)(225+t*25)); }
static void kb_rrect(HDC d, int x, int y, int w, int h, int r, COLORREF c){
  HBRUSH b=CreateSolidBrush(c); HPEN p=CreatePen(PS_SOLID,1,c); HGDIOBJ ob=SelectObject(d,b), op=SelectObject(d,p);
  RoundRect(d,x,y,x+w,y+h,r*2,r*2); SelectObject(d,ob); SelectObject(d,op); DeleteObject(b); DeleteObject(p); }
static void kb_frame(HDC d, int x, int y, int w, int h, int r, int th, COLORREF c){
  HPEN p=CreatePen(PS_INSIDEFRAME,th,c); HGDIOBJ op=SelectObject(d,p), ob=SelectObject(d,GetStockObject(NULL_BRUSH));
  RoundRect(d,x,y,x+w,y+h,r*2,r*2); SelectObject(d,op); SelectObject(d,ob); DeleteObject(p); }
static void kb_text(HDC d, HFONT f, const wchar_t* s, int n, int cx, int cy, COLORREF c){
  SelectObject(d,f); SIZE z; GetTextExtentPoint32W(d,s,n,&z); SetTextColor(d,c); TextOutW(d,cx-z.cx/2,cy-z.cy/2,s,n); }
static void kb_paint(HWND h){
  PAINTSTRUCT ps; HDC wd=BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc); int W=rc.right, H=rc.bottom; float s=KB.s;
  HDC d=CreateCompatibleDC(wd); HBITMAP bm=CreateCompatibleBitmap(wd,W,H); HGDIOBJ obm=SelectObject(d,bm);
  SetBkMode(d,TRANSPARENT);
  kb_rrect(d,0,0,W,H,(int)(6*s),RGB(70,70,70));                                   /* panel */
  kb_text(d,KB.fb,L"Enter text",10,W/2,(int)(54*s),RGB(255,255,255));
  /* text field */
  int fx=(int)(40*s), fy=(int)(84*s), fw=W-2*fx, fh=(int)(56*s), pad=(int)(16*s);
  kb_rrect(d,fx,fy,fw,fh,(int)(4*s),RGB(52,52,52));
  kb_frame(d,fx,fy,fw,fh,(int)(4*s),(int)(2*s)<1?1:(int)(2*s),RGB(0,200,225));
  SelectObject(d,KB.fb); int st=0; SIZE z; GetTextExtentPoint32W(d,KB.txt,KB.n,&z);
  while(st<KB.n && z.cx>fw-2*pad-(int)(8*s)){ st++; GetTextExtentPoint32W(d,KB.txt+st,KB.n-st,&z); }   /* keep the end visible */
  int ty=fy+(fh-z.cy)/2+(int)(2*s);
  if(KB.all && KB.n){ RECT sr={fx+pad-2,fy+(int)(10*s),fx+pad+z.cx+2,fy+fh-(int)(10*s)}; HBRUSH b=CreateSolidBrush(RGB(0,120,140)); FillRect(d,&sr,b); DeleteObject(b); }
  SetTextColor(d,RGB(255,255,255)); TextOutW(d,fx+pad,ty,KB.txt+st,KB.n-st);
  if(!KB.all && (KB.tick/16)%2==0){ RECT cr={fx+pad+z.cx+1,fy+(int)(12*s),fx+pad+z.cx+1+(int)(2*s<2?2:2*s),fy+fh-(int)(12*s)};
    HBRUSH b=CreateSolidBrush(RGB(0,200,225)); FillRect(d,&cr,b); DeleteObject(b); }
  { wchar_t c[32]; int k=swprintf(c,32,L"%d / %d",KB.n,KB.max); kb_text(d,KB.fh,c,k,W/2,fy+fh+(int)(20*s),RGB(160,160,160)); }
  /* button bar, like the tool's dialogs: separator, two halves, cyan labels, pulsing frame on the focused one */
  int by=H-(int)(KB_BAR*s), bw=W/2;
  { RECT l={0,by,W,by+1}; HBRUSH b=CreateSolidBrush(RGB(80,80,80)); FillRect(d,&l,b); RECT v={bw,by,bw+1,H}; FillRect(d,&v,b); DeleteObject(b); }
  const wchar_t* lab[2]={L"Cancel  (Esc)",L"OK  (Enter)"};
  for(int i=0;i<2;i++){ int bx=i*bw; int sel= KB.hover>=0? KB.hover==i : i==1;
    if(sel) kb_frame(d,bx+(int)(6*s),by+(int)(6*s),bw-(int)(12*s),(int)((KB_BAR-12)*s),(int)(4*s),(int)(4*s),kb_pulse());
    kb_text(d,KB.fh,lab[i],(int)wcslen(lab[i]),bx+bw/2,by+(int)(KB_BAR*s/2),RGB(0,200,225)); }
  BitBlt(wd,0,0,W,H,d,0,0,SRCCOPY); SelectObject(d,obm); DeleteObject(bm); DeleteDC(d); EndPaint(h,&ps); }
static LRESULT CALLBACK kb_proc(HWND h, UINT m, WPARAM w, LPARAM l){
  switch(m){
    case WM_PAINT: kb_paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: KB.tick++; InvalidateRect(h,NULL,0); return 0;
    case WM_KEYDOWN:
      if(w==VK_RETURN){ KB.res=1; return 0; } if(w==VK_ESCAPE){ KB.res=-1; return 0; }
      if(w==VK_BACK){ if(KB.all) KB.n=0; else if(KB.n) KB.n--; KB.all=0; InvalidateRect(h,NULL,0); return 0; }
      break;
    case WM_CHAR:
      if(w==0x16 && OpenClipboard(h)){ HANDLE c=GetClipboardData(CF_UNICODETEXT); const wchar_t* p= c? (const wchar_t*)GlobalLock(c) : NULL;   /* Ctrl+V */
        if(p){ if(KB.all) KB.n=0; KB.all=0; for(;*p && KB.n<KB.max;p++) if(*p>=0x20) KB.txt[KB.n++]=*p; GlobalUnlock(c); }
        CloseClipboard(); InvalidateRect(h,NULL,0); return 0; }
      if(w>=0x20 && w!=0x7F){ if(KB.all) KB.n=0; KB.all=0; if(KB.n<KB.max) KB.txt[KB.n++]=(wchar_t)w; InvalidateRect(h,NULL,0); }
      return 0;
    case WM_MOUSEMOVE: { RECT rc; GetClientRect(h,&rc); int y=(short)HIWORD(l), x=(short)LOWORD(l);
      KB.hover= y>=rc.bottom-(int)(KB_BAR*KB.s)? (x<rc.right/2? 0 : 1) : -1; return 0; }
    case WM_LBUTTONUP: if(KB.hover>=0) KB.res= KB.hover? 1 : -1; return 0;
    case WM_CLOSE: KB.res=-1; return 0;
  }
  return DefWindowProcW(h,m,w,l); }
int plat_text_input(const u16* def, u16* out, int max){
  static int font_done; if(!font_done){ DWORD nf=0; AddFontMemResourceEx((void*)g_hind_ttf,sizeof g_hind_ttf,NULL,&nf); font_done=1; }
  HINSTANCE hi=GetModuleHandleW(NULL);
  WNDCLASSW wc={0}; wc.lpfnWndProc=kb_proc; wc.hInstance=hi; wc.lpszClassName=L"SwrKeyboard"; wc.hCursor=LoadCursor(NULL,IDC_ARROW); RegisterClassW(&wc);
  WNDCLASSW wd={0}; wd.lpfnWndProc=DefWindowProcW; wd.hInstance=hi; wd.lpszClassName=L"SwrKeyboardDim"; wd.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&wd);
  /* over the game window (the foreground one), its height sets the scale like the tool's 1280x720 layout */
  RECT gr; HWND game=GetForegroundWindow(); if(!game || !GetWindowRect(game,&gr)) SetRect(&gr,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
  int gw=gr.right-gr.left, gh=gr.bottom-gr.top; float s=gh/720.0f; if(s<0.75f) s=0.75f; if(s>3) s=3;
  memset(&KB,0,sizeof KB); KB.max= max>0 && max<1000? max : 32; KB.s=s; KB.hover=-1; KB.res=0;
  if(def) while(def[KB.n] && KB.n<KB.max){ KB.txt[KB.n]=def[KB.n]; KB.n++; } KB.all=KB.n>0;
  KB.fb=CreateFontW(-(int)(24*s),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Hind");
  KB.fh=CreateFontW(-(int)(22*s),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Hind");
  HWND dim=CreateWindowExW(WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"SwrKeyboardDim",L"",WS_POPUP|WS_VISIBLE,gr.left,gr.top,gw,gh,NULL,NULL,hi,NULL);
  if(dim) SetLayeredWindowAttributes(dim,0,140,LWA_ALPHA);                        /* the tool dims to 55% */
  int w=(int)(KB_W*s), h=(int)(KB_H*s);
  HWND win=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"SwrKeyboard",L"Keyboard",WS_POPUP,gr.left+(gw-w)/2,gr.top+(gh-h)/2,w,h,NULL,NULL,hi,NULL);
  if(!win){ if(dim) DestroyWindow(dim); DeleteObject(KB.fb); DeleteObject(KB.fh); return -1; }
  SetWindowRgn(win,CreateRoundRectRgn(0,0,w+1,h+1,(int)(12*s),(int)(12*s)),1);
  ShowWindow(win,SW_SHOW); SetForegroundWindow(win); SetFocus(win); SetTimer(win,1,30,NULL);
  MSG m; while(!KB.res && GetMessageW(&m,NULL,0,0)>0){ TranslateMessage(&m); DispatchMessageW(&m); }
  int n=-1; if(KB.res==1){ n=KB.n; memcpy(out,KB.txt,n*2); }
  KillTimer(win,1); DestroyWindow(win); if(dim) DestroyWindow(dim); DeleteObject(KB.fb); DeleteObject(KB.fh);
  /* wait until Enter/Esc are released, if not the game reads it as a button press when it gets the focus back */
  for(int i=0;i<100 && ((GetAsyncKeyState(VK_RETURN)|GetAsyncKeyState(VK_ESCAPE))&0x8000);i++) Sleep(10);
  if(game) SetForegroundWindow(game);
  return n; }
#else
/* ------------------------------------------------------------------ POSIX */
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sched.h>
#include <errno.h>

void pm_init(PMutex* m){ pthread_mutex_init(&m->l,NULL); }

void pm_lock(PMutex* m){ pthread_mutex_lock(&m->l); }

void pm_unlock(PMutex* m){ pthread_mutex_unlock(&m->l); }

void pc_init(PCond* c){ pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a,CLOCK_MONOTONIC); pthread_cond_init(&c->c,&a); }

void pc_signal(PCond* c){ pthread_cond_signal(&c->c); }

void pc_broadcast(PCond* c){ pthread_cond_broadcast(&c->c); }

int pc_wait(PCond* c, PMutex* m, i64 ns){
  if(ns<0){ pthread_cond_wait(&c->c,&m->l); return 0; }
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); u64 t=(u64)ts.tv_sec*1000000000ull+ts.tv_nsec+(u64)ns;
  ts.tv_sec=t/1000000000ull; ts.tv_nsec=t%1000000000ull;
  return pthread_cond_timedwait(&c->c,&m->l,&ts)==ETIMEDOUT; }

typedef struct { PThreadFn fn; void* arg; } TArg;

static void* tstart(void* p){ TArg a=*(TArg*)p; free(p); a.fn(a.arg); return NULL; }

int plat_thread(PThreadFn fn, void* arg, size_t stack){
  TArg* a=malloc(sizeof *a); a->fn=fn; a->arg=arg; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,stack);
  pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED); pthread_t t; int r=pthread_create(&t,&at,tstart,a); pthread_attr_destroy(&at); return r? -1 : 0; }

void plat_thread_exit(void){ pthread_exit(NULL); }

void plat_sleep_ns(i64 ns){ if(ns<=0){ sched_yield(); return; } struct timespec ts={ns/1000000000,ns%1000000000}; while(nanosleep(&ts,&ts)==-1 && errno==EINTR); }

void plat_yield(void){ sched_yield(); }

void plat_thread_low_priority(void){ }

void plat_thread_high_priority(void){ }

u64 plat_thread_cpu_ns(void){ struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

u64 plat_process_cpu_ns(void){ struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

u64 plat_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+ts.tv_nsec; }

i64 plat_unix_time(void){ return (i64)time(NULL); }

i64 plat_tz_offset(void){ time_t t=time(NULL); struct tm lt; localtime_r(&t,&lt); return (i64)lt.tm_gmtoff; }

int g_ww_ok;

int plat_ww_collect(u8* base, u64 size, void (*cb)(u64)){ (void)base; (void)size; (void)cb; return -1; }

u8* plat_vm_reserve(u64 size){ void* p=mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); return p==MAP_FAILED? NULL : (u8*)p; }

void plat_vm_readonly(u8* p, u64 size){ mprotect(p,size,PROT_READ); }
int plat_vm_commit(u8* p, u64 size){ return mprotect(p,size,PROT_READ|PROT_WRITE); }

void plat_vm_decommit(u8* p, u64 size){ madvise(p,size,MADV_DONTNEED); mprotect(p,size,PROT_NONE); }

void* plat_dl_open(const char* path){ return dlopen(path,RTLD_NOW|RTLD_LOCAL); }

void* plat_dl_sym(void* h, const char* n){ return dlsym(h,n); }

int plat_mkdir(const char* p){ return mkdir(p,0755); }

int plat_is_dir(const char* p){ struct stat s; return !stat(p,&s) && S_ISDIR(s.st_mode); }

typedef struct { DIR* d; char path[1024]; } PD_;

int plat_dir_open(PDir* d, const char* path){ PD_* p=malloc(sizeof *p); p->d=opendir(path); if(!p->d){ free(p); return -1; } strncpy(p->path,path,1023); p->path[1023]=0; d->h=p; return 0; }

int plat_dir_next(PDir* d){ PD_* p=d->h; struct dirent* e;
  while((e=readdir(p->d))){ if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue; strncpy(d->name,e->d_name,511); d->name[511]=0;
    char f[1600]; snprintf(f,sizeof f,"%s/%s",p->path,e->d_name); struct stat s; if(stat(f,&s)) continue; d->is_dir=S_ISDIR(s.st_mode); d->size=(u64)s.st_size; return 1; }
  return 0; }

void plat_dir_close(PDir* d){ PD_* p=d->h; closedir(p->d); free(p); }

u64 plat_file_size(const char* p){ struct stat s; if(stat(p,&s)) return 0; return (u64)s.st_size; }

int plat_pread(FILE* f, void* buf, u64 size, u64 off){ return (int)pread(fileno(f),buf,size,(off_t)off); }

int plat_ncpu(void){ long n=sysconf(_SC_NPROCESSORS_ONLN); return n>0? (int)n : 1; }

void plat_battery(int* pct, int* chg){ *pct=100; *chg=1;
  FILE* f=fopen("/sys/class/power_supply/BAT0/capacity","r"); if(!f) return;
  if(fscanf(f,"%d",pct)!=1 || *pct<0 || *pct>100) *pct=100; fclose(f);
  f=fopen("/sys/class/power_supply/BAT0/status","r"); if(!f) return;
  char s[32]={0}; if(fgets(s,sizeof s,f)) *chg= strncmp(s,"Discharging",11)!=0; fclose(f); }

__thread void* plat_guard_rec;

void plat_guard_enter(void* rec){ plat_guard_rec=rec; }

void* plat_guard_get(void){ return plat_guard_rec; }

volatile int g_driver_recovered;   /* times we jumped out of a driver crash */

static void sighandler(int sig, siginfo_t* si, void* uc_){
  plat_jmpbuf* b=(plat_jmpbuf*)plat_guard_get();
  if(b){
    { extern void gpu_crash_note(void); gpu_crash_note(); }
    plat_guard_enter(NULL); g_driver_recovered++;
    logf_(0,"  (signal %d inside the video driver, recovered: only that effect is missing)\n",sig);
    siglongjmp(*b,1); }
  extern void crash_report(void* host_pc, void* fault_addr, int sig);
  void* pc=NULL;
#if defined(__x86_64__)
  #include <ucontext.h>
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
  #include <ucontext.h>
  ucontext_t* uc=(ucontext_t*)uc_; pc=(void*)uc->uc_mcontext.pc;
#else
  (void)uc_;
#endif
  crash_report(pc,si->si_addr,sig); _exit(1); }

/* errors and the F1 help as a desktop dialog: zenity (GNOME and most desktops) or kdialog (KDE, steam deck).
   without either the text only goes to the log. wait=0 leaves the dialog open while the game runs */
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;

static void reap(void* p){ int st; waitpid((pid_t)(size_t)p,&st,0); }

void plat_message(const char* text, int error, int wait){
  if(!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) return;
  char* z[]={"zenity",error? "--error" : "--info","--no-markup","--no-wrap","--title=SwitchRecomp","--text",(char*)text,NULL};
  char* k[]={"kdialog","--title","SwitchRecomp",error? "--error" : "--msgbox",(char*)text,NULL};
  pid_t pid;
  if(posix_spawnp(&pid,z[0],NULL,NULL,z,environ) && posix_spawnp(&pid,k[0],NULL,NULL,k,environ)) return;
  if(wait) reap((void*)(size_t)pid); else plat_thread(reap,(void*)(size_t)pid,1<<16); }

void plat_install_crash_handler(void){
  struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=sighandler; sa.sa_flags=SA_SIGINFO|SA_ONSTACK;
  static char altstack[65536]; stack_t ss; ss.ss_sp=altstack; ss.ss_size=sizeof altstack; ss.ss_flags=0; sigaltstack(&ss,NULL);
  sigaction(SIGSEGV,&sa,NULL); sigaction(SIGBUS,&sa,NULL); sigaction(SIGILL,&sa,NULL); sigaction(SIGFPE,&sa,NULL); }
#endif
#ifndef _WIN32
void plat_prof_start(int boot){ (void)boot; }

void* plat_thread_self(void){ return NULL; }

void plat_thread_forget(void* h){ (void)h; }

int plat_thread_pc(void* h, u64* pc){ (void)h; (void)pc; return 0; }

int plat_addr_module(u64 a, char* name, int n, u64* off){ (void)a; (void)name; (void)n; (void)off; return 0; }
int plat_text_input(const u16* def, u16* out, int max){ (void)def; (void)out; (void)max; return -1; }
#endif
