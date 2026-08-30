#include "qemu/osdep.h"
#include "hw/gpio/tricore_port.h"
#include "hw/core/irq.h"
static uint64_t port_read(void *o, hwaddr a, unsigned s) { TriCorePortState *p=o; switch(a){case 0:return p->in;case 4:return p->out_latch;case 8:return p->dir;case 0xc:return p->pdisc;case 0x10:return p->palt;default:return 0;} }
static void port_write(void *o, hwaddr a, uint64_t v, unsigned s) { TriCorePortState *p=o; if(a==4)p->out_latch=v; else if(a==8)p->dir=v; else if(a==0xc)p->pdisc=v; else if(a==0x10)p->palt=v; else return; for(unsigned i=0;i<16;i++) if(p->dir&BIT(i)) qemu_set_irq(p->out[i], !!(p->out_latch&BIT(i))); }
static const MemoryRegionOps ops={.read=port_read,.write=port_write,.endianness=DEVICE_LITTLE_ENDIAN,.valid.min_access_size=4,.valid.max_access_size=4};
static void port_init(Object *o){TriCorePortState*p=TRICORE_PORT(o);memory_region_init_io(&p->iomem,o,&ops,p,"tricore-port",0x100);sysbus_init_mmio(SYS_BUS_DEVICE(o),&p->iomem);for(unsigned i=0;i<16;i++)sysbus_init_irq(SYS_BUS_DEVICE(o),&p->out[i]);}
static const TypeInfo ti={.name=TYPE_TRICORE_PORT,.parent=TYPE_SYS_BUS_DEVICE,.instance_size=sizeof(TriCorePortState),.instance_init=port_init};
static void port_register_types(void) { type_register_static(&ti); }
type_init(port_register_types)
