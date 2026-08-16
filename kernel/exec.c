#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

// map ELF permissions to PTE permission bits.
int
flags2perm(int flags)
{
  int perm = 0;
  if (flags & 0x1)
    perm = PTE_X;
  if (flags & 0x2)
    perm |= PTE_W;
  return perm;
}

//
// the implementation of the exec() system call
//
// kexec() replaces the current process's user address space with
// a new program loaded from an ELF file. The process struct itself
// (pid, kstack, trapframe, ofile[], cwd) is reused.
//
// Process memory layout after exec (e.g., _cat):
//
//   User Virtual Address Space            Kernel Resources (per-process)
//   ──────────────────────────            ─────────────────────────────
//
//   0x00000000 ┌──────────────┐           struct proc (in kernel .bss)
//              │ .text +      │            ├─ pagetable → L2 (4KB)
//              │ .rodata      │ ← R|X      ├─ kstack   → KSTACK (4KB + guard)
//   0x00001000 ├──────────────┤            ├─ trapframe → 4KB (mapped at TRAPFRAME)
//              │ .data + .bss │ ← R|W      ├─ context (ra/sp/s0-s11)
//   0x00001220 ├──────────────┤ ← sz      ├─ ofile[16], cwd
//              │              │            ├─ pid, state, ...
//              │  heap        │            └─────────────────
//              │  (sbrk)    ↑ │
//              │              │           Kernel page table (global):
//              │  user stack  │            ├─ kernel text/data (R|X, R|W)
//              │  (1 page)  ↓ │            ├─ device MMIO (UART, PLIC, VIRTIO)
//   0x00003000 ├──────────────┤ ← stackbase├─ kernel stacks (KSTACK 0..63)
//   0x00004000 ├──────────────┤ ← sp       ├─ TRAMPOLINE  → trampoline page
//              │              │            └─ TRAPFRAME   → p->trapframe
//              │     ...      │
//              │              │
//   0x3FFFFFE000 ├──────────────┤ ← TRAPFRAME  (1 page, R|W, = p->trapframe)
//   0x3FFFFFF000 ├──────────────┤ ← TRAMPOLINE (1 page, R|X, = trampoline.S)
//   0x4000000000 └──────────────┘ ← MAXVA (Sv39: 256 GB)
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  // Open the executable file.
  if ((ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header. 读 elf 头部，判断是否是 elf 文件
  if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if (elf.magic != ELF_MAGIC)
    goto bad;

    // 创建一个新的页表，映射当前执行的这个 elf 程序
  if ((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    uint64 sz1;
    // 当前进程从虚拟地址 0 开始映射
    // oldsz 起始虚拟地址, newsz 结束虚拟地址, 这里 newsz = ph.vaddr + ph.memsz
    // 这里的 newsz 是虚拟地址, 不是物理地址
    if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz,
                        flags2perm(ph.flags))) == 0)
      goto bad;

    sz = sz1;
    // 把 load 段的内容，写到虚拟地址对应的物理地址中
    if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz); // 对齐到 4KB，准备建立栈空间的 va->pa 的映射
  uint64 sz1;// 建立用户态栈空间的虚拟地址映射
  if ((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK + 1) * PGSIZE, PTE_W)) ==
      0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz - (USERSTACK + 1) * PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK * PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for (argc = 0; argv[argc]; argc++) {
    if (argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if (sp < stackbase)
      goto bad;
    if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) // 将参数放到栈中
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc + 1) * sizeof(uint64);
  sp -= sp % 16;
  if (sp < stackbase)
    goto bad;
  if (copyout(pagetable, sp, (char *)ustack, (argc + 1) * sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp; // sp 这里给到 argv

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz; // 用户态进程入口设置为 elf.entry
  p->trapframe->epc = elf.entry; // initial program counter = ulib.c:start()
  p->trapframe->sp = sp;         // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

bad:
  if (pagetable)
    proc_freepagetable(pagetable, sz);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset,
        uint sz)
{
  uint i, n;
  uint64 pa;

  for (i = 0; i < sz; i += PGSIZE) {
    pa = walkaddr(pagetable, va + i);
    if (pa == 0)
      panic("loadseg: address should exist");
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, 0, (uint64)pa, offset + i, n) != n)
      return -1;
  }

  return 0;
}
