#!/usr/bin/env python3
"""Native x86_64 Linux Headless builder & runner for SwitchRecomp on 253.

Reuses the generated C files (`module.c` + `code_*.c`) and game assets (`swr_data`)
from an existing PS5 build directory (e.g. `/data/switchrecomp/ps5o2_fixed/swr_bananza_ps5_o2_fixed_build`),
compiles and caches native x86_64 Linux `.o` files into `<build>/linux_objs` -> `libgame_linux.so`,
and compiles the C runtime (`runtime/*.c`) with `-g -O1 -rdynamic` (and optional ASan)
into a native Linux executable (`bananza_headless`) in < 2 seconds on incremental edits.

Usage on 253:
  python3 tools/ps5/linux_headless_build.py \
      --build-dir /data/switchrecomp/ps5o2_fixed/swr_bananza_ps5_o2_fixed_build \
      --data-dir  /data/switchrecomp/ps5o2_fixed/PPSA99201/swr_data \
      --out-dir   /data/switchrecomp/linux_strict
"""
import argparse, glob, hashlib, os, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RT = os.path.join(REPO, 'runtime')

RUNTIME_SRC = [
    'plat', 'mem', 'kernel', 'ipc', 'svc_core', 'audio', 'svc_hid', 'svc_fs',
    'svc_vi', 'nvdrv', 'gpu', 'gpu_engines', 'gpu_3d', 'loader', 'main',
    'nup_ps5', 'gpu_nogl', 'shader_proc', 'host1x', 'snxvk_glue',
    'backend_headless', 'entry', 'glibc_compat'
]

DEFAULT_ZSTD_INC = '/data/code/EglantinePS5-build56/.deps/pacbrew-0.40.2/opt/ps5-payload-sdk/target/user/homebrew/include'


def get_git_hash():
    try:
        r = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=REPO,
                           capture_output=True, text=True, timeout=5)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except Exception:
        pass
    return 'unknown'


def find_zstd_flags():
    inc_flags = []
    if os.path.isdir('/usr/include') and os.path.isfile('/usr/include/zstd.h'):
        pass
    elif os.path.isdir(DEFAULT_ZSTD_INC):
        inc_flags = ['-I', DEFAULT_ZSTD_INC]

    # `-lzstd` only resolves when the *dev* symlink libzstd.so exists. Ubuntu
    # boxes with just libzstd1 installed ship libzstd.so.1 only, so link the file
    # we actually found by absolute path (ld accepts that directly).
    dirs = ('/usr/lib/x86_64-linux-gnu', '/lib/x86_64-linux-gnu',
            '/usr/lib64', '/usr/local/lib', '/lib64')
    found = None
    for d in dirs:
        for name in ('libzstd.so', 'libzstd.a'):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                found = p
                break
        if found:
            break
    if not found:                       # versioned runtime lib, e.g. libzstd.so.1.4.8
        for d in dirs:
            cands = sorted(glob.glob(os.path.join(d, 'libzstd.so.*')))
            if cands:
                found = cands[-1]
                break
    if found:
        print('[linux-headless] zstd: linking %s' % found, flush=True)
        return inc_flags, [found]
    print('[linux-headless] zstd: no library found, hoping -lzstd resolves', flush=True)
    return inc_flags, ['-lzstd']


