#!/usr/bin/env python3
"""Run the real HLE attribute and taskset creation path against a synthetic VM.

A taskset or attribute span the host cannot back ends in the named stop
`hle-guest-range`, never a fabricated task code (libsre 4.93 builds no
0x8041090D). The stop cases run in child processes, because a stop is terminal;
the driver checks both the children's exit status and the machine lines.
"""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone fixture requires POSIX C toolchain')
    raise SystemExit(0)
FIXTURE = r'''
#include "libs/spurs/cellSpurs.c"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
/* No SPU runs in this layout fixture; observer-only services are inert. */
uint32_t g_ww_lo, g_ww_hi;
int spu_coh_is_reserved(uint32_t ea) {(void)ea; return 0;}
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width) {(void)ea;(void)value;(void)width;}
static _Alignas(128) uint8_t memory[0x10000];
#define PTR(type, ea) ((type*)(uintptr_t)(ea))
static void expect(int ok, const char* what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); abort(); }
}
static void clean(uint32_t size) {
    for (uint32_t i=0;i<size;i++)
        if (i<0x60 || (i>=0x80 && i<0x1890) || i>=0x18A0)
            expect(memory[0x4000+i]==0,"requested storage is cleared");
    expect(memory[0x3FFF]==0xA5 && memory[0x4000+size]==0xA5,
           "taskset creation preserves both canaries");
}
CellSpursTasksetAttribute* attr;
CellSpursTaskset* ts;
CellSpurs* spurs;
/* Run one stop case in a child (a stop ends the process) and require exit 70. */
static void expect_stop(void (*fn)(void), const char* what) {
    fflush(stdout);                        /* the child must not flush our buffer */
    pid_t p = fork();
    expect(p >= 0, "fork");
    if (p == 0) { fn(); _exit(0); }        /* reaching here means no stop was raised */
    int st = 0;
    expect(waitpid(p, &st, 0) == p, "waitpid");
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 70)) {
        fprintf(stderr, "FAIL: %s did not stop (status 0x%X)\n", what, st);
        abort();
    }
    printf("PASS: %s stops by name\n", what);
}
static void stop_setter_unbacked(void) {
    (void)cellSpursTasksetAttributeSetTasksetSize(PTR(CellSpursTasksetAttribute,0x10000),0x2900);
}
static void stop_partial_attribute(void) {
    ppu_vm_size = sizeof(memory);
    (void)cellSpursCreateTasksetWithAttribute(spurs,ts,PTR(CellSpursTasksetAttribute,0xFFF8));
}
static void stop_partial_taskset(void) {
    /* The extended size, requested while the whole VM is backed; then the
     * backing shrinks below 0x4000 + 0x2900. */
    ppu_vm_size = sizeof(memory);
    expect(cellSpursTasksetAttributeSetTasksetSize(attr,0x2900)==0,"child size request");
    ppu_vm_size = 0x6800;
    (void)cellSpursCreateTasksetWithAttribute(spurs,ts,attr);
}
int main(void) {
    vm_base=memory; ppu_vm_size=sizeof(memory); s_inst[0].ea=0x1000;
    attr=PTR(CellSpursTasksetAttribute,0x2000);
    ts=PTR(CellSpursTaskset,0x4000);
    spurs=PTR(CellSpurs,0x1000);
    memset(memory,0xA5,sizeof(memory));
    expect(_cellSpursTasksetAttributeInitialize(attr,1,0,0,0,0)==0,"attribute init");
    expect(cellSpursTasksetAttributeSetTasksetSize(attr,0x2900)==0,"size request");
    expect(cellSpursCreateTasksetWithAttribute(spurs,ts,attr)==0,"extended creation");
    expect(vm_read32(0x4000+CSTS_SIZE_FIELD)==0x2900,"requested size retained in guest header");
    clean(0x2900);
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(NULL,0)==0x80410911u,"null precedence");
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(PTR(CellSpursTasksetAttribute,0x2001),0)==0x80410910u,"alignment precedence");
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(attr,0x2000)==0x80410902u,"unsupported size");
    expect(cellSpursCreateTasksetWithAttribute(spurs,ts,attr)==0 && vm_read32(0x4000+CSTS_SIZE_FIELD)==0x2900,"failed setter preserves size");
    expect((uint32_t)cellSpursTasksetAttributeSetTasksetSize(PTR(CellSpursTasksetAttribute,0x10000),0x2000)==0x80410902u,"invalid size precedes backing");
    expect((uint32_t)cellSpursCreateTasksetWithAttribute(spurs,ts,PTR(CellSpursTasksetAttribute,0x2001))==0x80410910u,"creation attribute alignment");
    /* A zero-size attribute is not the HLE's to reject: only the setter checks
     * the size. The header is still written, so the default span must be backed. */
    memset(memory+0x3FFF,0xA5,0x2902);
    expect(cellSpursTasksetAttributeInitialize(attr)==0,"public attribute reinit");
    expect(cellSpursCreateTasksetWithAttribute(spurs,ts,attr)==0,"default attribute creation");
    expect(vm_read32(0x4000+CSTS_SIZE_FIELD)==0x1900,"reinit restores default size");
    clean(0x1900);
    memset(memory+0x3FFF,0xA5,0x2902);
    expect(cellSpursCreateTaskset(spurs,ts,0,NULL,0)==0,"plain creation");
    expect(vm_read32(0x4000+CSTS_SIZE_FIELD)==0x1900,"plain creation uses full default guest size");
    clean(0x1900);

    /* Span failures: hle-guest-range, in a child each (a stop is terminal). */
    ppu_vm_size=sizeof(memory);
    expect_stop(stop_setter_unbacked,"an unbacked size setter");
    expect_stop(stop_partial_attribute,"a partial attribute span");
    expect_stop(stop_partial_taskset,"a partial taskset span");
    puts("PASS: real taskset requested-size retention and initialization");
}
'''
with tempfile.TemporaryDirectory(prefix='spurs-taskset-size-') as name:
    tmp=Path(name); fixture=tmp/'fixture.c'; fixture.write_text(FIXTURE)
    cflags=['-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',
            '-I'+str(ROOT),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime/spu'),
            '-I'+str(ROOT/'runtime/platform')]
    objects=[]
    for index,src in enumerate((fixture, ROOT/'libs/spurs/spurs_taskset.c')):
        obj=tmp/f'{index}.o'
        subprocess.run([os.environ.get('CC','cc'),*cflags,'-c',str(src),'-o',str(obj)],check=True)
        objects.append(str(obj))
    stop_obj=tmp/'stop.o'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-UNDEBUG','-ffunction-sections',
                    '-fdata-sections','-I'+str(ROOT/'include'),'-c',
                    str(ROOT/'runtime/diag/stop.cpp'),'-o',str(stop_obj)],check=True)
    objects.append(str(stop_obj))
    binary=tmp/'fixture'
    linker=['-Wl,-dead_strip'] if platform.system()=='Darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CXX','c++'),*objects,*linker,'-pthread','-lm','-o',str(binary)],check=True)
    p=subprocess.run([str(binary)],check=True,timeout=20,cwd=tmp,capture_output=True,text=True)
    lines=[l for l in p.stderr.splitlines() if l.startswith('PS3STOP ')]
    print(p.stdout,end='')
    assert len(lines)==3, p.stderr[-3000:]
    assert all('kind=hle-guest-range' in l for l in lines), lines
    for func,arg in (("cellSpursTasksetAttributeSetTasksetSize","attr"),
                     ("cellSpursCreateTasksetWithAttribute","attr"),
                     ("cellSpursCreateTasksetWithAttribute","taskset")):
        assert any(f'func={func}' in l and f'arg={arg}' in l for l in lines), (func,arg,lines)
