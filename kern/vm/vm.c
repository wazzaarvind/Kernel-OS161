#include <types.h>
#include <mainbus.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <bitmap.h>
#include <kern/iovec.h>
#include <uio.h>
#include <vfs.h>
#include <kern/fcntl.h>
#include <kern/stat.h>
#include <mips/tlb.h>
#include <spl.h>
#include <vm.h>

paddr_t size;

int numBytes;

void vm_initialise() {

  tpages = 0; // Total possible pages in the ram.
  size = 0;
  numBytes = 0;

  spinlock_init(&vmlock);

  // Get the size of the RAM.
  size = ram_getsize();
  tpages = size/PAGE_SIZE;

  freePages = tpages;

  // Check how much is used by Kernel + Exception handler
  paddr_t f_paddr = ram_getfirstfree();

  // Init of coremap - vm.h
  // Allocate the memory used by coremap - start in first free
  coremap = (struct coremap_struct *)PADDR_TO_KVADDR(f_paddr); // Still in physical **

  // Coremap usage:
  int coremap_size = sizeof(struct coremap_struct) * tpages;

// Knvalidate pages :  K + EC + CM
  int mem_Kused = coremap_size + (f_paddr);
  int pages_Kused = mem_Kused/PAGE_SIZE;

  // Check for fractions :
  if(mem_Kused % PAGE_SIZE > 0){
    pages_Kused++;
  }

  for(int i = 0; i < pages_Kused; i++){
    //coremap[i].available = 0;
    coremap[i].chunk_size = 0;
    coremap[i].state = FIXED;
  }

  // Make the rest available :
  for(int j = pages_Kused; j < tpages; j++){
    //coremap[j].available = 1;
    coremap[j].chunk_size = 0;
    coremap[j].state = FREE;
    coremap[j].page = NULL;

  }

  numBytes += pages_Kused * PAGE_SIZE;
}

vaddr_t alloc_kpages(unsigned npages)
{
  //kprintf("\nallock");

  spinlock_acquire(&vmlock);


  int alloc = 0;
  int req = (int) npages;

  int i = 0, startAlloc = 0;

  if(freePages != 0) {
    for(i = 0; i < (int) tpages; i++){
      if(coremap[i].state == FREE){
        for(int j = i;  j <  i + (int) npages; j++){
          if(coremap[j].state == FREE){
            alloc +=1;
          } else {
            break;
          }
          if(alloc == req){
            break;
          }
        }
      }

      if(alloc == req){
        break;
      } else {
        alloc = 0;
      }
    }
  }

  int flag = 0;

  struct page_table *store;

  if(alloc != req){
    //spinlock_release(&vmlock);
    //return (vaddr_t)NULL;
    if(swap_or_not == SWAP_ENABLED){
      i = evict_page();
      if(i == -1){
        panic("Test panic.");
        spinlock_release(&vmlock);
        return (vaddr_t) NULL;
      }
      store = coremap[i].page;
      flag = 1;
    }

    // RAM full of kernel pages.
    else
    {
      spinlock_release(&vmlock);
      return (vaddr_t) NULL;
    }
  }

  startAlloc = i;
  numBytes += alloc * PAGE_SIZE;

  while(req > 0){
    req--;
    //coremap[i].available = 0;
    freePages--;
    coremap[i].chunk_size = (int) npages;
    coremap[i].state = FIXED;
    // Kpage doesn't need a PTE.
    coremap[i].page = NULL;
    i++;
  }

  spinlock_release(&vmlock);

  if(flag == 1) {
    lock_acquire(store->pt_lock);
    swap_out(store);
    lock_release(store->pt_lock);
    flag = 0;
  }

  bzero((void *)(PADDR_TO_KVADDR(startAlloc * PAGE_SIZE)), PAGE_SIZE);

  return PADDR_TO_KVADDR(startAlloc * PAGE_SIZE);

}


void free_kpages(vaddr_t addr)
{
  //kprintf("\nfreek");

  spinlock_acquire(&vmlock);

  int i = 0;
  int pagesToInvalidate = 0;
  vaddr_t iter_addr;


  for(i = 0; i < (int) tpages; i++){
    iter_addr = PADDR_TO_KVADDR(i * PAGE_SIZE);
    if(iter_addr == addr){
      break;
    }
  }

  // Sanity checks :
  if(coremap[i].state == FREE || coremap[i].chunk_size == 0){
    spinlock_release(&vmlock);
    return;
  }

  pagesToInvalidate = coremap[i].chunk_size;

  numBytes -= pagesToInvalidate * PAGE_SIZE;

  while (pagesToInvalidate > 0) {
    //coremap[i].available = 1;
    freePages++;
    coremap[i].chunk_size = 0;
    coremap[i].state = FREE;
    i++;
    pagesToInvalidate--;
  }

  spinlock_release(&vmlock);

}

unsigned int coremap_used_bytes(void)
{
	//kprintf("Hi%d\n",numBytes);
	return numBytes;
}

