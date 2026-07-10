use hidapi::{HidApi, HidDevice};
use std::env;
use std::ffi::CString;
use std::fs::{self, File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::os::unix::fs::PermissionsExt;
use std::process;
use std::sync::atomic::{AtomicU8, Ordering};

const LOGITECH_VID: u16 = 0x046D;
const REPORT_ID_LONG: u8 = 0x11;
const MAX_PAYLOAD: usize = 18;
const DEFAULT_TIMEOUT_MS: i32 = 4000;
const FAST_TIMEOUT_MS: i32 = 500;
const FEATURE_CHANGE_HOST: u16 = 0x1814;
const FEATURE_FEATURE_SET: u16 = 0x0001;

#[derive(Clone, Copy, PartialEq, Eq)]
enum CacheMode {
    Auto,
    Off,
    Refresh,
}

fn parse_long_strict(text: &str, base: u32, min_value: i64, max_value: i64) -> Result<i64, ()> {
    if text.is_empty() {
        return Err(());
    }

    let value = match base {
        10 => text.parse::<i64>().map_err(|_| ())?,
        0 => parse_base0_i64(text)?,
        _ => return Err(()),
    };

    if value < min_value || value > max_value {
        return Err(());
    }

    Ok(value)
}

fn parse_base0_i64(text: &str) -> Result<i64, ()> {
    let (negative, body) = if let Some(rest) = text.strip_prefix('-') {
        (true, rest)
    } else if let Some(rest) = text.strip_prefix('+') {
        (false, rest)
    } else {
        (false, text)
    };

    if body.is_empty() {
        return Err(());
    }

    let (radix, digits) = if let Some(rest) = body.strip_prefix("0x").or_else(|| body.strip_prefix("0X")) {
        (16, rest)
    } else if let Some(rest) = body.strip_prefix("0o").or_else(|| body.strip_prefix("0O")) {
        (8, rest)
    } else if let Some(rest) = body.strip_prefix("0b").or_else(|| body.strip_prefix("0B")) {
        (2, rest)
    } else if body.len() > 1 && body.starts_with('0') {
        (8, body)
    } else {
        (10, body)
    };

    if digits.is_empty() {
        return Err(());
    }

    let unsigned = i64::from_str_radix(digits, radix).map_err(|_| ())?;
    Ok(if negative { -unsigned } else { unsigned })
}

fn get_default_cache_path() -> String {
    let uid = unsafe { libc::geteuid() };
    format!("/tmp/lunaar-device-cache-{}", uid)
}

fn load_cache(cache_path: &str) -> Result<(String, u8, u8), ()> {
    let file = File::open(cache_path).map_err(|_| ())?;
    let reader = BufReader::new(file);

    let mut cached_path: Option<String> = None;
    let mut cached_devnum: Option<u8> = None;
    let mut cached_feature_index: Option<u8> = None;

    for line in reader.lines() {
        let line = line.map_err(|_| ())?;
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };

        match key {
            "path" => {
                if value.is_empty() {
                    return Err(());
                }
                cached_path = Some(value.to_string());
            }
            "devnum" => {
                let parsed = parse_long_strict(value, 10, 0, 255)?;
                cached_devnum = Some(parsed as u8);
            }
            "feature_index" => {
                let parsed = parse_long_strict(value, 10, 0, 255)?;
                cached_feature_index = Some(parsed as u8);
            }
            _ => {}
        }
    }

    let path = cached_path.ok_or(())?;
    let devnum = cached_devnum.ok_or(())?;
    let feature_index = cached_feature_index.ok_or(())?;

    Ok((path, devnum, feature_index))
}

fn save_cache(cache_path: &str, device_path: &str, devnum: u8, feature_index: u8) -> Result<(), ()> {
    if device_path.is_empty() {
        return Err(());
    }

    let tmp_path = format!("{}.tmp.{}", cache_path, process::id());
    let mut fp = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(&tmp_path)
        .map_err(|_| ())?;

    let _ = fs::set_permissions(&tmp_path, fs::Permissions::from_mode(0o600));

    if writeln!(fp, "version=1").is_err()
        || writeln!(fp, "path={}", device_path).is_err()
        || writeln!(fp, "devnum={}", devnum).is_err()
        || writeln!(fp, "feature_index={}", feature_index).is_err()
    {
        let _ = fs::remove_file(&tmp_path);
        return Err(());
    }

    if fp.flush().is_err() {
        let _ = fs::remove_file(&tmp_path);
        return Err(());
    }

    drop(fp);

    if fs::rename(&tmp_path, cache_path).is_err() {
        let _ = fs::remove_file(&tmp_path);
        return Err(());
    }

    Ok(())
}

