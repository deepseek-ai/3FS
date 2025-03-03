use nix::dir::{self, Dir};
use nix::fcntl::OFlag;
use nix::sys::stat::Mode;
use nix::unistd::{self, getegid, geteuid, setegid, seteuid, Gid, Uid};
use nix::{ioctl_read, ioctl_write_ptr, NixPath};
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::abort;
use std::string::String;
use std::time::Duration;
use structopt::StructOpt;
use tracing::{error, info, trace, warn, Level};
use tracing_subscriber::{filter, prelude::*};

const HF3FS_MAGIC_NUM: u32 = 0x8f3f5fff;
ioctl_read!(ioctl_get_magic, b'h', 2, u32);

ioctl_read!(ioctl_get_version, b'h', 3, u32);

const NAME_MAX: usize = 255;

#[repr(C)]
struct Hf3fsIoctlRemove {
    parent: u64,
    name: [libc::c_char; NAME_MAX + 1],
    recursive: bool,
}

// Define the ioctl write operation
ioctl_write_ptr!(hf3fs_ioctl_remove, b'h', 15, Hf3fsIoctlRemove);

fn parse_datetime(s: &str) -> Option<chrono::DateTime<chrono::FixedOffset>> {
    if let Ok(native_datetime) = chrono::NaiveDateTime::parse_from_str(s, "%Y%m%d_%H%M") {
        let utc8 = chrono::FixedOffset::east_opt(8 * 60 * 60).unwrap();
        Some(native_datetime.and_local_timezone(utc8).unwrap())
    } else {
        None
    }
}

#[derive(serde::Serialize)]
struct DirEntry {
    ino: u64,
    ftype: u8,
    name: String,
}

impl DirEntry {
    pub fn new(entry: nix::dir::Entry) -> DirEntry {
        DirEntry {
            ino: entry.ino(),
            ftype: entry.file_type().unwrap() as u8,
            name: entry
                .file_name()
                .to_str()
                .map(|str| str.to_string())
                .unwrap_or_else(|_| format!("non-utf8-{:?}", entry.file_name())),
        }
    }
}

struct Trash {
    user: Uid,
    user_name: String,
    path: PathBuf,
    dir: nix::dir::Dir,
    dir_stat: nix::sys::stat::FileStat,
}

impl Trash {
    pub fn open<P: AsRef<Path>>(user: libc::uid_t, user_name: &str, path: P) -> nix::Result<Trash> {
        const ROOT: Uid = Uid::from_raw(0);
        assert_ne!(geteuid(), ROOT);
        assert_eq!(geteuid(), Uid::from_raw(user));

        // open trash directory
        let trash_dir = Dir::open(
            path.as_ref(),
            OFlag::O_RDONLY | OFlag::O_DIRECTORY,
            nix::sys::stat::Mode::empty(),
        )
        .map_err(|err| {
            error!(
                "open trash directory {:?} failed, error {}",
                path.as_ref(),
                err
            );
            err
        })?;

        // try stat trash directory and check user
        let trash_stat = nix::sys::stat::fstat(trash_dir.as_raw_fd())?;
        if trash_stat.st_uid != user || trash_stat.st_gid != user {
            error!(
                "trash directory {:?}, owner {}:{} != {}, {}",
                path.as_ref(),
                trash_stat.st_uid,
                trash_stat.st_gid,
                user,
                user_name
            );
            return Err(nix::Error::from(nix::Error::EACCES));
        }

        // ioctl to check magic number, magic number must match
        let mut magic_number: u32 = 0;
        unsafe {
            ioctl_get_magic(trash_dir.as_raw_fd(), &mut magic_number)
                .expect(&format!("trash directory {:?} not on 3fs", path.as_ref()));
        }
        if magic_number != HF3FS_MAGIC_NUM {
            error!(
                "trash directory {:?} magic number {:x} != {:x}, not 3fs",
                path.as_ref(),
                magic_number,
                HF3FS_MAGIC_NUM
            );
            std::process::abort();
        }

        // get ioctl version
        let mut version: u32 = 0;
        unsafe {
            ioctl_get_version(trash_dir.as_raw_fd(), &mut version)
                .expect("get ioctl version failed, should update fuse version");
        }

        return Ok(Trash {
            user: Uid::from_raw(user),
            user_name: user_name.to_string(),
            path: path.as_ref().to_owned(),
            dir: trash_dir,
            dir_stat: trash_stat,
        });
    }

