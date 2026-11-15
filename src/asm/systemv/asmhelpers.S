.section .text

.globl EnterNaked
.globl ExitNaked
.globl TailcallNaked

EnterNaked:
    push %rax
    push %rcx
    push %rdx
    call EnterStub
    pop  %rdx
    pop  %rcx
    pop  %rax
    ret

ExitNaked:
    push %rax
    push %rcx
    push %rdx
    call ExitStub
    pop  %rdx
    pop  %rcx
    pop  %rax
    ret

TailcallNaked:
    push %rax
    push %rcx
    push %rdx
    call TailcallStub
    pop  %rdx
    pop  %rcx
    pop  %rax
    ret
