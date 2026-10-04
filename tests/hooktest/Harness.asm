; HookTest.cpp's harness: calls a function with every register set to known values and records them all afterwards,
; like an LTCG caller that keeps values in volatile registers.

; HT_State = the start of DK2ML_Regs: gpr[16] (rax rbx rcx rdx rsi rdi rbp r8..r15, rflags), xmm[16]
S_RAX = 0
S_RBX = 8
S_RCX = 16
S_RDX = 24
S_RSI = 32
S_RDI = 40
S_RBP = 48
S_R8  = 56
S_R9  = 64
S_R10 = 72
S_R11 = 80
S_R12 = 88
S_R13 = 96
S_R14 = 104
S_R15 = 112
S_FLAGS = 120
S_XMM = 128

; locals: [rsp] home space, [rsp+32] out, [rsp+40] fn, [rsp+48] xmm6-15
HT_FRAME = 216

.code

; void HT_Call(const HT_State* in, HT_State* out, void* fn)
HT_Call PROC
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, HT_FRAME
    mov [rsp + 32], rdx
    mov [rsp + 40], r8
    movdqu [rsp + 48 + 0*16], xmm6
    movdqu [rsp + 48 + 1*16], xmm7
    movdqu [rsp + 48 + 2*16], xmm8
    movdqu [rsp + 48 + 3*16], xmm9
    movdqu [rsp + 48 + 4*16], xmm10
    movdqu [rsp + 48 + 5*16], xmm11
    movdqu [rsp + 48 + 6*16], xmm12
    movdqu [rsp + 48 + 7*16], xmm13
    movdqu [rsp + 48 + 8*16], xmm14
    movdqu [rsp + 48 + 9*16], xmm15

    movdqu xmm0, [rcx + S_XMM + 0*16]
    movdqu xmm1, [rcx + S_XMM + 1*16]
    movdqu xmm2, [rcx + S_XMM + 2*16]
    movdqu xmm3, [rcx + S_XMM + 3*16]
    movdqu xmm4, [rcx + S_XMM + 4*16]
    movdqu xmm5, [rcx + S_XMM + 5*16]
    movdqu xmm6, [rcx + S_XMM + 6*16]
    movdqu xmm7, [rcx + S_XMM + 7*16]
    movdqu xmm8, [rcx + S_XMM + 8*16]
    movdqu xmm9, [rcx + S_XMM + 9*16]
    movdqu xmm10, [rcx + S_XMM + 10*16]
    movdqu xmm11, [rcx + S_XMM + 11*16]
    movdqu xmm12, [rcx + S_XMM + 12*16]
    movdqu xmm13, [rcx + S_XMM + 13*16]
    movdqu xmm14, [rcx + S_XMM + 14*16]
    movdqu xmm15, [rcx + S_XMM + 15*16]
    push qword ptr [rcx + S_FLAGS]
    popfq
    mov rax, [rcx + S_RAX]
    mov rbx, [rcx + S_RBX]
    mov rdx, [rcx + S_RDX]
    mov rsi, [rcx + S_RSI]
    mov rdi, [rcx + S_RDI]
    mov rbp, [rcx + S_RBP]
    mov r8, [rcx + S_R8]
    mov r9, [rcx + S_R9]
    mov r10, [rcx + S_R10]
    mov r11, [rcx + S_R11]
    mov r12, [rcx + S_R12]
    mov r13, [rcx + S_R13]
    mov r14, [rcx + S_R14]
    mov r15, [rcx + S_R15]
    mov rcx, [rcx + S_RCX]

    call qword ptr [rsp + 40]

    pushfq
    pop qword ptr [rsp + 8]                 ; address computed after the pop: home slot 1
    mov [rsp], rax
    mov rax, [rsp + 32]
    mov [rax + S_RBX], rbx
    mov [rax + S_RCX], rcx
    mov [rax + S_RDX], rdx
    mov [rax + S_RSI], rsi
    mov [rax + S_RDI], rdi
    mov [rax + S_RBP], rbp
    mov [rax + S_R8], r8
    mov [rax + S_R9], r9
    mov [rax + S_R10], r10
    mov [rax + S_R11], r11
    mov [rax + S_R12], r12
    mov [rax + S_R13], r13
    mov [rax + S_R14], r14
    mov [rax + S_R15], r15
    mov rcx, [rsp]
    mov [rax + S_RAX], rcx
    mov rcx, [rsp + 8]
    mov [rax + S_FLAGS], rcx
    movdqu [rax + S_XMM + 0*16], xmm0
    movdqu [rax + S_XMM + 1*16], xmm1
    movdqu [rax + S_XMM + 2*16], xmm2
    movdqu [rax + S_XMM + 3*16], xmm3
    movdqu [rax + S_XMM + 4*16], xmm4
    movdqu [rax + S_XMM + 5*16], xmm5
    movdqu [rax + S_XMM + 6*16], xmm6
    movdqu [rax + S_XMM + 7*16], xmm7
    movdqu [rax + S_XMM + 8*16], xmm8
    movdqu [rax + S_XMM + 9*16], xmm9
    movdqu [rax + S_XMM + 10*16], xmm10
    movdqu [rax + S_XMM + 11*16], xmm11
    movdqu [rax + S_XMM + 12*16], xmm12
    movdqu [rax + S_XMM + 13*16], xmm13
    movdqu [rax + S_XMM + 14*16], xmm14
    movdqu [rax + S_XMM + 15*16], xmm15

    movdqu xmm6, [rsp + 48 + 0*16]
    movdqu xmm7, [rsp + 48 + 1*16]
    movdqu xmm8, [rsp + 48 + 2*16]
    movdqu xmm9, [rsp + 48 + 3*16]
    movdqu xmm10, [rsp + 48 + 4*16]
    movdqu xmm11, [rsp + 48 + 5*16]
    movdqu xmm12, [rsp + 48 + 6*16]
    movdqu xmm13, [rsp + 48 + 7*16]
    movdqu xmm14, [rsp + 48 + 8*16]
    movdqu xmm15, [rsp + 48 + 9*16]
    add rsp, HT_FRAME
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    cld
    ret
HT_Call ENDP

; Targets: each starts with enough plain instructions for MinHook's 5-byte patch.

ALIGN 16
HT_Nop PROC                                 ; touches nothing
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    ret
HT_Nop ENDP

ALIGN 16
HT_RetRcx PROC                              ; rax = rcx, xmm0 = xmm1
    mov rax, rcx
    movdqa xmm0, xmm1
    nop
    nop
    ret
HT_RetRcx ENDP

ALIGN 16
HT_Recurse PROC                             ; rax = rcx (counts down through the hooked entry); keeps rcx, uses flags
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    test rcx, rcx
    jz recurse_done
    dec rcx
    sub rsp, 40
    call HT_Recurse
    add rsp, 40
    inc rcx
    add rax, 1
    ret
recurse_done:
    xor eax, eax
    ret
HT_Recurse ENDP

; Overwrites every register a C function may (as a plain detour may).
ALIGN 16
HT_Clobber PROC
    mov rax, 0BADBADBADBADBADh
    mov rcx, rax
    mov rdx, rax
    mov r8, rax
    mov r9, rax
    mov r10, rax
    mov r11, rax
    movq xmm0, rax
    movq xmm1, rax
    movq xmm2, rax
    movq xmm3, rax
    movq xmm4, rax
    movq xmm5, rax
    xor eax, eax                            ; also changes flags
    ret
HT_Clobber ENDP

END
