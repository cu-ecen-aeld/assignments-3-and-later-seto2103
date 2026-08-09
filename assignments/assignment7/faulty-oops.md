# Kernel Oops Analysis: `echo "hello_world" > /dev/faulty`

## Trigger

On the assignment 7 buildroot QEMU target, with the `faulty` module loaded
(via `S98lddmodules` on boot):

```
# echo "hello_world" > /dev/faulty
```

Writing to `/dev/faulty` calls `faulty_write()` in
[`misc-modules/faulty.c`](../../../misc-modules/faulty.c):

```c
ssize_t faulty_write (struct file *filp, const char __user *buf, size_t count,
		loff_t *pos)
{
	/* make a simple fault by dereferencing a NULL pointer */
	*(int *)0 = 0;
	return 0;
}
```

This line intentionally writes through a NULL pointer, which the kernel
cannot allow, producing a kernel oops.

## Captured oops

```
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Mem abort info:
  ESR = 0x0000000096000045
  EC = 0x25: DABT (current EL), IL = 32 bits
  SET = 0, FnV = 0
  EA = 0, S1PTW = 0
  FSC = 0x05: level 1 translation fault
Data abort info:
  ISV = 0, ISS = 0x00000045
  CM = 0, WnR = 1
user pgtable: 4k pages, 39-bit VAs, pgdp=0000000041b57000
[0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
Internal error: Oops: 0000000096000045 [#1] SMP
Modules linked in: hello(O) faulty(O) scull(O)
CPU: 0 PID: 154 Comm: sh Tainted: G           O       6.1.44 #1
Hardware name: linux,dummy-virt (DT)
pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
pc : faulty_write+0x10/0x20 [faulty]
lr : vfs_write+0xc8/0x390
sp : ffffffc008e0bd20
x1 : 0000000000000000 x0 : 0000000000000000
Call trace:
 faulty_write+0x10/0x20 [faulty]
 ksys_write+0x74/0x110
 __arm64_sys_write+0x1c/0x30
 invoke_syscall+0x54/0x130
 el0_svc_common.constprop.0+0x44/0xf0
 do_el0_svc+0x2c/0xc0
 el0_svc+0x2c/0x90
 el0t_64_sync_handler+0xf4/0x120
 el0t_64_sync+0x18c/0x190
Code: d2800001 d2800000 d503233f d50323bf (b900003f)
---[ end trace 0000000000000000 ]---
```

(Register dump trimmed to the relevant `x0`/`x1`; the rest were all zero or
irrelevant to the fault.)

## Analysis

- **Fault address**: `virtual address 0000000000000000` — a NULL pointer was
  dereferenced, matching `*(int *)0 = 0;` in `faulty_write()`.
- **Call trace**: shows the normal `write(2)` syscall path —
  `el0_svc → do_el0_svc → __arm64_sys_write → ksys_write → vfs_write →
  faulty_write` — i.e. the shell's `echo ... > /dev/faulty` redirection
  reached the device's `.write` file operation and crashed immediately.
- **Taint flag `O`**: `hello`, `faulty`, and `scull` are all out-of-tree
  modules, so the kernel is tainted `O`. This just flags that non-upstream
  code is involved; it isn't itself a bug indicator.
- **Oops, not panic**: This is an "Oops", not a full panic. Because the fault
  happened in normal process context, the kernel's `die()` handler printed
  the dump above and killed only the offending process
  (`Comm: sh`, `PID 154`, the shell running the `echo` redirection) with a
  fatal signal. The rest of the system kept running — the QEMU console
  returned to a fresh `login:` prompt right after, rather than the system
  hanging or rebooting.
- **Root cause**: intentional — `faulty.c` is a teaching driver whose
  `faulty_write()` exists specifically to demonstrate what a NULL pointer
  dereference inside a kernel module looks like from a live system.