def compile_game_objs(cc, build_dir, obj_dir, opt, jobs):
    c_files = sorted(glob.glob(os.path.join(build_dir, 'code_*.c')))
    mod_c = os.path.join(build_dir, 'module.c')
    if not os.path.isfile(mod_c) or not c_files:
        raise SystemExit('No module.c / code_*.c found in %s' % build_dir)
    all_c = [mod_c] + c_files
    os.makedirs(obj_dir, exist_ok=True)

    with open(os.path.join(RT, 'cpu.h'), 'rb') as f1, open(os.path.join(RT, 'cpu_helpers.c'), 'rb') as f2:
        sig_in = ('%s|-O%s|' % (cc, opt)).encode('utf-8') + f1.read() + f2.read()
    sig = hashlib.sha1(sig_in).hexdigest()[:10]

    flags = ['-std=gnu17', '-O' + str(opt), '-g1', '-fno-strict-aliasing', '-fwrapv', '-w',
             '-fPIC', '-fvisibility=hidden', '-foptimize-sibling-calls',
             '-fno-semantic-interposition', '-I', RT]

    to_build = []
    objs = []
    for src in all_c:
        base = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(obj_dir, '%s.%s.o' % (base, sig))
        objs.append(obj)
        if not (os.path.isfile(obj) and os.path.getmtime(obj) >= os.path.getmtime(src)):
            to_build.append((src, obj))

    if to_build:
        print('[linux-headless] compiling %d/%d game C files with %s (-O%s, jobs=%d)...' % (
            len(to_build), len(all_c), cc, opt, jobs), flush=True)
        t0 = time.time()
        done = [0]

        def _one(item):
            src, obj = item
            tmp = obj + '.tmp'
            r = subprocess.run([cc] + flags + ['-c', src, '-o', tmp],
                               capture_output=True, text=True, errors='replace')
            if r.returncode != 0:
                raise RuntimeError('compile %s failed:\n%s' % (src, (r.stderr or r.stdout)[-1000:]))
            os.replace(tmp, obj)
            return obj

        # pool.map() yields in *input* order: one slow translation unit (a 4 MB
        # generated code_*.c can take ~40 min at -O1) stalls the progress counter
        # while the other workers keep producing .o files. Count real completions
        # with as_completed() so the log reflects actual progress.
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = [pool.submit(_one, item) for item in to_build]
            for fut in as_completed(futs):
                fut.result()          # propagate compile errors like pool.map did
                done[0] += 1
                if done[0] % 25 == 0 or done[0] == len(to_build):
                    print('  game objs: %d/%d (%.1fs)' % (done[0], len(to_build), time.time() - t0), flush=True)
    else:
        print('[linux-headless] reusing %d cached game objects (sig=%s)' % (len(objs), sig), flush=True)

    so_path = os.path.join(obj_dir, 'libgame_linux.%s.so' % sig)
    need_so = not os.path.isfile(so_path) or any(os.path.getmtime(o) > os.path.getmtime(so_path) for o in objs)
    if need_so:
        rsp = os.path.join(obj_dir, 'linux_objects.rsp')
        with open(rsp, 'w', encoding='utf-8') as f:
            for o in objs:
                f.write('"%s"\n' % os.path.basename(o))
        print('[linux-headless] linking %s...' % so_path, flush=True)
        r = subprocess.run([cc, '-shared', '-o', so_path, '@linux_objects.rsp', '-lm'],
                           cwd=obj_dir, capture_output=True, text=True, errors='replace')
        if r.returncode != 0:
            raise SystemExit('linking libgame_linux.so failed:\n%s' % (r.stderr or r.stdout)[-2000:])
    return so_path, objs[0]


def build_runtime_exe(cc, out_exe, rt_obj_dir, so_path, mod_obj, opt, asan, jobs):
    os.makedirs(rt_obj_dir, exist_ok=True)
    git_hash = get_git_hash()
    zstd_inc, zstd_lib = find_zstd_flags()

    # Check if OS is macOS (for local syntax/compile smoke test) vs Linux
    is_macos = sys.platform == 'darwin'
    srcs = [s for s in RUNTIME_SRC if not (is_macos and s == 'glibc_compat')]

    common_flags = [
        '-std=gnu17', '-O' + str(opt), '-g', '-fno-omit-frame-pointer',
        '-fno-strict-aliasing', '-fwrapv', '-w',
        '-DSWR_NO_H264', '-DSWR_NO_OPUS',
        '-DSWR_GIT_HASH="%s"' % git_hash,
        '-I', RT,
        '-I', os.path.join(RT, 'bcn'),
        '-I', os.path.join(RT, 'spvc'),
        '-I', os.path.join(RT, 'oh264', 'include'),
        '-I', os.path.join(RT, 'opus', 'include'),
    ] + zstd_inc
    if asan:
        common_flags += ['-fsanitize=address,undefined']

    def _comp_rt(name):
        src = os.path.join(RT, name + '.c')
        obj = os.path.join(rt_obj_dir, name + ('.asan.o' if asan else '.o'))
        r = subprocess.run([cc] + common_flags + ['-c', src, '-o', obj],
                           capture_output=True, text=True, errors='replace')
        if r.returncode != 0:
            raise RuntimeError('runtime compile %s failed:\n%s' % (src, (r.stderr or r.stdout)[-1500:]))
        return obj

    # Collect *all* runtime compile failures in one pass instead of bailing on
    # the first one (pool.map raises as soon as its head result fails). Keep the
    # original srcs order for the link command.
    t0 = time.time()
    obj_by_name, failures = {}, []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futs = {pool.submit(_comp_rt, s): s for s in srcs}
        for fut in as_completed(futs):
            name = futs[fut]
            try:
                obj_by_name[name] = fut.result()
            except RuntimeError as e:
                failures.append(str(e))
    if failures:
        raise SystemExit('runtime compile failed (%d/%d files):\n\n%s' % (
            len(failures), len(srcs), '\n\n'.join(sorted(failures))))
    rt_objs = [obj_by_name[s] for s in srcs]
    print('[linux-headless] compiled %d runtime files in %.2fs (git=%s, asan=%s)' % (
        len(rt_objs), time.time() - t0, git_hash, asan), flush=True)

    link_cmd = [cc, '-g', '-o', out_exe]
    if not is_macos:
        link_cmd += ['-rdynamic', '-Wl,-rpath,' + os.path.dirname(os.path.abspath(so_path)),
                     '-Wl,-u,swr_module_static']
    if asan:
        link_cmd += ['-fsanitize=address,undefined']
    # mod_obj (module.o) defines swr_module_static. The game code lives in a
    # shared library here, and `-Wl,-u,swr_module_static` only forces extraction
    # from a *static* archive -- with --as-needed the .so does not satisfy that
    # strong reference. Link module.o into the executable directly (the same
    # symbol also exists in the .so, which is harmless: the exe definition wins).
    link_cmd += rt_objs + [mod_obj, so_path] + zstd_lib + ['-lpthread', '-ldl', '-lm']
    r = subprocess.run(link_cmd, capture_output=True, text=True, errors='replace')
    if r.returncode != 0:
        raise SystemExit('linking %s failed:\ncmd: %s\n%s' % (
            out_exe, ' '.join(link_cmd), (r.stderr or r.stdout)[-2000:]))
    os.chmod(out_exe, 0o755)
    print('[linux-headless] built executable: %s' % out_exe, flush=True)


