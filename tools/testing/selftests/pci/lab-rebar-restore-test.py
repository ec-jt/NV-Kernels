#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Exercise the actual PF/VF restore functions with failed PCI config reads.
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[4]
def function(path,name):
 s=(root/path).read_text();a=s.index(name+'(');a=s.rfind('\n',0,a)+1;b=s.index('{',a);depth=1;i=b+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
code=r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t resource_size_t;
#define PCI_STD_NUM_BARS 6
#define PCI_SRIOV_NUM_BARS 6
#define PCI_REBAR_CTRL 8
#define PCI_VF_REBAR_CTRL 8
#define PCI_REBAR_CTRL_NBAR_MASK 0xe0
#define PCI_VF_REBAR_CTRL_NBAR_MASK 0xe0
#define PCI_REBAR_CTRL_BAR_IDX 7
#define PCI_VF_REBAR_CTRL_BAR_IDX 7
#define PCI_REBAR_CTRL_BAR_SIZE 0x3f00
#define PCI_VF_REBAR_CTRL_BAR_SIZE 0x3f00
#define IORESOURCE_MEM 0x200
#define PCI_POSSIBLE_ERROR(v) ((v)==UINT32_MAX)
#define FIELD_GET(m,v) (((v)&(m))>>__builtin_ctz(m))
#define FIELD_PREP(m,v) (((v)<<__builtin_ctz(m))&(m))
struct resource { uint64_t start,end;unsigned flags; };
struct pci_sriov { uint64_t barsz[6]; };
struct pci_dev { unsigned rebar_cap;struct resource resource[6];struct pci_sriov *sriov; };
static u32 input[16],output[16];static int result[16],reads,writes;
static int pci_read_config_dword(struct pci_dev*p,unsigned pos,u32*v){assert(reads<16);*v=input[reads];return result[reads++];}
static int pci_write_config_dword(struct pci_dev*p,unsigned pos,u32 v){assert(writes<16);output[writes++]=v;return 0;}
static unsigned pci_iov_vf_rebar_cap(struct pci_dev*p){return p->rebar_cap;}
static struct resource *pci_resource_n(struct pci_dev*p,int n){assert(n>=0&&n<6);return &p->resource[n];}
static uint64_t resource_size(struct resource*r){return r->end-r->start+1;}
static int pci_rebar_bytes_to_size(uint64_t n){assert(n && !(n&(n-1)));return 63-__builtin_clzll(n)-20;}
'''
code+=function('drivers/pci/rebar.c','pci_restore_rebar_state')+'\n'
code+=function('drivers/pci/iov.c','sriov_restore_vf_rebar_state')+'\n'
code+=r'''
static void init(struct pci_dev*p,struct pci_sriov*i){memset(p,0,sizeof(*p));memset(i,0,sizeof(*i));memset(result,0,sizeof(result));memset(input,0,sizeof(input));reads=writes=0;p->rebar_cap=0x100;p->sriov=i;for(int n=0;n<6;n++){p->resource[n]=(struct resource){0,(1ULL<<35)-1,IORESOURCE_MEM};i->barsz[n]=1ULL<<35;}}
int main(void){struct pci_dev p;struct pci_sriov i;for(int vf=0;vf<2;vf++){void(*f)(struct pci_dev*)=vf?sriov_restore_vf_rebar_state:pci_restore_rebar_state;
init(&p,&i);input[0]=input[1]=(1<<5)|1|(8<<8);f(&p);assert(writes==1&&FIELD_GET(PCI_REBAR_CTRL_BAR_SIZE,output[0])==15);
init(&p,&i);input[0]=UINT32_MAX;f(&p);assert(!writes);
init(&p,&i);input[0]=(1<<5)|1;input[1]=UINT32_MAX;f(&p);assert(!writes);
init(&p,&i);input[0]=(1<<5)|1;result[0]=-1;f(&p);assert(!writes);
init(&p,&i);input[0]=(1<<5)|1;result[1]=-1;f(&p);assert(!writes);
for(int n=6;n<=7;n++){init(&p,&i);input[0]=input[1]=(1<<5)|n;f(&p);assert(!writes);}
init(&p,&i);input[0]=(7<<5)|1;f(&p);assert(!writes);
init(&p,&i);input[0]=input[1]=(1<<5)|1;i.barsz[1]=0;p.resource[1].flags=0;f(&p);assert(!writes);
init(&p,&i);input[0]=input[1]=(2<<5)|0;input[2]=2;f(&p);assert(writes==2);
}puts("PASS: PF/VF valid restoration, failed reads, all-ones reads, invalid indices/counts, empty resources");}
'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'test.c';exe=Path(d)/'test';c.write_text(code)
 subprocess.run(['cc','-std=c11','-fsanitize=undefined,address','-g',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
