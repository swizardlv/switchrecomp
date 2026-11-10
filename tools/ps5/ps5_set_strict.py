#!/usr/bin/env python3
"""Turn the PS5 strict debug mode on/off by editing swr.ini over FTP.

strict=1 keeps the guest's page 0 PROT_NONE, so the first NULL(+off) dereference
faults immediately and the backtrace points at the stub that returned 0 (see
docs/ps5_blackscreen_triage_and_debug_strategy.md section 5.2).

    python3 tools/ps5/ps5_set_strict.py 1     # enable
    python3 tools/ps5/ps5_set_strict.py 0     # disable
    python3 tools/ps5/ps5_set_strict.py --show

The ini lives at /data/switchrecomp/swr.ini (main.c pins exename="swr" and
exedir=/data/switchrecomp/ on SWR_PS5). Unknown keys survive ini_write() in an
[Other] section, so the setting persists across runs. Host/port come from
<repo>/.env via ps5_env.

Idempotent: reads the current ini, backs it up locally, adds or updates the
strict key, uploads, then reads back to verify.
"""
import argparse
import ftplib
import io
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps5_env   # host/port settings come from <repo>/.env (see .env.example)

REMOTE_DIR = ps5_env.DEV_WORK_ROOT
REMOTE_INI = 'swr.ini'
BACKUP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'runs')


def connect():
    ftp = ftplib.FTP()
    ftp.connect(ps5_env.PS5_HOST, ps5_env.PS5_FTP_PORT, timeout=20)
    ftp.login()
    ftp.set_pasv(True)
    ftp.cwd(REMOTE_DIR)
    return ftp


def show(ftp):
    buf = io.BytesIO()
    ftp.retrbinary('RETR ' + REMOTE_INI, buf.write)
    text = buf.getvalue().decode('utf-8', 'replace')
    hits = [l for l in text.splitlines()
            if re.match(r'^\s*strict\s*=', l) and not l.lstrip().startswith((';', '#'))]
    print('%s/%s: %d bytes, strict lines = %s' % (REMOTE_DIR, REMOTE_INI, len(buf.getvalue()), hits))
    return hits


def set_strict(ftp, val):
    buf = io.BytesIO()
    ftp.retrbinary('RETR ' + REMOTE_INI, buf.write)
    orig = buf.getvalue()
    os.makedirs(BACKUP_DIR, exist_ok=True)
    backup = os.path.join(BACKUP_DIR, 'swr.ini.ps5.bak.%s' % time.strftime('%Y%m%d_%H%M%S'))
    with open(backup, 'wb') as f:
        f.write(orig)
    print('downloaded %d bytes; local backup: %s' % (len(orig), backup))

    lines = orig.decode('utf-8', 'replace').splitlines()
    found = False
    out = []
    for ln in lines:
        if re.match(r'^\s*strict\s*=', ln) and not ln.lstrip().startswith((';', '#')):
            out.append('strict=%s' % val)
            found = True
        else:
            out.append(ln)
    if not found:
        out += ['', '[Other]', 'strict=%s' % val]
    new = ('\n'.join(out) + '\n').encode('utf-8')
    print('strict key %s -> %s (new size %d)' % ('FOUND' if found else 'ADDED', val, len(new)))

    ftp.storbinary('STOR ' + REMOTE_INI, io.BytesIO(new))
    print('uploaded')
    hits = show(ftp)
    if hits != ['strict=%s' % val]:
        print('VERIFY FAILED: %r' % hits, file=sys.stderr)
        return 1
    print('VERIFY OK: strict=%s is live in %s/%s' % (val, REMOTE_DIR, REMOTE_INI))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('value', nargs='?', default=None, help='1 = on, 0 = off')
    ap.add_argument('--show', action='store_true', help='only print the current strict setting')
    a = ap.parse_args()
    if not a.show and a.value not in ('0', '1'):
        ap.error('value must be 0 or 1 (or use --show)')
    ftp = connect()
    try:
        print('pwd =', repr(ftp.pwd()))
        if a.show:
            show(ftp)
            return 0
        return set_strict(ftp, a.value)
    finally:
        try:
            ftp.quit()
        except Exception:
            pass


if __name__ == '__main__':
    raise SystemExit(main())
