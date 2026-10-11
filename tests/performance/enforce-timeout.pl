#!/usr/bin/perl
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Purpose: Bound one benchmark-owned process without requiring GNU timeout on AIX.
# Usage: perl enforce-timeout.pl seconds command arguments...
use strict;
use warnings;
use POSIX qw(WNOHANG);

use constant TIMEOUT_STATUS => 124;
use constant SIGNAL_STATUS_BASE => 128;
use constant TERMINATION_GRACE_SECONDS => 5;

my $deadline_seconds = shift @ARGV;
die "Failed to select deadline: expected positive seconds and a command\n"
    unless defined $deadline_seconds && $deadline_seconds =~ /^[1-9][0-9]*$/ && @ARGV;

my $child_pid = fork();
die "Failed to fork benchmark process: $!\n" unless defined $child_pid;
if ($child_pid == 0) {
    exec @ARGV;
    die "Failed to execute $ARGV[0]: $!\n";
}

my $timed_out = 0;
$SIG{ALRM} = sub {
    $timed_out = 1;
    print STDERR "Failed to complete $ARGV[0]: deadline of $deadline_seconds seconds expired\n";
    kill 'TERM', $child_pid;
};
alarm $deadline_seconds;
while (waitpid($child_pid, WNOHANG) == 0) {
    if ($timed_out) {
        sleep TERMINATION_GRACE_SECONDS;
        kill 'KILL', $child_pid;
        waitpid($child_pid, 0);
        exit TIMEOUT_STATUS;
    }
    select undef, undef, undef, 0.1;
}
my $child_status = $?;
alarm 0;
exit TIMEOUT_STATUS if $timed_out;
exit SIGNAL_STATUS_BASE + ($child_status & 127) if $child_status & 127;
exit ($child_status >> 8);
