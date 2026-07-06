#!/usr/bin/env python3
"""build-nvim.py — the compile_commands→cc9 harvest bridge (gl9 pattern).

Recompiles exactly the TUs that the host reference build linked into bin/nvim
(ninja -t inputs), with the host's -D/-I flags, minus macOS-isms, against:
  - a plan9-patched copy of cmake.config (auto/config.h),
  - our cross-built libuv/luajit headers (before .deps' host ones),
then links a 9front a.out via n9link + the _out dep archives.
Generated sources (build/src/nvim/auto) come straight from the host build.
"""
import json, os, re, shlex, subprocess, sys, shutil

PORT = os.path.dirname(os.path.abspath(__file__))
N9 = os.path.dirname(PORT)
NVIM = os.path.join(N9, 'vendor', 'neovim')
BUILD = os.path.join(NVIM, 'build')
OUT = os.path.join(N9, '_out')
OBJ = os.path.join(OUT, 'nvim-obj')
CFG = os.path.join(OUT, 'nvim-config')

DROP_HAVES = [
    'HAVE__NSGETENVIRON', 'HAVE_LANGINFO_H', 'HAVE_NL_LANGINFO_CODESET',
    'HAVE_SYS_SDT_H', 'HAVE_WORKING_LIBINTL',
    'HAVE_EXECINFO_BACKTRACE', 'HAVE_DIRFD_AND_FLOCK', 'HAVE_PWD_FUNCS',
    'HAVE_STRPTIME',
]

def make_config():
    if os.path.exists(CFG):
        shutil.rmtree(CFG)
    shutil.copytree(os.path.join(BUILD, 'cmake.config'), CFG)
    p = os.path.join(CFG, 'auto', 'config.h')
    s = open(p).read()
    for h in DROP_HAVES:
        s = s.replace('#define %s\n' % h, '/* %s: not on plan9 */\n' % h)
    s = s.replace('#define CASE_INSENSITIVE_FILENAME', '/* case-sensitive on plan9 */')
    s = s.replace('#define ENDIAN_INCLUDE_FILE <sys/endian.h>',
                  '#define ENDIAN_INCLUDE_FILE <endian.h>')
    open(p, 'w').write(s)

def objects():
    out = subprocess.run(['ninja', '-t', 'inputs', 'bin/nvim'], cwd=BUILD,
                         capture_output=True, text=True).stdout.split()
    # nlua0 is the BUILD-TIME lua host; its objects must not link into nvim
    return [o for o in out if o.endswith('.o') and 'nlua0.dir' not in o]

def translate(args):
    keep = ['-O2']
    i = 0
    while i < len(args):
        a = args[i]
        if a in ('-isystem',):
            path = args[i+1]; i += 2
            if 'luajit-2.1' in path:
                keep += ['-I', os.path.join(OUT, 'luajit')]
            elif path.endswith('.deps/usr/include'):
                keep += ['-I', os.path.join(OUT, 'libuv'), '-isystem', path]
            # homebrew gettext etc: dropped
            continue
        if a.startswith('-I'):
            path = a[2:]
            if path.endswith('cmake.config'):
                keep.append('-I' + CFG)
            else:
                keep.append(a)
        elif a.startswith('-D') or a in ('-std=gnu99', '-fsigned-char',
                                         '-Wimplicit-fallthrough', '-pedantic'):
            keep.append(a)
        i += 1
    return keep

def main():
    make_config()
    os.makedirs(OBJ, exist_ok=True)
    cc = json.load(open(os.path.join(BUILD, 'compile_commands.json')))
    by_output = {e['output']: e for e in cc if 'output' in e}
    objs = []
    todo = []
    for o in objects():
        e = by_output.get(o) or by_output.get(os.path.join('build', o))
        if e is None:
            # outputs in compile_commands are absolute or build-relative
            cand = [k for k in by_output if k.endswith(o)]
            if not cand:
                print('NO ENTRY for', o); sys.exit(1)
            e = by_output[cand[0]]
        name = re.sub(r'[/]', '_', o)
        obj = os.path.join(OBJ, name)
        objs.append(obj)
        src = e['file']
        flags = translate(shlex.split(e['command'])[1:])
        if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src):
            continue
        todo.append((src, flags, obj))

    import concurrent.futures
    fails = 0
    def one(t):
        src, flags, obj = t
        r = subprocess.run([os.path.join(PORT, 'n9cc')] + flags + ['-c', src, '-o', obj],
                           capture_output=True, text=True)
        return (src, r)
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        for src, r in ex.map(one, todo):
            if r.returncode != 0:
                fails += 1
                print('FAIL', src)
                print(r.stderr[:3000])
                if fails >= int(os.environ.get('NVIM9_MAXFAIL', '3')):
                    sys.exit(1)
    if fails:
        sys.exit(1)

    libs = [os.path.join(OUT, 'deps', 'lib%s.a' % n) for n in
            ('luv', 'tsparser_c', 'tsparser_lua', 'tsparser_vim', 'tsparser_vimdoc',
             'tsparser_query', 'tsparser_markdown', 'tree-sitter', 'lpeg',
             'unibilium', 'utf8proc')]
    libs += [os.path.join(OUT, 'luajit', 'libluajit.a'),
             os.path.join(OUT, 'libuv', 'libuv.a')]
    r = subprocess.run([os.path.join(PORT, 'n9link'), '-o', os.path.join(OUT, 'nvim.aout')]
                       + objs + libs)
    sys.exit(r.returncode)

if __name__ == '__main__':
    main()