def main():
    ap = argparse.ArgumentParser(description='Build native x86_64 Linux Headless SwitchRecomp binary on 253.')
    ap.add_argument('--build-dir', default='/data/switchrecomp/ps5o2_fixed/swr_bananza_ps5_o2_fixed_build',
                    help='directory containing generated module.c and code_*.c')
    ap.add_argument('--data-dir', default='/data/switchrecomp/ps5o2_fixed/PPSA99201/swr_data',
                    help='game data directory (exefs/ + romfs.nup or romfs/)')
    ap.add_argument('--out-dir', default='/data/switchrecomp/linux_strict',
                    help='output directory for bananza_headless binary, logs, and data symlink')
    ap.add_argument('--name', default='bananza_headless', help='executable base name')
    ap.add_argument('--cc', default=os.environ.get('CC', 'clang'), help='host C compiler (clang or gcc)')
    ap.add_argument('--game-opt', default='1', help='optimization level for game code_*.c (default: 1 for fast build)')
    ap.add_argument('--rt-opt', default='1', help='optimization level for runtime/*.c (default: 1 for debuggability)')
    ap.add_argument('--asan', action='store_true', help='enable AddressSanitizer + UBSan on runtime')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 16)
    ap.add_argument('--runtime-syntax-only', action='store_true',
                    help='only syntax-check runtime/*.c with the host compiler and exit')
    a = ap.parse_args()

    cc = shutil.which(a.cc) or shutil.which('gcc') or shutil.which('clang')
    if not cc:
        raise SystemExit('No C compiler found (%s)' % a.cc)

    if a.runtime_syntax_only:
        git_hash = get_git_hash()
        is_macos = sys.platform == 'darwin'
        srcs = [s for s in RUNTIME_SRC if not (is_macos and s in ('glibc_compat', 'nup_ps5'))]
        flags = ['-fsyntax-only', '-std=gnu17', '-Wall', '-Wextra', '-Wno-unused-parameter',
                 '-Wno-sign-compare', '-Wno-missing-field-initializers',
                 '-DSWR_NO_H264', '-DSWR_NO_OPUS', '-DSWR_GIT_HASH="%s"' % git_hash, '-I', RT]
        for s in srcs:
            p = os.path.join(RT, s + '.c')
            r = subprocess.run([cc] + flags + [p], capture_output=True, text=True, errors='replace')
            if r.returncode != 0:
                print('FAIL: %s\n%s' % (s, r.stderr or r.stdout), flush=True)
                return 1
        print('[linux-headless] syntax check PASSED for %d runtime files (git=%s)' % (len(srcs), git_hash))
        return 0

    os.makedirs(a.out_dir, exist_ok=True)
    obj_dir = os.path.join(a.build_dir, 'linux_objs')
    rt_obj_dir = os.path.join(a.out_dir, 'rt_objs')
    out_exe = os.path.join(a.out_dir, a.name)

    so_path, mod_obj = compile_game_objs(cc, a.build_dir, obj_dir, a.game_opt, a.jobs)
    build_runtime_exe(cc, out_exe, rt_obj_dir, so_path, mod_obj, a.rt_opt, a.asan, a.jobs)

    # Link <out_dir>/<name>_data -> data_dir so runtime/main.c finds exefs/ and romfs
    data_link = os.path.join(a.out_dir, a.name + '_data')
    if os.path.isdir(a.data_dir):
        if os.path.islink(data_link) or os.path.exists(data_link):
            if os.path.islink(data_link):
                os.unlink(data_link)
        if not os.path.exists(data_link):
            os.symlink(os.path.abspath(a.data_dir), data_link)
            print('[linux-headless] linked %s -> %s' % (data_link, a.data_dir), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
