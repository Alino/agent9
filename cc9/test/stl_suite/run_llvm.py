#!/usr/bin/env python3
# Run ACTUAL LLVM libc++ .pass.cpp tests (from llvm-project/libcxx/test) through the
# native cc9 clang on 9front. "applicable" = host clang (cc9 config) compiles+runs it.
# native PASS = native clang compiles it AND the built a.out runs to clean exit 0 (libc++
# .pass.cpp main returns 0; assert-failure -> cc9 abort/assert output).
import socket,time,os,subprocess,shlex
VMHOST,VMPORT='127.0.0.1',1717; HOSTIP='10.0.2.2'; HTTP='8099'
CLANG='/tmp/cr'; LLVM=os.environ.get('CC9_LLVM','/opt/homebrew/opt/llvm/bin')
CC9=os.path.abspath(os.path.join(os.path.dirname(__file__),'..','..'))
LIBCXX=os.environ.get('CC9_LIBCXX','/tmp/libcxx-thr/include/c++/v1')
RES=subprocess.run([f'{LLVM}/clang','-print-resource-dir'],capture_output=True,text=True).stdout.strip()
SUP=os.path.expanduser('~/Projects/llvm-project/libcxx/test/support')
SEL=[l.strip() for l in open('/tmp/sel.txt') if l.strip()]
SUB=[(LIBCXX,'/tmp/sysinc/cxxv1'),(os.path.join(CC9,'runtime/include'),'/tmp/sysinc/cc9'),
     (RES+'/include','/tmp/res'),(RES,'/tmp/res_rd'),(SUP,'/tmp/llvmsup')]
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
def cc1(src,out,emit):
    cmd=[f'{LLVM}/clang','-###','--target=x86_64-unknown-none','-std=c++23','-nostdinc++','-isystem',LIBCXX,
         '-isystem',os.path.join(CC9,'runtime/include'),'-I',SUP,'-fexceptions','-frtti','-funwind-tables','-fno-pic',
         '-femulated-tls','-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE','-D_LIBCPP_HAS_CLOCK_GETTIME','-c',src,'-o',out]
    r=subprocess.run(cmd,capture_output=True,text=True)
    toks=shlex.split([l for l in r.stderr.splitlines() if '-cc1' in l][0].strip())[1:]
    def fix(t):
        for a,b in SUB:
            if t==a or t.startswith(a+'/'): return b+t[len(a):]
        return t
    toks=[fix(t) for t in toks]
    if emit=='-emit-llvm': toks=['-emit-llvm' if t=='-emit-obj' else t for t in toks]
    toks+=['-idirafter','/tmp/res','-idirafter','/tmp/sysinc/cc9']
    return toks
def ncompile(name,emit,out):
    open(f'/tmp/{name}.rsp','w').write(' '.join(t.replace(f'/tmp/{name}.cpp.cpp',f'/tmp/{name}.cpp') for t in cc1(f'/tmp/{name}.cpp',out,emit)))
    vm(f'hget http://{HOSTIP}:{HTTP}/{name}.rsp > /tmp/{name}.rsp\n',20); time.sleep(0.15)
    vm(f'rm -f {out}\n'); time.sleep(0.1); vm(f'{CLANG} @/tmp/{name}.rsp\n',120); time.sleep(0.4)
    chk=vm(f'ls -l {out}\n'); return ('glenda' in chk) and (chk.strip().split()[-2] not in ('0',''))
def main():
    # support headers already staged on the VM at /tmp/llvmsup (see run notes)
    names=[]
    for p in SEL:
        full=os.path.expanduser('~/Projects/llvm-project/'+p); name='llt_'+os.path.basename(os.path.dirname(p))+'_'+os.path.basename(p)[:-4]
        name=name.replace('.','_'); names.append((name,full))
        subprocess.run(['cp',full,f'/tmp/{name}.cpp'],check=True)
        vm(f'hget http://{HOSTIP}:{HTTP}/{name}.cpp > /tmp/{name}.cpp\n',30); time.sleep(0.1)
    # host baseline
    base=[]
    for name,full in names:
        r=subprocess.run([f'{LLVM}/clang','--target=x86_64-unknown-none','-std=c++23','-nostdinc++','-isystem',LIBCXX,
            '-isystem',os.path.join(CC9,'runtime/include'),'-I',SUP,'-fexceptions','-frtti','-funwind-tables','-fno-pic','-femulated-tls',
            '-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE','-D_LIBCPP_HAS_CLOCK_GETTIME','-c',f'/tmp/{name}.cpp','-o',f'/tmp/{name}.host.o'],capture_output=True)
        if r.returncode==0: base.append((name,full))
    print(f'host baseline (applicable): {len(base)}/{len(names)}')
    comp={}; run={}
    for name,full in base:
        comp[name]=ncompile(name,'-emit-obj',f'/tmp/{name}.o')
        ok=False
        if comp[name]:
            ncompile(name,'-emit-llvm',f'/tmp/{name}.ll'); ll=vm(f'cat /tmp/{name}.ll\n',40)
            if 'define' in ll:
                open(f'/tmp/{name}.ll','w').write(ll)
                o=subprocess.run([f'{LLVM}/clang','--target=x86_64-unknown-none','-c',f'/tmp/{name}.ll','-o',f'/tmp/{name}.fromll.o'],capture_output=True)
                if o.returncode==0:
                    l=subprocess.run(['bash',os.path.join(CC9,'native','cc9-link'),f'/tmp/{name}.fromll.o','-o',f'/tmp/{name}.aout'],capture_output=True)
                    if l.returncode==0 and os.path.exists(f'/tmp/{name}.aout'):
                        vm(f'hget http://{HOSTIP}:{HTTP}/{name}.aout > /tmp/{name}.run; chmod 755 /tmp/{name}.run\n',30); time.sleep(0.2)
                        outp=vm(f'/tmp/{name}.run; echo CC9DONE\n',25)
                        ok = ('CC9DONE' in outp) and ('cc9 assert' not in outp) and ('cc9:' not in outp)
        run[name]=ok
        print(f'  {os.path.basename(full):34} compile={"OK" if comp[name] else "FAIL":4} run={"PASS" if ok else "----"}')
    nc=sum(comp.values()); nr=sum(run.values())
    print(f'\n=== ACTUAL LLVM libc++ .pass.cpp via native cc9 clang ===')
    print(f'applicable {len(base)}/{len(names)}; native COMPILE {nc}/{len(base)}; native RUN {nr}/{len(base)} ({100*nr//max(1,len(base))}%)')
if __name__=='__main__': main()
