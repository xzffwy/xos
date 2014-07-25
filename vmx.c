#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/sched.h>
#include <linux/ftrace_event.h>
#include <linux/slab.h>
#include <linux/tboot.h>
#include <linux/smp.h>
#include <linux/percpu.h>
#include <linux/syscalls.h>
#include <linux/version.h>

#include <asm/desc.h>
#include <asm/unistd_64.h>
#include <asm/vmx.h>
#include <asm/virtext.h>
#include <asm/i387.h>

#include "vmx.h"

#define PRINT_ERR(x)  printk(KERN_ERR "%s\n",x)
static DEFINE_PER_CPU(struct vmcs *,vmxarea);
static unsigned long *msr_bitmap;
static DECLARE_BITMAP(vmx_vpid_bitmap,VMX_NR_VPIDS);
//unsigned long vmx_vpid_bitmap[vmx_NR_VPIDS/8+1];

bool msr_true;
struct vmx_capability vmx_capability;
static struct vmcs_config{
	int size;
	int order;
	u32 revision_id;
	u32 pin_based_exec_ctrl;
	u32 cpu_based_exec_ctrl;
	u32 cpu_based_2nd_exec_ctrl;
	u32 vmexit_ctrl;
	u32 vmentry_ctrl;
}*vmcs_conf;


static inline bool cpu_has_vmx_vpid(void ){
	return vmcs_conf->cpu_based_2nd_exec_ctrl &
		SECONDARY_EXEC_ENABLE_VPID;

}

static inline bool cpu_has_vmx_ept(void){
	return vmcs_conf->cpu_based_2nd_exec_ctrl &
		SECONDARY_EXEC_ENABLE_VPID;
}

static struct vmcs *__vmx_alloc_vmcs(int cpu){
	int node =cpu_to_node(cpu);

	struct page *pages;
	struct vmcs *vmcs;

	pages = alloc_pages_exact_node(node,GFP_KERNEL,vmcs_conf->order);

	if(!pages)
		return NULL;

	vmcs=page_address(pages);
	memset(vmcs,0,vmcs_conf->size);
	vmcs->revision_id = vmcs_conf->revision_id;

	return vmcs;
}


static void vmx_free_vmcs(struct vmcs *vmcs){
	free_pages((unsigned long)vmcs,vmcs_conf->order);
}


static void vmx_free_vmxon_area(void){
	int cpu;

	for_each_possible_cpu(cpu){
		if(per_cpu(vmxarea,cpu)){
			vmx_free_vmcs(per_cpu(vmxarea,cpu));
			per_cpu(vmxarea,cpu)=NULL;
		}
	}
}
static __init bool check_cpu_support_vmx(void){
	return cpu_has_vmx();
	/*
	   unsigned int ecx=cpuid(1);
	   test_bit(5,ecx);
	 */
}

static __init int check_vmx_info(void){
	if(!check_cpu_support_vmx()){
		PRINT_ERR("vmx:the cpu not support the vmx!");
		return -EIO;
	}

	/*
	   if(test_bit(63,MSR_IA32_VMX_PROCBASED_CTLS)==0)
	   {
	   PRINT_ERR("vmx:the IA32_VMX_PROCBASED_CTLS2 is useless");
	   return -EIO;
	   }

	   if(test_bit(33,MSR_IA32_VMX_PROCBASED_CTLS2)==0)
	   {
	   PRINT_ERR("vmx:the IA32_VMX_EPT_VPID_CAP is useless");
	   return -EIO;
	   }

	   if(test_bit(45,MSR_IA32_VMX_PROCBASED_CTLS2)==0)
	   {
	   PRINT_ERR("vmx:the IA32_vmx_vmfunc is useless");
	   return -EIO;
	   }*/

	msr_true=false;

	if((MSR_IA32_VMX_BASIC & (1UL<<55)))
		msr_true=true;
	return 0;
}

static __init int adjust_vmx_controls(u32 ctl_min,u32 ctl_opt,
		u32 msr,u32 *result){
	u32 vmx_msr_low,vmx_msr_high;
	u32 ctl=ctl_min | ctl_opt;
	rdmsr(msr,vmx_msr_low,vmx_msr_high);
	ctl |= vmx_msr_low;
	ctl &= vmx_msr_high;
	if(ctl_min & ~ctl)
		return -EIO;
	*result = ctl;
	return 0;
}