fn invalidate_cache(cache_path: &str) {
    if !cache_path.is_empty() {
        let _ = fs::remove_file(cache_path);
    }
}

fn next_sw_id() -> u16 {
    static SW: AtomicU8 = AtomicU8::new(0x0F);

    loop {
        let current = SW.load(Ordering::Relaxed);
        let next = if current < 0x0F { current + 1 } else { 0x02 };
        if SW
            .compare_exchange(current, next, Ordering::Relaxed, Ordering::Relaxed)
            .is_ok()
        {
            return next as u16;
        }
    }
}

fn write_long(dev: &HidDevice, devnum: u8, payload: &[u8]) -> Result<(), ()> {
    let mut buf = [0u8; 2 + MAX_PAYLOAD];
    buf[0] = REPORT_ID_LONG;
    buf[1] = devnum;
    buf[2..2 + payload.len()].copy_from_slice(payload);

    let written = dev.write(&buf).map_err(|_| ())?;
    if written == buf.len() {
        Ok(())
    } else {
        Err(())
    }
}

fn read_matching(
    dev: &HidDevice,
    expected_devnum: u8,
    expected_req: u16,
    timeout_ms: i32,
) -> Result<Vec<u8>, ()> {
    let slice_ms = 200;
    let mut elapsed = 0;
    let mut buf = [0u8; 32];

    while elapsed < timeout_ms {
        let remaining = timeout_ms - elapsed;
        let this_timeout = remaining.min(slice_ms);

        let n = dev.read_timeout(&mut buf, this_timeout).map_err(|_| ())?;
        elapsed += this_timeout;

        if n == 0 {
            continue;
        }

        if !matches!(buf[0], 0x10 | 0x11 | 0x20 | 0x21) {
            continue;
        }

        let devnum = buf[1];
        if !(devnum == expected_devnum || devnum == (expected_devnum ^ 0xFF)) {
            continue;
        }

        let data_len = n.saturating_sub(2);
        if data_len < 2 {
            continue;
        }

        let resp_req = u16::from(buf[2]) << 8 | u16::from(buf[3]);
        if resp_req == expected_req {
            return Ok(buf[4..n].to_vec());
        }
    }

    Err(())
}

fn hidpp_request(
    dev: &HidDevice,
    devnum: u8,
    mut request_id: u16,
    params: &[u8],
    expect_reply: bool,
    timeout_ms: i32,
) -> Result<Vec<u8>, ()> {
    if devnum != 0xFF && (request_id & 0x8000) == 0 {
        let sw_id = next_sw_id();
        request_id = (request_id & 0xFFF0) | (sw_id & 0x0F);
    }

    if params.len() > (MAX_PAYLOAD - 2) {
        return Err(());
    }

    let mut payload = [0u8; MAX_PAYLOAD];
    payload[0] = (request_id >> 8) as u8;
    payload[1] = (request_id & 0xFF) as u8;
    if !params.is_empty() {
        payload[2..2 + params.len()].copy_from_slice(params);
    }

    write_long(dev, devnum, &payload)?;

    if !expect_reply {
        return Ok(Vec::new());
    }

    read_matching(dev, devnum, request_id, timeout_ms)
}

fn get_feature_set_index(dev: &HidDevice, devnum: u8) -> Result<u8, ()> {
    let params = [
        (FEATURE_FEATURE_SET >> 8) as u8,
        (FEATURE_FEATURE_SET & 0xFF) as u8,
    ];
    let resp = hidpp_request(dev, devnum, 0x0000, &params, true, DEFAULT_TIMEOUT_MS)?;
    resp.first().copied().ok_or(())
}

fn get_feature_index(dev: &HidDevice, devnum: u8, feature_id: u16) -> Result<u8, ()> {
    let params = [(feature_id >> 8) as u8, (feature_id & 0xFF) as u8];
    let resp = hidpp_request(dev, devnum, 0x0000, &params, true, DEFAULT_TIMEOUT_MS)?;
    let idx = resp.first().copied().ok_or(())?;
    if idx == 0 {
        return Err(());
    }
    Ok(idx)
}

