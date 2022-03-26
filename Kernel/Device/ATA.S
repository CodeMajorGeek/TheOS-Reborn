read_ATA_st:
    pushq %rdx
    pushq %rcx
    pushq %rax
    testl %ebx, %ebx
    js read_ATA_st.reset
    cmpl $0x3FFFFF, %ebx
    stc
    jg read_ATA_st.r_don
    movl dd_prtlen(%esi), %edx
    decl %edx
    cmpl %ebp, %edx
    jb read_ATA_st.r_don
    subl %ebx, %ebx
    incl %edx
    cmpl %ebp, %edx
    jb read_ATA_st.r_don
    movw dd_dcr(%esi), %dx
    inb %dx, %al
    testb $0x88, %al
    je read_ATA_st.stat_ok
read_ATA_st.reset:
    callq srst_ATA_st
    testl %ebx, %ebx
    jns read_ATA_st.stat_ok
    xorl %ebx, %ebx
    jmp read_ATA_st.r_don
read_ATA_st.stat_ok:
    cmpl $0xFFFFFFF, %ebp
    jg read_ATA_st.setreg
    movl dd_stLBA(%esi), %eax
    cmpl $0xFFFFFFF, %eax
    jg read_ATA_st.setreg
    addl %ebp, %eax
    cmpl $0xFFFFFFF, %eax
    jg read_ATA_st.setreg
    addl %ebx, %eax
    cmpl $0xFFFFFFF, %eax
read_ATA_st.setreg:
    movw dd_tf(%esi), %dx
    jle read_ATA_st.read28
read_ATA_st.read48:
    testl %ebx, %ebx
    je read_ATA_st.r_don
    callq pio48_read
    je read_ATA_st.read48
    jmp read_ATA_st.r_don
read_ATA_st.read28:
    testl %ebx, %ebx
    je read_ATA_st.r_don
    callq pio28_read
    je read_ATA_st.read28
read_ATA_st.r_don:
    popq %rax
    popq %rcx
    popq %rdx
    retq

pio28_read:
    addl dd_stLBA(%esi), %ebp
    movl %ebp, %ecx
    movb %bl, %al
    orb $2, %dl
    outb %al, %dx
    movb %cl, %al
    incl %edx
    outb %al, %dx
    movb %ch, %al
    inc %edx
    outb %al, %dx
    bswapl %ecx
    movb %ch, %al
    incl %edx
    outb %al, %dx
    movb %cl, %al
    orb dd_sbits(%esi), %al
    incl %edx
    outb %al, %dx

    incl %edx
    movb $0x20, %al
    outb %al, %dx

    movl $4, %ecx
pio28_read.lp1:
    inb %dx, %al
    testb $0x80, %al
    jne pio28_read.retry
    testb $8, %al
    jne pio28_read.data_rdy
pio28_read.retry:
    decl %edx
    jg pio28_read.lp1

pio28_read.pior_l:
    inb %dx, %al
    testb $0x80, %al
    jne pio28_read.pior_l
    testb $0x21, %al
    jne pio28_read.fail
pio28_read.data_rdy:
    subb $7, %dl
    movw $256, %cx
    rep insw
    orb $7, %dl
    inb %dx, %al
    inb %dx, %al
    inb %dx, %al
    inb %dx, %al


    incl %ebp
    decl %ebx
    testb %bl, %bl
    jne pio28_read.pior_l

    subw $7, %dx
    subl dd_stLBA(%esi), %ebp
    testb $0x21, %al
    je pio28_read.done
pio28_read.fail:
    stc
pio28_read.done:
    retq


pio48_read:
    xorl %eax, %eax
    addl dd_stLBA(%esi), %ebp
    adcb $0, %ah
    movl %ebp, %ecx


    bswapl %ecx
    orb $2, %dl
    movb %bh, %al
    outb %al, %dx
    movb %cl, %al
    incl %edx
    outb %al, %dx
    incl %edx
    movb %ah, %al
    outb %al, %dx
    incl %edx
    movb $0, %al
    outb %al, %dx

    subb $3, %dl
    movb %bl, %al
    outb %al, %dx
    movw %bp, %ax
    incl %edx
    outb %al, %dx
    movb %ah, %al
    incl %edx
    outb %al, %dx
    movb %ch, %al
    incl %edx
    outb %al, %dx

    movb dd_sbits(%esi), %al
    incl %edx
    andb $0x50, %al
    outb %al, %dx

    incl %edx
    movb $0x24, %al
    outb %al, %dx


    movl $4, %ecx
pio48_read.lp1:
    inb %dx, %al
    testb $0x80, %al
    jne pio48_read.retry
    testb $8, %al
    jne pio48_read.data_rdy
pio48_read.retry:
    decl %ecx
    jg pio48_read.lp1

pio48_read.pior_l:
    inb %dx, %al
    testb $0x80, %al
    jne pio48_read.pior_l
    testb $0x21, %al
    jne pio48_read.fail
pio48_read.data_rdy:
    subb $7, %dl
    movw $256, %cx
    rep insw
    orb $7, %dl
    inb %dx, %al
    inb %dx, %al
    inb %dx, %al
    inb %dx, %al


    incl %ebp
    decl %ebx
    testw %bx, %bx
    jne pio48_read.pior_l

    subw $7, %dx
    subl dd_stLBA(%esi), %ebp
    testb $0x21, %al
    je pio48_read.done
pio48_read.fail:
    stc
pio48_read.done:
    retq

srst_ATA_st:
    pushq %rax
    movb $4, %al
    outb %al, %dx
    xorl %eax, %eax
    outb %al, %dx
    inb %dx, %al
    inb %dx, %al
    inb %dx, %al
    inb %dx, %al
srst_ATA_st.rdylp:
    inb %dx, %al
    andb $0xC0, %al
    cmpb $0x40, %al
    jne srst_ATA_st.rdylp
    popq %rax
    retq