    pub fn clean(self, clean_unknown: bool) -> nix::Result<usize> {
        assert!(!clean_unknown);

        let dir_fd = unistd::dup(self.dir.as_raw_fd()).map_err(|err| {
            error!("trash directory {:?} dup fd failed, {}", self.path, err);
            err
        })?;
        let dir = Dir::from_fd(dir_fd).map_err(|err| {
            error!("trash directory {:?} from fd failed, {}", self.path, err);
            err
        })?;

        let mut removed = 0;
        for entry in dir {
            let entry = entry.map_err(|err| {
                error!("scan trash directory {:?} failed, error {}", self.path, err);
                err
            })?;
            trace!(
                "scan trash directory {:?}, found {:?}",
                self.path,
                entry.file_name()
            );

            // skip . and ..
            if let Ok(name) = entry.file_name().to_str() {
                if name == "." || name == ".." {
                    continue;
                }
            }

            if self.clean_if_expired(&entry) {
                removed += 1
            }
        }
        Ok(removed)
    }

    fn clean_if_expired(&self, entry: &dir::Entry) -> bool {
        let entry_name = entry.file_name();
        match Self::check_item(&entry) {
            Ok(expired) => {
                if !expired {
                    trace!(
                        "trash directory {:?}, not expired item {:?}",
                        self.path,
                        entry_name
                    );
                    return false;
                }

                match self.remove_item(entry) {
                    Ok(_) => {
                        info!(
                            "trash directory {:?}, item {:?} removed",
                            self.path, entry_name
                        );
                        return true;
                    }
                    Err(errno) => {
                        error!(
                            "trash directory {:?}, item {:?} remove failed {}",
                            self.path, entry_name, errno
                        );
                        return false;
                    }
                }
            }
            Err(msg) => {
                warn!(
                    "scan trash directory {:?}, unknown item {:?}, msg {}",
                    self.path,
                    entry.file_name(),
                    msg
                );
                // todo!(support clean unknown)
                return false;
            }
        }
    }

    fn check_item(entry: &nix::dir::Entry) -> Result<bool, String> {
        if entry.file_type().unwrap() != nix::dir::Type::Directory {
            return Err("not directory".to_string());
        }
        let name = entry.file_name().to_str();
        if let Err(_) = name {
            return Err("not utf8".to_string());
        }
        let name = name.unwrap();
        let parts: Vec<&str> = name.split("-").collect();
        if parts.len() != 3 {
            return Err("invalid name".to_string());
        }
        match (parse_datetime(parts[1]), parse_datetime(parts[2])) {
            (Some(begin_time), Some(expire_time)) => {
                if begin_time > expire_time {
                    return Err(format!(
                        "invalid timestamp, begin {} > {}",
                        begin_time, expire_time
                    ));
                }
                let now = chrono::Utc::now();
                trace!(
                    "now {}, expire_time {}, expired {}",
                    now,
                    expire_time,
                    now > expire_time
                );
                return Ok(now > expire_time);
            }
            _ => {
                return Err("invalid name".to_string());
            }
        }
    }

