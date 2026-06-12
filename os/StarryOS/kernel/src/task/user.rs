use ax_runtime::hal::cpu::{
    asm::user_copy,
    uspace::{ExceptionInfo, ExceptionKind, ReturnReason, UserContext},
};
use ax_task::TaskInner;
use starry_process::Pid;
use starry_signal::{SignalInfo, Signo};
use starry_vm::{VmMutPtr, VmPtr};
use syscalls::Sysno;
use core::mem::size_of;

use super::{
    AsThread, SyscallRestartInfo, SyscallTraceState, TimerState, check_signals,
    ptrace_stop_current, ptrace_syscall_stop_current, raise_signal_fatal, set_timer_state,
    unblock_next_signal,
};
use crate::syscall::{handle_syscall, syscall_allows_signal_restart};

/// Save return address in `uctx` and redirect `ip` to the async completion handler.
/// Returns false on stack manipulation failure (x86_64 only).
fn inject_handler_call(uctx: &mut UserContext, handler: usize, userdata: u64, result: i64) -> bool {
    #[cfg(target_arch = "riscv64")]
    {
        // Save all caller-saved registers in a CqFrame on the user stack,
        // then redirect the handler's return to a trampoline that re-enters
        // the kernel so sys_cq_return can restore the frame.
        //
        // Frame layout (144 bytes = 9 x 16-byte aligned):
        //   offset  field
        //     0     sepc     (original return address after ecall)
        //     8     ra
        //    16     sp
        //    24     t0
        //    32     t1
        //    40     t2
        //    48     a1
        //    56     a2
        //    64     a3
        //    72     a4
        //    80     a5
        //    88     a6
        //    96     a7
        //   104     t3
        //   112     t4
        //   120     t5
        //   128     t6
        //   136     (padding)

        #[repr(C)]
        #[derive(Clone, Copy)]
        struct CqFrame {
            sepc: usize,
            ra: usize,
            sp: usize,
            t0: usize,
            t1: usize,
            t2: usize,
            a1: usize,
            a2: usize,
            a3: usize,
            a4: usize,
            a5: usize,
            a6: usize,
            a7: usize,
            t3: usize,
            t4: usize,
            t5: usize,
            t6: usize,
            _padding: usize,
        }

        const FRAME_SIZE: usize = size_of::<CqFrame>();
        // FRAME_SIZE must be 144 (9 × 16).
        debug_assert!(FRAME_SIZE == 144);
        debug_assert!(FRAME_SIZE % 16 == 0);

        let saved_sepc = uctx.ip();
        let old_sp = uctx.sp();

        let Some(new_sp) = old_sp.checked_sub(FRAME_SIZE) else {
            return false;
        };
        uctx.set_sp(new_sp);

        let frame = CqFrame {
            sepc: saved_sepc,
            ra: uctx.regs.ra,
            sp: old_sp,
            t0: uctx.regs.t0,
            t1: uctx.regs.t1,
            t2: uctx.regs.t2,
            a1: uctx.regs.a1,
            a2: uctx.regs.a2,
            a3: uctx.regs.a3,
            a4: uctx.regs.a4,
            a5: uctx.regs.a5,
            a6: uctx.regs.a6,
            a7: uctx.regs.a7,
            t3: uctx.regs.t3,
            t4: uctx.regs.t4,
            t5: uctx.regs.t5,
            t6: uctx.regs.t6,
            _padding: 0,
        };

        unsafe {
            user_copy(
                new_sp as *mut u8,
                &frame as *const CqFrame as *const u8,
                FRAME_SIZE,
            );
        }

        uctx.set_ra(crate::config::CQ_TRAMPOLINE);
        uctx.set_ip(handler);
        uctx.set_arg0(userdata as usize);
        uctx.set_arg1(result as usize);
    }
    #[cfg(not(target_arch = "riscv64"))]
    {
        let ra = uctx.ip();
        uctx.set_ra(ra);
        uctx.set_ip(handler);
        uctx.set_arg0(userdata as usize);
        uctx.set_arg1(result as usize);
    }
    true
}

