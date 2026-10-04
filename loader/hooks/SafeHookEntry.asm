; Register-preserving hook entry/exit, see SafeHook in SafeHook.cpp and "safe hooks" in dk2ml.h.
;
; The game is built with link-time code generation, so callers may keep values in registers the x64 ABI calls
; volatile across a call. These routines save every register (GPRs, rflags, xmm0-15), call into C++, restore every
; register (with whatever the callbacks changed) and continue exactly as the caller set things up.
;
; Per-hook stub (generated in SafeHook.cpp):   push rax / mov rax, record / xchg [rsp], rax / jmp SafeHookEntry
; so on entry here: [rsp] = record, [rsp+8] = return address, all registers = the caller's.

EXTERN SafeHook_Pre:PROC   ; int SafeHook_Pre(DK2ML_Regs*, SafeHookRecord*): 0 call original, 1 skip
EXTERN SafeHook_Post:PROC  ; uint64_t SafeHook_Post(DK2ML_Regs*): returns the real return address

; DK2ML_Regs layout (dk2ml.h)
R_RAX    = 0
R_RBX    = 8
R_RCX    = 16
R_RDX    = 24
R_RSI    = 32
R_RDI    = 40
R_RBP    = 48
R_R8     = 56
R_R9     = 64
R_R10    = 72
R_R11    = 80
R_R12    = 88
R_R13    = 96
R_R14    = 104
R_R15    = 112
R_FLAGS  = 120
R_XMM    = 128
R_STACK  = 384
R_SCRATCH = 392
REGS_SPACE = 432           ; sizeof(DK2ML_Regs) = 424, rounded up

REGS     = 32              ; DK2ML_Regs lives above the 32-byte home space for the C++ calls
ENTRY_FRAME = 32 + REGS_SPACE + 8   ; = 472, keeps rsp 16-aligned at the call (entry rsp is 8 mod 16 after pushfq)
POST_FRAME  = 32 + REGS_SPACE       ; = 464 (post entry: rsp 0 mod 16, then sub 8 + pushfq)

SAVE_REGS MACRO frame
    mov [rsp + REGS + R_RAX], rax
    mov [rsp + REGS + R_RBX], rbx
    mov [rsp + REGS + R_RCX], rcx
    mov [rsp + REGS + R_RDX], rdx
    mov [rsp + REGS + R_RSI], rsi
    mov [rsp + REGS + R_RDI], rdi
    mov [rsp + REGS + R_RBP], rbp
    mov [rsp + REGS + R_R8], r8
    mov [rsp + REGS + R_R9], r9
    mov [rsp + REGS + R_R10], r10
    mov [rsp + REGS + R_R11], r11
    mov [rsp + REGS + R_R12], r12
    mov [rsp + REGS + R_R13], r13
    mov [rsp + REGS + R_R14], r14
    mov [rsp + REGS + R_R15], r15
    mov rax, [rsp + frame]              ; rflags pushed by pushfq
    mov [rsp + REGS + R_FLAGS], rax
    movdqu [rsp + REGS + R_XMM + 0*16], xmm0
    movdqu [rsp + REGS + R_XMM + 1*16], xmm1
    movdqu [rsp + REGS + R_XMM + 2*16], xmm2
    movdqu [rsp + REGS + R_XMM + 3*16], xmm3
    movdqu [rsp + REGS + R_XMM + 4*16], xmm4
    movdqu [rsp + REGS + R_XMM + 5*16], xmm5
    movdqu [rsp + REGS + R_XMM + 6*16], xmm6
    movdqu [rsp + REGS + R_XMM + 7*16], xmm7
    movdqu [rsp + REGS + R_XMM + 8*16], xmm8
    movdqu [rsp + REGS + R_XMM + 9*16], xmm9
    movdqu [rsp + REGS + R_XMM + 10*16], xmm10
    movdqu [rsp + REGS + R_XMM + 11*16], xmm11
    movdqu [rsp + REGS + R_XMM + 12*16], xmm12
    movdqu [rsp + REGS + R_XMM + 13*16], xmm13
    movdqu [rsp + REGS + R_XMM + 14*16], xmm14
    movdqu [rsp + REGS + R_XMM + 15*16], xmm15
    xor eax, eax
    mov [rsp + REGS + R_SCRATCH + 0], rax
    mov [rsp + REGS + R_SCRATCH + 8], rax
    mov [rsp + REGS + R_SCRATCH + 16], rax
    mov [rsp + REGS + R_SCRATCH + 24], rax
    cld                                 ; ABI requirement for the C++ call
