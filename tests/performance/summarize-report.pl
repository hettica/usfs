#!/usr/bin/perl
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Summarize backend-neutral raw CSV. Execute only on the benchmark AIX host.
use strict;
use warnings;
use File::Basename qw(basename);
use POSIX qw(uname);

use constant NANOSECONDS_PER_MICROSECOND => 1000;
use constant NANOSECONDS_PER_SECOND => 1_000_000_000;
use constant BYTES_PER_MIB => 1024 * 1024;

my ($system) = uname();
die "Failed to summarize overhead: requires AIX\n" unless $system eq 'AIX';
my $directory = shift @ARGV // die "Usage: summarize-report.pl campaign-output\n";

sub read_csv {
    my ($path) = @_;
    open my $input, '<', $path or die "Failed to read $path: $!\n";
    my $header = <$input>;
    chomp $header;
    my @columns = split /,/, $header;
    my @rows;
    while (my $line = <$input>) {
        chomp $line;
        my @values = split /,/, $line, -1;
        die "Failed to parse $path: unexpected CSV width\n" unless @columns == @values;
        my %row;
        @row{@columns} = @values;
        push @rows, \%row;
    }
    close $input or die "Failed to close $path: $!\n";
    return @rows;
}

sub mean {
    my @values = @_;
    return 0 unless @values;
    my $sum = 0;
    $sum += $_ for @values;
    return $sum / @values;
}

my %excluded_pairs;
if (-f "$directory/data-exclusions.csv") {
    for my $row (read_csv("$directory/data-exclusions.csv")) {
        $excluded_pairs{join ',', @{$row}{qw(configuration operation transfer_bytes clients files round)}} = 1;
    }
}

sub percentile {
    my ($fraction, @values) = @_;
    return 0 unless @values;
    @values = sort { $a <=> $b } @values;
    my $index = int($fraction * @values);
    $index = $#values if $index > $#values;
    return $values[$index];
}

