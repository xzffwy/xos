#include <linux/mmu_notifier.h>
#include <linux/types.h>
#include <asm/vmx.h>
#include <linux/kvm_types.h>


struct vmcs{
	u32 revision_id;
	u32 abort;
	char data[0];
};

struct vmx_capability{
	u32 ept;
	u32 vpid;
	int has_load_efer:1;
};

extern struct vmx_capability vmx_capability;
extern __init int vmx_init(void);
extern void vmx_exit(void);
