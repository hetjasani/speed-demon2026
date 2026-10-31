use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{self, Write};
use std::os::fd::AsRawFd;
use std::path::Path;
use std::thread;

const MAX_ENDPOINTS: usize = 1842;
const MAX_GROUPS: usize = 72 * MAX_ENDPOINTS;
const WORKERS: usize = 16;
const ENDPOINT_TABLE_SIZE: usize = 4096;

const PROT_READ: i32 = 1;
const MAP_PRIVATE: i32 = 2;

unsafe extern "C" {
    fn mmap(
        addr: *mut std::ffi::c_void,
        length: usize,
        prot: i32,
        flags: i32,
        fd: i32,
        offset: isize,
    ) -> *mut std::ffi::c_void;

    fn munmap(addr: *mut std::ffi::c_void, length: usize) -> i32;
    fn memchr(s: *const std::ffi::c_void, c: i32, n: usize)
        -> *mut std::ffi::c_void;
}

struct Group {
    hour: u8,
    endpoint_id: u16,
    errors: u32,
    latencies: Vec<u32>,
}

struct LocalResult {
    endpoints: Vec<Vec<u8>>,
    groups: Vec<Group>,
}

#[inline]
fn parse_u32(bytes: &[u8]) -> u32 {
    let d = |i: usize| (bytes[i] - b'0') as u32;
    let p = |i: usize| d(i) * 10 + d(i + 1);

    match bytes.len() {
        1 => d(0),
        2 => p(0),
        3 => d(0) * 100 + p(1),
        4 => p(0) * 100 + p(2),
        5 => d(0) * 10000 + p(1) * 100 + p(3),
        6 => p(0) * 10000 + p(2) * 100 + p(4),
        _ => {
            let mut value = 0u32;
            for &b in bytes {
                value = value * 10 + (b - b'0') as u32;
            }
            value
        }
    }
}

#[inline]
fn two_digits(b: &[u8]) -> usize {
    ((b[0] - b'0') as usize) * 10 + (b[1] - b'0') as usize
}

#[inline]
#[inline]
fn hour_index(ts: &[u8]) -> u8 {
    let month = ((ts[5] - b'0') as usize) * 10
        + (ts[6] - b'0') as usize;
    let day = ((ts[8] - b'0') as usize) * 10
        + (ts[9] - b'0') as usize;
    let hour = ((ts[11] - b'0') as usize) * 10
        + (ts[12] - b'0') as usize;

    let offset = if month == 10 { day - 30 } else { day + 1 };
    (offset * 24 + hour - 18) as u8
}

#[inline]
fn endpoint_hash(bytes: &[u8]) -> u64 {
    let mut hash = 0x9e3779b97f4a7c15u64;
    let mut chunks = bytes.chunks_exact(8);

    for chunk in &mut chunks {
        hash = hash.rotate_left(5) ^ chunk[0] as u64;
        hash = hash.rotate_left(5) ^ chunk[1] as u64;
        hash = hash.rotate_left(5) ^ chunk[2] as u64;
        hash = hash.rotate_left(5) ^ chunk[3] as u64;
        hash = hash.rotate_left(5) ^ chunk[4] as u64;
        hash = hash.rotate_left(5) ^ chunk[5] as u64;
        hash = hash.rotate_left(5) ^ chunk[6] as u64;
        hash = hash.rotate_left(5) ^ chunk[7] as u64;
    }

    for &b in chunks.remainder() {
        hash = hash.rotate_left(5) ^ b as u64;
    }

    hash ^= hash >> 30;
    hash ^= hash >> 27;
    hash ^= hash >> 31;

    if hash == 0 { 1 } else { hash }
}

