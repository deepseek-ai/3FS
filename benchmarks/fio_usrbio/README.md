# FIO engine for 3FS USRBIO

This repository contains the [fio] external plugin used for benchmarking [3FS] USRBIO.

## Build

First, build 3FS and fio.

Configure the following variables:
- `HF3FS_LIB_DIR`: directory contains `libhf3fs_api_shared.so`, the default path in 3FS repo is `3FS/build/src/lib/api`.
- `HF3FS_INCLUDE_DIR`: directory contains `hf3fs_usrbio.h`, the default path in 3FS repo is `3FS/src/lib/api`.
- `FIO_SRC_DIR`: directory contains `config-host.h`. After building fio, this header will be in the root of the fio repo.

Then run:
```
make HF3FS_LIB_DIR=${HF3FS_LIB_DIR} HF3FS_INCLUDE_DIR=${HF3FS_INCLUDE_DIR} FIO_SRC_DIR=${FIO_SRC_DIR}
```

You will get the external plugin as `hf3fs_usrbio.so`.

## Usage

To use this plugin, set the `ioengine` args in fio as `external:hf3fs_usrbio.so`. Please refer to [fio documentation] for further explanation.

Since multiple 3FS instances might be mounted, you need to specify the 3FS instance during benchmarking. This can be done by adding the `mountpoint` arguments (in the `.fio`):
```
mountpoint=/hf3fs/mount/point
```

To benchmark batched small I/Os, please set these four parameters to `batch_size` simultaneously:
```
iodepth=1024
iodepth_batch_submit=1024
iodepth_batch_complete_min=1024
iodepth_batch_complete_max=1024
```

[fio]: https://github.com/axboe/fio
[3FS]: https://github.com/deepseek-ai/3FS
[fio documentation]: https://fio.readthedocs.io/en/latest/fio_doc.html
