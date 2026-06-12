use alloc::{sync::Arc, vec, vec::Vec};
use core::mem::size_of;

use ax_errno::{AxError, AxResult};
use ax_runtime::hal::cpu::uspace::UserContext;
use linux_raw_sys::net::sockaddr;

use crate::{
    file::{FileLike, get_file_like},
    mm::UserConstPtr,
    task::{
        AsThread,
        async_io::{
            AsyncContext, IO_REQ_QUEUE, IO_WQ, IoOp, IoRequest, MAX_IO_SIZE, REQ_QUEUE_CAP,
        },
    },
};

pub fn sys_async_setup(handler: usize) -> AxResult<isize> {
    if handler == 0 {
        return Err(AxError::InvalidInput);
    }
    let curr = ax_task::current();
    let thr = curr.as_thread();
    if thr.async_ctx.lock().is_some() {
        return Err(AxError::AlreadyExists);
    }
    let ctx = Arc::new(AsyncContext::new(handler));
    *thr.async_ctx.lock() = Some(ctx);
    Ok(0)
}

fn submit_io_request(
    op: IoOp,
    file: Arc<dyn FileLike>,
    buf: usize,
    n: usize,
    offset: i64,
    userdata: u64,
    kbuf: Vec<u8>,
) -> AxResult<isize> {
    let curr = ax_task::current();
    let thr = curr.as_thread();
    let ctx = thr.async_ctx.lock().clone().ok_or(AxError::InvalidInput)?;
    let req = IoRequest {
        op,
        file,
        userdata,
        buf,
        count: n,
        offset,
        ctx,
        kbuf,
    };
    let mut q = IO_REQ_QUEUE.lock();
    if q.len() >= REQ_QUEUE_CAP {
        return Err(AxError::WouldBlock);
    }
    q.push_back(req);
    drop(q);
    IO_WQ.notify_one(true);
    Ok(0)
}

pub fn sys_async_read(
    fd: i32,
    buf: usize,
    count: usize,
    offset: i64,
    userdata: u64,
) -> AxResult<isize> {
    let file = get_file_like(fd)?;
    if buf == 0 {
        return Err(AxError::BadAddress);
    }
    let n = count.min(MAX_IO_SIZE);
    submit_io_request(IoOp::Read, file, buf, n, offset, userdata, vec![0u8; n])
}

pub fn sys_async_write(
    fd: i32,
    buf: usize,
    count: usize,
    offset: i64,
    userdata: u64,
) -> AxResult<isize> {
    let file = get_file_like(fd)?;
    let n = count.min(MAX_IO_SIZE);
    let kbuf = UserConstPtr::<u8>::from(buf as *const u8)
        .get_as_slice(n)?
        .to_vec();
    submit_io_request(IoOp::Write, file, buf, n, offset, userdata, kbuf)
}

pub fn sys_async_connect(fd: i32, addr: usize, addrlen: u32, userdata: u64) -> AxResult<isize> {
    use axnet::{SocketAddrEx, SocketOps};

    use crate::syscall::net::addr::{SocketAddrExt, normalize_socket_addr_ex_for_ip_stack};

    let socket = crate::file::Socket::from_fd(fd)?;
    let user_addr = UserConstPtr::<sockaddr>::from(addr as *const sockaddr);
    let mut addr_ex = SocketAddrEx::read_from_user(user_addr, addrlen)?;

    if socket.ip_domain() == linux_raw_sys::net::AF_INET6 {
        addr_ex = normalize_socket_addr_ex_for_ip_stack(addr_ex, false)?;
    }

    socket.set_nonblocking(true)?;

    match socket.connect(addr_ex) {
        Ok(()) | Err(AxError::WouldBlock) => {
            submit_io_request(IoOp::Connect, socket, 0, 0, 0, userdata, Vec::new())
        }
        Err(e) => Err(e),
    }
}

/// Called by the CQ completion trampoline after the async handler returns.
/// Restores all caller-saved registers from the CqFrame on the user stack,
/// then returns to `user_return_loop` which delivers the next CQ entry.
pub fn sys_cq_return(uctx: &mut UserContext) -> AxResult<isize> {
    #[cfg(target_arch = "riscv64")]
    {
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

        let sp = uctx.sp();
        let mut frame: CqFrame = unsafe { core::mem::zeroed() };
        let frame_slice = unsafe {
            core::slice::from_raw_parts_mut(
                &mut frame as *mut CqFrame as *mut u8
                    as *mut core::mem::MaybeUninit<u8>,
                size_of::<CqFrame>(),
            )
        };
        starry_vm::vm_read_slice(sp as *const u8, frame_slice)?;

        uctx.set_ip(frame.sepc);
        uctx.regs.ra = frame.ra;
        uctx.regs.sp = frame.sp;
        uctx.regs.t0 = frame.t0;
        uctx.regs.t1 = frame.t1;
        uctx.regs.t2 = frame.t2;
        uctx.regs.a1 = frame.a1;
        uctx.regs.a2 = frame.a2;
        uctx.regs.a3 = frame.a3;
        uctx.regs.a4 = frame.a4;
        uctx.regs.a5 = frame.a5;
        uctx.regs.a6 = frame.a6;
        uctx.regs.a7 = frame.a7;
        uctx.regs.t3 = frame.t3;
        uctx.regs.t4 = frame.t4;
        uctx.regs.t5 = frame.t5;
        uctx.regs.t6 = frame.t6;
    }
    Ok(0)
}