fn process_chunk(data: &[u8]) -> LocalResult {
    let mut endpoint_table = vec![(0u64, 0u16); ENDPOINT_TABLE_SIZE];

    let mut endpoints: Vec<Vec<u8>> =
        Vec::with_capacity(MAX_ENDPOINTS);

    let mut group_ids = vec![usize::MAX; MAX_GROUPS];
    let mut groups: Vec<Group> = Vec::new();

    let mut pos = 0usize;

    while pos < data.len() {
        let newline = unsafe {
            memchr(
                data[pos..].as_ptr() as *const std::ffi::c_void,
                b'\n' as i32,
                data.len() - pos,
            )
        };

        let end = if newline.is_null() {
            data.len()
        } else {
            unsafe {
                (newline as *const u8).offset_from(data.as_ptr()) as usize
            }
        };

        let line = &data[pos..end];

        if !line.is_empty() {
            // Timestamp is always exactly 27 bytes.
            let s1 = 27;
            let s2 = s1 + 1 + unsafe {
                memchr(
                    line[s1 + 1..].as_ptr() as *const std::ffi::c_void,
                    b' ' as i32,
                    line.len() - s1 - 1,
                ).offset_from(line[s1 + 1..].as_ptr() as *const std::ffi::c_void) as usize
            };
            let s3 = s2 + 1 + unsafe {
                memchr(
                    line[s2 + 1..].as_ptr() as *const std::ffi::c_void,
                    b' ' as i32,
                    line.len() - s2 - 1,
                ).offset_from(line[s2 + 1..].as_ptr() as *const std::ffi::c_void) as usize
            };
            let s4 = s3 + 1 + unsafe {
                memchr(
                    line[s3 + 1..].as_ptr() as *const std::ffi::c_void,
                    b' ' as i32,
                    line.len() - s3 - 1,
                ).offset_from(line[s3 + 1..].as_ptr() as *const std::ffi::c_void) as usize
            };

            let hour = hour_index(&line[..s1]);
            let endpoint = &line[s2 + 1..s3];
            let status_bytes = &line[s3 + 1..s4];
            let is_error = status_bytes.len() == 3
                && status_bytes[0] == b'5';
            let latency = parse_u32(&line[s4 + 1..]);

            let hash = endpoint_hash(endpoint);
            let mut slot = hash as usize & (ENDPOINT_TABLE_SIZE - 1);

            let endpoint_id = loop {
                let (stored_hash, stored_id) = endpoint_table[slot];

                if stored_hash == 0 {
                    let id = endpoints.len() as u16;
                    endpoints.push(endpoint.to_vec());
                    endpoint_table[slot] = (hash, id);
                    break id;
                }

                if stored_hash == hash
                    && endpoints[stored_id as usize].as_slice() == endpoint
                {
                    break stored_id;
                }

                slot = (slot + 1) & (ENDPOINT_TABLE_SIZE - 1);
            };

            let slot =
                hour as usize * MAX_ENDPOINTS + endpoint_id as usize;

            let group_index = if group_ids[slot] != usize::MAX {
                group_ids[slot]
            } else {
                let index = groups.len();

                groups.push(Group {
                    hour,
                    endpoint_id,
                    errors: 0,
                    latencies: Vec::with_capacity(16),
                });

                group_ids[slot] = index;
                index
            };

            let group = &mut groups[group_index];
            group.latencies.push(latency);

            if is_error {
                group.errors += 1;
            }
        }

        pos = if end < data.len() { end + 1 } else { end };
    }

    LocalResult { endpoints, groups }
}

#[inline]
fn percentile(sorted: &[u32], p: usize) -> u32 {
    let rank = (p * sorted.len() + 99) / 100;
    sorted[rank - 1]
}

fn write_hour<W: Write>(out: &mut W, index: u8) -> io::Result<()> {
    let absolute = 18 + index as usize;
    let day_offset = absolute / 24;
    let hour = absolute % 24;

    let (month, day) = if day_offset <= 1 {
        (10, 30 + day_offset)
    } else {
        (11, day_offset - 1)
    };

    write!(out, "2026-{month:02}-{day:02}T{hour:02}:00:00Z")
}