    fn remove_item(&self, entry: &nix::dir::Entry) -> nix::Result<()> {
        match Self::check_item(entry) {
            Ok(true) => (),
            Ok(false) => {
                error!(
                    "trash directory {:?} try clean unexpired item {:?}",
                    self.path,
                    entry.file_name()
                );
                std::process::abort();
            }
            Err(msg) => {
                error!(
                    "trash directory {:?} try clean unknown item {:?}, {}",
                    self.path,
                    entry.file_name(),
                    msg
                );
                std::process::abort();
            }
        }
        assert_eq!(Self::check_item(entry), Ok(true));
        const ROOT: Uid = Uid::from_raw(0);
        assert_ne!(geteuid(), ROOT);
        assert_eq!(geteuid(), self.user);

        if entry.file_name().len() > NAME_MAX {
            return Err(nix::errno::Errno::ENAMETOOLONG);
        }

        // record trash item entries before remove
        let sub_entries = Dir::openat(
            Some(self.dir.as_raw_fd()),
            entry.file_name(),
            OFlag::O_RDONLY | OFlag::O_DIRECTORY,
            Mode::empty(),
        )
        .and_then(|mut subdir| {
            let mut sub_entries = Vec::new();
            for sub_entry in subdir.iter() {
                match sub_entry {
                    Ok(sub_entry) => sub_entries.push(DirEntry::new(sub_entry)),
                    Err(errno) => return Err(errno),
                }
            }
            Ok(sub_entries)
        });

        match sub_entries {
            Ok(sub_entries) => {
                info!(
                    target: "event",
                    user = self.user.as_raw(),
                    user_name = self.user_name,
                    op = "list",
                    item = format!("{:?}", entry.file_name()),
                    sub_entries = serde_json::to_string(&sub_entries).unwrap_or("null".to_string()),
                    errno = 0
                );
            }
            Err(err) => {
                info!(
                    target: "event",
                    user = self.user.as_raw(),
                    user_name = self.user_name,
                    op = "list",
                    item = format!("{:?}", entry.file_name()),
                    sub_entries = "",
                    errno = err as i32
                );
            }
        }

        let mut remove_arg = Hf3fsIoctlRemove {
            parent: self.dir_stat.st_ino,
            name: [0 as libc::c_char; NAME_MAX + 1],
            recursive: true,
        };
        let ret = unsafe {
            let file_name = entry.file_name();
            assert!(file_name.len() <= NAME_MAX);
            std::ptr::copy(
                file_name.as_ptr(),
                remove_arg.name.as_mut_ptr(),
                file_name.len(),
            );
            hf3fs_ioctl_remove(self.dir.as_raw_fd(), &mut remove_arg)
        };
        info!(
            target: "event",
            user = self.user.as_raw(),
            user_name = self.user_name,
            op = "remove",
            item = format!("{:?}", entry.file_name()),
            errno = match ret {
                Ok(_) => 0,
                Err(errno) => errno as i32
            }
        );

        return ret.map(|_| ());
    }
}

struct UserContext {
    orig_uid: Uid,
    orig_gid: Gid,
}

impl UserContext {
    pub fn new(uid: Uid, gid: Gid) -> UserContext {
        assert_ne!(uid, Uid::from_raw(0));
        assert_ne!(gid, Gid::from_raw(0));

        let orig_uid = geteuid();
        let orig_gid = getegid();

        setegid(gid).expect("setegid failed, should be root or has CAP_SETUID");
        seteuid(uid).expect("seteuid failed, should be root or has CAP_SETGID");
        UserContext { orig_uid, orig_gid }
    }
}

impl Drop for UserContext {
    fn drop(&mut self) {
        seteuid(self.orig_uid).expect("restore euid failed");
        setegid(self.orig_gid).expect("restore egid failed");
    }
}

