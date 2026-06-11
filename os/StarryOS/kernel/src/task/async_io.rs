use alloc::{collections::VecDeque, string::String, sync::Arc, vec, vec::Vec};

use ax_kspin::SpinNoIrq;
use ax_task::{WaitQueue, spawn_raw};

use crate::file::FileLike;

const CQ_CAP: usize = 256;

// ---------- Completion Queue ----------

/// A completed async I/O entry.
pub struct CqEntry {
    pub userdata: u64,
    /// >0 = bytes transferred, 0 = EOF, <0 = -errno
    pub result: i64,
    pub buf: usize,
    pub count: usize,
    /// Kernel bounce buffer (for reads: data to copy to user).
    pub kbuf: Vec<u8>,
}

/// Per-thread async I/O context.
pub struct AsyncContext {
    pub cq: SpinNoIrq<VecDeque<CqEntry>>,
    pub handler: usize,
}

unsafe impl Send for AsyncContext {}
unsafe impl Sync for AsyncContext {}

impl AsyncContext {
    pub fn new(handler: usize) -> Self {
        Self {
            cq: SpinNoIrq::new(VecDeque::with_capacity(CQ_CAP)),
            handler,
        }
    }

    pub fn push_completion(&self, entry: CqEntry) {
        let mut cq = self.cq.lock();
        if cq.len() < CQ_CAP {
            cq.push_back(entry);
        }
    }

    pub fn pop_one(&self) -> Option<CqEntry> {
        self.cq.lock().pop_front()
    }
}

// ---------- Request / IO worker ----------

pub(crate) const MAX_IO_SIZE: usize = 65536;

pub enum IoOp {
    Read,
    Write,
}

/// A submitted I/O request.
///
/// Semantics:
/// - Read: `kbuf` is allocated by syscall handler. IO worker reads into kbuf, then pushes
///   CqEntry that owns kbuf. CQ delivery copies kbuf→user buf.
/// - Write: syscall handler fills kbuf from user buf (via user_copy with user CR3). IO worker
///   writes kbuf to file.
pub struct IoRequest {
    pub op: IoOp,
    pub file: Arc<dyn FileLike>,
    pub userdata: u64,
    pub buf: usize,
    pub count: usize,
    /// -1 = use file position, >=0 = fixed offset (pread/pwrite semantics)
    pub offset: i64,
    pub ctx: Arc<AsyncContext>,
    pub kbuf: Vec<u8>,
}

unsafe impl Send for IoRequest {}

pub(crate) const REQ_QUEUE_CAP: usize = 256;
pub(crate) static IO_REQ_QUEUE: SpinNoIrq<VecDeque<IoRequest>> = SpinNoIrq::new(VecDeque::new());
pub(crate) static IO_WQ: WaitQueue = WaitQueue::new();

/// Spawn the IO worker. Call once during boot.
pub fn init() {
    spawn_raw(io_worker_main, String::from("async-io"), 0x10000);
}

fn io_worker_main() {
    use alloc::sync::Arc as StdArc;
    use core::task::{Context, Waker};

    use axpoll::IoEvents;

    struct IoWaker;
    impl alloc::task::Wake for IoWaker {
        fn wake(self: StdArc<Self>) {
            IO_WQ.notify_one(false);
        }
    }

    let waker: Waker = Waker::from(StdArc::new(IoWaker));
    let mut cx = Context::from_waker(&waker);
    let mut pending: Vec<IoRequest> = Vec::new();

    loop {
        // 1. Accept new submissions.
        while let Some(req) = IO_REQ_QUEUE.lock().pop_front() {
            let _ = req.file.set_nonblocking(true);
            pending.push(req);
        }

        // 2. Register wakers.
        for req in &pending {
            let ev = match req.op {
                IoOp::Read => IoEvents::IN,
                IoOp::Write => IoEvents::OUT,
            };
            req.file.register(&mut cx, ev);
        }

        // 3. Poll and process ready ones.
        let mut i = 0;
        while i < pending.len() {
            let ev = match pending[i].op {
                IoOp::Read => IoEvents::IN,
                IoOp::Write => IoEvents::OUT,
            };
            let ready = pending[i].file.poll().intersects(ev);
            if ready {
                let req = pending.swap_remove(i);
                let entry = do_io(&req);
                req.ctx.push_completion(entry);
                IO_WQ.notify_one(true);
            } else {
                i += 1;
            }
        }

        // 4. Wait.
        IO_WQ.wait();
    }
}

/// Execute one I/O and produce a CQ entry.
fn do_io(req: &IoRequest) -> CqEntry {
    match req.op {
        IoOp::Read => {
            let mut kbuf = vec![0u8; req.count];
            let result = if req.offset >= 0 {
                match req.file.read_at(&mut &mut kbuf[..req.count], req.offset as u64) {
                    Ok(m) => m as i64,
                    Err(e) => -(e.code() as i64),
                }
            } else {
                match req.file.read(&mut &mut kbuf[..req.count]) {
                    Ok(m) => m as i64,
                    Err(e) => -(e.code() as i64),
                }
            };
            CqEntry {
                userdata: req.userdata,
                result,
                buf: req.buf,
                count: req.count,
                kbuf,
            }
        }
        IoOp::Write => {
            let result = if req.offset >= 0 {
                match req.file.write_at(&mut &req.kbuf[..req.count], req.offset as u64) {
                    Ok(m) => m as i64,
                    Err(e) => -(e.code() as i64),
                }
            } else {
                match req.file.write(&mut &req.kbuf[..req.count]) {
                    Ok(m) => m as i64,
                    Err(e) => -(e.code() as i64),
                }
            };
            CqEntry {
                userdata: req.userdata,
                result,
                buf: 0,
                count: 0,
                kbuf: Vec::new(),
            }
        }
    }
}
