// Speed Demon 2026 - Track A - V48
//
// Architecture (no per-group Vec, no HashMap merge, no comparison sort):
//   1. mmap input (MAP_POPULATE), split into one newline-aligned chunk per worker.
//   2. PARSE: each worker scans its chunk once and appends one packed u64 record
//      per request (latency | local endpoint id | hour | is_5xx). Endpoints are
//      interned in a tiny per-worker open-addressing table (no string copies:
//      endpoint slices point straight into the mmap).
//   3. Main thread: union of all endpoint strings -> sorted unique list. An
//      endpoint's rank in that list IS its output sort order.
//   4. COUNT: each worker rewrites its records to (key | latency) with
//      key = hour * E + rank, and builds a per-worker histogram over keys
//      (count in the low 32 bits, 5xx errors in the high 32 bits).
//   5. PREFIX: key-range-parallel exclusive prefix sum over (key, worker) ->
//      every (key, worker) pair gets a private write offset into ONE array.
//      Also produces per-key error totals.
//   6. SCATTER: each worker writes its latencies into the contiguous array
//      (counting sort by key). Group slices are now contiguous and already in
//      (hour, endpoint) output order.
//   7. RENDER: threads take element-balanced key ranges, compute P50/P95/P99
//      with chained quickselect, format rows with raw pointer writes.
//   8. Write parts sequentially, exit(0) without freeing anything.
//
// Usage: track-a-ingest [input=/data/requests.log] [output=/out/summary.txt]
// Env:   SD_TIMING=1 prints phase times; SD_WORKERS=n overrides thread count;
//        SD_NOPOPULATE=1 disables MAP_POPULATE (for A/B testing).

use std::ffi::c_void;
use std::fs::{self, File};
use std::io::{self, Write};
use std::os::fd::AsRawFd;
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Barrier;
use std::thread;
use std::time::Instant;

const HOURS: usize = 72;
const TABLE: usize = 4096;
const MASK: usize = TABLE - 1;
const MAX_LOCAL_EPS: usize = TABLE * 3 / 4;
const MIN_LINE: usize = 36;

const PROT_READ: i32 = 1;
const MAP_PRIVATE: i32 = 2;
const MAP_POPULATE: i32 = 0x8000;

unsafe extern "C" {
    fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: i32,
        flags: i32,
        fd: i32,
        offset: isize,
    ) -> *mut c_void;
    fn memchr(s: *const c_void, c: i32, n: usize) -> *mut c_void;
    fn memrchr(s: *const c_void, c: i32, n: usize) -> *mut c_void;
}

struct Chunk {
    recs: Vec<u64>,
    eps: Vec<&'static [u8]>,
}

#[cold]
#[inline(never)]
fn bad_hour(h: usize) -> ! {
    panic!("timestamp outside the supported 72-hour window (index {h})");
}

#[cold]
#[inline(never)]
fn too_many_endpoints() -> ! {
    panic!("too many distinct endpoints in one chunk");
}

#[inline(always)]
unsafe fn hour_of(line: *const u8) -> usize {
    let d = |i: usize| unsafe { *line.add(i) }.wrapping_sub(b'0') as usize;
    let month = d(5) * 10 + d(6);
    let day = d(8) * 10 + d(9);
    let hr = d(11) * 10 + d(12);
    let off = if month == 10 {
        day.wrapping_sub(30)
    } else {
        day.wrapping_add(1)
    };
    let h = off
        .wrapping_mul(24)
        .wrapping_add(hr)
        .wrapping_sub(18);
    if h >= HOURS {
        bad_hour(h);
    }
    h
}

#[inline(always)]
fn hash_ep(b: &[u8]) -> u64 {
    const K: u64 = 0x9E37_79B9_7F4A_7C15;
    let n = b.len();
    let p = b.as_ptr();
    let mut h = (n as u64).wrapping_mul(K);
    unsafe {
        if n >= 8 {
            let mut i = 0usize;
            while i + 8 <= n {
                let w = (p.add(i) as *const u64).read_unaligned();
                h = (h.rotate_left(5) ^ w).wrapping_mul(K);
                i += 8;
            }
            if i < n {
                let w = (p.add(n - 8) as *const u64).read_unaligned();
                h = (h.rotate_left(5) ^ w).wrapping_mul(K);
            }
        } else {
            for i in 0..n {
                h = (h.rotate_left(5) ^ (*p.add(i) as u64)).wrapping_mul(K);
            }
        }
    }
    h ^= h >> 29;
    if h == 0 { 1 } else { h }
}

