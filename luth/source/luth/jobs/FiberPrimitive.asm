; ────────────────────────────────────────────────────────────────────────────
;  FiberPrimitive.asm — x86_64 Windows MSVC-ABI fiber context switch for Luth.
;  Replaces CreateFiberEx + SwitchToFiber so ASan can track per-fiber stack
;  bounds (Win32 fibers expose no bounds before the first switch).
;
;  Save area (280 bytes, relative to the saved RSP value):
;    +0    RBP, RBX, RDI, RSI, R12, R13, R14, R15        (8 × 8 = 64 B)
;    +64   XMM6..XMM15                                   (10 × 16 = 160 B)
;    +224  MXCSR (4) + pad (4)                           (8 B)
;    +232  NT_TIB StackBase            (gs:[0x08])       (8 B)
;    +240  NT_TIB StackLimit           (gs:[0x10])       (8 B)
;    +248  NT_TIB DeallocationStack    (gs:[0x1478])     (8 B)
;    +256  NT_TIB FiberData            (gs:[0x20])       (8 B)
;    +264  NT_TIB ArbitraryUserPointer (gs:[0x28])       (8 B)
;    +272  pad to 280                                    (8 B)
;
;  Above each fiber's save area sits a 16-aligned return-address slot. For a
;  freshly-made fiber this holds fiber_entry_trampoline; on subsequent yields
;  it holds the saved RIP of the call site that invoked jump_fcontext.
; ────────────────────────────────────────────────────────────────────────────

.code

PUBLIC make_fcontext
PUBLIC jump_fcontext

; ──── jump_fcontext(out_from_sp: rcx, to_sp: rdx) ───────────────────────────

jump_fcontext PROC
    sub rsp, 280

    ; GPRs
    mov [rsp +   0], rbp
    mov [rsp +   8], rbx
    mov [rsp +  16], rdi
    mov [rsp +  24], rsi
    mov [rsp +  32], r12
    mov [rsp +  40], r13
    mov [rsp +  48], r14
    mov [rsp +  56], r15

    ; XMM6..XMM15
    movaps [rsp +  64], xmm6
    movaps [rsp +  80], xmm7
    movaps [rsp +  96], xmm8
    movaps [rsp + 112], xmm9
    movaps [rsp + 128], xmm10
    movaps [rsp + 144], xmm11
    movaps [rsp + 160], xmm12
    movaps [rsp + 176], xmm13
    movaps [rsp + 192], xmm14
    movaps [rsp + 208], xmm15

    ; MXCSR
    stmxcsr [rsp + 224]

    ; NT_TIB fields (gs-relative on x86_64 Windows)
    mov rax, gs:[8]
    mov [rsp + 232], rax
    mov rax, gs:[16]
    mov [rsp + 240], rax
    mov rax, gs:[5240]              ; 0x1478 = DeallocationStack
    mov [rsp + 248], rax
    mov rax, gs:[32]                ; 0x20 = FiberData
    mov [rsp + 256], rax
    mov rax, gs:[40]                ; 0x28 = ArbitraryUserPointer
    mov [rsp + 264], rax

    ; Publish from's saved SP, switch stacks
    mov [rcx], rsp
    mov rsp, rdx

    ; Restore NT_TIB
    mov rax, [rsp + 232]
    mov gs:[8], rax
    mov rax, [rsp + 240]
    mov gs:[16], rax
    mov rax, [rsp + 248]
    mov gs:[5240], rax
    mov rax, [rsp + 256]
    mov gs:[32], rax
    mov rax, [rsp + 264]
    mov gs:[40], rax

    ; Restore MXCSR
    ldmxcsr [rsp + 224]

    ; Restore XMM
    movaps xmm6,  [rsp +  64]
    movaps xmm7,  [rsp +  80]
    movaps xmm8,  [rsp +  96]
    movaps xmm9,  [rsp + 112]
    movaps xmm10, [rsp + 128]
    movaps xmm11, [rsp + 144]
    movaps xmm12, [rsp + 160]
    movaps xmm13, [rsp + 176]
    movaps xmm14, [rsp + 192]
    movaps xmm15, [rsp + 208]

    ; Restore GPRs
    mov rbp, [rsp +   0]
    mov rbx, [rsp +   8]
    mov rdi, [rsp +  16]
    mov rsi, [rsp +  24]
    mov r12, [rsp +  32]
    mov r13, [rsp +  40]
    mov r14, [rsp +  48]
    mov r15, [rsp +  56]

    add rsp, 280
    ret
