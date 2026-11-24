// @file
// Copyright (c) 2026, Cory Bennett. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

// Each wrapper loads CoreDisplay's input registers and records the values
// live after the mode check in ModeResult, as defined in display_patch_test.py.
.text
.macro run_mode name, file
.globl _\name
_\name:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rdi, %rbp
#if defined(BRANCH_R12) || defined(BRANCH_R13)
    movq %rsi, %rbx
    movl %ecx, %r15d
#if defined(BRANCH_R12)
    movq (%r9), %r12
#else
    movq (%r9), %r13
#endif
#elif defined(PACKED)
    pcmpeqd %xmm0, %xmm0
    movl %edx, %r15d
    movl %ecx, %r13d
#elif defined(FORMAT_REGISTER)
    movl %edx, %ebx
    movl %ecx, %r14d
    movl 8(%r9), %r12d
#elif defined(BRANCH)
    movq %rsi, %rbx
    movl %ecx, %r15d
#elif defined(SETCC)
    movl %edx, %r14d
    movl %ecx, %ebx
#elif defined(REGISTER_FLAGS)
    movl %edx, %r15d
    movl %ecx, %r14d
#else
#error Unknown CoreDisplay register layout
#endif
#if defined(PACKED)
    // PCMPGTD reads its thresholds from another page. Point its RIP-relative
    // operand at the copy of that vector in this test library.
    .incbin "\file", 0, VECTOR_DISP
    .long L_thresholds - . - 4
    .incbin "\file", VECTOR_DISP + 4
#else
    .incbin "\file", BODY_START
#endif
#if defined(BRANCH_R12) || defined(BRANCH_R13)
#if defined(BRANCH_R12)
    movq %r12, (%r8)
#else
    movq %r13, (%r8)
#endif
    movq %rdi, 8(%r8)
    movl %r15d, 16(%r8)
    movl %eax, 20(%r8)
    movl %ecx, 24(%r8)
#elif defined(PACKED)
    movdqu %xmm0, 32(%r8)
    movq %rdx, (%r8)
    movl %eax, 8(%r8)
    movl %r13d, 16(%r8)
    movl %r15d, 20(%r8)
#elif defined(FORMAT_REGISTER)
    movq %rdi, (%r8)
    movl %eax, 8(%r8)
    movl %r14d, 16(%r8)
    movl %ebx, 20(%r8)
    movl %r12d, 24(%r8)
#elif defined(BRANCH)
    movq %rsi, (%r8)
    movl %eax, 8(%r8)
    movl %r15d, 16(%r8)
    movl %ecx, 20(%r8)
#elif defined(SETCC)
    movq %rdx, (%r8)
    movl %edi, 8(%r8)
    movl %ebx, 16(%r8)
    movl %eax, 20(%r8)
#elif defined(REGISTER_FLAGS)
    movq %rax, (%r8)
    movl %ecx, 8(%r8)
    movl %r14d, 16(%r8)
    movl %r15d, 20(%r8)
#endif
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.endm

run_mode original, original.bin
run_mode patched, patched.bin
#ifdef VALIDATED_REPLACEMENT
run_mode validated, validated.bin
#endif

#ifdef PACKED
.p2align 4
L_thresholds:
    .incbin "thresholds.bin"
#endif
