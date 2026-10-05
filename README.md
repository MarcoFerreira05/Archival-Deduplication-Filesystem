# Archival Deduplication Filesystem

## Grade: 19.3/20

## Group Participants:
- Marco Rocha Ferreira - A106857 - MarcoFerreira05
- Nuno Henrique Macedo Rebelo - A107373 - NunoRebelo05
- Diogo José Ribeiro e Ribeiro - A106906 - DIOGO4810

## Description

> Experimental block-level deduplication filesystem built on top of FUSE, developed as part of the **Operating Systems Technologies** course at the University of Minho.

Rather than focusing solely on implementing deduplication, this project explores the engineering process behind building an efficient storage system. The filesystem was developed through several iterations, where each optimization was motivated by profiling, experimentally validated, and only kept if it produced measurable improvements.

The project combines systems programming, Linux performance analysis, custom eBPF instrumentation, and reproducible benchmarking to study the trade-offs involved in block-level deduplication.


## Overview

The filesystem implements **block-level deduplication** on top of a FUSE passthrough filesystem.

Files are divided into fixed-size blocks (4 KiB), each block is identified through a SHA-512 hash, and identical blocks are stored only once inside a shared master file. Logical files are therefore represented as references to shared physical blocks, reducing storage usage whenever duplicated data exists.


## Engineering Process

Instead of attempting to build the "best" implementation immediately, we followed an iterative workflow:

```text
Baseline implementation
          │
          ▼
Performance profiling using reproducible workloads
(perf + custom eBPF tools)
          │
          ▼
Identify bottlenecks
          │
          ▼
Redesign critical paths
          │
          ▼
Validate improvements
          │
          └──────────────► Repeat
```

Every major optimization included in the final implementation was introduced only after identifying a measurable bottleneck in the previous version.


# Evolution of the Filesystem

The implementation evolved through multiple versions.

## Initial implementation

The first version focused entirely on correctness.

It successfully implemented block-level deduplication, but every logical block generated its own filesystem operations, resulting in a large number of system calls and significant synchronization overhead.

Rather than optimizing prematurely, this version served as the baseline for subsequent profiling.


## Identifying bottlenecks

Performance analysis revealed several important observations.

Using `perf`, we observed that a considerable fraction of execution time was spent issuing individual `pwrite()` operations, while hashing also represented a significant cost in the write path.

Additional instrumentation showed that our filesystem generated substantially more read and write syscalls than the benchmark program was making.

These measurements motivated a redesign of both the read and write paths.


## Batching system calls

The second iteration introduced batching of contiguous operations.

Instead of issuing one syscall per block, consecutive blocks are grouped whenever possible into larger read/write requests.

This considerably reduced syscall overhead while preserving the filesystem semantics.

Experimental evaluation showed, under workloads with 15% duplicated blocks:

- ~40% fewer `pwrite()` syscalls;
- ~65% fewer `pread()` syscalls.

As for 75% duplicated blocks:
- ~10% fewer `pwrite()` syscalls;
- ~15% fewer `pread()` syscalls.

These numbers were completely expected due to deduplication being more aggressive in the latter results.

## Concurrency redesign

As the project evolved, scalability under concurrent workloads became the next bottleneck.

The original implementation relied on coarse-grained synchronization, limiting throughput as the number of concurrent clients increased.

The final version introduced a more granular synchronization strategy together with a redesigned write pipeline that minimizes time spent inside critical sections.

Benchmarking with FIO demonstrated that this redesign improved scalability under concurrent workloads, increasing peak throughput by approximately **16%** compared to the previous version.


# Experimental Evaluation

A large part of the project focused on building an experimental framework capable of evaluating each optimization objectively.

Rather than relying only on end-to-end execution time, the filesystem was analyzed using several complementary metrics:

- storage efficiency
- syscall behavior
- page cache utilization
- syscall latency
- throughput under concurrent workloads

All benchmarks were executed multiple times and statistically aggregated to ensure reproducibility.


## Custom eBPF Instrumentation

One of the main contributions of this project was the development of a benchmarking and instrumentation framework based on **eBPF**.

Two custom tracing tools were developed specifically for this project.

### Syscall instrumentation

A custom eBPF program was developed to:

- count filesystem-related syscalls
- measure syscall latency
- filter events by process
- export measurements in CSV format for automated analysis

This allowed each filesystem iteration to be compared not only in terms of execution time but also in terms of how efficiently it interacted with the kernel.


### Page cache analysis

A second eBPF program (adapted from the BCC `cachestat` tool) was extended with:

- PID filtering
- JSON output
- integration into the benchmarking pipeline

This made it possible to quantify page cache hits, dirty pages and overall cache behavior across different workloads.


## Synthetic workload generator

A custom Python workload generator was developed to complement FIO.

Unlike traditional benchmarking tools, it allowed precise control over:

- read/write/unlink ratios
- duplication percentage
- request sizes
- number of files
- duplicated block pools

This provided reproducible workloads specifically tailored for evaluating a deduplicating filesystem.


## Automated benchmarking pipeline

All instrumentation was integrated into an automated benchmarking pipeline.

For each experiment, the pipeline:

1. Starts the FUSE filesystem.
2. Launches the eBPF instrumentation.
3. Executes the benchmark workload.
4. Collects raw measurements.
5. Aggregates results over multiple executions.
6. Computes averages and standard deviations automatically.

This made it straightforward to compare successive filesystem versions under identical conditions.


# Future Work

The experimental evaluation also highlighted several opportunities for future improvements.

Potential directions include:

- overlapping hashing and I/O using asynchronous execution (`io_uring` or a pool of hasher threads),
- sharding metadata structures to further reduce write contention,
- improving free-space management for highly dynamic workloads,
- migrating to the FUSE low-level API for finer control over request scheduling.


# Complete Report

The accompanying report documents the filesystem architecture, experimental methodology and performance analysis in significantly greater detail. Besides, it also includes figures and graphics which help understand the implementation and the experimental evaluation results.