jump_fcontext ENDP


; ──── make_fcontext(stack_top: rcx, usable_size: rdx, entry: r8, args: r9) ──
; Lays down an initial save area so the first jump_fcontext to the returned
; context resumes at fiber_entry_trampoline, which then calls entry(args).

make_fcontext PROC
    ; Round stack_top down to 16-aligned, then leave the topmost 16 bytes free
    ; so the return-address slot lands on a 16-aligned address (X). After the
    ; trampoline is reached via ret, RSP = X + 8 = 16n + 8 (Win64 ABI).
    mov rax, rcx
    and rax, NOT 0Fh
    sub rax, 16

    ; Plant trampoline address at [X]
    lea r10, fiber_entry_trampoline
    mov [rax], r10

    ; Initial RSP for this fiber points at the bottom of the save area.
    sub rax, 280

    ; GPR slots — zero except R12 = entry_fn, R13 = args. The trampoline below
    ; reads those after jump_fcontext restores them.
    mov qword ptr [rax +   0], 0     ; RBP
    mov qword ptr [rax +   8], 0     ; RBX
    mov qword ptr [rax +  16], 0     ; RDI
    mov qword ptr [rax +  24], 0     ; RSI
    mov [rax +  32], r8                ; R12 = entry
    mov [rax +  40], r9                ; R13 = args
    mov qword ptr [rax +  48], 0     ; R14
    mov qword ptr [rax +  56], 0     ; R15

    ; XMM slots — zeroed
    pxor xmm0, xmm0
    movaps [rax +  64], xmm0
    movaps [rax +  80], xmm0
    movaps [rax +  96], xmm0
    movaps [rax + 112], xmm0
    movaps [rax + 128], xmm0
    movaps [rax + 144], xmm0
    movaps [rax + 160], xmm0
    movaps [rax + 176], xmm0
    movaps [rax + 192], xmm0
    movaps [rax + 208], xmm0

    ; MXCSR = 0x1F80 (default, all SSE exceptions masked, round-to-nearest)
    mov dword ptr [rax + 224], 1F80h
    mov dword ptr [rax + 228], 0       ; pad

    ; NT_TIB slots — describe the new fiber's stack to anything reading TEB.
    mov [rax + 232], rcx               ; StackBase = stack_top
    mov r10, rcx
    sub r10, rdx
    mov [rax + 240], r10               ; StackLimit = stack_top - usable_size
    mov [rax + 248], r10               ; DeallocationStack mirrors StackLimit
    mov qword ptr [rax + 256], 0     ; FiberData = 0 (not a Win32 fiber)
    mov qword ptr [rax + 264], 0     ; ArbitraryUserPointer = 0 (set by caller)
    mov qword ptr [rax + 272], 0     ; pad

    ; rax holds initial_rsp — return value
    ret
make_fcontext ENDP


; ──── fiber_entry_trampoline ────────────────────────────────────────────────
; Reached via ret from jump_fcontext on the first switch into a freshly-made
; fiber. R12 = entry, R13 = args (placed by make_fcontext, restored by jump).
; RSP is 16n+8 here (Win64 ABI). Reserve shadow space (32 B) before calling
; entry. If entry ever returns, the fiber is abandoned — trap.

fiber_entry_trampoline PROC
    sub rsp, 32                        ; Win64 shadow space for call
    mov rcx, r13                       ; arg 1 = args
    call r12                           ; entry(args)
    int 3                              ; entry must SwitchTo before returning
fiber_entry_trampoline ENDP

END