static __init bool allow_1_setting(u32 msr, u32 ctl)
{
	u32 vmx_msr_low,vmx_msr_high;
	rdmsr(msr,vmx_msr_low,vmx_msr_high);
	return vmx_msr_high & ctl;
}

static void __vmx_disable_intercept_for_msr(unsigned long *msr_bitmap, u32 msr)
{
     if(msr <= 0x1fff){
		 __clear_bit(msr,msr_bitmap+0x000/f);//read_low
		 __clear_bit(msr,msr_bitmap+0x800/f);//write_low
	 }
	 else if(msr>=0xc0000000) && (msr<=0xc0001fff)){
		 __clear_bit(msr,msr_bitmap+0x400/f);//read_high
		 __clear_bit(msr,msr_bitmap+0xc00/f);
	 }
}

static __init int setup_vmcs_config(void){

	//no consider compat
	u32 vmx_msr_low,vmx_msr_high;
	u32 min2,opt2,min,opt;
	u32 _pin_based_exec_control=0;
	u32 _cpu_based_exec_control=0;
	u32 _cpu_based_2nd_exec_control=0;
	u32 _vmexit_control=0;
	u32 _vmentry_control=0;

	u32  ia32_vmx_temp;


	min = PIN_BASED_EXT_INTR_MASK | PIN_BASED_NMI_EXITING;
	opt = PIN_BASED_VIRTUAL_NMIS;

	ia32_vmx_temp=msr_true ? MSR_IA32_VMX_TRUE_PINBASED_CTLS:
		MSR_IA32_VMX_PINBASED_CTLS;
	if(adjust_vmx_controls(min,opt,ia32_vmx_temp,
				&_pin_based_exec_control)<0)
		return -EIO;

	min= 
#ifdef 	CONFIG_X86_64
		CPU_BASED_CR8_LOAD_EXITING |
		CPU_BASED_CR8_STORE_EXITING |
#endif
		CPU_BASED_CR3_LOAD_EXITING |
		CPU_BASED_CR3_STORE_EXITING |
		CPU_BASED_MOV_DR_EXITING |
		CPU_BASED_USE_TSC_OFFSETING |
		CPU_BASED_INVLPG_EXITING;


	opt= CPU_BASED_TPR_SHADOW |
		CPU_BASED_USE_MSR_BITMAPS |
		CPU_BASED_ACTIVATE_SECONDARY_CONTROLS;

	ia32_vmx_temp=msr_true?MSR_IA32_VMX_TRUE_PROCBASED_CTLS:
		MSR_IA32_VMX_PROCBASED_CTLS;

	if(adjust_vmx_controls(min,opt,ia32_vmx_temp,
				&_cpu_based_exec_control)<0)
		return -EIO;
	//tpr_shadow not understand
#ifdef CONFIG_X86_64
	if((_cpu_based_exec_control & CPU_BASED_TPR_SHADOW))
		_cpu_based_exec_control &=
			~CPU_BASED_CR8_LOAD_EXITING
			& ~CPU_BASED_CR8_STORE_EXITING;
#endif

	if(_cpu_based_exec_control & CPU_BASED_ACTIVATE_SECONDARY_CONTROLS)
	{
		min2=0;
		opt2=SECONDARY_EXEC_WBINVD_EXITING |
			SECONDARY_EXEC_ENABLE_VPID |
			SECONDARY_EXEC_ENABLE_EPT |
			SECONDARY_EXEC_RDTSCP |
			SECONDARY_EXEC_ENABLE_INVPCID;
		if(adjust_vmx_controls(min2,opt2,MSR_IA32_VMX_PROCBASED_CTLS2,
					&_cpu_based_2nd_exec_control)<0)
			return -EIO;

#ifndef CONFIG_X86_64
		if(!(_cpu_based_2nd_exec_control &
					SECONDARY_EXEC_VIRTUALIZE_APIC_ACCESS))
			_cpu_based_exec_control &= ~CPU_BASED_TPR_SHADOW;
#endif
		if(_cpu_based_2nd_exec_control &
				SECONDARY_EXEC_ENABLE_EPT)
			rdmsr(MSR_IA32_VMX_EPT_VPID_CAP,vmx_capability.ept,
					vmx_capability.vpid);
	}


	min=0;
#ifdef CONFIG_X86_64
	min|=VM_EXIT_HOST_ADDR_SPACE_SIZE;
#endif

	opt=0;
	ia32_vmx_temp=msr_true?MSR_IA32_VMX_TRUE_EXIT_CTLS:
		MSR_IA32_VMX_EXIT_CTLS;
	if(adjust_vmx_controls(min,opt,ia32_vmx_temp,
				&_vmexit_control)<0)
		return -EIO;

	min=0;
	opt=0;
	ia32_vmx_temp=msr_true?MSR_IA32_VMX_TRUE_ENTRY_CTLS:
		MSR_IA32_VMX_ENTRY_CTLS;
	if(adjust_vmx_controls(min,opt,ia32_vmx_temp,
				&_vmentry_control)<0)
		return -EIO;

	rdmsr(MSR_IA32_VMX_BASIC,vmx_msr_low,vmx_msr_high);

	if((vmx_msr_high & 0x1fff)>PAGE_SIZE)
		return -EIO;

#ifdef CONFIG_X86_64
	if(vmx_msr_high & (1u<<16))
		return -EIO;
#endif

	//write_back   P148
	if(((vmx_msr_high >> 18)& 15 )!=6)
		return -EIO;

	vmcs_conf->size=vmx_msr_high & 0x1fff;
	vmcs_conf->order = get_order(vmcs_conf->size);
	vmcs_conf->revision_id=vmx_msr_low;
	vmcs_conf->pin_based_exec_ctrl=_pin_based_exec_control;
	vmcs_conf->cpu_based_exec_ctrl=_cpu_based_exec_control;
	vmcs_conf->cpu_based_2nd_exec_ctrl=_cpu_based_2nd_exec_control;
	vmcs_conf->vmexit_ctrl=_vmexit_control;
	vmcs_conf->vmentry_ctrl=_vmentry_control;

	vmx_capability.has_load_efer =
		allow_1_setting(MSR_IA32_VMX_ENTRY_CTLS,
				VM_ENTRY_LOAD_IA32_EFER)
		&& allow_1_setting(MSR_IA32_VMX_EXIT_CTLS,
				VM_EXIT_LOAD_IA32_EFER);	
	return 0;

}