fn parse_chunk(data: &'static [u8]) -> Chunk {
    let len = data.len();
    let base = data.as_ptr();
    let mut recs: Vec<u64> = Vec::with_capacity(len / 40 + 16);
    let mut table = vec![0u64; TABLE];
    let mut eps: Vec<&'static [u8]> = Vec::with_capacity(2048);

    let mut last_key = u64::MAX;
    let mut last_hour = 0u64;
    let mut pos = 0usize;

    while pos < len {
        let nl = unsafe { memchr(base.add(pos) as *const c_void, b'\n' as i32, len - pos) };
        let end = if nl.is_null() {
            len
        } else {
            nl as usize - base as usize
        };
        let ll = end - pos;

        if ll >= MIN_LINE {
            unsafe {
                let line = base.add(pos);

                // The 27-byte timestamp is followed by a space, so the method
                // starts at 28. Find the space that ends the method.
                let method_end = memchr(
                    line.add(28) as *const c_void,
                    b' ' as i32,
                    ll - 28,
                );

                if !method_end.is_null() {
                    let s2 = method_end as usize - line as usize;

                    if s2 + 8 <= ll {
                        let sep = memrchr(
                            line.add(s2 + 1) as *const c_void,
                            b' ' as i32,
                            ll - s2 - 1,
                        );

                        if !sep.is_null() {
                            let s4 = sep as usize - line as usize;

                            if s4 >= s2 + 6 {
                                let mut lat = 0u32;
                                let mut j = s4 + 1;
                                while j < ll {
                                    lat = lat.wrapping_mul(10)
                                        .wrapping_add((*line.add(j)).wrapping_sub(b'0') as u32);
                                    j += 1;
                                }
                        // " 200 " : status is exactly 3 digits before s4.
                        let s3 = s4 - 4;
                        let ep = std::slice::from_raw_parts(line.add(s2 + 1), s3 - s2 - 1);
                        let is_err = (*line.add(s3 + 1) == b'5') as u64;

                        // Timestamps are normally sorted, so the date/hour part
                        // (bytes 5..13) repeats for long runs of lines.
                        let tsk = (line.add(5) as *const u64).read_unaligned();
                        let hour = if tsk == last_key {
                            last_hour
                        } else {
                            let h = hour_of(line) as u64;
                            last_key = tsk;
                            last_hour = h;
                            h
                        };

                        let h = hash_ep(ep);
                        let tag = (h >> 16) | 1;
                        let mut slot = (h as usize) & MASK;
                        let id = loop {
                            let e = *table.get_unchecked(slot);
                            if e == 0 {
                                let id = eps.len();
                                if id >= MAX_LOCAL_EPS {
                                    too_many_endpoints();
                                }
                                eps.push(ep);
                                *table.get_unchecked_mut(slot) = (tag << 16) | id as u64;
                                break id;
                            }
                            if (e >> 16) == tag {
                                let id = (e & 0xFFFF) as usize;
                                if *eps.get_unchecked(id) == ep {
                                    break id;
                                }
                            }
                            slot = (slot + 1) & MASK;
                        };

                        recs.push(
                            (lat as u64)
                                | ((id as u64) << 32)
                                | (hour << 48)
                                | (is_err << 56),
                        );
                            }
                        }
                    }
                }
            }
        }

        pos = end + 1;
    }

    Chunk { recs, eps }
}
/// Rewrites records to (key << 32 | latency) and returns the per-worker
/// histogram: low 32 bits = request count, high 32 bits = 5xx count.
fn count_chunk(chunk: &mut Chunk, all: &[&'static [u8]], e: usize, k: usize) -> Vec<u64> {
    let remap: Vec<u32> = chunk
        .eps
        .iter()
        .map(|ep| all.binary_search(ep).expect("endpoint missing from union") as u32)
        .collect();

    let mut cnt = vec![0u64; k];
    for r in chunk.recs.iter_mut() {
        let v = *r;
        let lat = v & 0xFFFF_FFFF;
        let lep = ((v >> 32) & 0xFFFF) as usize;
        let hour = ((v >> 48) & 0xFF) as usize;
        let err = (v >> 56) & 1;
        let key = hour * e + remap[lep] as usize;
        cnt[key] += 1 + (err << 32);
        *r = lat | ((key as u64) << 32);
    }
    cnt
}

/// Nearest-rank P50/P95/P99 of `v` (reorders `v`). Ranks are non-decreasing,
/// so each selection only needs to look at the part right of the previous one.
#[inline]
fn percentiles(v: &mut [u32]) -> (u32, u32, u32) {
    let n = v.len();
    let ranks = [
        (50 * n + 99) / 100 - 1,
        (95 * n + 99) / 100 - 1,
        (99 * n + 99) / 100 - 1,
    ];

    if n <= 32 {
        v.sort_unstable();
        return (v[ranks[0]], v[ranks[1]], v[ranks[2]]);
    }

    let mut out = [0u32; 3];
    let mut cur: &mut [u32] = v;
    let mut base = 0usize;
    let mut last: Option<(usize, u32)> = None;

    for (i, &r) in ranks.iter().enumerate() {
        if let Some((lr, lv)) = last {
            if r == lr {
                out[i] = lv;
                continue;
            }
        }
        let c = std::mem::take(&mut cur);
        let (_, m, right) = c.select_nth_unstable(r - base);
        out[i] = *m;
        last = Some((r, *m));
        base = r + 1;
        cur = right;
    }
    (out[0], out[1], out[2])
}

#[inline(always)]
unsafe fn put_u64(p: *mut u8, mut n: u64) -> *mut u8 {
    unsafe {
        if n == 0 {
            *p = b'0';
            return p.add(1);
        }
        let mut buf = [0u8; 20];
        let mut i = 20usize;
        while n > 0 {
            i -= 1;
            buf[i] = b'0' + (n % 10) as u8;
            n /= 10;
        }
        let l = 20 - i;
        std::ptr::copy_nonoverlapping(buf.as_ptr().add(i), p, l);
        p.add(l)
    }
}

fn append_two(out: &mut Vec<u8>, n: usize) {
    out.push(b'0' + (n / 10) as u8);
    out.push(b'0' + (n % 10) as u8);
}

fn append_hour(out: &mut Vec<u8>, index: usize) {
    let absolute = 18 + index;
    let day_offset = absolute / 24;
    let hour = absolute % 24;
    let (month, day) = if day_offset <= 1 {
        (10, 30 + day_offset)
    } else {
        (11, day_offset - 1)
    };
    out.extend_from_slice(b"2026-");
    append_two(out, month);
    out.push(b'-');
    append_two(out, day);
    out.push(b'T');
    append_two(out, hour);
    out.extend_from_slice(b":00:00Z");
}

fn render_range(
    a: usize,
    b: usize,
    e: usize,
    starts: &[u32],
    err_tot: &[u32],
    out_p: usize,
    all: &[&'static [u8]],
    hour_strs: &[[u8; 20]; HOURS],
) -> Vec<u8> {
    let mut rows = 0usize;
    for key in a..b {
        if starts[key + 1] != starts[key] {
            rows += 1;
        }
    }
    let mut buf: Vec<u8> = Vec::with_capacity(rows * 100 + 64);
    let outp = out_p as *mut u32;

    for key in a..b {
        let s = starts[key] as usize;
        let n = starts[key + 1] as usize - s;
        if n == 0 {
            continue;
        }
        let v = unsafe { std::slice::from_raw_parts_mut(outp.add(s), n) };
        let (p50, p95, p99) = percentiles(v);

        let hour = key / e;
        let rank = key - hour * e;
        let ep = all[rank];
        let errors = err_tot[key] as u64;
        let ppm = errors * 1_000_000 / n as u64;

        buf.reserve(ep.len() + 160);
        unsafe {
            let start = buf.as_mut_ptr().add(buf.len());
            let mut p = start;
            std::ptr::copy_nonoverlapping(hour_strs[hour].as_ptr(), p, 20);
            p = p.add(20);
            *p = b' ';
            p = p.add(1);
            std::ptr::copy_nonoverlapping(ep.as_ptr(), p, ep.len());
            p = p.add(ep.len());
            *p = b' ';
            p = p.add(1);
            p = put_u64(p, n as u64);
            *p = b' ';
            p = p.add(1);
            p = put_u64(p, p50 as u64);
            *p = b' ';
            p = p.add(1);
            p = put_u64(p, p95 as u64);
            *p = b' ';
            p = p.add(1);
            p = put_u64(p, p99 as u64);
            *p = b' ';
            p = p.add(1);
            p = put_u64(p, errors);
            *p = b' ';
            p = p.add(1);
            p = put_u64(p, ppm);
            *p = b'\n';
            p = p.add(1);
            let new_len = buf.len() + (p as usize - start as usize);
            buf.set_len(new_len);
        }
    }
    buf
}
fn run() -> io::Result<()> {
    let t_total = Instant::now();
    let args: Vec<String> = std::env::args().collect();
    let in_path = args.get(1).map(String::as_str).unwrap_or("/data/requests.log");
    let out_path = args.get(2).map(String::as_str).unwrap_or("/out/summary.txt");
    let timing = std::env::var_os("SD_TIMING").is_some();

    let input = File::open(in_path)?;
    let len = input.metadata()?.len() as usize;

    if let Some(parent) = Path::new(out_path).parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    if len == 0 {
        File::create(out_path)?;
        return Ok(());
    }

    // ---- mmap ----
    let t = Instant::now();
    let mut flags = MAP_PRIVATE;
    if std::env::var_os("SD_POPULATE").is_some() {
        flags |= MAP_POPULATE;
    }
    let ptr = unsafe {
        mmap(
            std::ptr::null_mut(),
            len,
            PROT_READ,
            flags,
            input.as_raw_fd(),
            0,
        )
    };
    if ptr as isize == -1 {
        return Err(io::Error::last_os_error());
    }
    // Never unmapped: the process exits right after writing the output.
    let data: &'static [u8] = unsafe { std::slice::from_raw_parts(ptr as *const u8, len) };
    let t_mmap = t.elapsed();

    // ---- chunk boundaries ----
    let hw = thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    let wanted = std::env::var("SD_WORKERS")
        .ok()
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(hw.min(32));
    let workers = wanted.max(1).min((len / 65536).max(1));

    let mut bounds = Vec::with_capacity(workers + 1);
    bounds.push(0usize);
    for i in 1..workers {
        let mut b = len * i / workers;
        while b < len && data[b - 1] != b'\n' {
            b += 1;
        }
        if b > *bounds.last().unwrap() && b < len {
            bounds.push(b);
        }
    }
    bounds.push(len);
    let w = bounds.len() - 1;

    // ---- 1. parse ----
    let t = Instant::now();
    let mut chunks: Vec<Chunk> = thread::scope(|s| {
        let hs: Vec<_> = bounds
            .windows(2)
            .map(|pair| {
                let c: &'static [u8] = &data[pair[0]..pair[1]];
                s.spawn(move || parse_chunk(c))
            })
            .collect();
        hs.into_iter()
            .map(|h| h.join().expect("parse worker failed"))
            .collect()
    });
    let t_parse = t.elapsed();

    // ---- 2. union of endpoints, sorted = output order ----
    let t = Instant::now();
    let mut all: Vec<&'static [u8]> = Vec::new();
    for c in &chunks {
        all.extend_from_slice(&c.eps);
    }
    all.sort_unstable();
    all.dedup();
    let e = all.len();
    let k = HOURS * e;
    let total_recs: usize = chunks.iter().map(|c| c.recs.len()).sum();
    if e == 0 || total_recs == 0 {
        File::create(out_path)?;
        return Ok(());
    }
    let t_union = t.elapsed();

    // ---- 3. count / rewrite records ----
    let t = Instant::now();
    let mut cnts: Vec<Vec<u64>> = thread::scope(|s| {
        let all_ref = &all;
        let hs: Vec<_> = chunks
            .iter_mut()
            .map(|c| s.spawn(move || count_chunk(c, all_ref, e, k)))
            .collect();
        hs.into_iter()
            .map(|h| h.join().expect("count worker failed"))
            .collect()
    });
    let t_count = t.elapsed();

    // ---- 4. prefix sums (key-range parallel) ----
    let t = Instant::now();
    let mut starts = vec![0u32; k + 1];
    let mut err_tot = vec![0u32; k];
    let totals: Vec<AtomicU64> = (0..w).map(|_| AtomicU64::new(0)).collect();
    {
        let cnt_ptrs: Vec<usize> = cnts.iter_mut().map(|v| v.as_mut_ptr() as usize).collect();
        let starts_p = starts.as_mut_ptr() as usize;
        let err_p = err_tot.as_mut_ptr() as usize;
        let barrier = Barrier::new(w);
        thread::scope(|s| {
            for tix in 0..w {
                let cnt_ptrs = &cnt_ptrs;
                let totals = &totals;
                let barrier = &barrier;
                s.spawn(move || {
                    let a = k * tix / w;
                    let b = k * (tix + 1) / w;
                    let mut sum = 0u64;
                    unsafe {
                        for &cp in cnt_ptrs.iter() {
                            let p = cp as *const u64;
                            for key in a..b {
                                sum += *p.add(key) & 0xFFFF_FFFF;
                            }
                        }
                    }
                    totals[tix].store(sum, Ordering::Relaxed);
                    barrier.wait();
                    let mut run: u64 = 0;
                    for x in 0..tix {
                        run += totals[x].load(Ordering::Relaxed);
                    }
                    let sp = starts_p as *mut u32;
                    let ep = err_p as *mut u32;
                    unsafe {
                        for key in a..b {
                            *sp.add(key) = run as u32;
                            let mut errs = 0u32;
                            for &cp in cnt_ptrs.iter() {
                                let p = cp as *mut u64;
                                let v = *p.add(key);
                                let c = v & 0xFFFF_FFFF;
                                errs += (v >> 32) as u32;
                                *p.add(key) = (v & 0xFFFF_FFFF_0000_0000) | run;
                                run += c;
                            }
                            *ep.add(key) = errs;
                        }
                    }
                });
            }
        });
    }
    let n_total: usize = totals.iter().map(|a| a.load(Ordering::Relaxed) as usize).sum();
    starts[k] = n_total as u32;
    let t_prefix = t.elapsed();

    // ---- 5. scatter (counting sort) ----
    let t = Instant::now();
    let mut out: Vec<u32> = Vec::with_capacity(n_total);
    let out_p = out.as_mut_ptr() as usize;
    thread::scope(|s| {
        for (c, cnt) in chunks.iter().zip(cnts.iter_mut()) {
            let recs: &[u64] = &c.recs;
            let cnt: &mut [u64] = cnt.as_mut_slice();
            s.spawn(move || unsafe {
                let outp = out_p as *mut u32;
                let cp = cnt.as_mut_ptr();
                for &r in recs {
                    let key = (r >> 32) as usize;
                    let slot = cp.add(key);
                    let pos = (*slot & 0xFFFF_FFFF) as usize;
                    *slot += 1;
                    *outp.add(pos) = r as u32;
                }
            });
        }
    });
    unsafe { out.set_len(n_total) };
    let t_scatter = t.elapsed();

    // ---- 6. percentiles + render ----
    let t = Instant::now();
    let mut hour_strs = [[0u8; 20]; HOURS];
    for h in 0..HOURS {
        let mut v = Vec::with_capacity(20);
        append_hour(&mut v, h);
        hour_strs[h].copy_from_slice(&v);
    }

    let tn = w;
    let mut rb = vec![0usize; tn + 1];
    rb[tn] = k;
    for tix in 1..tn {
        let target = (n_total as u64 * tix as u64 / tn as u64) as u32;
        let idx = starts.partition_point(|&s| s < target);
        rb[tix] = idx.min(k).max(rb[tix - 1]);
    }

    let parts: Vec<Vec<u8>> = thread::scope(|s| {
        let (starts, err_tot, all, hour_strs) = (&starts, &err_tot, &all, &hour_strs);
        let hs: Vec<_> = (0..tn)
            .map(|tix| {
                let (a, b) = (rb[tix], rb[tix + 1]);
                s.spawn(move || render_range(a, b, e, starts, err_tot, out_p, all, hour_strs))
            })
            .collect();
        hs.into_iter()
            .map(|h| h.join().expect("render worker failed"))
            .collect()
    });
    let t_render = t.elapsed();

    // ---- 7. write ----
    let t = Instant::now();
    let mut file = File::create(out_path)?;
    for part in &parts {
        file.write_all(part)?;
    }
    let t_write = t.elapsed();

    if timing {
        let ms = |d: std::time::Duration| d.as_secs_f64() * 1000.0;
        eprintln!("workers={w} records={total_recs} endpoints={e} groups_slots={k}");
        eprintln!("PHASE mmap:    {:.3} ms", ms(t_mmap));
        eprintln!("PHASE parse:   {:.3} ms", ms(t_parse));
        eprintln!("PHASE union:   {:.3} ms", ms(t_union));
        eprintln!("PHASE count:   {:.3} ms", ms(t_count));
        eprintln!("PHASE prefix:  {:.3} ms", ms(t_prefix));
        eprintln!("PHASE scatter: {:.3} ms", ms(t_scatter));
        eprintln!("PHASE render:  {:.3} ms (includes percentiles)", ms(t_render));
        eprintln!("PHASE write:   {:.3} ms", ms(t_write));
        eprintln!("PHASE total:   {:.3} ms", ms(t_total.elapsed()));
    }

    // Skip all destructors / munmap: exit immediately.
    std::process::exit(0);
}

fn main() {
    if let Err(e) = run() {
        eprintln!("track-a-ingest: {e}");
        std::process::exit(1);
    }
}
