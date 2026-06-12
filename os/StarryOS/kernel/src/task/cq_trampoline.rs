core::arch::global_asm!(
    "
.section .text
.balign 4096
.global cq_trampoline
cq_trampoline:
    .cfi_startproc
    li a7, 467
    ecall
    .cfi_endproc

.fill 4096 - (. - cq_trampoline), 1, 0
"
);

pub fn cq_trampoline_address() -> usize {
    unsafe extern "C" {
        static cq_trampoline: u8;
    }
    unsafe { &cq_trampoline as *const u8 as usize }
}
