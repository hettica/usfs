#!/usr/bin/perl
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Verify raw overhead measurements before interpreting their summaries.
# Usage: perl audit-measurements.pl campaign-output
use strict;
use warnings;
use File::Basename qw(basename);
use POSIX qw(uname);

use constant CALLBACK_TRANSFER_BYTES => 64 * 1024;
use constant MINIMUM_SAMPLE_NANOSECONDS => 1_000_000_000;

my ($system) = uname();
die "Failed to audit overhead: requires AIX\n" unless $system eq 'AIX';
my $directory = shift @ARGV // die "Usage: audit-measurements.pl campaign-output\n";
open my $report, '>', "$directory/audit.md" or die "Failed to create audit: $!\n";
print $report "# Raw measurement audit\n\n";
my ($samples, $instrumented_samples, $operations, $rpc_requests) = (0, 0, 0, 0);
my @failures;
my %sync_calls;
for my $path (sort glob "$directory/data-*-callbacks.csv") {
    my $configuration = basename($path);
    $configuration =~ s/-callbacks\.csv$//;
    open my $input, '<', $path or die "Failed to read $path: $!\n";
    <$input>;
    while (my $line = <$input>) {
        my ($operation, $round, $callback, $calls) = split /,/, $line;
        next unless $callback eq 'syncfs';
        $sync_calls{"$configuration,$operation,$round"} += $calls;
    }
    close $input;
}
open my $exclusions, '>', "$directory/data-exclusions.csv" or die "Failed to create exclusions: $!\n";
print $exclusions "configuration,operation,transfer_bytes,clients,files,round,reason\n";
my $excluded_pairs = 0;

for my $path (sort glob "$directory/data-*-data.csv") {
    my $configuration = basename($path);
    $configuration =~ s/-data\.csv$//;
    open my $input, '<', $path or die "Failed to read $path: $!\n";
    my $header = <$input>;
    chomp $header;
    my @columns = split /,/, $header;
    while (my $line = <$input>) {
        chomp $line;
        my @values = split /,/, $line, -1;
        die "Failed to audit $path: invalid CSV width\n" unless @values == @columns;
        my %row;
        @row{@columns} = @values;
        my $sample = join '/', $configuration, @row{qw(operation path read_bytes clients files round)};
        $samples++;
        $operations += $row{operations};
        $rpc_requests += $row{rpc_requests};
        push @failures, "$sample: no completed operations" unless $row{operations} > 0;
        push @failures, "$sample: byte count mismatch" unless $row{bytes} == $row{operations} * $row{read_bytes};
        push @failures, "$sample: sample shorter than one second" unless $row{elapsed_ns} >= MINIMUM_SAMPLE_NANOSECONDS;

        my $data_calls = int(($row{read_bytes} + CALLBACK_TRANSFER_BYTES - 1) / CALLBACK_TRANSFER_BYTES) * $row{operations};
        my $expected_calls = $data_calls;
        $expected_calls += $row{operations} if $configuration =~ /^data-user-/ && $row{path} eq 'usfs' && $row{operation} eq 'write';
        my $expected_requests = $row{path} eq 'usfs' ? $expected_calls : 0;
        my $extra_requests = $row{rpc_requests} - $expected_requests;
        my $callback_sample = join '-', @row{qw(operation path read_bytes clients files)};
        my $background_syncs = $sync_calls{"$configuration,$callback_sample,$row{round}"} // 0;
        my $exclude_pair = $extra_requests == 1 && (!$row{instrumented} || $background_syncs == 1);
        if ($exclude_pair) {
            my $reason = $row{instrumented} ? 'observed-background-syncfs' : 'extra-global-RPC-consistent-with-periodic-sync';
            print $exclusions join(',', $configuration, @row{qw(operation read_bytes clients files round)}, $reason), "\n";
            $excluded_pairs++;
        }
        push @failures, "$sample: RPC count $row{rpc_requests}, expected $expected_requests" unless $extra_requests == 0 || $exclude_pair;
        if (!$row{instrumented}) {
            push @failures, "$sample: unexpected uninstrumented callback counters" if $row{callback_calls} || $row{callback_wall_ns} || $row{callback_cpu_ns};
            next;
        }
        $instrumented_samples++;
        $expected_calls += $background_syncs;
        push @failures, "$sample: callback count $row{callback_calls}, expected $expected_calls" unless $row{callback_calls} == $expected_calls;
        my $process_cpu = $row{client_cpu_ns};
        $process_cpu += $row{daemon_cpu_ns} if $row{path} eq 'usfs';
        push @failures, "$sample: callback CPU exceeds process CPU" if $row{callback_cpu_ns} > $process_cpu;
        push @failures, "$sample: callback wall exceeds serialized elapsed" if !$exclude_pair && $row{clients} == 1 && $row{callback_wall_ns} > $row{elapsed_ns};
    }
    close $input or die "Failed to close $path: $!\n";
}

print $report "Audited $samples sustained samples ($instrumented_samples instrumented), $operations application operations, and $rpc_requests mounted RPCs.\n\n";
print $report "$excluded_pairs paired shapes/rounds are excluded from numerical read/write summaries because one member contains an extra background request. Instrumented exclusions identify an actual syncfs callback. Uninstrumented exclusions are classified conservatively from the extra global RPC; they are consistent with the observed syncd 60 process but cannot identify its opcode. The entire direct/USFS pair is excluded; raw measurements remain unchanged. See data-exclusions.csv.\n\n";
print $report "Checks: positive completions; exact byte totals; minimum one-second duration; exact 64 KiB request splitting; one extra ordinary-user write attribute request; zero direct-path RPCs; absent uninstrumented counters; callback counts; bounded callback CPU; nonnegative serialized wall residual.\n\n";
if (@failures) {
    print $report "Failures:\n\n", map { "- $_\n" } @failures;
    close $report;
    die "Failed raw measurement audit: " . scalar(@failures) . " discrepancies; see $directory/audit.md\n";
}
print $report "All retained-pair checks passed. Observed background requests are disclosed and excluded; no unexplained instrumented request-count discrepancies remain.\n";
close $exclusions or die "Failed to close exclusions: $!\n";
close $report or die "Failed to close audit: $!\n";
print "Created $directory/audit.md\n";