#[inline]
fn append_two_digits(out: &mut Vec<u8>, n: usize) {
    out.push(b'0' + (n / 10) as u8);
    out.push(b'0' + (n % 10) as u8);
}

#[inline]
fn append_u32(out: &mut Vec<u8>, mut n: u32) {
    let mut buf = [0u8; 10];
    let mut i = buf.len();

    if n == 0 {
        out.push(b'0');
        return;
    }

    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }

    out.extend_from_slice(&buf[i..]);
}

#[inline]
fn append_hour(out: &mut Vec<u8>, index: u8) {
    let absolute = 18 + index as usize;
    let day_offset = absolute / 24;
    let hour = absolute % 24;

    let (month, day) = if day_offset <= 1 {
        (10, 30 + day_offset)
    } else {
        (11, day_offset - 1)
    };

    out.extend_from_slice(b"2026-");
    append_two_digits(out, month);
    out.push(b'-');
    append_two_digits(out, day);
    out.push(b'T');
    append_two_digits(out, hour);
    out.extend_from_slice(b":00:00Z");
}

struct Aggregate {
    errors: u32,
    latencies: Vec<u32>,
}


fn render_rows(
    rows: &mut [(u8, u16, Aggregate)],
    global_endpoints: &[Vec<u8>],
) -> Vec<u8> {
    let mut output = Vec::with_capacity(rows.len() * 100);

    for (hour, endpoint_id, aggregate) in rows {
        let endpoint = &global_endpoints[*endpoint_id as usize];
        let count = aggregate.latencies.len();

        let (p50, p95, p99) = {
            let values = &mut aggregate.latencies;
            let r50 = (50 * count + 99) / 100 - 1;
            let r95 = (95 * count + 99) / 100 - 1;
            let r99 = (99 * count + 99) / 100 - 1;

            let (_, value, _) = values.select_nth_unstable(r50);
            let p50 = *value;
            let (_, value, _) = values.select_nth_unstable(r95);
            let p95 = *value;
            let (_, value, _) = values.select_nth_unstable(r99);
            let p99 = *value;
            (p50, p95, p99)
        };

        let errors = aggregate.errors;
        let error_ppm = errors as u64 * 1_000_000 / count as u64;

        append_hour(&mut output, *hour);
        output.push(b' ');
        output.extend_from_slice(endpoint);
        output.push(b' ');
        append_u32(&mut output, count as u32);
        output.push(b' ');
        append_u32(&mut output, p50);
        output.push(b' ');
        append_u32(&mut output, p95);
        output.push(b' ');
        append_u32(&mut output, p99);
        output.push(b' ');
        append_u32(&mut output, errors);
        output.push(b' ');

        let mut ppm_buf = [0u8; 20];
        let mut ppm = error_ppm;
        let mut i = ppm_buf.len();
        if ppm == 0 {
            i -= 1;
            ppm_buf[i] = b'0';
        } else {
            while ppm > 0 {
                i -= 1;
                ppm_buf[i] = b'0' + (ppm % 10) as u8;
                ppm /= 10;
            }
        }
        output.extend_from_slice(&ppm_buf[i..]);
        output.push(b'\n');
    }
    output
}


