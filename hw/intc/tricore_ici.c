#include "qemu/osdep.h"
#include "hw/intc/tricore_ici.h"
#include "hw/core/irq.h"
static uint64_t r(void*o,hwaddr a,unsigned z){TriCoreICIState*s=o;switch(a){case 0:return s->pending;case 4:return s->reset_mask;case 8:return s->provider;case 0x10:return s->barrier;default:return 0;}}
static void w(void*o,hwaddr a,uint64_t v,unsigned z){TriCoreICIState*s=o; if(a==0){s->pending|=v&0x3f;for(unsigned i=0;i<6;i++)if(v&BIT(i))qemu_set_irq(s->irq[i],1);}else if(a==4)s->reset_mask=v&0x3f;else if(a==8)s->provider=v&0x3f;else if(a==0xc){s->pending&=~v;for(unsigned i=0;i<6;i++)if(v&BIT(i))qemu_set_irq(s->irq[i],0);}else if(a==0x10)s->barrier=v;}
static const MemoryRegionOps ops={.read=r,.write=w,.endianness=DEVICE_LITTLE_ENDIAN,.valid.min_access_size=4,.valid.max_access_size=4};
static void init(Object*o){TriCoreICIState*s=TRICORE_ICI(o);memory_region_init_io(&s->iomem,o,&ops,s,"tricore-ici",0x100);sysbus_init_mmio(SYS_BUS_DEVICE(o),&s->iomem);for(unsigned i=0;i<6;i++)sysbus_init_irq(SYS_BUS_DEVICE(o),&s->irq[i]);}
static const TypeInfo ti={.name=TYPE_TRICORE_ICI,.parent=TYPE_SYS_BUS_DEVICE,.instance_size=sizeof(TriCoreICIState),.instance_init=init};static void reg(void){type_register_static(&ti);}type_init(reg)
