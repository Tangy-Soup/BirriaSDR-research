/* Injected I/O only. Compile actual capture/chain code, never link USB. */
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(t) do {if(!(t)){fprintf(stderr,"line %d: %s\n",__LINE__,#t);exit(1);}}while(0)
struct word {uint32_t address,value;};
struct model {struct word words[32];unsigned count,ops,fail;uint32_t drop,read_fail;};
static uint32_t *word(struct model *m,uint32_t a) {
    for(unsigned i=0;i<m->count;++i) if(m->words[i].address==a) return &m->words[i].value;
    CHECK(m->count<32);m->words[m->count++]=(struct word){a,0};return &m->words[m->count-1].value;
}
static int read32(void *p,enum bs_space s,uint32_t a,uint32_t *v) {
    struct model *m=p;CHECK(s==BS_MMIO);++m->ops;
    if(m->ops==m->fail||a==m->read_fail) return BIRRIASDR_ERROR_IO;
    *v=*word(m,a);return 0;
}
static int write32(void *p,enum bs_space s,uint32_t a,uint32_t v) {
    struct model *m=p;CHECK(s==BS_MMIO);++m->ops;
    if(m->ops==m->fail) return BIRRIASDR_ERROR_IO;
    if(a!=m->drop) *word(m,a)=v;
    return 0;
}
int mt_rf_read(birriasdr_dev_t *d,unsigned port,uint32_t a,uint32_t *v) {
    if(port==MT_TOP_PORT&&a==0x2c){*v=0x79728a10;return 0;}
    CHECK(port==MT_SYNTH_PORT&&a==0x1e4);*v=*word(d->io.context,a);return 0;
}
int mt_rf_update(birriasdr_dev_t *d,unsigned p,uint32_t a,uint32_t mask,uint32_t v) {
    CHECK(p==MT_SYNTH_PORT&&a==0x1e4);uint32_t *x=word(d->io.context,a);*x=(*x&~mask)|v;return 0;
}
static void init(struct model *m,birriasdr_dev_t *d,uint32_t rate,unsigned chain,bool filtered) {
    memset(m,0,sizeof(*m));memset(d,0,sizeof(*d));d->rate=rate;
    atomic_init(&d->receive_chain,chain);atomic_init(&d->channel_filter,filtered);
    d->io=(struct bs_io){m,read32,write32};
    *word(m,0x800210b8)=0x50020000; // stale packer must be cleared
    *word(m,0x830ad448)=rate==MT_RATE_80?0x500c0000:0x50080000;
    *word(m,0x1e4)=0x23c1;
}
int main(void) {
    unsigned cases=0;
    for(unsigned rate=40;rate<=80;rate+=40) for(unsigned c=0;c<=2;++c) for(unsigned f=0;f<2;++f) {
        struct model m;birriasdr_dev_t d;init(&m,&d,rate*1000000,c,f);
        int r=mt_configure_capture(&d);
        if(rate==80&&c==2){CHECK(r==BIRRIASDR_ERROR_UNSUPPORTED&&m.ops==0);continue;}
        CHECK(!r);unsigned ops=m.ops;
        CHECK(!(*word(&m,0x800210b8)&0x20000));
        CHECK((*word(&m,0x830ad448)&0xc0000)==(rate==80?0x80000u:0xc0000u));
        CHECK(!mt_configure_chain(&d));
        if(c==2) {
            CHECK((*word(&m,0x830ad440)&0xff)==0x11);
            CHECK((*word(&m,0x830ad448)&0x033f00ff)==0x0023000c);
        } else CHECK((*word(&m,0x830ad448)&0xf00)==((f?12u:4u)+c)*256);
        CHECK(!mt_restore_chain(&d));
        for(unsigned i=1;i<=ops;++i) {
            init(&m,&d,rate*1000000,c,f);m.fail=i;
            CHECK(mt_configure_capture(&d)==BIRRIASDR_ERROR_IO);
        }
        for(unsigned field=0;field<2;++field) {
            init(&m,&d,rate*1000000,c,f);m.drop=field?0x830ad448:0x800210b8;
            CHECK(mt_configure_capture(&d)==BIRRIASDR_ERROR_STATE);
            CHECK(*word(&m,0x80021090)!=0x4f1); // no configured-ready state
        }
        ++cases;
    }
    puts("PASS capture readback: 10 rate/chain/tap combinations, every I/O failure, ignored packer/rate writes; both-chain 80M refused");
    CHECK(cases==10);
    return 0;
}