fn run() -> io::Result<()> {
    let input = File::open("/data/requests.log")?;
    let len = input.metadata()?.len() as usize;

    if len == 0 {
        fs::create_dir_all("/out")?;
        File::create("/out/summary.txt")?;
        return Ok(());
    }

    let ptr = unsafe {
        mmap(
            std::ptr::null_mut(),
            len,
            PROT_READ,
            MAP_PRIVATE,
            input.as_raw_fd(),
            0,
        )
    };

    if ptr as isize == -1 {
        return Err(io::Error::last_os_error());
    }

    let data = unsafe {
        std::slice::from_raw_parts(ptr as *const u8, len)
    };

    // Find line-aligned chunk boundaries.
    let workers = WORKERS.min(len.max(1));
    let mut boundaries = Vec::with_capacity(workers + 1);
    boundaries.push(0usize);

    for i in 1..workers {
        let target = len * i / workers;
        let mut boundary = target;

        while boundary < len && data[boundary - 1] != b'\n' {
            boundary += 1;
        }

        if boundary > *boundaries.last().unwrap() && boundary < len {
            boundaries.push(boundary);
        }
    }

    boundaries.push(len);

    let mut results = Vec::with_capacity(boundaries.len() - 1);

    thread::scope(|scope| {
        let mut handles = Vec::new();

        for pair in boundaries.windows(2) {
            let start = pair[0];
            let end = pair[1];
            let chunk = &data[start..end];

            handles.push(scope.spawn(move || process_chunk(chunk)));
        }

        for handle in handles {
            results.push(handle.join().expect("Worker thread failed"));
        }
    });

    // Merge using global endpoint IDs and direct-indexed group slots.
    let mut global_endpoints: Vec<Vec<u8>> =
        Vec::with_capacity(MAX_ENDPOINTS);
    let mut global_ids: HashMap<Vec<u8>, u16> =
        HashMap::with_capacity(MAX_ENDPOINTS);

    let mut merged: Vec<Option<Aggregate>> =
        std::iter::repeat_with(|| None).take(MAX_GROUPS).collect();

    for result in results {
        let mut remap = Vec::with_capacity(result.endpoints.len());

        for endpoint in result.endpoints {
            let id = match global_ids.get(endpoint.as_slice()) {
                Some(&id) => id,
                None => {
                    let id = global_endpoints.len() as u16;
                    global_ids.insert(endpoint.clone(), id);
                    global_endpoints.push(endpoint);
                    id
                }
            };
            remap.push(id);
        }

        for group in result.groups {
            let endpoint_id = remap[group.endpoint_id as usize] as usize;
            let slot_index = group.hour as usize * MAX_ENDPOINTS + endpoint_id;

            match merged[slot_index].as_mut() {
                Some(entry) => {
                    entry.errors += group.errors;
                    entry.latencies.extend(group.latencies);
                }
                None => {
                    merged[slot_index] = Some(Aggregate {
                        errors: group.errors,
                        latencies: group.latencies,
                    });
                }
            }
        }
    }

    let mut rows: Vec<(u8, u16, Aggregate)> = Vec::new();

    for (index, aggregate) in merged.into_iter().enumerate() {
        if let Some(aggregate) = aggregate {
            let hour = (index / MAX_ENDPOINTS) as u8;
            let endpoint_id = (index % MAX_ENDPOINTS) as u16;
            rows.push((hour, endpoint_id, aggregate));
        }
    }

    rows.sort_unstable_by(|a, b| {
        a.0.cmp(&b.0).then_with(|| {
            global_endpoints[a.1 as usize]
                .cmp(&global_endpoints[b.1 as usize])
        })
    });

    fs::create_dir_all("/out")?;

    let output_workers = WORKERS.min(rows.len().max(1));
    let chunk_size = rows.len().div_ceil(output_workers);
    let mut output_parts = Vec::with_capacity(output_workers);

    thread::scope(|scope| {
        let mut handles = Vec::new();
        for chunk in rows.chunks_mut(chunk_size) {
            let endpoints = &global_endpoints;
            handles.push(scope.spawn(move || render_rows(chunk, endpoints)));
        }
        for handle in handles {
            output_parts.push(handle.join().expect("Output worker failed"));
        }
    });

    let total_output: usize = output_parts.iter().map(|part| part.len()).sum();
    let mut output = Vec::with_capacity(total_output);
    for part in output_parts {
        output.extend_from_slice(&part);
    }

    let mut file = File::create("/out/summary.txt")?;
    file.write_all(&output)?;

    unsafe {
        munmap(ptr, len);
    }

    Ok(())
}

fn main() {
    if let Err(e) = run() {
        eprintln!("track-a-ingest: {e}");
        std::process::exit(1);
    }
}