__init int vmx_init(void)
{

	/*	if(!check_cpu_support_vmx())
		printk(KERN_ERROR "vmx:the cpu not support the vmx!\n");
	 */	
	if(check_vmx_info()<0)
		return -EIO;

	if(setup_vmcs_config()<0){
		PRINT_ERR("init:vmcs config is failed!");
		return -EIO;
	}

	if(!cpu_has_vmx_vpid())
	{
		PRINT_ERR("init: the cpu nod support the vpid");
		return -EIO;
	}

	if(!cpu_has_vmx_ept()){
		PRINT_ERR("init: the cpu not support the ept");
		return -EIO;
	}

	if(!vmx_capability.has_load_efer){
		PRINT_ERR("init :the support of efer register is needed");
		return -EIO;
	}

	//alloct vmxon area
	msr_bitmap = (unsigned long *)__get_free_page(GFP_KERNEL);
	if(!msr_bitmap)
		return -ENOMEM;
	//???????
	memset(msr_bitmap,0xff,PAGE_SIZE);
   ///????????? repeat
	__vmx_disable_intercept_for_msr(msr_bitmap,MSR_FS_BASE);
	__vmx_disable_intercept_for_msr(msr_bitmap,MSR_GS_BASE);

	set_bit(0,vmx_vpid_bitmap);

	int cpu;
	for_each_passible_cpu(cpu){
		struct vmcs *vmxon_buf;
		vmxon_buf = __vmx_alloc_vmcs(cpu);
		if(!vmxon_buf){
			vmx_free_vmxon_area();
			return -ENOMEM;
		}
		//per cpu has a vmcs type name is vmxarea;
		per_cpu(vmxarea,cpu)=vmxon_buf;//addres;;
	}

	
	return 0;
}

void vmx_exit(void){

}
