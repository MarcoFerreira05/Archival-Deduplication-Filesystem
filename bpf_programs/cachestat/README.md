# Cachestat Standalone

A minimal, standalone build of the `cachestat` BPF tool from [iovisor/bcc](https://github.com/iovisor/bcc).

`cachestat` counts cache kernel function calls and reports hits and misses to the file system page cache.

## Prerequisites

- **x86_64 Linux** with kernel 5.7+ and BTF enabled (`CONFIG_DEBUG_INFO_BTF=y`)
- System-installed BPF toolchain:

```bash
# Debian/Ubuntu
sudo apt-get install -y clang llvm llvm-strip bpftool libbpf-dev libelf-dev zlib1g-dev

# Fedora/RHEL
sudo dnf install -y clang llvm bpftool libbpf-devel elfutils-libelf-devel zlib-devel
```

## Building

```bash
make
```

## Usage

```bash
sudo ./cachestat          # 1 second intervals
sudo ./cachestat -T       # with timestamps
sudo ./cachestat 1 10     # 10 samples, 1 second apart
sudo ./cachestat --output stats.json 1 10   # write final global JSON stats on exit
```

## License

- Userspace code: LGPL-2.1 OR BSD-2-Clause
- BPF code: GPL-2.0
