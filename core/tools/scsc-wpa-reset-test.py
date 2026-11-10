#!/usr/bin/env python3
"""Regression: preserve new WPA credentials across the pre-join VIF reset."""
from pathlib import Path
import tempfile,subprocess
core=Path(__file__).resolve().parents[1]
s=(core/'drivers/wifi/hakios.scsc.c').read_text();a=s.index('static int\nhip_join_vif_reset(');b=s.index('/* NULL when',a)
harness=r'''
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include "lx_wpa.h"
typedef uintptr_t vm_offset_t;
struct hip_control {int unused;};
static struct lx_wpa hip_wpa;
static int fail;
static int memset_s(void *p,size_t n,int c,size_t len){assert(len<=n);volatile unsigned char *v=p;while(len--)*v++=c;return 0;}
/* DEL_VIF's real side effect; the request must survive it only on success. */
static int hip_vif_reset(vm_offset_t a,vm_offset_t b,struct hip_control *h){(void)a;(void)b;(void)h;memset(&hip_wpa,0,sizeof hip_wpa);return fail;}
'''+s[a:b]+r'''
int main(void){
 const unsigned char rsn[22]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
 struct lx_wpa initial={0};
 memset(initial.pmk,0x31,32);memset(initial.own_nonce,0x71,32);
 initial.own[0]=2;initial.own[5]=1;initial.peer[0]=2;initial.peer[5]=2;
 initial.rsn=rsn;initial.rsn_length=sizeof rsn;
 unsigned char m1[99]={2,3,0,95,2,0,0x8a,0,16},out[256];unsigned n;const char *why;
 m1[16]=1;memset(m1+17,0x51,32);
 hip_wpa=initial;hip_vif_reset(0,0,0);
 assert(lx_wpa_message(&hip_wpa,m1,sizeof m1,out,&n,&why)==LX_WPA_ANSWER);assert(n==99);
 hip_wpa=initial;assert(!hip_join_vif_reset(0,0,0));assert(!memcmp(&hip_wpa,&initial,sizeof initial));
 assert(lx_wpa_message(&hip_wpa,m1,sizeof m1,out,&n,&why)==LX_WPA_ANSWER);
 assert(n==121 && out[97]==0 && out[98]==22 && !memcmp(out+99,rsn,22));
 fail=5;hip_wpa=initial;assert(hip_join_vif_reset(0,0,0)==5);
 const unsigned char *p=(void*)&hip_wpa;for(unsigned i=0;i<sizeof hip_wpa;i++)assert(!p[i]);
 puts("PASS: reproduced pre-fix M2=99; fixed M2=121 with RSN and intact credentials; failure wipes context");
}
'''
with tempfile.TemporaryDirectory() as d:
 d=Path(d);(d/'lx_wpa.h').write_text((core/'drivers/wifi/hakios.lx_wpa.h').read_text());(d/'test.c').write_text(harness)
 subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(d),str(d/'test.c'),str(core/'drivers/wifi/hakios.lx_wpa.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
