/* sys_memory containers take their memory out of the default pool, as LV2's do. Manufactured; no title code or data.
 *
 * Creating a container did not charge the default pool, so the free size a title read back for it did not drop;
 * allocations inside the container were also checked against the default pool, so a full default pool refused them.
 * A container with live allocations could be destroyed and its slot reused, and freeing the old allocation then
 * underflowed the new container's used_size.
 *
 * The console golden (tests/syscalls/lv2conf/golden.csv, container.*) adds: a container is a whole number of MiB
 * (smaller is CELL_ENOMEM, larger rounds down, the 64-bit size is judged whole), and the default pool's total as
 * well as its available amount drops by a container's size. */
#include "../runtime/syscalls/sys_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guest_range_catch.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base=0x20000000u;
static lv2_syscall_table table;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
#define MB (1024u*1024u)
#define USER_TOTAL (213u*MB)
#define DEFAULT_CID 0xFFFFFFFFu
#define OUT 0x10000u
static uint32_t rd(uint32_t a) { uint8_t* p=vm_base+a; return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static int64_t create64(uint64_t size) { ppu_context c={0};c.gpr[3]=OUT;c.gpr[4]=size;return sys_memory_container_create(&c); }
static int64_t create(uint32_t size) { return create64(size); }
static int64_t destroy(uint32_t cid) { ppu_context c={0};c.gpr[3]=cid;return sys_memory_container_destroy(&c); }
static int64_t alloc(uint32_t size) { ppu_context c={0};c.gpr[3]=size;c.gpr[4]=0x400;c.gpr[5]=OUT;return sys_memory_allocate(&c); }
static int64_t alloc_in(uint32_t cid,uint32_t size) {
 ppu_context c={0};c.gpr[3]=size;c.gpr[4]=cid;c.gpr[5]=0x400;c.gpr[6]=OUT;return sys_memory_allocate_from_container(&c);
}
static int64_t release(uint32_t addr) { ppu_context c={0};c.gpr[3]=addr;return sys_memory_free(&c); }
/* {total, available} of a container, or of the default pool. */
static int64_t size_of(uint32_t cid,uint32_t* total,uint32_t* avail) {
 ppu_context c={0};c.gpr[3]=OUT+8;c.gpr[4]=cid;int64_t rc=sys_memory_container_get_size(&c);
 *total=rd(OUT+8);*avail=rd(OUT+12);return rc;
}
/* The same pair, through sys_memory_get_user_memory_size (syscall 352). */
static int64_t user_memory(uint32_t* total,uint32_t* avail) {
 ppu_context c={0};c.gpr[3]=OUT+8;int64_t rc=sys_memory_get_user_memory_size(&c);
 *total=rd(OUT+8);*avail=rd(OUT+12);return rc;
}
/* sys_memory_allocate_from_container on other threads while the container is destroyed and created again: the
 * container is looked at under the allocator lock, so an allocation either lands in a live container (and holds
 * its destroy off with CELL_EBUSY until it is freed) or answers CELL_ESRCH, and the pool balances afterwards. Run
 * under TSan by the tsan preset (SANITIZERS=thread). Each thread has its own out word.
 * The container is published for a while and withdrawn before its destroy, and a racer starts no allocation
 * without one: allocations that started before the withdrawal still race the destroy, but the destroy is not
 * starved. Racers that never stopped allocating kept some allocation live almost all the time, and on a 3-core
 * CI runner the destroy's CELL_EBUSY retries outlasted the driver's timeout. */
typedef struct { uint32_t out; unsigned ok, esrch, other; } racer;
static atomic_uint s_race_cid;
static atomic_int s_race_stop;
static atomic_uint s_race_tries;   /* allocation calls the racers have started, all rounds */
static void* alloc_racer(void* p) {
 racer* r=(racer*)p;
 while (!atomic_load_explicit(&s_race_stop,memory_order_acquire)) {
   const uint32_t cid=atomic_load_explicit(&s_race_cid,memory_order_acquire);
   if (!cid) { sched_yield(); continue; }
   atomic_fetch_add_explicit(&s_race_tries,1,memory_order_relaxed);
   ppu_context c={0};c.gpr[3]=MB;c.gpr[4]=cid;c.gpr[5]=0x400;
   c.gpr[6]=r->out;
   const int64_t rc=sys_memory_allocate_from_container(&c);
   if (rc==CELL_OK) { ++r->ok; if (release(rd(r->out))!=CELL_OK) ++r->other; }
   else if (rc==(int64_t)(int32_t)CELL_ESRCH) ++r->esrch;
   else ++r->other;
 }
 return NULL;
}
static int race_allocate_against_destroy(void) {
 enum { THREADS=3, ROUNDS=400 };
 pthread_t th[THREADS]; racer rs[THREADS];
 atomic_store(&s_race_cid,0); atomic_store(&s_race_stop,0);
 for (unsigned i=0;i<THREADS;++i) { memset(&rs[i],0,sizeof rs[i]); rs[i].out=OUT+0x100+16*i;
   CHECK(pthread_create(&th[i],NULL,alloc_racer,&rs[i])==0); }
 int bad=0;
 for (unsigned round=0;round<ROUNDS&&!bad;++round) {
   if (create(4*MB)!=CELL_OK) { bad=1; break; }
   const uint32_t cid=rd(OUT);
   const unsigned tries=atomic_load_explicit(&s_race_tries,memory_order_relaxed);
   atomic_store_explicit(&s_race_cid,cid,memory_order_release);
   /* Published until a racer has started an allocation (bounded), so the round races. One sched_yield could pass
    * with no racer scheduled, and the case then raced nothing and still passed. */
   for (unsigned spin=0;spin<100000&&atomic_load_explicit(&s_race_tries,memory_order_relaxed)==tries;++spin)
     sched_yield();
   atomic_store_explicit(&s_race_cid,0,memory_order_release);   /* no new allocation; those in flight race on */
   int64_t rc;
   while ((rc=destroy(cid))==(int64_t)(int32_t)CELL_EBUSY) sched_yield();
   if (rc!=CELL_OK) bad=1;
 }
 atomic_store_explicit(&s_race_stop,1,memory_order_release);
 unsigned other=0,ok=0,esrch=0;
 for (unsigned i=0;i<THREADS;++i) { pthread_join(th[i],NULL); other+=rs[i].other; ok+=rs[i].ok; esrch+=rs[i].esrch; }
 printf("allocate vs destroy: %u rounds, %u allocations, %u ESRCH\n",(unsigned)ROUNDS,ok,esrch);
 CHECK(!bad); CHECK(other==0);
 CHECK(ok+esrch>0);   /* some allocation met a live or a destroyed container: the case raced */
 uint32_t total,avail;
 CHECK(user_memory(&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL&&avail==USER_TOTAL);
 return 0;
}
int main(void) {
 CHECK(vm_init()==CELL_OK);
 sys_memory_init(&table);
 uint32_t total,avail;

 /* Creating a container takes its size out of the default pool. */
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL&&avail==USER_TOTAL);
 CHECK(create(16*MB)==CELL_OK); uint32_t a=rd(OUT); CHECK(a!=0);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==USER_TOTAL-16*MB);
 { ppu_context c={0};c.gpr[3]=OUT+8;CHECK(sys_memory_get_user_memory_size(&c)==CELL_OK);CHECK(rd(OUT+8)==USER_TOTAL-16*MB&&rd(OUT+12)==USER_TOTAL-16*MB); }
 CHECK(size_of(a,&total,&avail)==CELL_OK); CHECK(total==16*MB&&avail==16*MB);

 /* An allocation in the container is charged to it alone, not a second time to the default pool. */
 CHECK(alloc_in(a,4*MB)==CELL_OK); uint32_t in_a=rd(OUT);
 CHECK(size_of(a,&total,&avail)==CELL_OK); CHECK(avail==12*MB);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==USER_TOTAL-16*MB);

 /* With the default pool used up the container still has its own memory to hand out. */
 CHECK(alloc(USER_TOTAL-16*MB)==CELL_OK); uint32_t big=rd(OUT);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==0);
 CHECK(alloc_in(a,4*MB)==CELL_OK); uint32_t in_a2=rd(OUT);
 CHECK(alloc_in(a,8*MB+1)==(int64_t)(int32_t)CELL_ENOMEM);   /* but no more than its own */
 /* And a container the default pool cannot cover is refused. */
 CHECK(create(MB)==(int64_t)(int32_t)CELL_ENOMEM);
 CHECK(release(big)==CELL_OK);

 /* A container with live allocations cannot be destroyed, so its slot is not handed to another. */
 CHECK(destroy(a)==(int64_t)(int32_t)CELL_EBUSY);
 CHECK(size_of(a,&total,&avail)==CELL_OK); CHECK(total==16*MB&&avail==8*MB);
 CHECK(release(in_a)==CELL_OK); CHECK(release(in_a2)==CELL_OK);
 CHECK(size_of(a,&total,&avail)==CELL_OK); CHECK(avail==16*MB);

 /* Once empty it goes, and its memory goes back to the default pool. */
 CHECK(destroy(a)==CELL_OK);
 CHECK(size_of(a,&total,&avail)==(int64_t)(int32_t)CELL_ESRCH);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==USER_TOTAL);

 /* The slot's next container starts with nothing used. */
 CHECK(create(8*MB)==CELL_OK); uint32_t b=rd(OUT); CHECK(b==a);
 CHECK(size_of(b,&total,&avail)==CELL_OK); CHECK(total==8*MB&&avail==8*MB);
 CHECK(destroy(b)==CELL_OK);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(avail==USER_TOTAL);

 /* The default pool's total is the user memory less its containers: it drops with a container's size and comes
  * back with it, next to the available amount (golden container.changes_user_memory). */
 CHECK(user_memory(&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL&&avail==USER_TOTAL);
 CHECK(create(4*MB)==CELL_OK); uint32_t c4=rd(OUT);
 CHECK(user_memory(&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL-4*MB&&avail==USER_TOTAL-4*MB);
 CHECK(size_of(DEFAULT_CID,&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL-4*MB&&avail==USER_TOTAL-4*MB);
 CHECK(destroy(c4)==CELL_OK);
 CHECK(user_memory(&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL&&avail==USER_TOTAL);

 /* A size below 1 MiB, 0 included, is nothing to create (CELL_ENOMEM): no id is written and nothing is taken
  * (golden container.create_sizes). */
 { const uint64_t small[]={0,1,MB-1};
   for (unsigned i=0;i<3;++i) {
     memset(vm_base+OUT,0x5A,4);
     CHECK(create64(small[i])==(int64_t)(int32_t)CELL_ENOMEM); CHECK(rd(OUT)==0x5A5A5A5Au);
     CHECK(user_memory(&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL&&avail==USER_TOTAL);
   } }

 /* An accepted size rounds down to a whole number of MiB, and that is what the container holds and costs. */
 { const uint64_t in[]={MB+1,MB+MB/2,2*MB+1}; const uint32_t out[]={MB,MB,2*MB};
   for (unsigned i=0;i<3;++i) {
     CHECK(create64(in[i])==CELL_OK); uint32_t id=rd(OUT);
     CHECK(size_of(id,&total,&avail)==CELL_OK); CHECK(total==out[i]&&avail==out[i]);
     CHECK(user_memory(&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL-out[i]&&avail==USER_TOTAL-out[i]);
     CHECK(destroy(id)==CELL_OK);
   } }

 /* More than the pool holds is CELL_ENOMEM, and the 64-bit size is judged whole: 4 GiB + 1 MiB is not 1 MiB. */
 CHECK(create64(1ull<<30)==(int64_t)(int32_t)CELL_ENOMEM);
 CHECK(create64(1ull<<32)==(int64_t)(int32_t)CELL_ENOMEM);
 CHECK(create64((1ull<<32)+MB)==(int64_t)(int32_t)CELL_ENOMEM);
 CHECK(user_memory(&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL&&avail==USER_TOTAL);

 /* With every slot taken a size the pool can cover is CELL_EAGAIN, but one it cannot (or 0) is CELL_ENOMEM first
  * (RPCS3 6e4b784d1 sys_memory.cpp:436-460; the golden is silent). */
 { uint32_t ids[SYS_MEMORY_CONTAINER_MAX];
   for (unsigned i=0;i<SYS_MEMORY_CONTAINER_MAX;++i) { CHECK(create(MB)==CELL_OK); ids[i]=rd(OUT); }
   CHECK(create(MB)==(int64_t)(int32_t)CELL_EAGAIN);
   CHECK(create64(1ull<<30)==(int64_t)(int32_t)CELL_ENOMEM);
   CHECK(create64(0)==(int64_t)(int32_t)CELL_ENOMEM);
   for (unsigned i=0;i<SYS_MEMORY_CONTAINER_MAX;++i) CHECK(destroy(ids[i])==CELL_OK);
 }
 CHECK(user_memory(&total,&avail)==CELL_OK); CHECK(total==USER_TOTAL&&avail==USER_TOTAL);

 /* Allocation in a container racing its destroy. */
 CHECK(race_allocate_against_destroy()==0);

 /* The out pointer of the size query is a span the title chose: outside guest memory is the named stop. */
 { ppu_context c={0}; c.gpr[3]=0xFFFFFFFCu;
   GR_TRY(sys_memory_get_user_memory_size(&c));
   CHECK(gr_named("sys_memory_get_user_memory_size","info")); }

 vm_shutdown();printf("PASS %u memory container checks\n",checks);return 0;
}
