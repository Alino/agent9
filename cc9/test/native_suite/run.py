#!/usr/bin/env python3
# Drive the cc9 NATIVE clang (running on 9front) over the native_suite tests, using the
# response-file workaround (cc-on9). Two metrics:
#   COMPILE: native clang on-box compiles test.cpp -> test.o (the compiler works).
#   RUN:     the native-emitted IR, built into an a.out and run on 9front, prints PASS
#            (the compiler's output is correct).
import socket, time, os, subprocess, glob, sys

VMHOST, VMPORT = '127.0.0.1', 1717
HOSTIP = '10.0.2.2'         # host as seen from the QEMU user-net VM
HTTP = '8099'               # python http.server on the host serving /tmp
CLANG = '/tmp/cr'           # native cc9 clang on the VM
D = os.path.dirname(os.path.abspath(__file__))
LLVM = os.environ.get('CC9_LLVM','/opt/homebrew/opt/llvm/bin')

def vm(cmd, wait=60):
    s=socket.create_connection((VMHOST,VMPORT),timeout=8)
    s.sendall(cmd.encode()); s.shutdown(socket.SHUT_WR); s.settimeout(wait)
    out=b''
    try:
        while True:
            b=s.recv(8192)
            if not b: break
            out+=b
    except socket.timeout: pass
    s.close(); return out.decode('latin-1')

def vm_ok():  # control: listener + a trivial native compile works this round
    vm("echo 'int main(){return 0;}' > /tmp/ctl.cpp\n"); time.sleep(0.2)
    vm("echo '-cc1 -triple x86_64-unknown-none -x c++ -emit-llvm /tmp/ctl.cpp -o /tmp/ctl.ll' > /tmp/ctlrsp\n"); time.sleep(0.2)
    vm('rm -f /tmp/ctl.ll; '+CLANG+' @/tmp/ctlrsp\n',60); time.sleep(0.3)
    return 'glenda' in vm('ls -l /tmp/ctl.ll\n')

def native_compile(name, emit, out):
    # native clang on 9front compiles via @file (avoids the >=9 kernel-argv crash)
    rsp=f'-cc1 -triple x86_64-unknown-none -x c++ -std=c++17 {emit} /tmp/{name}.cpp -o {out}'
    vm(f"echo '{rsp}' > /tmp/{name}.rsp\n"); time.sleep(0.15)
    vm(f'rm -f {out}\n'); time.sleep(0.15)
    vm(f'{CLANG} @/tmp/{name}.rsp\n', 90); time.sleep(0.3)
    chk=vm(f'ls -l {out}\n')
    return ('glenda' in chk) and (chk.strip().split()[-2] not in ('0',''))

def main():
    tests=sorted(os.path.basename(f)[:-4] for f in glob.glob(os.path.join(D,'*.cpp')))
    # stage all .cpp into /tmp (served over http) and hget on the VM
    for t in tests: subprocess.run(['cp',os.path.join(D,t+'.cpp'),f'/tmp/{t}.cpp'],check=True)
    for t in tests:
        vm(f'hget http://{HOSTIP}:{HTTP}/{t}.cpp > /tmp/{t}.cpp\n',30); time.sleep(0.1)
    # host baseline: which tests host-clang accepts (the "applicable" set)
    base={}
    for t in tests:
        r=subprocess.run([f'{LLVM}/clang','-cc1','-triple','x86_64-unknown-none','-x','c++','-std=c++17',
                          '-emit-obj',f'/tmp/{t}.cpp','-o',f'/tmp/{t}.host.o'],capture_output=True)
        base[t]= r.returncode==0
    applicable=[t for t in tests if base[t]]
    print(f'host baseline: {len(applicable)}/{len(tests)} compile')

    comp={}; run={}
    for t in applicable:
        for _ in range(4):
            if not vm_ok(): time.sleep(1); continue   # skip flaky round
            comp[t]=native_compile(t,'-emit-obj',f'/tmp/{t}.o')
            break
        # RUN verify: native -> IR, pull, build a.out on host, push, run
        ok_run=False
        if comp.get(t):
            native_compile(t,'-emit-llvm',f'/tmp/{t}.ll')
            ll=vm(f'cat /tmp/{t}.ll\n',30)
            if '@main' in ll or 'define' in ll:
                open(f'/tmp/{t}.ll','w').write(ll)
                # build a.out from the NATIVE-emitted IR using the cc9 host link pipeline
                o=subprocess.run([f'{LLVM}/clang','--target=x86_64-unknown-none','-c',f'/tmp/{t}.ll','-o',f'/tmp/{t}.fromll.o'],capture_output=True)
                if o.returncode==0:
                    l=subprocess.run(['bash',os.path.join(D,'..','..','native','cc9-link'),f'/tmp/{t}.fromll.o','-o',f'/tmp/{t}.aout'],capture_output=True)
                    if l.returncode==0 and os.path.exists(f'/tmp/{t}.aout'):
                        vm(f'hget http://{HOSTIP}:{HTTP}/{t}.aout > /tmp/{t}.run; chmod 755 /tmp/{t}.run\n',30); time.sleep(0.2)
                        outp=vm(f'/tmp/{t}.run\n',20)
                        ok_run = 'PASS' in outp
        run[t]=ok_run
        print(f'  {t:22} compile={"OK" if comp.get(t) else "FAIL":4} run={"PASS" if ok_run else "----"}')

    nc=sum(1 for t in applicable if comp.get(t)); nr=sum(1 for t in applicable if run.get(t))
    print(f'\n=== RESULTS (native cc9 clang on 9front) ===')
    print(f'applicable (host-clang accepts): {len(applicable)}/{len(tests)}')
    print(f'native COMPILE: {nc}/{len(applicable)}  ({100*nc//max(1,len(applicable))}%)')
    print(f'native RUN:     {nr}/{len(applicable)}  ({100*nr//max(1,len(applicable))}%)')

if __name__=='__main__': main()
