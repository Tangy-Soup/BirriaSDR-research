/* Host-only injected backend. Real device.c controls; no USB implementation linked. */
#include "chips/mt7921.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
static atomic_int calls, fail_at, recoveries, running;
static birriasdr_dev_t *last;
static int validate(uint64_t hz,uint32_t rate,unsigned chain,const struct birriasdr_gain *g){
 (void)hz; (void)chain; return (rate!=40000000 && rate!=80000000) ? BIRRIASDR_ERROR_UNSUPPORTED : mt_validate_gain(hz,chain,g);
}

static int gain(birriasdr_dev_t *d,bool manual,const struct birriasdr_gain *g){
 (void)d;(void)manual;(void)g;int n=atomic_fetch_add(&calls,1)+1;return n==atomic_load(&fail_at)?BIRRIASDR_ERROR_IO:0;
}
static int bw(uint32_t n){return n==20000000||n==40000000?0:BIRRIASDR_ERROR_INVALID;}
static int reset(birriasdr_dev_t *d){(void)d;return 0;}
static const struct bs_backend fake={.name="host-only",.capabilities=BIRRIASDR_CAP_GAIN_DB|BIRRIASDR_CAP_RX|BIRRIASDR_CAP_CHAIN_SELECT|BIRRIASDR_CAP_RAW_GAIN|BIRRIASDR_CAP_LIVE_GAIN|BIRRIASDR_CAP_TUNING|BIRRIASDR_CAP_LIVE_TUNING|BIRRIASDR_CAP_FILTER,.default_frequency=2437000000ULL,.min_frequency=2190000000ULL,.max_frequency=6200000000ULL,.default_rate=40000000,.default_bandwidth=40000000,.validate_bandwidth=bw,.validate=validate,.change_gain=gain,.gain_limits=mt_gain_limits,.gain_steps=mt_gain_steps,.gain_tenths_db=mt_gain_tenths_db,.reset=reset};
int bs_usb_discover(uint32_t n,struct birriasdr_device_info *info,birriasdr_dev_t *d){
 if(n)return BIRRIASDR_ERROR_INVALID;
 if(info){info->vid=0x0e8d;info->pid=0x7961;info->capabilities=fake.capabilities;snprintf(info->name,sizeof(info->name),"host-only");}
 if(d){d->backend=&fake;atomic_store(&d->frequency,fake.default_frequency);d->rate=40000000;last=d;}
 return d||info?0:1;
}
void bs_usb_close(birriasdr_dev_t *d){(void)d;}
void bs_sleep_us(unsigned us){struct timespec t={.tv_sec=us/1000000,.tv_nsec=(us%1000000)*1000};nanosleep(&t,0);}
int bs_stream(birriasdr_dev_t *d,birriasdr_read_async_cb_t cb,void *ctx,uint32_t depth,uint32_t size,uint32_t packets){
 (void)cb;(void)ctx;(void)depth;(void)size;(void)packets;
 pthread_mutex_lock(&d->control_mutex);atomic_store(&d->run_state,BS_RUNNING);d->control_ready=true;pthread_mutex_unlock(&d->control_mutex);atomic_store(&running,1);
 while(atomic_load(&d->run_state)!=BS_CANCELLING){
  if(atomic_load(&d->run_state)==BS_CONTROL_FAILED){
   /* Model completed reset/recovery; keep the API's actual committed tuple. */
   bs_sleep_us(10000);pthread_mutex_lock(&d->control_mutex);if(atomic_load(&d->run_state)==BS_CONTROL_FAILED){d->control_ready=true;atomic_store(&d->run_state,BS_RUNNING);atomic_fetch_add(&recoveries,1);}pthread_mutex_unlock(&d->control_mutex);
  }
  bs_sleep_us(1000);
 }
 d->control_ready=false;atomic_store(&d->run_state,BS_IDLE);atomic_store(&running,0);return 0;
}
void mock_fail_second_live_gain(void){atomic_store(&calls,0);atomic_store(&fail_at,2);atomic_store(&recoveries,0);}
int mock_recovered(void){return atomic_load(&recoveries);}
int mock_running(void){return atomic_load(&running);}
void mock_committed(int *l,int *p,int *v,int *m){struct birriasdr_gain g;birriasdr_get_gain_codes(last,&g,m);*l=g.lna;*p=g.lpf;*v=g.digital;}

void mock_fail_first_live_gain(void){atomic_store(&calls,0);atomic_store(&fail_at,1);atomic_store(&recoveries,0);}

/* Gain table functions are real; RF/MMIO entry points must never run here. */
int mt_rf_read(birriasdr_dev_t *d,unsigned p,uint32_t a,uint32_t *v){(void)d;(void)p;(void)a;(void)v;abort();}
int mt_rf_update(birriasdr_dev_t *d,unsigned p,uint32_t a,uint32_t m,uint32_t v){(void)d;(void)p;(void)a;(void)m;(void)v;abort();}
int bs_read32(birriasdr_dev_t *d,enum bs_space s,uint32_t a,uint32_t *v){(void)d;(void)s;(void)a;(void)v;abort();}
int bs_update32(birriasdr_dev_t *d,enum bs_space s,uint32_t a,uint32_t m,uint32_t v){(void)d;(void)s;(void)a;(void)m;(void)v;abort();}