ENDM

; restores everything from DK2ML_Regs; rflags goes back into the pushfq slot (popped by the caller of the macro)
RESTORE_REGS MACRO frame
    mov rax, [rsp + REGS + R_FLAGS]
    mov [rsp + frame], rax
    movdqu xmm0, [rsp + REGS + R_XMM + 0*16]
    movdqu xmm1, [rsp + REGS + R_XMM + 1*16]
    movdqu xmm2, [rsp + REGS + R_XMM + 2*16]
    movdqu xmm3, [rsp + REGS + R_XMM + 3*16]
    movdqu xmm4, [rsp + REGS + R_XMM + 4*16]
    movdqu xmm5, [rsp + REGS + R_XMM + 5*16]
    movdqu xmm6, [rsp + REGS + R_XMM + 6*16]
    movdqu xmm7, [rsp + REGS + R_XMM + 7*16]
    movdqu xmm8, [rsp + REGS + R_XMM + 8*16]
    movdqu xmm9, [rsp + REGS + R_XMM + 9*16]
    movdqu xmm10, [rsp + REGS + R_XMM + 10*16]
    movdqu xmm11, [rsp + REGS + R_XMM + 11*16]
    movdqu xmm12, [rsp + REGS + R_XMM + 12*16]
    movdqu xmm13, [rsp + REGS + R_XMM + 13*16]
    movdqu xmm14, [rsp + REGS + R_XMM + 14*16]
    movdqu xmm15, [rsp + REGS + R_XMM + 15*16]
    mov rbx, [rsp + REGS + R_RBX]
    mov rcx, [rsp + REGS + R_RCX]
    mov rdx, [rsp + REGS + R_RDX]
    mov rsi, [rsp + REGS + R_RSI]
    mov rdi, [rsp + REGS + R_RDI]
    mov rbp, [rsp + REGS + R_RBP]
    mov r8, [rsp + REGS + R_R8]
    mov r9, [rsp + REGS + R_R9]
    mov r10, [rsp + REGS + R_R10]
    mov r11, [rsp + REGS + R_R11]
    mov r12, [rsp + REGS + R_R12]
    mov r13, [rsp + REGS + R_R13]
    mov r14, [rsp + REGS + R_R14]
    mov r15, [rsp + REGS + R_R15]
    mov rax, [rsp + REGS + R_RAX]
ENDM

.code

; [rsp] = record, [rsp+8] = return address
SafeHookEntry PROC
    pushfq                                          ; [rsp] flags, [rsp+8] record, [rsp+16] return address
    sub rsp, ENTRY_FRAME
    SAVE_REGS ENTRY_FRAME
    lea rax, [rsp + ENTRY_FRAME + 16]               ; -> return address (stack[0])
    mov [rsp + REGS + R_STACK], rax

    lea rcx, [rsp + REGS]
    mov rdx, [rsp + ENTRY_FRAME + 8]                ; record
    call SafeHook_Pre
    test eax, eax
    jnz skip_original

    ; continue into the original: its trampoline goes into the record slot and `ret` jumps there
    mov rax, [rsp + ENTRY_FRAME + 8]
    mov rax, [rax]                                  ; SafeHookRecord::trampoline (first field)
    mov [rsp + ENTRY_FRAME + 8], rax
    RESTORE_REGS ENTRY_FRAME
    add rsp, ENTRY_FRAME
    popfq
    ret                                             ; -> trampoline, with [rsp] = return address

skip_original:
    RESTORE_REGS ENTRY_FRAME
    add rsp, ENTRY_FRAME
    popfq
    lea rsp, [rsp + 8]                              ; drop the record slot (lea keeps the restored flags)
    ret                                             ; -> caller
SafeHookEntry ENDP

; The original returns here when the hook has a post callback (SafeHook_Pre replaced its return address).
SafeHookPostEntry PROC
    lea rsp, [rsp - 8]                              ; slot for the real return address (lea: flags not saved yet)
    pushfq
    sub rsp, POST_FRAME
    SAVE_REGS POST_FRAME
    lea rax, [rsp + POST_FRAME + 8]
    mov [rsp + REGS + R_STACK], rax

    lea rcx, [rsp + REGS]
    call SafeHook_Post                              ; runs post, returns the real return address
    mov [rsp + POST_FRAME + 8], rax
    RESTORE_REGS POST_FRAME
    add rsp, POST_FRAME
    popfq
    ret                                             ; -> caller
SafeHookPostEntry ENDP

END