void vm_bootstrap(void)
{
  swap_or_not = SWAP_DISABLED;
  //kprintf("\nHi1");

  char *arg1 = kstrdup("lhd0raw:");

  int check = vfs_open(arg1,O_RDWR,0,&swap_vnode);

  if(!check){

    struct stat stats_file;

    VOP_STAT(swap_vnode, &stats_file);

    int swapDiskSize = stats_file.st_size;

    int swapPages = swapDiskSize/PAGE_SIZE;

    kprintf("Swap size %d\n", swapPages);

    swapTable = bitmap_create(swapPages);

    bitmap_lock = lock_create("Bitmap lock");

    swapStart = 0;
    tlbStart = 0;

    swap_or_not = SWAP_ENABLED;
    //kprintf("\nHi2");

  }

  kfree(arg1);

}

int vm_fault(int faulttype, vaddr_t faultaddress) // we cannot return int, no index in linked list
{

    // int spl = 0;
    // uint32_t ehi,elo;

    (void) faulttype;


    if (curproc == NULL) {
      /*
      * No process. This is probably a kernel fault early
      * in boot. Return EFAULT so as to panic instead of
      * getting into an infinite faulting loop.
      */
      return EFAULT;
    }

    //as = proc_getas();

    if (curproc->p_addrspace == NULL) {
      /*
      * No address space set up. This is probably also a
      * kernel fault early in boot.
      */
      return EFAULT;
    }

    KASSERT(curproc->p_addrspace->sgmt!=NULL);
    KASSERT(curproc->p_addrspace->heap_top!=0);
    KASSERT(curproc->p_addrspace->heap_bottom!=0);
      //KASSERT for stack?



    int segFlag = 0;

    // Can I use curproc?
    // TODO : curproc->p_addrspace->sgmt KASSERT
    //
    // Page align to retrieve the PTE.
    //faultaddress &= PAGE_FRAME;

    struct segment *curseg = curproc->p_addrspace->sgmt;

    // struct segment *tempIter = curproc->p_addrspace->sgmt;
    // int segCount = 0;
    // while(tempIter != NULL){
    //   segCount++;
    //   tempIter = tempIter->next;
    // }
    faultaddress &= PAGE_FRAME;

    while(curseg != NULL){
         if(faultaddress >= curseg->start && faultaddress < curseg->end){
            segFlag = 1;
            break;
         }
         curseg = curseg->next;
    }

    if(segFlag != 1){
      if(faultaddress >= curproc->p_addrspace->stack_top && faultaddress < curproc->p_addrspace->stack_bottom){
          segFlag = 1;
      }
    }

    if(segFlag != 1){
      if(faultaddress >= curproc->p_addrspace->heap_top && faultaddress < curproc->p_addrspace->heap_bottom){
          segFlag = 1;
      }
    }

    // If its not in one of the segments.
    if(segFlag != 1){
      return EFAULT;
    }

    // If it is, then find the corresponding PTE.
    struct page_table *first = curproc->p_addrspace->first_page;

    int pageFlag = 0;

    // page align faultaddress and find coresponding page in the PTE.
    while(first != NULL){
      //kprintf("\n%d",first->vaddr);
      //
      if(first->vaddr == faultaddress)  //does this necessarily need to be the case? Will it never be in between?
      {
        pageFlag=1;
        break;
      }
      first=first->next;
    }

    //check the same for heap and stack areas as well!
    // if pageFlag == 1 : The requested page already exists in the page table.
    // else : Allocate a new page and physical memory.
    if(pageFlag==1){
      /* Disable interrupts on this CPU while frobbing the TLB. */
      // TODO : KASSERT
        if(swap_or_not == SWAP_ENABLED)
        {
          lock_acquire(first->pt_lock);

          if(first->mem_or_disk == IN_DISK){

             first->paddr = alloc_upages();

             if(first->paddr == 0){
               return ENOMEM;
             }

             swap_in(first);

             int index = 0;

             index = first->paddr/PAGE_SIZE;

             if(coremap[index].state == DESTORY){
              lock_release(coremap[index].page->pt_lock);
         			lock_destroy(coremap[index].page->pt_lock);
         			kfree(coremap[index].page);
         		 }


          }


          //lock_release(first->pt_lock);
        }


        paddr_t paddr = first->paddr;

        updateTLB(faultaddress, paddr);


        // spl = splhigh();
        //
        // for (int i=0; i<NUM_TLB; i++) {
        //   //kprintf("\nFails2?\n");
        //     tlb_read(&ehi, &elo, i);
        //     if (elo & TLBLO_VALID) {
        //         continue;
        //       }
        //     ehi = faultaddress;
        //     elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
        //
        //
        //     DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
        //     tlb_write(ehi, elo, i);
        //      //kprintf("\nFails3\n");
        //     splx(spl);
        //     int index = 0;
        //     index = first->paddr/PAGE_SIZE;
        //     coremap[index].page = first;
        //     coremap[index].state = RECENTLY_USED;
        //     if(swap_or_not == SWAP_ENABLED)
        //       lock_release(first->pt_lock);
        //     return 0;
        // }
        //
        //
        // // TLB is currently full so evicting a TLB entry
        // ehi = faultaddress;
        // elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
        // tlb_random(ehi, elo);
        // splx(spl);
        int index = 0;
        index = first->paddr/PAGE_SIZE;
        coremap[index].page = first;
        coremap[index].state = RECENTLY_USED;
        if(swap_or_not == SWAP_ENABLED)
          lock_release(first->pt_lock);
        return 0;
    }

    // There is no PTE for the current vaddr_t.
    if(pageFlag==0)
    {
      // So create one
      struct page_table *cur_page = kmalloc(sizeof(struct page_table));
      cur_page->pt_lock = lock_create("page lock");

      if(cur_page == NULL){
  			return ENOMEM;
  		}

      cur_page->vaddr = faultaddress;
      cur_page->paddr = alloc_upages(); //page aligned address?

      //kprintf("\n Here 0?\n");

      if(cur_page->paddr == 0){
        return ENOMEM;
      }

      int index = 0;
      index = cur_page->paddr/PAGE_SIZE;

      if(coremap[index].state == DESTORY){
        lock_release(coremap[index].page->pt_lock);
  			lock_destroy(coremap[index].page->pt_lock);
  			kfree(coremap[index].page);
  		}

      cur_page->mem_or_disk = IN_MEMORY;
      cur_page->bitmapIndex = -1;

      if(cur_page->paddr == 0){
          return ENOMEM;
      }

      cur_page->next = NULL;
      //kprintf("\nNew PTE is %d",cur_page->vaddr);

      if(curproc->p_addrspace->last_page == NULL){
        curproc->p_addrspace->last_page = cur_page;
        curproc->p_addrspace->first_page = cur_page;
      } else {
        curproc->p_addrspace->last_page->next = cur_page;
        curproc->p_addrspace->last_page = curproc->p_addrspace->last_page->next;
      }

      paddr_t paddr = cur_page->paddr;

      updateTLB(faultaddress, paddr);

      // Fill up the TLB.
      // Load TLB and return.
      // spl = splhigh();
      //
      // for (int i=0; i<NUM_TLB; i++) {
      //     tlb_read(&ehi, &elo, i);
      //     if (elo & TLBLO_VALID) {
      //         continue;
      //       }
      //     ehi = faultaddress;
      //     elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
      //     DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
      //     tlb_write(ehi, elo, i);
      //     splx(spl);
      //     coremap[index].page = cur_page;
      //     coremap[index].state = RECENTLY_USED;
      //     return 0;
      // }
      //
      // ehi = faultaddress;
      // elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
      // tlb_random(ehi, elo);
      // splx(spl);
      coremap[index].page = cur_page;
      coremap[index].state = RECENTLY_USED;
      return 0;

      // TLB is currently full so evicting a TLB entry
    }

    return EFAULT;
}

void vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("VM tried to do tlb shootdown?!\n");
}


vaddr_t alloc_upages(void){

  //kprintf("\nallocu");

  int npages=1;
  int alloc = 0;
  int req = 1;

  int i = 0, startAlloc = 0;

  spinlock_acquire(&vmlock);
  if(freePages != 0) {
    for(i = 0; i < (int) tpages; i++){
      if(coremap[i].state == FREE){
        for(int j = i;  j <  i + (int) npages; j++){
          if(coremap[j].state == FREE){
            alloc +=1;
          } else {
            break;
          }
          if(alloc == req){
            break;
          }
        }
      }

      if(alloc == req){
        break;
      } else {
        alloc = 0;
      }
    }
  }

  int flag = 0;
  struct page_table *store;

  if(alloc != req){
    if(swap_or_not == SWAP_ENABLED){
      i = evict_page();
      if(i == -1){
        spinlock_release(&vmlock);
        return (vaddr_t) NULL;
      }
      store = coremap[i].page;
      flag = 1;
    }
    else {
      //kprintf("\nHi3\n");
      spinlock_release(&vmlock);
      return (vaddr_t) NULL;
    }

  }

  startAlloc = i;
  numBytes += alloc * PAGE_SIZE;

  while(req > 0){
    req--;
    freePages--;
    coremap[i].chunk_size = npages;
    coremap[i].state = VICTIM;
    i++;
  }

  spinlock_release(&vmlock);

  if(flag == 1){
    lock_acquire(store->pt_lock);
    swap_out(store);
    lock_release(store->pt_lock);
    flag = 0;
  }

  bzero((void *)(PADDR_TO_KVADDR(startAlloc * PAGE_SIZE)), PAGE_SIZE);

  return startAlloc * PAGE_SIZE;
}


// Change this
void free_upage(paddr_t addr)
{
  //kprintf("\nfreeu");

  int i = 0;

  spinlock_acquire(&vmlock);

  i = addr/PAGE_SIZE;

  if(coremap[i].state == FREE){
    spinlock_release(&vmlock);
    return;
  }

  freePages++;
  numBytes -= PAGE_SIZE;
  coremap[i].chunk_size = 0;
  coremap[i].state = FREE;
  coremap[i].page = NULL;

  spinlock_release(&vmlock);

}
