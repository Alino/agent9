#!/usr/bin/env python3
# Run the WHOLE suite through the FULLY ON-BOX toolchain (cc.rc driver = native clang
# + native ld.lld + elf2aout, all on 9front) with normal command lines. The only host
# involvement is staging the .cpp and reading results over the listener.
import socket,time,os,subprocess,glob
VM=('127.0.0.1',1717); HOSTIP='10.0.2.2'; HTTP='8099'
CC9=os.path.abspath(os.path.join(os.path.dirname(__file__),'..','..'))
SEL=[l.strip() for l in open('/tmp/sel.txt') if l.strip()]
def vm(cmd,wait=200):
    s=socket.create_connection(VM,timeout=8); s.sendall(cmd.encode()); s.shutdown(socket.SHUT_WR); s.settimeout(wait)
    out=b''
    try:
        while True:
            b=s.recv(8192)
            if not b: break
            out+=b
    except socket.timeout: pass
    s.close(); return out.decode('latin-1')
def collect():
    t=[]
    for f in sorted(glob.glob(os.path.join(CC9,'test/native_suite/*.cpp'))): t.append(('nat',os.path.basename(f)[:-4],f))
    for f in sorted(glob.glob(os.path.join(CC9,'test/stl_suite/*.cpp'))):    t.append(('stl',os.path.basename(f)[:-4],f))
    for p in SEL:
        full=os.path.expanduser('~/Projects/llvm-project/'+p)
        nm='llt_'+os.path.basename(os.path.dirname(p))+'_'+os.path.basename(p)[:-4]; nm=nm.replace('.','_')
        t.append(('llvm',nm,full))
    return t
def main():
    tests=collect(); npass=0; results=[]
    for kind,name,full in tests:
        subprocess.run(['cp',full,f'/tmp/{name}.cpp'],check=True)
        vm(f'hget http://{HOSTIP}:{HTTP}/{name}.cpp > /tmp/{name}.cpp\n',30); time.sleep(0.1)
        vm(f'rm -f /tmp/{name}.out /tmp/{name}.o /tmp/{name}.elf\n'); time.sleep(0.15)
        vm(f'rc /tmp/cc.rc /tmp/{name}.cpp -o /tmp/{name}.out >[2]/tmp/{name}.berr\n',200); time.sleep(0.3)
        built = '/tmp/'+name+'.out' in vm(f'ls -l /tmp/{name}.out\n')
        outp = vm(f'/tmp/{name}.out; echo CC9DONE\n',25) if built else ''
        ok = built and ('CC9DONE' in outp) and ('cc9 assert' not in outp) and ('cc9:' not in outp) and ('FAIL' not in outp)
        # native/stl tests print PASS explicitly; llvm tests are silent-on-success
        if kind in ('nat','stl'): ok = ok and ('PASS' in outp)
        npass += 1 if ok else 0
        results.append((kind,name,'PASS' if ok else ('build-fail' if not built else 'run-fail')))
        print(f'  [{kind}] {name:34} {"PASS" if ok else "FAIL"}')
    print(f'\n=== FULLY ON-BOX (cc driver: native clang+lld+elf2aout on 9front) ===')
    print(f'TOTAL: {npass}/{len(tests)} pass')
    for k in ('nat','stl','llvm'):
        n=[r for r in results if r[0]==k]; p=sum(1 for r in n if r[2]=='PASS')
        print(f'  {k}: {p}/{len(n)}')
if __name__=='__main__': main()
