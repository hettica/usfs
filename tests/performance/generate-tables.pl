#!/usr/bin/perl
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Create concise report tables from AIX-computed overhead summaries.
# Usage: perl generate-tables.pl campaign-output
use strict;
use warnings;
use POSIX qw(uname);
my ($system) = uname();
die "Failed to write report tables: requires AIX\n" unless $system eq 'AIX';
my $directory = shift @ARGV // die "Usage: generate-tables.pl campaign-output\n";

sub load_summary {
    my ($path, @keys) = @_;
    open my $input, '<', $path or die "Failed to read $path: $!\n";
    my $header = <$input>;
    chomp $header;
    my @columns = split /,/, $header;
    my %summary;
    while (my $line = <$input>) {
        chomp $line;
        my %row;
        @row{@columns} = split /,/, $line;
        my $key = join ',', @row{@keys};
        die "Failed to load $path: duplicate $key\n" if exists $summary{$key};
        $summary{$key} = \%row;
    }
    close $input;
    return %summary;
}

my %data = load_summary("$directory/data-summary.csv", qw(configuration operation transfer_bytes clients files));
my %metadata = load_summary("$directory/metadata-summary.csv", qw(configuration operation));
my %callback_mix;
open my $callbacks, '<', "$directory/metadata-instrumented-callbacks.csv" or die "Failed to read callback mix: $!\n";
<$callbacks>;
while (my $line = <$callbacks>) {
    chomp $line;
    my ($operation, $round, $callback, $calls) = split /,/, $line;
    $callback_mix{$operation}{$callback} += $calls;
}
close $callbacks or die "Failed to close callback mix: $!\n";

