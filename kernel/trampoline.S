	#
        # code to switch between user and kernel space.
        #
        # this code is mapped at the same virtual address
        # (TRAMPOLINE) in user and kernel space so that
        # it continues to work when it switches page tables.
	#
	# kernel.ld causes this to be aligned
        # to a page boundary.
        #
	.section trampsec
.globl trampoline
trampoline:
.align 4
.globl uservec
uservec:    
	#
        # trap.c sets stvec to point here, so
        # traps from user space start here,
        # in supervisor mode, but with a
        # user page table.
        #
        # sscratch points to where the process's p->trapframe is
        # mapped into user space, at TRAPFRAME.
        #
        
	# swap a0 and sscratch
        # so that a0 is TRAPFRAME
        csrrw a0, sscratch, a0

        # save the user registers in TRAPFRAME
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

	# save the user a0 in p->trapframe->a0
  # 把a0释放出来
        csrr t0, sscratch
        sd t0, 112(a0)

        # restore kernel stack pointer from p->trapframe->kernel_sp (set previously<kernel to user>)
        ld sp, 8(a0)

        # make tp hold the current hartid, from p->trapframe->kernel_hartid
        ld tp, 32(a0)

        # load the address of usertrap(), p->trapframe->kernel_trap
        ld t0, 16(a0)

        # restore kernel page table from p->trapframe->kernel_satp
        ld t1, 0(a0)

        csrw satp, t1
        sfence.vma zero, zero

        # a0 is no longer valid, since the kernel page
        # table does not specially map p->tf.

        # jump to usertrap(), which does not return
        jr t0

.globl userret
userret:
        # userret(TRAPFRAME, pagetable)
        # switch from kernel to user.
        # usertrapret() calls here.
        # a0: TRAPFRAME, in user page table.
        # a1: user page table, for satp.

        # switch to the user page table.
        # 将 user page table(在usertrapret中作为第二个参数传递给了这里的userret函数,所以存在a1寄存器中)存储在SATP寄存器中
        csrw satp, a1
        # 幸运的是 user page table也映射了trampoline page,所以程序还能继续执行而不是崩溃.(sfence.vma是清空页表缓)
        sfence.vma zero, zero

        # put the saved user a0 in sscratch, so we
        # can swap it with our a0 (TRAPFRAME) in the last step.
        # 第一件事情就是交换SSRATCH和a0寄存器,而这里,我们将SSCRATCH寄存器恢复成保存好的用户的a0寄存器
        # 在这里a0是trapframe的地址 因为C代码usertrapret函数中将trapframe地址作为第一个参数传递过来了
        # 112是a0寄存器在trapframe中的位置(注,这里有点绕,本质就是通过当前的a0寄存器找出存在trapframe中的a0寄存器)
        # 我们先将这个地址里的数值保存在t0寄存器中,之后再将t0寄存器的数值保存在SSCRATCH寄存器中
        ld t0, 112(a0)
        csrw sscratch, t0

        # 学生提问:现在<trapframe>中的a0寄存器是我们执行系统调用的返回值吗?
        # Robert教授:是的,系统调用的返回值覆盖了我们保存在trapframe中的a0寄存器的值.
        # 我们希望用户程序Shell在a0寄存器中看到系统调用的返回值 所以,trapframe中的a0寄存器现在是系统调用的返回值2
        # 相应的SSCRATCH寄存器中的数值也应该是2
        # restore all but a0 from TRAPFRAME
        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

	      # restore user a0, and save TRAPFRAME in sscratch
        # 交换SSCRATCH寄存器和a0寄存器的值,前面我们看过了SSCRATCH现在的值是系统调用的返回值2,
        # a0寄存器是trapframe的地址,交换完成之后,a0持有的是系统调用的返回值,SSCRATCH持有的是trapframe的地址
        # 之后trapframe的地址会一直保存在SSCRATCH中,直到用户程序执行了另一次trap
        csrrw a0, sscratch, a0
        
        # return to user mode and user pc.
        # usertrapret() set up sstatus and sepc.
        # 1. 程序会切换回user mode
        # 2. SEPC寄存器的数值会被拷贝到PC寄存器(程序计数器)
        # 3. 重新打开中断
        sret
