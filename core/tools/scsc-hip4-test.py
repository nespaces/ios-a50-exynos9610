#!/usr/bin/env python3
"""Exercise the actual diagnostic RX parser with hostile shared-memory descriptors.
Requires a host C compiler with AddressSanitizer and UndefinedBehaviorSanitizer.
"""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1] / 'drivers/wifi/hakios.scsc.c').read_text()
a=s.index('static uint8_t hip_rx_data');b=s.index('static void\nhip_probe',a)
c='''#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t;
#define HIP_RX_OFFSET 0x100
#define HIP_RX_SIZE 0x10000
static uint32_t hip_ref;
static uint16_t hip_u16(const uint8_t *p) {return p[0]|(uint16_t)p[1]<<8;}
'''+s[a:b]+r'''
static uint8_t memory[0x10100];
static void put16(unsigned p,unsigned v){memory[p]=v;memory[p+1]=v>>8;}
static void node(unsigned p,unsigned flags,unsigned sig,unsigned head,unsigned len,unsigned next){
 memset(memory+p,0,128);memory[p+4]=flags;put16(p+10,sig);put16(p+14,head);put16(p+12,len);put16(p+16,next);put16(p+18,next>>16);
}
static bool collect(unsigned ref){uint32_t refs[16];unsigned n,sl,dl;const uint8_t *sig;return hip_rx_collect((uintptr_t)memory,ref,refs,&n,&sig,&sl,&dl);}
int main(void){
 node(0x100,0xc5,62,64,0,0);assert(collect(0x100));
 node(0x100,0xc5,62,64,0,0x200);node(0x200,0,0,0,5,0);memcpy(memory+0x214,"hello",5);assert(collect(0x100));assert(!memcmp(hip_rx_data,"hello",5));
 node(0x200,0x80,0,0,5,0x100);assert(!collect(0x100));
 assert(!collect(0));assert(!collect(0x101));assert(!collect(0x100ff));
 node(0x100,1,13,13,0,0);assert(!collect(0x100));
 node(0x100,1,14,65535,2,0);assert(!collect(0x100));
 node(0x100,1,14,14,8193,0);assert(!collect(0x100));
 for(unsigned i=0;i<17;i++)node(0x100+128*i,i?0x80:0xc1,i?0:14,14,0,i==16?0:0x180+128*i);
 assert(!collect(0x100));puts("HIP4 RX: signal-only padding, chained payload, cycle, invalid refs, truncated signal, bounds, size and chain limits passed");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(c)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
