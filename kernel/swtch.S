# Context switch
#
#   void swtch(struct context *old, struct context *new);
# 
# Save current registers in old. Load from new.	
# swtch函数在保存完当前内核线程的内核寄存器之后,
# 就会恢复当前CPU核的调度器线程的寄存器,并继续执行当前CPU核的调度器线程


# 这里有个有趣的问题,或许你们已经注意到了
# swtch函数的上半部分保存了ra，sp等等寄存器,但是并没有保存程序计数器pc
# (Program Counter),为什么会这样呢?
# 学生回答:因为程序计数器不管怎样都会随着函数调用更新
# 是的,程序计数器并没有有效信息,我们现在知道我们在swtch函数中执行,
# 所以保存程序计数器并没有意义.但是我们关心的是我们是从哪调用进到swtch函数的
# 因为当我们通过switch恢复执行当前线程并且从swtch函数返回时,
# 我们希望能够从调用点继续执行.ra寄存器保存了swtch函数的调用点,
# 所以这里保存的是ra寄存器
# ---------------------------
# 为什么RISC-V中有32个寄存器,但是swtch函数中只保存并恢复了14个寄存器?
# 学生答:因为switch是按照一个普通函数来调用的,对于有些寄存器,swtch函数的调用者默认swtch函数会做修改,
# 所以调用者已经在自己的栈上保存了这些寄存器,当函数返回时,这些寄存器会自动恢复
# 所以swtch函数里只需要保存Callee Saved Register就行
# 完全正确！因为swtch函数是从C代码调用的
# 所以我们知道Caller Saved Register会被C编译器保存在当前的栈上
# Caller Saved Register大概有15-18个,而我们在swtch函数中只需要处理C编译器不会保存的
# 但是对于swtch函数又有用的一些寄存器.所以在切换线程的时候,
# 我们只需要保存Callee Saved Register
.globl swtch
swtch:
        sd ra, 0(a0)
        sd sp, 8(a0)
        sd s0, 16(a0)
        sd s1, 24(a0)
        sd s2, 32(a0)
        sd s3, 40(a0)
        sd s4, 48(a0)
        sd s5, 56(a0)
        sd s6, 64(a0)
        sd s7, 72(a0)
        sd s8, 80(a0)
        sd s9, 88(a0)
        sd s10, 96(a0)
        sd s11, 104(a0)

        ld ra, 0(a1)
        ld sp, 8(a1)
        ld s0, 16(a1)
        ld s1, 24(a1)
        ld s2, 32(a1)
        ld s3, 40(a1)
        ld s4, 48(a1)
        ld s5, 56(a1)
        ld s6, 64(a1)
        ld s7, 72(a1)
        ld s8, 80(a1)
        ld s9, 88(a1)
        ld s10, 96(a1)
        ld s11, 104(a1)
        
        ret


# swtch函数是线程切换的核心,但是swtch函数中只有保存寄存器,再加载寄存器的操作
# 线程除了寄存器以外的还有很多其他状态,它有变量,堆中的数据等等,
# 但是所有的这些数据都在内存中,并且会保持不变
# 我们没有改变线程的任何栈或者堆数据,所以线程切换的过程中
# 处理器中的寄存器是唯一的不稳定状态,且需要保存并恢复
# 而所有其他在内存中的数据会保存在内存中不被改变,所以不用特意保存并恢复
# 我们只是保存并恢复了处理器中的寄存器,因为我们想在新的线程中也使用相同的一组寄存器
	
