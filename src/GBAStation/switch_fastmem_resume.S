.text
.p2align 4

.global SwitchFastmemResumeException
.type SwitchFastmemResumeException, %function
SwitchFastmemResumeException:
    mov x16, x0

    ldp q0, q1, [x16, #272]
    ldp q2, q3, [x16, #304]
    ldp q4, q5, [x16, #336]
    ldp q6, q7, [x16, #368]
    ldp q8, q9, [x16, #400]
    ldp q10, q11, [x16, #432]
    ldp q12, q13, [x16, #464]
    ldp q14, q15, [x16, #496]
    ldp q16, q17, [x16, #528]
    ldp q18, q19, [x16, #560]
    ldp q20, q21, [x16, #592]
    ldp q22, q23, [x16, #624]
    ldp q24, q25, [x16, #656]
    ldp q26, q27, [x16, #688]
    ldp q28, q29, [x16, #720]
    ldp q30, q31, [x16, #752]

    ldr x19, [x16, #784]
    msr nzcv, x19

    ldr x17, [x16, #256]
    ldr x18, [x16, #248]

    ldp x0, x1, [x16, #0]
    ldp x2, x3, [x16, #16]
    ldp x4, x5, [x16, #32]
    ldp x6, x7, [x16, #48]
    ldp x8, x9, [x16, #64]
    ldp x10, x11, [x16, #80]
    ldp x12, x13, [x16, #96]
    ldp x14, x15, [x16, #112]
    ldp x19, x20, [x16, #152]
    ldp x21, x22, [x16, #168]
    ldp x23, x24, [x16, #184]
    ldp x25, x26, [x16, #200]
    ldp x27, x28, [x16, #216]
    ldp x29, x30, [x16, #232]

    mov sp, x18
    br x17

.size SwitchFastmemResumeException, .-SwitchFastmemResumeException

.section .note.GNU-stack,"",@progbits
