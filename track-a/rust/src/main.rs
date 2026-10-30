use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, BufWriter, Write};
use std::path::Path;

const MAX_ENDPOINTS: usize = 1_842;
const MAX_GROUPS: usize = 72 * MAX_ENDPOINTS;

struct Group {
    hour: u8,
    endpoint_id: u16,
    errors: u32,
    latencies: Vec<u32>,
}

#[inline]
fn parse_u32(bytes: &[u8]) -> u32 {
    let mut value = 0u32;
    for &b in bytes {
        value = value * 10 + (b - b'0') as u32;
    }
    value
}

#[inline]
fn parse_two_digits(bytes: &[u8]) -> usize {
    ((bytes[0] - b'0') as usize) * 10 + (bytes[1] - b'0') as usize
}

// The specification fixes the input span to 2026-10-30 18:00 UTC
// through 2026-11-02 17:59:59.999999 UTC.
#[inline]
fn hour_index(ts: &[u8]) -> u8 {
    let month = parse_two_digits(&ts[5..7]);
    let day = parse_two_digits(&ts[8..10]);
    let hour = parse_two_digits(&ts[11..13]);

    let day_offset = if month == 10 {
        day - 30
    } else {
        // November 1 -> offset 2; November 2 -> offset 3.
        day + 1
    };

    (day_offset * 24 + hour - 18) as u8
}

#[inline]
fn next_space(line: &[u8], from: usize) -> usize {
    line[from..]
        .iter()
        .position(|&b| b == b' ')
        .map(|p| from + p)
        .expect("well-formed input line must contain spaces")
}

fn write_hour<W: Write>(out: &mut W, index: u8) -> io::Result<()> {
    let absolute_hour = 18 + index as usize;
    let day_offset = absolute_hour / 24;
    let hour = absolute_hour % 24;

    let (month, day) = if day_offset <= 1 {
        (10, 30 + day_offset)
    } else {
        (11, day_offset - 1)
    };

    write!(out, "2026-{month:02}-{day:02}T{hour:02}:00:00Z")
}

fn percentile(sorted: &[u32], p: usize) -> u32 {
    let n = sorted.len();
    let rank = (p * n + 99) / 100;
    sorted[rank - 1]
}

fn run(data_dir: &Path, out_dir: &Path) -> io::Result<()> {
    let input_path = data_dir.join("requests.log");
    let file = File::open(input_path)?;
    let mut reader = BufReader::with_capacity(1 << 20, file);
    let mut line = Vec::with_capacity(160);

    let mut endpoint_ids: HashMap<Vec<u8>, u16> =
        HashMap::with_capacity(MAX_ENDPOINTS);
    let mut endpoints: Vec<Vec<u8>> = Vec::with_capacity(MAX_ENDPOINTS);

    // Composite key: hour_index in the high 16 bits, endpoint ID in low 16 bits.
    let mut group_ids: HashMap<u32, usize> = HashMap::with_capacity(MAX_GROUPS);
    let mut groups: Vec<Group> = Vec::with_capacity(MAX_GROUPS);

    while reader.read_until(b'\n', &mut line)? != 0 {
        if line.last() == Some(&b'\n') {
            line.pop();
        }

        let s1 = next_space(&line, 0);       // timestamp ends
        let s2 = next_space(&line, s1 + 1);  // method ends
        let s3 = next_space(&line, s2 + 1);  // endpoint ends
        let s4 = next_space(&line, s3 + 1);  // status ends

        let hour = hour_index(&line[..s1]);
        let endpoint = &line[s2 + 1..s3];
        let status = parse_u32(&line[s3 + 1..s4]);
        let latency = parse_u32(&line[s4 + 1..]);

        let endpoint_id = match endpoint_ids.get(endpoint) {
            Some(&id) => id,
            None => {
                let id = endpoints.len() as u16;
                let owned = endpoint.to_vec();
                endpoint_ids.insert(owned.clone(), id);
                endpoints.push(owned);
                id
            }
        };

        let key = ((hour as u32) << 16) | endpoint_id as u32;
        let group_index = match group_ids.get(&key) {
            Some(&index) => index,
            None => {
                let index = groups.len();
                groups.push(Group {
                    hour,
                    endpoint_id,
                    errors: 0,
                    latencies: Vec::new(),
                });
                group_ids.insert(key, index);
                index
            }
        };

        let group = &mut groups[group_index];
        group.latencies.push(latency);
        if (500..=599).contains(&status) {
            group.errors += 1;
        }

        line.clear();
    }

    // Hour first, then endpoint as unsigned byte strings.
    groups.sort_unstable_by(|a, b| {
        a.hour.cmp(&b.hour).then_with(|| {
            endpoints[a.endpoint_id as usize]
                .as_slice()
                .cmp(endpoints[b.endpoint_id as usize].as_slice())
        })
    });

    fs::create_dir_all(out_dir)?;
    let output_path = out_dir.join("summary.txt");
    let output_file = File::create(output_path)?;
    let mut out = BufWriter::with_capacity(1 << 20, output_file);

    for mut group in groups {
        group.latencies.sort_unstable();
        let count = group.latencies.len();
        let p50 = percentile(&group.latencies, 50);
        let p95 = percentile(&group.latencies, 95);
        let p99 = percentile(&group.latencies, 99);
        let errors = group.errors;
        let error_ppm = (errors as u64 * 1_000_000u64) / count as u64;

        write_hour(&mut out, group.hour)?;
        out.write_all(b" ")?;
        out.write_all(&endpoints[group.endpoint_id as usize])?;
        writeln!(
            out,
            " {count} {p50} {p95} {p99} {errors} {error_ppm}"
        )?;
    }

    out.flush()?;
    Ok(())
}

fn main() {
    if let Err(error) = run(Path::new("/data"), Path::new("/out")) {
        eprintln!("track-a-ingest: {error}");
        std::process::exit(1);
    }
}
