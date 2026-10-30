/* Host-only injected read engine: real recorder and public device controls. */
#include "internal.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int kind(const char *s) { const char *p=getenv("MOCK_CASE"); return p&&!strcmp(p,s); }
static void event(const char *s) {
    FILE *f=fopen(getenv("MOCK_EVENTS"),"a"); if(!f) abort();
    fprintf(f,"%s\n",s); fclose(f);
}
static void pause_ms(unsigned ms) {
    struct timespec t={ms/1000,(long)(ms%1000)*1000000};
    while(nanosleep(&t,&t)&&errno==EINTR) {}
}
static int validate(uint64_t hz,uint32_t r,unsigned c,const struct birriasdr_gain *g) {
    (void)hz;(void)c;
    if(kind("setup")) {event("SETUP_BEGIN");pause_ms(100);}
    return (r!=40000000&&r!=80000000)||g->lna>5||g->lpf>8||g->digital>12 ? BIRRIASDR_ERROR_INVALID:0;
}
static int limits(uint64_t h,unsigned c,struct birriasdr_gain *g) {
    (void)h;(void)c;*g=(struct birriasdr_gain){5,8,12};return 0;
}
static int reset(birriasdr_dev_t *d) {(void)d;event("RESET");return 0;}
static const struct bs_backend fake={.name="host-only",.capabilities=BIRRIASDR_CAP_RX|BIRRIASDR_CAP_RAW_GAIN|BIRRIASDR_CAP_TUNING|BIRRIASDR_CAP_CHAIN_SELECT,
    .default_frequency=2437000000ULL,.min_frequency=2190000000ULL,.max_frequency=2610000000ULL,
    .default_rate=40000000,.default_bandwidth=40000000,.validate=validate,.gain_limits=limits,.reset=reset};
int bs_usb_discover(uint32_t n,struct birriasdr_device_info *i,birriasdr_dev_t *d) {
    if(n) return BIRRIASDR_ERROR_INVALID;
    if(i) {*i=(struct birriasdr_device_info){.capabilities=fake.capabilities};}
    if(d) {
        if(kind("open")) {event("OPEN_BEGIN");pause_ms(100);}
        d->backend=&fake;atomic_store(&d->frequency,fake.default_frequency);d->rate=40000000;
    }
    return d||i?0:1;
}
void bs_usb_close(birriasdr_dev_t *d) {(void)d;event("CLOSE");}
void bs_sleep_us(unsigned us) {pause_ms((us+999)/1000);}
// The linker wrapper injects watcher failure or a signal at thread creation.
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p) {
    if(kind("watch-fail")) {event("WATCH_FAIL");return EAGAIN;}
    if(kind("before-read")) {event("BEFORE_READ");raise(atoi(getenv("MOCK_SIGNAL")));}
    return __real_pthread_create(t,a,f,p);
}
// Count STATE retries before the read claims ownership; detect any use after close.
int __real_birriasdr_cancel_async(birriasdr_dev_t *);
int __wrap_birriasdr_cancel_async(birriasdr_dev_t *d) {
    int r=__real_birriasdr_cancel_async(d);event(r?"CANCEL_STATE":"CANCEL_OK");return r;
}
int bs_stream(birriasdr_dev_t *d,birriasdr_read_async_cb_t cb,void *ctx,uint32_t depth,uint32_t size,uint32_t packets) {
    (void)depth;(void)size;
    if(kind("before-read")) pause_ms(80); // watcher must retry STATE, not give up
    atomic_store(&d->run_state,BS_RUNNING);event("READ_BEGIN");
    static int16_t iq[4096]; for(unsigned i=0;i<4096;++i) iq[i]=(int16_t)(i%31-15);
    unsigned sent=0;
    while(atomic_load(&d->run_state)!=BS_CANCELLING) {
        if(kind("callback")||kind("finite")||kind("recover")) {
            if(kind("recover")&&sent==1&&d->recovery_callback) {
                struct birriasdr_recovery_event e={.next_sample=4096,.attempt=1,.error=BIRRIASDR_ERROR_IO};
                d->recovery_callback(&e,d->recovery_context);event("GAP");
            }
            cb(iq,2048,(uint64_t)sent*2048,ctx);++sent;event("CALLBACK");
            if(packets&&sent>=packets) break;
        }
        pause_ms(5);
    }
    event("CLEANUP");atomic_store(&d->run_state,BS_IDLE);return 0;
}

int __real_pthread_join(pthread_t,void **);
int __wrap_pthread_join(pthread_t t,void **r) {
    int result=__real_pthread_join(t,r);event("WATCH_JOIN");return result;
}
