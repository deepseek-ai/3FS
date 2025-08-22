# trash_cleaner

usage:
```.bash
trash_cleaner 0.1.0

USAGE:
    trash_cleaner [FLAGS] [OPTIONS] --interval <interval>

FLAGS:
        --abort-on-error    
    -h, --help              Prints help information
    -V, --version           Prints version information

OPTIONS:
    -i, --interval <interval>            Scan interval (in seconds), exit after one scan if set to 0
        --log <log>                      Log path, default is current directory [default: ./]
        --log-level <log-level>          Log level, default is info [default: info]
    -p, --paths <paths>...               path to trash directory
        --stdout-level <stdout-level>    stdout log level, default is warn [default: warn]
```