sub confidence_half_width {
    my @values = @_;
    return 0 if @values < 2;
    my %critical_t = (2 => 12.706, 3 => 4.303, 4 => 3.182, 5 => 2.776, 6 => 2.571,
                      7 => 2.447, 8 => 2.365, 9 => 2.306, 10 => 2.262);
    my $average = mean(@values);
    my $squares = 0;
    $squares += ($_ - $average) ** 2 for @values;
    return ($critical_t{scalar @values} // 1.96) * sqrt($squares / (@values - 1) / @values);
}

sub microseconds_per_operation {
    my ($row) = @_;
    die "Failed to summarize sample: no operations\n" unless $row->{operations} > 0;
    return $row->{elapsed_ns} / $row->{operations} / NANOSECONDS_PER_MICROSECOND;
}

sub framework_cpu_per_operation {
    my ($row) = @_;
    my $cpu = $row->{client_cpu_ns};
    $cpu += $row->{daemon_cpu_ns} if $row->{path} eq 'usfs';
    $cpu -= $row->{callback_cpu_ns};
    return $cpu / $row->{operations} / NANOSECONDS_PER_MICROSECOND;
}

open my $report, '>', "$directory/summary.md" or die "Failed to create summary: $!\n";
open my $data_output, '>', "$directory/data-summary.csv" or die "Failed to create data summary: $!\n";
print $data_output "configuration,operation,transfer_bytes,clients,files,pairs,direct_mib_s,usfs_mib_s,incremental_elapsed_us,incremental_elapsed_ci95_half_us,incremental_cpu_us,incremental_cpu_ci95_half_us,usfs_residual_wall_us,callback_us_per_call,rpc_per_operation,usfs_elapsed_us,direct_elapsed_us,callback_calls_per_operation\n";
print $report "# Read/write calibration\n\nElapsed deltas for one client estimate incremental per-operation time. For multiple clients they are reciprocal-throughput deltas, not request latency. CPU excludes measured callback CPU only in instrumented runs. Confidence intervals use paired round deltas and Student t.\n\n";
print $report "| Configuration | Op | Bytes | Clients/files | MiB/s direct | MiB/s USFS | Added elapsed µs/op (95% half width) | Added CPU µs/op | Callback µs/call | RPC/op |\n|---|---|---:|---|---:|---:|---:|---:|---:|---:|\n";

for my $path (sort glob "$directory/data-*-data.csv") {
    my $configuration = basename($path);
    $configuration =~ s/-data\.csv$//;
    my %groups;
    for my $row (read_csv($path)) {
        my $key = join ',', @{$row}{qw(operation read_bytes clients files)};
        die "Failed to pair sample $key: duplicate round/path\n" if exists $groups{$key}{$row->{round}}{$row->{path}};
        $groups{$key}{$row->{round}}{$row->{path}} = $row;
    }
    for my $key (sort keys %groups) {
        my (@elapsed, @cpu, @direct_rates, @usfs_rates, @callback_time, @rpc_counts, @residual, @usfs_elapsed, @direct_elapsed, @callback_counts);
        for my $round (sort { $a <=> $b } keys %{$groups{$key}}) {
            next if $excluded_pairs{"$configuration,$key,$round"};
            my $pair = $groups{$key}{$round};
            die "Failed to pair $key round $round\n" unless $pair->{direct} && $pair->{usfs};
            my ($direct, $usfs) = @{$pair}{qw(direct usfs)};
            push @elapsed, microseconds_per_operation($usfs) - microseconds_per_operation($direct);
            push @usfs_elapsed, microseconds_per_operation($usfs);
            push @direct_elapsed, microseconds_per_operation($direct);
            push @callback_counts, $usfs->{callback_calls} / $usfs->{operations};
            push @cpu, framework_cpu_per_operation($usfs) - framework_cpu_per_operation($direct);
            push @direct_rates, $direct->{bytes} / $direct->{elapsed_ns} * NANOSECONDS_PER_SECOND / BYTES_PER_MIB;
            push @usfs_rates, $usfs->{bytes} / $usfs->{elapsed_ns} * NANOSECONDS_PER_SECOND / BYTES_PER_MIB;
            push @callback_time, $usfs->{callback_wall_ns} / $usfs->{callback_calls} / NANOSECONDS_PER_MICROSECOND if $usfs->{callback_calls};
            push @rpc_counts, $usfs->{rpc_requests} / $usfs->{operations};
            if ($usfs->{clients} == 1 && $usfs->{instrumented}) {
                push @residual, ($usfs->{elapsed_ns} - $usfs->{callback_wall_ns}) / $usfs->{operations} / NANOSECONDS_PER_MICROSECOND;
            }
        }
        my ($operation, $bytes, $clients, $files) = split /,/, $key;
        die "Failed to summarize $configuration/$key: fewer than two uncontaminated pairs\n" unless @elapsed >= 2;
        printf $data_output "%s,%s,%d,%d,%s,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
            $configuration, $operation, $bytes, $clients, $files, scalar(@elapsed), mean(@direct_rates), mean(@usfs_rates),
            mean(@elapsed), confidence_half_width(@elapsed), mean(@cpu), confidence_half_width(@cpu), mean(@residual), mean(@callback_time), mean(@rpc_counts), mean(@usfs_elapsed), mean(@direct_elapsed), mean(@callback_counts);
        printf $report "| %s | %s | %d | %d/%s | %.1f | %.1f | %.2f ± %.2f | %.2f | %.2f | %.2f |\n",
            $configuration, $operation, $bytes, $clients, $files, mean(@direct_rates), mean(@usfs_rates), mean(@elapsed), confidence_half_width(@elapsed), mean(@cpu), mean(@callback_time), mean(@rpc_counts);
    }
}
close $data_output or die "Failed to close data summary: $!\n";

print $report "\n# Native operation costs\n\nUninstrumented elapsed time is the primary latency. Instrumented residual = elapsed minus measured callback brackets; it still includes adapter/timer overhead outside the brackets. Lifecycle and identifiers are separate composite diagnostics.\n\n";
print $report "| Configuration | Operation | Samples | Median elapsed µs | P95 elapsed µs | Median callback µs | Median residual µs | Calls/op | RPC/op | errno | Negative residual samples |\n|---|---|---:|---:|---:|---:|---:|---:|---:|---|---:|\n";
open my $metadata_output, '>', "$directory/metadata-summary.csv" or die "Failed to create metadata summary: $!\n";
print $metadata_output "configuration,operation,samples,median_elapsed_us,p95_elapsed_us,median_callback_us,median_residual_us,calls_per_operation,rpc_per_operation,negative_residuals\n";
for my $path (sort glob "$directory/*-metadata.csv") {
    next if $path =~ /smoke/;
    my $configuration = basename($path);
    $configuration =~ s/-metadata\.csv$//;
    my %groups;
    push @{$groups{$_->{operation}}}, $_ for read_csv($path);
    for my $operation (sort keys %groups) {
        my @rows = @{$groups{$operation}};
        my @elapsed = map { $_->{elapsed_ns} / NANOSECONDS_PER_MICROSECOND } @rows;
        my @callback = map { $_->{callback_wall_ns} / NANOSECONDS_PER_MICROSECOND } @rows;
        my @residual = map { ($_->{elapsed_ns} - $_->{callback_wall_ns}) / NANOSECONDS_PER_MICROSECOND } @rows;
        my $calls = mean(map { $_->{callback_calls} } @rows);
        my $rpc = mean(map { $_->{rpc_requests} } @rows);
        my $negative = grep { $_ < 0 } @residual;
        my %errors = map { $_->{observed_errno} => 1 } @rows;
        printf $report "| %s | %s | %d | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %s | %d |\n",
            $configuration, $operation, scalar(@rows), percentile(.5, @elapsed), percentile(.95, @elapsed), percentile(.5, @callback), percentile(.5, @residual), $calls, $rpc, join('/', sort keys %errors), $negative;
        printf $metadata_output "%s,%s,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d\n",
            $configuration, $operation, scalar(@rows), percentile(.5, @elapsed), percentile(.95, @elapsed), percentile(.5, @callback), percentile(.5, @residual), $calls, $rpc, $negative;
    }
}
close $metadata_output or die "Failed to close metadata summary: $!\n";

my %coverage;
my %callback_errors;
my %lifecycle;
for my $path (sort glob "$directory/*-callbacks.csv") {
    next if $path =~ /smoke/;
    for my $row (read_csv($path)) {
        $coverage{$row->{callback}} += $row->{calls};
        $callback_errors{$row->{callback}} += $row->{errors};
        die "Failed calibration: read/write callback error\n" if $row->{callback} =~ /^(read|write)$/ && $row->{errors};
    }
}
for my $path (sort glob "$directory/*.log") {
    my $configuration = basename($path);
    $configuration =~ s/\.log$//;
    open my $input, '<', $path or die "Failed to read lifecycle log $path: $!\n";
    while (my $line = <$input>) {
        if ($line =~ /^lifecycle,(mount|unmount),(\d+),/) {
            push @{$lifecycle{"$configuration,$1"}}, $2 / NANOSECONDS_PER_MICROSECOND;
        }
        if ($line =~ /^lifecycle-callback,[^,]+,([^,]+),(\d+),/) {
            $coverage{$1} += $2;
        }
    }
    close $input;
}
print $report "\n# Lifecycle composites\n\nEach configuration has one mount/unmount observation, not a repeated latency distribution.\n\n| Configuration | Phase | Observations | Median µs |\n|---|---|---:|---:|\n";
for my $key (sort keys %lifecycle) {
    my ($configuration, $phase) = split /,/, $key;
    my @samples = @{$lifecycle{$key}};
    printf $report "| %s | %s | %d | %.2f |\n", $configuration, $phase, scalar(@samples), percentile(.5, @samples);
}
my @callbacks = qw(initialize getattr statfs readlink chmod chown truncate utimens mkdir rmdir unlink rename link symlink create create_attr open read write flush fsync release syncfs opendir readdir fsyncdir releasedir export_id resolve_id shutdown);
print $report "\n# Public callback coverage\n\n| Callback | Instrumented calls | Negative results |\n|---|---:|---:|\n";
my @missing;
for my $callback (@callbacks) {
    my $calls = $coverage{$callback} // 0;
    push @missing, $callback unless $calls;
    printf $report "| %s | %d | %d |\n", $callback, $calls, $callback_errors{$callback} // 0;
}
print $report "\nMissing callbacks: ", (@missing ? join(', ', @missing) : 'none'), ".\n";
close $report or die "Failed to close summary: $!\n";
print "Created $directory/summary.md\n";
die "Failed callback inventory: missing @missing\n" if @missing;
