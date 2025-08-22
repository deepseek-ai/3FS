# hf3fs_cli

build:
```bash
python3 setup_hf3fs_utils.py bdist_wheel
```

usage:
```.bash
$ hf3fs_cli rmtree --help
Usage: hf3fs_cli rmtree [OPTIONS] [DIR_PATHS]...

  Move a directory tree to the trash and set an expiration time, it will be automatically deleted after expiration

  Example:
  hf3fs_cli rmtree <path/to/remove> --expire <expire_time>

  - Use --expire [1h|3h|8h|1d|3d|7d] to specify the expiration time, the directory will be deleted after expiration.
  - Before expiration, you can restore the directory from the trash using `hf3fs_cli mv <trash_path> <target_path>`.
  - If you need to free up space immediately, you can use `hf3fs_cli rmtree <trash_path>` to delete the data in the trash immediately, this operation cannot be undone!
  - Use `ls /path/to/hf3fs/trash` to view the trash.

Options:
  --expire [1h|3h|8h|1d|3d|7d]  Expiration time, contents in the trash will be automatically deleted after expiration
  -y, --yes                     Skip confirmation prompt and delete immediately
  --help                        Show this message and exit.

$ hf3fs_cli mv --help
Usage: hf3fs_cli mv [OPTIONS] OLD_PATH NEW_PATH

  Move files, supports moving files between different mount points within the same 3FS

Options:
  --help  Show this message and exit.
```

If you want to use `rmtree` command, the administrator needs to create a trash directory for each user at `/{3fs_mountpoint}/trash/{user_name}`. The cleanup of the trash directory is handled by the `trash_cleaner`. For instructions on how to use it, please refer to `src/client/trash_cleaner/`.