fn switch_host(dev: &HidDevice, devnum: u8, feature_index: u8, host_slot: u8) -> Result<(), ()> {
    let request_id = u16::from(feature_index) << 8 | 0x10;
    let params = [host_slot];
    let _ = hidpp_request(dev, devnum, request_id, &params, false, FAST_TIMEOUT_MS)?;
    Ok(())
}

fn open_by_path(api: &HidApi, dev_path: &str) -> Option<HidDevice> {
    let c_path = CString::new(dev_path.as_bytes()).ok()?;
    api.open_path(c_path.as_c_str()).ok()
}

fn open_first_device(api: &HidApi) -> Option<(HidDevice, u8, u8, Option<String>)> {
    for info in api.device_list() {
        if info.vendor_id() != LOGITECH_VID {
            continue;
        }

        let handle = match info.open_device(api) {
            Ok(handle) => handle,
            Err(_) => continue,
        };

        for dn in 0u8..=7 {
            let fs_index = match get_feature_set_index(&handle, dn) {
                Ok(idx) if idx != 0 => idx,
                _ => continue,
            };

            let _ = fs_index;
            let ch_index = match get_feature_index(&handle, dn, FEATURE_CHANGE_HOST) {
                Ok(idx) => idx,
                Err(_) => continue,
            };

            let path = Some(info.path().to_string_lossy().into_owned());
            return Some((handle, dn, ch_index, path));
        }
    }

    None
}

fn open_device_by_path(api: &HidApi, dev_path: &str) -> Option<(HidDevice, u8, u8)> {
    let handle = open_by_path(api, dev_path)?;

    for dn in 0u8..=7 {
        let fs_index = match get_feature_set_index(&handle, dn) {
            Ok(idx) if idx != 0 => idx,
            _ => continue,
        };

        let _ = fs_index;
        let ch_index = match get_feature_index(&handle, dn, FEATURE_CHANGE_HOST) {
            Ok(idx) => idx,
            Err(_) => continue,
        };

        return Some((handle, dn, ch_index));
    }

    None
}