sub describe_callback_mix {
    my ($operation, $samples) = @_;
    my @descriptions;
    for my $callback (sort keys %{$callback_mix{$operation} // {}}) {
        push @descriptions, sprintf '%s×%.2g', $callback, $callback_mix{$operation}{$callback} / $samples;
    }
    return @descriptions ? join(', ', @descriptions) : 'none';
}

open my $output, '>', "$directory/report-tables.md" or die "Failed to create report tables: $!\n";
print $output "## Primary read/write costs\n\nOne caller, separate file, eight daemon workers, zero injected delay. Five uninstrumented pairs were planned; background-contaminated pairs are excluded and retained counts are shown. ± is the nominal 95% Student-t half width. CPU uses instrumented callback subtraction and is AIX-accounted process CPU.\n\n";
print $output "| Operation | Application bytes | Pairs | USFS MiB/s | Added elapsed µs/op | Added CPU µs/op | RPC/op |\n|---|---:|---:|---:|---:|---:|---:|\n";
for my $operation (qw(read write)) {
    for my $bytes (4096, 65536, 65544, 1048576) {
        my $row = $data{"data-uninstrumented,$operation,$bytes,1,separate"} // die "Missing primary sample\n";
        my $instrumented = $data{"data-instrumented,$operation,$bytes,1,separate"} // die "Missing instrumented sample\n";
        printf $output "| %s | %d | %d | %.1f | %.2f ± %.2f | %.2f | %.2f |\n", $operation, $bytes, $row->{pairs},
            @{$row}{qw(usfs_mib_s incremental_elapsed_us incremental_elapsed_ci95_half_us)}, $instrumented->{incremental_cpu_us}, $row->{rpc_per_operation};
    }
}

print $output "\n## Observer effects\n\nOne caller; the uninstrumented column is primary. Instrumented residual subtracts callback brackets, including callback timer costs, and is not an exact correction for perturbation.\n\n";
print $output "| Operation | Bytes | Uninstrumented USFS µs/op | Instrumented USFS µs/op | Instrumented residual µs/op | Direct uninstrumented µs/op | Uninstrumented CPU delta µs/op | Instrumented CPU tax µs/op |\n|---|---:|---:|---:|---:|---:|---:|---:|\n";
for my $operation (qw(read write)) {
    for my $bytes (4096, 65536, 65544, 1048576) {
        my $row = $data{"data-uninstrumented,$operation,$bytes,1,separate"};
        my $instrumented = $data{"data-instrumented,$operation,$bytes,1,separate"};
        printf $output "| %s | %d | %.2f | %.2f | %.2f | %.3f | %.2f | %.2f |\n", $operation, $bytes, $row->{usfs_elapsed_us},
            @{$instrumented}{qw(usfs_elapsed_us usfs_residual_wall_us)}, @{$row}{qw(direct_elapsed_us incremental_cpu_us)}, $instrumented->{incremental_cpu_us};
    }
}

print $output "\n## Backend service sensitivity\n\nOne serialized caller, eight workers. Callback cost is the actual measured mean bracket, not the requested sleep. Residual excludes those brackets; it retains dispatch, scheduling, copying and adapter costs outside them.\n\n";
print $output "| Op | Bytes | Requested delay µs | Measured callback µs/call | Calls/op | USFS elapsed µs/op | Residual µs/op |\n|---|---:|---:|---:|---:|---:|---:|\n";
for my $operation (qw(read write)) {
    for my $bytes (4096, 1048576) {
        for my $configuration ('data-instrumented', 'data-delay100', 'data-delay1000') {
            my $row = $data{"$configuration,$operation,$bytes,1,separate"} // die "Missing delay sample\n";
            my $delay = $configuration eq 'data-instrumented' ? 0 : $configuration eq 'data-delay100' ? 100 : 1000;
            printf $output "| %s | %d | %d | %.2f | %.2f | %.2f | %.2f |\n", $operation, $bytes, $delay,
                @{$row}{qw(callback_us_per_call callback_calls_per_operation usfs_elapsed_us usfs_residual_wall_us)};
        }
    }
}

print $output "\n## Concurrency and dispatch\n\nFour callers. Values are aggregate throughput, not per-request latency. Zero injected delay, no callback probes. Writes use separate inodes.\n\n";
print $output "| Op | Bytes | Files | Pool 8 MiB/s | Pool 1 MiB/s |\n|---|---:|---|---:|---:|\n";
for my $operation (qw(read write)) {
    for my $bytes (4096, 1048576) {
        for my $files ($operation eq 'read' ? ('separate', 'shared') : ('separate')) {
            my $many = $data{"data-uninstrumented,$operation,$bytes,4,$files"};
            my $one = $data{"data-pool1,$operation,$bytes,4,$files"} // die "Missing pool sample\n";
            printf $output "| %s | %d | %s | %.1f | %.1f |\n", $operation, $bytes, $files, $many->{usfs_mib_s}, $one->{usfs_mib_s};
        }
    }
}

print $output "\n### Blocking-backend worker control\n\nFour callers, separate inodes, instrumented callbacks with a requested 1,000 µs delay. These runs test whether the fast-backend worker result transfers to blocking backends.\n\n";
print $output "| Op | Bytes | Pool 8/1 pairs | Pool 8 MiB/s | Pool 1 MiB/s |\n|---|---:|---|---:|---:|\n";
for my $operation (qw(read write)) {
    for my $bytes (4096, 1048576) {
        my $many = $data{"data-delay1000,$operation,$bytes,4,separate"} // die "Missing blocking pool sample\n";
        my $one = $data{"data-pool1-delay1000,$operation,$bytes,4,separate"} // die "Missing blocking single-worker sample\n";
        printf $output "| %s | %d | %d/%d | %.1f | %.1f |\n", $operation, $bytes, $many->{pairs}, $one->{pairs}, $many->{usfs_mib_s}, $one->{usfs_mib_s};
    }
}

print $output "\n## Ordinary-user data I/O\n\nThree pairs were planned per configuration, using the same executable; contaminated pairs are excluded. Root and UID/GID 10000 use owned 0600 files. Additional RPCs on ordinary-user writes expose framework-induced backend attribute work.\n\n";
print $output "| Op | Bytes | Root/user pairs | Root µs/op | User µs/op | User added µs/op | User CPU tax µs/op | Root RPC/op | User RPC/op |\n|---|---:|---|---:|---:|---:|---:|---:|---:|\n";
for my $operation (qw(read write)) {
    for my $bytes (4096, 65536, 65544, 1048576) {
        my $root = $data{"data-root-reference,$operation,$bytes,1,separate"} // die "Missing root identity sample\n";
        my $user = $data{"data-user-uninstrumented,$operation,$bytes,1,separate"} // die "Missing user identity sample\n";
        my $instrumented = $data{"data-user-instrumented,$operation,$bytes,1,separate"} // die "Missing instrumented identity sample\n";
        printf $output "| %s | %d | %d/%d | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f |\n", $operation, $bytes, $root->{pairs}, $user->{pairs},
            $root->{usfs_elapsed_us}, @{$user}{qw(usfs_elapsed_us incremental_elapsed_us)}, $instrumented->{incremental_cpu_us},
            $root->{rpc_per_operation}, $user->{rpc_per_operation};
    }
}

print $output "\n## Native operation latency inventory\n\nFive rounds × 200 samples. Primary median/p95 are uninstrumented. The instrumented residual is elapsed minus measured callback brackets; it includes probe work outside the brackets and AIX/application work. Callback and RPC fan-out use instrumented samples. Rejection rows are explicitly errors, not successful operations.\n\n";
print $output "| Native operation | Median µs | P95 µs | Instrumented residual median µs | Callbacks/op | RPC/op | Callback mix/op |\n|---|---:|---:|---:|---:|---:|---|\n";
for my $key (sort keys %metadata) {
    next unless $key =~ /^metadata-uninstrumented,/;
    my $row = $metadata{$key};
    my $operation = $row->{operation};
    my $instrumented = $metadata{"metadata-instrumented,$operation"} // die "Missing instrumented metadata\n";
    printf $output "| %s | %.2f | %.2f | %.2f | %.2f | %.2f | %s |\n", $operation,
        @{$row}{qw(median_elapsed_us p95_elapsed_us)}, @{$instrumented}{qw(median_residual_us calls_per_operation rpc_per_operation)},
        describe_callback_mix($operation, $instrumented->{samples});
}
print $output "\n## Fallback and test-only diagnostics\n\nFive rounds × 200 instrumented samples. Native vnode diagnostics use the testing kernel; their composite elapsed times are separate from the production floor.\n\n";
print $output "| Configuration | Operation | Median µs | P95 µs | Residual median µs | Calls/op | RPC/op |\n|---|---|---:|---:|---:|---:|---:|\n";
for my $key (sort keys %metadata) {
    next unless $key =~ /^(create-fallback|identifiers),/;
    my $row = $metadata{$key};
    printf $output "| %s | %s | %.2f | %.2f | %.2f | %.2f | %.2f |\n",
        @{$row}{qw(configuration operation median_elapsed_us p95_elapsed_us median_residual_us calls_per_operation rpc_per_operation)};
}
close $output or die "Failed to close report tables: $!\n";
print "Created $directory/report-tables.md\n";