/// Create a new user task.
pub fn new_user_task(name: &str, mut uctx: UserContext, set_child_tid: usize) -> TaskInner {
    TaskInner::new(
        move || {
            let curr = ax_task::current();

            if let Some(tid) = (set_child_tid as *mut Pid).nullable() {
                tid.vm_write(curr.id().as_u64() as Pid).ok();
            }

            info!("Enter user space: ip={:#x}, sp={:#x}", uctx.ip(), uctx.sp());

            let thr = curr.as_thread();
            if thr.proc_data.ptrace_stop_signo().is_some() {
                let _ = ptrace_stop_current(thr, Signo::SIGSTOP, &mut uctx);
            }
            while !thr.pending_exit() {
                if thr.proc_data.is_ptrace_singlestep()
                    && (thr.proc_data.is_ptrace_traceme() || thr.proc_data.is_ptrace_attached())
                {
                    #[cfg(target_arch = "riscv64")]
                    crate::syscall::ptrace_setup_singlestep(&thr.proc_data, &mut uctx);
                }

                let reason = uctx.run();

                set_timer_state(&curr, TimerState::Kernel);

                let saved_a0 = uctx.arg0();
                let saved_sysno = uctx.sysno();
                let is_syscall = matches!(reason, ReturnReason::Syscall);

                match reason {
                    ReturnReason::Syscall => {
                        let trace_state = thr.proc_data.take_ptrace_syscall_trace();
                        if matches!(trace_state, SyscallTraceState::Entry)
                            && ptrace_syscall_stop_current(thr, Signo::SIGTRAP, &mut uctx).is_some()
                        {
                            match thr.proc_data.take_ptrace_syscall_trace() {
                                SyscallTraceState::Entry | SyscallTraceState::Exit => thr
                                    .proc_data
                                    .set_ptrace_syscall_trace_state(SyscallTraceState::Exit),
                                SyscallTraceState::None => {}
                            }
                        }

                        if let Some(exit_code) = ptrace_exit_event_code(saved_sysno, saved_a0)
                            && crate::syscall::ptrace_notify_exit(
                                thr.proc_data.proc.pid(),
                                exit_code,
                            )
                        {
                            let _ = ptrace_stop_current(thr, Signo::SIGTRAP, &mut uctx);
                        }

                        handle_syscall(&mut uctx);
                        if matches!(
                            thr.proc_data.take_ptrace_syscall_trace(),
                            SyscallTraceState::Exit
                        ) {
                            let _ = ptrace_syscall_stop_current(thr, Signo::SIGTRAP, &mut uctx);
                        }
                        if thr.proc_data.take_ptrace_exec_stop_pending() {
                            let _is_event =
                                crate::syscall::ptrace_notify_exec(thr.proc_data.proc.pid());
                            if let Some(_resume_sig) =
                                ptrace_stop_current(thr, Signo::SIGTRAP, &mut uctx)
                            {
                                continue;
                            }
                        }
                    }
                    ReturnReason::PageFault(addr, flags) => {
                        if !thr.proc_data.aspace().lock().handle_page_fault(addr, flags) {
                            info!(
                                "{:?}: segmentation fault at {:#x} {:?}",
                                thr.proc_data.proc, addr, flags
                            );
                            raise_signal_fatal(SignalInfo::new_kernel(Signo::SIGSEGV), &uctx)
                                .expect("Failed to send SIGSEGV");
                        }
                    }
                    ReturnReason::Interrupt => {
                        ax_task::yield_now();
                    }
                    #[allow(unused_labels)]
                    ReturnReason::Exception(exc_info) => 'exc: {
                        let kind = exc_info.kind();
                        if matches!(kind, ExceptionKind::Breakpoint)
                            && (thr.proc_data.is_ptrace_traceme()
                                || thr.proc_data.is_ptrace_attached())
                        {
                            let saved_insn = thr.proc_data.take_ptrace_ss_saved_insn();
                            if let Some((addr, insn)) = saved_insn {
                                if addr == uctx.ip() {
                                    let aspace = thr.proc_data.aspace();
                                    let aspace = aspace.lock();
                                    let _ = aspace.write(
                                        ax_memory_addr::VirtAddr::from_usize(addr),
                                        &(insn as u16).to_ne_bytes(),
                                    );
                                    #[cfg(target_arch = "riscv64")]
                                    ax_runtime::hal::cpu::asm::flush_icache_all();
                                } else {
                                    thr.proc_data.set_ptrace_ss_saved_insn(Some((addr, insn)));
                                }
                            }
                            if let Some(_resume_sig) =
                                ptrace_stop_current(thr, Signo::SIGTRAP, &mut uctx)
                            {
                                break 'exc;
                            }
                        }
                        warn!(
                            "user exception: ip={:#x}, fault_addr={:#x}, kind={:?}, esr={:#x}, \
                             ec={:#x}, iss={:#x}, info={:?}",
                            uctx.ip(),
                            exception_fault_addr(&exc_info),
                            kind,
                            exception_esr_value(&exc_info),
                            exception_ec_value(&exc_info),
                            exception_iss_value(&exc_info),
                            exc_info
                        );
                        let signo = match kind {
                            ExceptionKind::Misaligned => {
                                #[cfg(target_arch = "loongarch64")]
                                if unsafe { uctx.emulate_unaligned() }.is_ok() {
                                    break 'exc;
                                }
                                Signo::SIGBUS
                            }
                            ExceptionKind::Breakpoint => Signo::SIGTRAP,
                            ExceptionKind::IllegalInstruction => Signo::SIGILL,
                            _ => Signo::SIGTRAP,
                        };
                        raise_signal_fatal(SignalInfo::new_kernel(signo), &uctx)
                            .expect("Failed to send SIGTRAP");
                    }
                    r => {
                        warn!("Unexpected return reason: {r:?}");
                        raise_signal_fatal(SignalInfo::new_kernel(Signo::SIGSEGV), &uctx)
                            .expect("Failed to send SIGSEGV");
                    }
                }

                // CQ injection: deliver one pending async I/O completion.
                if let Some(ctx) = thr.async_ctx.lock().clone()
                    && let Some(entry) = ctx.pop_one()
                {
                    // Copy kernel bounce buffer → user buf (user CR3 is active).
                    if entry.result > 0 && entry.buf != 0 {
                        unsafe {
                            user_copy(
                                entry.buf as *mut u8,
                                entry.kbuf.as_ptr(),
                                entry.result as usize,
                            );
                        }
                    }

                    if !inject_handler_call(&mut uctx, ctx.handler, entry.userdata, entry.result) {
                        continue;
                    }

                    set_timer_state(&curr, TimerState::User);
                    curr.clear_interrupt();
                    continue;
                }

                if !unblock_next_signal() {
                    let eintr_code = -(ax_errno::LinuxError::EINTR.code() as isize);
                    let restart = if is_syscall
                        && (uctx.retval() as isize) == eintr_code
                        && syscall_allows_signal_restart(saved_sysno)
                    {
                        Some(SyscallRestartInfo {
                            saved_a0,
                            saved_sysno,
                        })
                    } else {
                        None
                    };
                    // Single-shot: the first delivered signal decides
                    // whether to restart. Subsequent signals in the same
                    // loop must not re-apply the decision.
                    let mut pending_restart = restart.as_ref();
                    while check_signals(thr, &mut uctx, None, pending_restart) {
                        pending_restart = None;
                    }
                }

                set_timer_state(&curr, TimerState::User);
                curr.clear_interrupt();
            }
        },
        name.into(),
        crate::config::KERNEL_STACK_SIZE,
    )
}