fn scan_trash<P: AsRef<Path>>(trash_root: &P) -> nix::Result<()> {
    let trash_root = trash_root.as_ref();
    let mut dir = Dir::open(
        trash_root,
        OFlag::O_DIRECTORY | OFlag::O_RDONLY,
        nix::sys::stat::Mode::empty(),
    )
    .map_err(|err| {
        error!("open trash root {:?} failed, {}", trash_root, err);
        err
    })?;

    let dir_fd = dir.as_raw_fd();
    let dir_stat = nix::sys::stat::fstat(dir_fd)?;
    if dir_stat.st_uid != 0 || dir_stat.st_gid != 0 {
        error!(
            "trash root {:?} is not owned by root user, {}:{}",
            trash_root, dir_stat.st_uid, dir_stat.st_gid
        );
        abort();
    }

    for entry in dir.iter() {
        let entry = entry.map_err(|err| {
            error!("scan trash root {:?} failed, {}", trash_root, err);
            err
        })?;

        let entry_name = match entry.file_name().to_str() {
            Ok(name) => name,
            Err(err) => {
                error!(
                    "trash directory name {:?} is not utf8, {}",
                    entry.file_name(),
                    err
                );
                continue;
            }
        };
        if entry_name == "." || entry_name == ".." {
            continue;
        }

        let entry_stat = match nix::sys::stat::fstatat(
            Some(dir_fd),
            entry.file_name(),
            nix::fcntl::AtFlags::AT_SYMLINK_NOFOLLOW,
        ) {
            Ok(stat) => stat,
            Err(err) => {
                error!(
                    "stat trash direstory {:?} failed, {}",
                    entry.file_name(),
                    err
                );
                continue;
            }
        };
        if !nix::sys::stat::SFlag::from_bits_truncate(entry_stat.st_mode)
            .contains(nix::sys::stat::SFlag::S_IFDIR)
        {
            error!("trash directory {:?} is not directory", entry.file_name());
            continue;
        }
        if entry_stat.st_uid == 0 || entry_stat.st_gid == 0 {
            error!("trash directory {:?} owned by root user", entry.file_name());
            continue;
        }

        // create user context
        let _user_ctx = UserContext::new(
            Uid::from_raw(entry_stat.st_uid),
            Gid::from_raw(entry_stat.st_gid),
        );

        // open trash
        let trash = match Trash::open(entry_stat.st_uid, entry_name, trash_root.join(entry_name)) {
            Ok(trash) => trash,
            Err(errno) => {
                error!(
                    "open trash directory {:?} failed, errno {}",
                    entry_name, errno
                );
                continue;
            }
        };

        // clean trash
        match trash.clean(false) {
            Ok(n) => {
                if n > 0 {
                    info!(
                        "clean trash directory {} success, cleaned {} item",
                        entry_name, n
                    );
                } else {
                    trace!(
                        "clean trash directory {} success, cleaned {} item",
                        entry_name,
                        n
                    );
                }
            }
            Err(errno) => {
                error!(
                    "clean trash directory {} failed, errno {}",
                    entry_name, errno
                );
            }
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::Timelike;
    use tempfile::TempDir;

    #[test]
    #[ignore]
    fn test_usercontext() {
        let orig_uid = geteuid();
        let orig_gid = getegid();
        println!("uid: {}, gid {}", geteuid(), getegid());

        let guard1 = UserContext::new(Uid::from_raw(1000), Gid::from_raw(2000));
        assert_eq!(geteuid(), Uid::from_raw(1000));
        assert_eq!(getegid(), Gid::from_raw(2000));
        drop(guard1);

        assert_eq!(geteuid(), orig_uid);
        assert_eq!(getegid(), orig_gid);
    }

    #[test]
    fn test_parse_item() {
        let tmp_dir = TempDir::new().unwrap();
        std::fs::create_dir_all(tmp_dir.path().join("invalid-name")).unwrap();
        std::fs::File::create(tmp_dir.path().join("file")).unwrap();
        std::fs::create_dir_all(tmp_dir.path().join("1d-20240801_0000-20240701_0000")).unwrap();
        std::fs::create_dir_all(tmp_dir.path().join("1d-20240801_0000-20240802_0100")).unwrap();

        let five_minutes_after = chrono::Local::now()
            .checked_add_signed(chrono::TimeDelta::minutes(5))
            .unwrap()
            .with_second(0)
            .unwrap()
            .with_nanosecond(0)
            .unwrap();

        let five_minutes_after_str = five_minutes_after.format("%Y%m%d_%H%M").to_string();
        println!("{}", five_minutes_after_str);
        assert_eq!(
            parse_datetime(&five_minutes_after_str).unwrap(),
            five_minutes_after
        );
        let not_expired = format!("nd-20240801_0000-{}", five_minutes_after_str);
        std::fs::create_dir_all(tmp_dir.path().join(not_expired.clone())).unwrap();

        let five_minutes_before = chrono::Local::now()
            .checked_sub_signed(chrono::TimeDelta::minutes(5))
            .unwrap()
            .with_second(0)
            .unwrap()
            .with_nanosecond(0)
            .unwrap();
        let five_minutes_before_str = five_minutes_before.format("%Y%m%d_%H%M").to_string();
        println!("{}", five_minutes_before);
        assert_eq!(
            parse_datetime(&five_minutes_before_str).unwrap(),
            five_minutes_before
        );
        let expired = format!("nd-20240801_0000-{}", five_minutes_before_str);
        std::fs::create_dir_all(tmp_dir.path().join(expired.clone())).unwrap();

        let dir = Dir::open(
            tmp_dir.path(),
            OFlag::O_DIRECTORY | OFlag::O_RDONLY,
            Mode::empty(),
        )
        .unwrap();
        for entry in dir {
            let entry = entry.unwrap();
            match entry.file_name().to_str().unwrap() {
                "invalid-name" => {
                    assert_eq!(Trash::check_item(&entry), Err("invalid name".to_string()))
                }
                "file" => assert_eq!(Trash::check_item(&entry), Err("not directory".to_string())),
                "1d-20240801_0000-20240701_0000" => {
                    assert!(Trash::check_item(&entry).is_err())
                }
                "1d-20240801_0000-20240802_0100" => assert_eq!(Trash::check_item(&entry), Ok(true)),
                other => {
                    if other == not_expired {
                        assert_eq!(Trash::check_item(&entry), Ok(false));
                    } else if other == expired {
                        assert_eq!(Trash::check_item(&entry), Ok(true));
                    } else {
                        assert!(other == "." || other == "..");
                    }
                }
            }
        }
    }
}

#[derive(StructOpt, Debug)]
#[structopt(name = "trash_cleaner")]
struct Opt {
    /// Path to the file
    #[structopt(short, long, parse(from_os_str), help = "path to trash directory")]
    paths: Vec<PathBuf>,

    /// Interval in seconds
    #[structopt(
        short,
        long,
        help = "Scan interval (in seconds), exit after one scan if set to 0"
    )]
    interval: u64,

    #[structopt(long)]
    abort_on_error: bool,

    #[structopt(
        long,
        parse(from_os_str),
        default_value = "./",
        help = "Log path, default is current directory"
    )]
    log: PathBuf,

    #[structopt(long, default_value = "info", help = "Log level, default is info")]
    log_level: Level,

    #[structopt(
        long,
        default_value = "warn",
        help = "stdout log level, default is warn"
    )]
    stdout_level: Level,
}

