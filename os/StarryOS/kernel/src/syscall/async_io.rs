use alloc::{sync::Arc, vec, vec::Vec};

use ax_errno::{AxError, AxResult};
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