fn ptrace_exit_event_code(sysno: usize, arg0: usize) -> Option<i32> {
    match Sysno::new(sysno) {
        Some(Sysno::exit | Sysno::exit_group) => Some((arg0 as i32) << 8),
        _ => None,
    }
}

#[cfg(target_arch = "aarch64")]
fn exception_fault_addr(exc_info: &ExceptionInfo) -> usize {
    exc_info.far
}

#[cfg(target_arch = "aarch64")]
fn exception_esr_value(exc_info: &ExceptionInfo) -> u64 {
    exc_info.esr_value()
}

#[cfg(target_arch = "aarch64")]
fn exception_ec_value(exc_info: &ExceptionInfo) -> u64 {
    exc_info.ec_value()
}

#[cfg(target_arch = "aarch64")]
fn exception_iss_value(exc_info: &ExceptionInfo) -> u64 {
    exc_info.iss_value()
}

#[cfg(target_arch = "riscv64")]
fn exception_fault_addr(exc_info: &ExceptionInfo) -> usize {
    exc_info.stval
}

#[cfg(target_arch = "riscv64")]
fn exception_esr_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}

#[cfg(target_arch = "riscv64")]
fn exception_ec_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}

#[cfg(target_arch = "riscv64")]
fn exception_iss_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}

#[cfg(target_arch = "loongarch64")]
fn exception_fault_addr(exc_info: &ExceptionInfo) -> usize {
    exc_info.badv
}

#[cfg(target_arch = "loongarch64")]
fn exception_esr_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}

#[cfg(target_arch = "loongarch64")]
fn exception_ec_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}

#[cfg(target_arch = "loongarch64")]
fn exception_iss_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}

#[cfg(target_arch = "x86_64")]
fn exception_fault_addr(exc_info: &ExceptionInfo) -> usize {
    exc_info.cr2
}

#[cfg(target_arch = "x86_64")]
fn exception_esr_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}

#[cfg(target_arch = "x86_64")]
fn exception_ec_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}

#[cfg(target_arch = "x86_64")]
fn exception_iss_value(_exc_info: &ExceptionInfo) -> u64 {
    0
}