fn usage(prog: &str) {
    eprintln!(
        "Usage: {} [-s] [--path PATH] [--devnum DEVNUM] [--feature-index INDEX] [--slot SLOT] [--cache auto|off|refresh] [--cache-file PATH] <host-number-1-3>",
        prog
    );
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let prog = args.first().map(String::as_str).unwrap_or("lunaar-switch");

    let mut device_path: Option<String> = None;
    let mut cache_file_override: Option<String> = None;
    let mut devnum_override: Option<i32> = None;
    let mut feature_index_override: i32 = 14;
    let mut feature_index_set = false;
    let mut cache_mode = CacheMode::Auto;
    let mut silent = false;
    let mut host: Option<i64> = None;
    let mut host_arg_idx = 1usize;

    while host_arg_idx < args.len() {
        match args[host_arg_idx].as_str() {
            "-s" => {
                silent = true;
                host_arg_idx += 1;
            }
            "--path" if host_arg_idx + 1 < args.len() => {
                device_path = Some(args[host_arg_idx + 1].clone());
                host_arg_idx += 2;
            }
            "--devnum" if host_arg_idx + 1 < args.len() => {
                let parsed = match parse_long_strict(&args[host_arg_idx + 1], 0, 0, 255) {
                    Ok(v) => v,
                    Err(_) => {
                        usage(prog);
                        process::exit(1);
                    }
                };
                devnum_override = Some(parsed as i32);
                host_arg_idx += 2;
            }
            "--feature-index" if host_arg_idx + 1 < args.len() => {
                let parsed = match parse_long_strict(&args[host_arg_idx + 1], 0, 0, 255) {
                    Ok(v) => v,
                    Err(_) => {
                        usage(prog);
                        process::exit(1);
                    }
                };
                feature_index_override = parsed as i32;
                feature_index_set = true;
                host_arg_idx += 2;
            }
            "--slot" if host_arg_idx + 1 < args.len() => {
                let parsed = match parse_long_strict(&args[host_arg_idx + 1], 10, 1, 3) {
                    Ok(v) => v,
                    Err(_) => {
                        usage(prog);
                        process::exit(1);
                    }
                };
                host = Some(parsed);
                host_arg_idx += 2;
            }
            "--cache" if host_arg_idx + 1 < args.len() => {
                cache_mode = match args[host_arg_idx + 1].as_str() {
                    "auto" => CacheMode::Auto,
                    "off" => CacheMode::Off,
                    "refresh" => CacheMode::Refresh,
                    _ => {
                        usage(prog);
                        process::exit(1);
                    }
                };
                host_arg_idx += 2;
            }
            "--cache-file" if host_arg_idx + 1 < args.len() => {
                cache_file_override = Some(args[host_arg_idx + 1].clone());
                host_arg_idx += 2;
            }
            _ => break,
        }
    }

    if host.is_none() {
        if host_arg_idx >= args.len() {
            usage(prog);
            process::exit(1);
        }

        host = match parse_long_strict(&args[host_arg_idx], 10, 1, 3) {
            Ok(v) => Some(v),
            Err(_) => {
                usage(prog);
                process::exit(1);
            }
        };
    }

    let host = host.unwrap();
    if !(1..=3).contains(&host) {
        usage(prog);
        process::exit(1);
    }
    let host_slot = (host - 1) as u8;

    let api = match HidApi::new() {
        Ok(api) => api,
        Err(_) => {
            eprintln!("hidapi init failed");
            process::exit(1);
        }
    };

    let cache_path = cache_file_override.unwrap_or_else(get_default_cache_path);

    let mut devnum = 0u8;
    let mut ch_index = 0u8;
    let mut path: Option<String> = None;
    let mut dev: Option<HidDevice> = None;

    let has_explicit_fast_path = device_path.is_some() && devnum_override.is_some();

    if cache_mode == CacheMode::Auto
        && !has_explicit_fast_path
        && device_path.is_none()
        && devnum_override.is_none()
        && !feature_index_set
    {
        if let Ok((cached_path, cached_devnum, cached_feature_index)) = load_cache(&cache_path) {
            if let Some(cached_dev) = open_by_path(&api, &cached_path) {
                if switch_host(&cached_dev, cached_devnum, cached_feature_index, host_slot).is_ok() {
                    if !silent {
                        println!(
                            "Switched host to slot {} (device {}, feature index {}) via {}",
                            host, cached_devnum, cached_feature_index, cached_path
                        );
                    }
                    process::exit(0);
                }
            }
            invalidate_cache(&cache_path);
        }
    }

    if cache_mode == CacheMode::Refresh {
        invalidate_cache(&cache_path);
    }

    if let (Some(ref dev_path), Some(devnum_o)) = (device_path.as_ref(), devnum_override) {
        if let Some(handle) = open_by_path(&api, dev_path) {
            dev = Some(handle);
            devnum = devnum_o as u8;
            ch_index = feature_index_override as u8;
            if !silent {
                path = Some(dev_path.clone());
            }
        }
    } else if let Some(ref dev_path) = device_path {
        if let Some((handle, found_devnum, found_ch_index)) = open_device_by_path(&api, dev_path) {
            dev = Some(handle);
            devnum = found_devnum;
            ch_index = found_ch_index;
            if !silent {
                path = Some(dev_path.clone());
            }
        } else {
            eprintln!("Failed to open device at path: {}", dev_path);
            eprintln!("Use auto-discovery or provide --devnum and --feature-index to skip discovery");
            process::exit(1);
        }
    } else if let Some((handle, found_devnum, found_ch_index, found_path)) = open_first_device(&api) {
        dev = Some(handle);
        devnum = found_devnum;
        ch_index = found_ch_index;
        path = found_path;
    }

    let Some(dev) = dev else {
        eprintln!("No Logitech HID++ device with CHANGE_HOST found");
        process::exit(1);
    };

    let rc = switch_host(&dev, devnum, ch_index, host_slot);
    if rc.is_ok() && !silent {
        if let Some(ref p) = path {
            println!(
                "Switched host to slot {} (device {}, feature index {}) via {}",
                host, devnum, ch_index, p
            );
        } else {
            println!(
                "Switched host to slot {} (device {}, feature index {})",
                host, devnum, ch_index
            );
        }
    } else if rc.is_err() {
        eprintln!("Failed to switch host");
    }

    if rc.is_ok() && cache_mode != CacheMode::Off {
        let path_for_cache = device_path.as_deref().or(path.as_deref());
        if let Some(path_for_cache) = path_for_cache {
            if !path_for_cache.is_empty() {
                let _ = save_cache(&cache_path, path_for_cache, devnum, ch_index);
            }
        }
    }

    if rc.is_ok() {
        process::exit(0);
    }

    process::exit(1);
}