fn main() {
    let opt = Opt::from_args();
    // log
    let log_appender = tracing_appender::rolling::RollingFileAppender::builder()
        .rotation(tracing_appender::rolling::Rotation::DAILY)
        .filename_prefix("trash_cleaner")
        .filename_suffix("log")
        .build(opt.log.join("log"))
        .unwrap();
    let (log_non_blocking, _log_guard) = tracing_appender::non_blocking(log_appender);
    let log = tracing_subscriber::fmt::layer()
        .with_ansi(false)
        .with_line_number(true)
        .with_timer(tracing_subscriber::fmt::time::ChronoLocal::rfc_3339())
        .with_writer(log_non_blocking)
        .with_filter(filter::filter_fn(|meta| meta.target() != "event"))
        .with_filter(filter::LevelFilter::from_level(opt.log_level));

    // stdout
    let stdout = tracing_subscriber::fmt::layer()
        .with_ansi(true)
        .with_line_number(true)
        .with_timer(tracing_subscriber::fmt::time::ChronoLocal::rfc_3339())
        .with_filter(filter::filter_fn(|meta| meta.target() != "event"))
        .with_filter(filter::LevelFilter::from_level(opt.stdout_level));

    // json event
    let event_appender = tracing_appender::rolling::RollingFileAppender::builder()
        .rotation(tracing_appender::rolling::Rotation::DAILY)
        .filename_prefix("event")
        .filename_suffix("log")
        .build(opt.log.join("event"))
        .unwrap();
    let (event_non_blocking, _event_guard) = tracing_appender::non_blocking(event_appender);
    let event = tracing_subscriber::fmt::layer()
        .json()
        .with_ansi(false)
        .with_timer(tracing_subscriber::fmt::time::ChronoLocal::rfc_3339())
        .with_writer(event_non_blocking)
        .with_filter(filter::filter_fn(|meta| meta.target() == "event"))
        .with_filter(filter::LevelFilter::from_level(opt.log_level));

    tracing_subscriber::registry()
        .with(log)
        .with(stdout)
        .with(event)
        .init();

    for path in &opt.paths {
        let path = path.to_str().unwrap();
        if !path.contains("trash") {
            error!("trash path {} doesn't contains trash", path);
            std::process::abort();
        }
    }

    loop {
        for path in &opt.paths {
            info!("scan trash {:?}", path);
            match scan_trash(&path) {
                Ok(_) => info!("scan trash {:?} success", path),
                Err(errno) => {
                    error!("scan trash {:?} failed, {}", path, errno);
                    if opt.abort_on_error {
                        std::process::abort();
                    }
                }
            }
        }
        if opt.interval == 0 {
            break;
        }
        std::thread::sleep(Duration::from_secs(opt.interval));
    }
}
