#!/usr/bin/env python3
# Drive the cc9 NATIVE clang (on 9front) over STL tests using the REAL libc++ from LLVM.
# clang -cc1 needs the exact frontend flags the driver normally adds; since there is no
# driver on-box (exec is stubbed), we capture the driver's -cc1 line on the host and
# rewrite its absolute paths to the on-box staged trees. Each invocation goes through a
# response file (avoids the >=9-kernel-argv crash). COMPILE = native clang produces a .o;
# RUN = the native-emitted IR, built into an a.out, runs on 9front and prints PASS.
import socket, time, os, subprocess, glob, shlex

VMHOST,VMPORT='127.0.0.1',1717; HOSTIP='10.0.2.2'; HTTP='8099'
CLANG='/tmp/cr'; LLVM=os.environ.get('CC9_LLVM','/opt/homebrew/opt/llvm/bin')
D=os.path.dirname(os.path.abspath(__file__)); CC9=os.path.abspath(os.path.join(D,'..','..'))
LIBCXX=os.environ.get('CC9_LIBCXX','/tmp/libcxx-thr/include/c++/v1')
RES=subprocess.run([f'{LLVM}/clang','-print-resource-dir'],capture_output=True,text=True).stdout.strip()
# on-box path map
SUB=[(LIBCXX,'/tmp/sysinc/cxxv1'),(os.path.join(CC9,'runtime/include'),'/tmp/sysinc/cc9'),
     (RES+'/include','/tmp/res'),(RES,'/tmp/res_rd')]

def vm(cmd,wait=90):
    s=socket.create_connection((VMHOST,VMPORT),timeout=8); s.sendall(cmd.encode()); s.shutdown(socket.SHUT_WR); s.settimeout(wait)
    out=b''
    try:
        while True:
            b=s.recv(8192)
            if not b: break
            out+=b
    except socket.timeout: pass
    s.close(); return out.decode('latin-1')

def cc1_tokens(src,out,emit):
    # host driver -> the exact -cc1 invocation, then rewrite paths for on-box
    cmd=[f'{LLVM}/clang','-###','--target=x86_64-unknown-none','-std=c++23','-nostdinc++',
         '-isystem',LIBCXX,'-isystem',os.path.join(CC9,'runtime/include'),
         '-fexceptions','-frtti','-funwind-tables','-fno-pic','-femulated-tls',
         '-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE','-D_LIBCPP_HAS_CLOCK_GETTIME',
         '-c',src,'-o',out]
    r=subprocess.run(cmd,capture_output=True,text=True)
    line=[l for l in r.stderr.splitlines() if '-cc1' in l][0].strip()
    toks=shlex.split(line)[1:]  # drop the clang binary path; keep -cc1...
    def fix(t):
        for a,b in SUB:
            if t==a or t.startswith(a+'/'): t=b+t[len(a):]
        return t
    toks=[fix(t) for t in toks]
    # swap emit kind (-emit-obj default) for IR runs
    if emit=='-emit-llvm':
        toks=['-emit-llvm' if t=='-emit-obj' else t for t in toks]
    # builtin (stddef/stdarg) + cc9 libc headers must be reachable by include_next
    toks += ['-idirafter','/tmp/res','-idirafter','/tmp/sysinc/cc9']
    return toks

def native_compile(name,src_vm,out_vm,emit):
    toks=cc1_tokens(f'/tmp/{name}.cpp',out_vm,emit)
    # rewrite the input token to the on-box source path
    toks=[ (src_vm if t.endswith(f'{name}.cpp') else t) for t in toks ]
    rsp=' '.join(toks)
    # write the response file on the host, serve it, hget on the VM (no base64 on 9front)
    open(f'/tmp/{name}.rsp','w').write(rsp)
    vm(f'hget http://{HOSTIP}:{HTTP}/{name}.rsp > /tmp/{name}.rsp\n',20); time.sleep(0.15)
    vm(f'rm -f {out_vm}\n'); time.sleep(0.1)
    vm(f'{CLANG} @/tmp/{name}.rsp\n',120); time.sleep(0.4)
    chk=vm(f'ls -l {out_vm}\n')
    return ('glenda' in chk) and (chk.strip().split()[-2] not in ('0',''))

def main():
    tests=sorted(os.path.basename(f)[:-4] for f in glob.glob(os.path.join(D,'*.cpp')))
    for t in tests:
        subprocess.run(['cp',os.path.join(D,t+'.cpp'),f'/tmp/{t}.cpp'],check=True)
        vm(f'hget http://{HOSTIP}:{HTTP}/{t}.cpp > /tmp/{t}.cpp\n',30); time.sleep(0.1)
    # host baseline (applicable set)
    base=[]
    for t in tests:
        r=subprocess.run([f'{LLVM}/clang']+cc1_tokens_host(t),capture_output=True)
        if r.returncode==0: base.append(t)
    print(f'host baseline: {len(base)}/{len(tests)} compile')
    comp={}; run={}
    for t in base:
        comp[t]=native_compile(t,f'/tmp/{t}.cpp',f'/tmp/{t}.o','-emit-obj')
        okrun=False
        if comp[t]:
            native_compile(t,f'/tmp/{t}.cpp',f'/tmp/{t}.ll','-emit-llvm')
            ll=vm(f'cat /tmp/{t}.ll\n',40)
            if 'define' in ll:
                open(f'/tmp/{t}.ll','w').write(ll)
                o=subprocess.run([f'{LLVM}/clang','--target=x86_64-unknown-none','-c',f'/tmp/{t}.ll','-o',f'/tmp/{t}.fromll.o'],capture_output=True)
                if o.returncode==0:
                    l=subprocess.run(['bash',os.path.join(CC9,'native','cc9-link'),f'/tmp/{t}.fromll.o','-o',f'/tmp/{t}.aout'],capture_output=True)
                    if l.returncode==0 and os.path.exists(f'/tmp/{t}.aout'):
                        vm(f'hget http://{HOSTIP}:{HTTP}/{t}.aout > /tmp/{t}.run; chmod 755 /tmp/{t}.run\n',30); time.sleep(0.2)
                        okrun='PASS' in vm(f'/tmp/{t}.run\n',20)
        run[t]=okrun
        print(f'  {t:22} compile={"OK" if comp[t] else "FAIL":4} run={"PASS" if okrun else "----"}')
    nc=sum(comp.values()); nr=sum(run.values())
    print(f'\n=== RESULTS (native cc9 clang + real libc++ on 9front) ===')
    print(f'applicable: {len(base)}/{len(tests)}; native COMPILE {nc}/{len(base)} ({100*nc//max(1,len(base))}%); native RUN {nr}/{len(base)} ({100*nr//max(1,len(base))}%)')

def cc1_tokens_host(t):
    return ['--target=x86_64-unknown-none','-std=c++23','-nostdinc++','-isystem',LIBCXX,
            '-isystem',os.path.join(CC9,'runtime/include'),'-fexceptions','-frtti','-funwind-tables','-fno-pic',
            '-femulated-tls','-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE','-D_LIBCPP_HAS_CLOCK_GETTIME',
            '-c',f'/tmp/{t}.cpp','-o',f'/tmp/{t}.host.o']

if __name__=='__main__': main()
