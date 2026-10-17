#!/usr/bin/perl
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Convert the merged AIX LCOV report to Sonar generic coverage XML.
# Usage: perl lcov-to-sonar.pl SOURCE_ROOT REPORT.info > coverage.xml

use strict;
use warnings;

sub source_relative_path
{
    my ($source_root, $source_path) = @_;
    my $prefix = "$source_root/";

    if (index($source_path, $prefix) != 0) {
        die "Failed to map coverage source $source_path: outside $source_root\n";
    }

    my $relative_path = substr($source_path, length($prefix));
    if ($relative_path =~ m{(?:^|/)\.\.(?:/|$)}) {
        die "Failed to map coverage source $source_path: parent traversal\n";
    }

    if (!-f "$source_root/$relative_path") {
        die "Failed to map coverage source $source_path: source file missing\n";
    }

    return $relative_path;
}

sub read_coverage
{
    my ($source_root, $report_path) = @_;
    open my $report, '<', $report_path
        or die "Failed to open coverage report $report_path: $!\n";

    my %files;
    my $source_path;
    while (my $record = <$report>) {
        $record =~ s/\r?\n\z//;
        if ($record =~ /^SF:(.+)$/) {
            $source_path = source_relative_path($source_root, $1);
            next;
        }

        if ($record eq 'end_of_record') {
            undef $source_path;
            next;
        }

        next unless $record =~ /^(?:DA|BRDA):/;
        if (!defined $source_path) {
            die "Failed to parse coverage report: counters have no source file\n";
        }

        # Only production source roots belong in a Sonar coverage report.
        # Keep source-path validation above, even for excluded test records.
        next unless $source_path =~ m{^(?:src|examples)/};

        if ($record =~ /^DA:([1-9][0-9]*),([0-9]+)(?:,[^,]+)?$/) {
            my ($line_number, $hits) = ($1, $2);
            $files{$source_path}{$line_number}{covered} ||= ($hits > 0 ? 1 : 0);
            next;
        }

        if ($record =~ /^BRDA:([1-9][0-9]*),([0-9]+),([0-9]+),(-|[0-9]+)$/) {
            my ($line_number, $block, $branch, $hits) = ($1, $2, $3, $4);
            my $covered = $hits ne '-' && $hits > 0;
            $files{$source_path}{$line_number}{branches}{"$block,$branch"}
                ||= ($covered ? 1 : 0);
            next;
        }

        die "Failed to parse coverage report: malformed counter $record\n";
    }

    close $report or die "Failed to close coverage report $report_path: $!\n";
    if (!%files) {
        die "Failed to read coverage report $report_path: no source counters\n";
    }

    return \%files;
}

sub xml_escape
{
    my ($value) = @_;
    $value =~ s/&/&amp;/g;
    $value =~ s/</&lt;/g;
    $value =~ s/>/&gt;/g;
    $value =~ s/"/&quot;/g;
    $value =~ s/'/&apos;/g;
    return $value;
}

sub write_line
{
    my ($line_number, $line) = @_;
    if (!exists $line->{covered}) {
        die "Failed to render coverage line $line_number: missing line counter\n";
    }

    my $covered = $line->{covered} ? 'true' : 'false';
    print qq{    <lineToCover lineNumber="$line_number" covered="$covered"};

    if (exists $line->{branches}) {
        my @branches = values %{$line->{branches}};
        my $total_branches = scalar @branches;
        my $covered_branches = grep { $_ } @branches;
        print qq{ branchesToCover="$total_branches" coveredBranches="$covered_branches"};
    }

    print "/>\n";
}

sub write_coverage
{
    my ($files) = @_;
    print qq{<?xml version="1.0" encoding="UTF-8"?>\n<coverage version="1">\n};
    for my $source_path (sort keys %$files) {
        my $escaped_path = xml_escape($source_path);
        print qq{  <file path="$escaped_path">\n};
        my $lines = $files->{$source_path};
        for my $line_number (sort { $a <=> $b } keys %$lines) {
            write_line($line_number, $lines->{$line_number});
        }
        print "  </file>\n";
    }
    print "</coverage>\n";
}

if (@ARGV != 2) {
    die "Usage: perl lcov-to-sonar.pl SOURCE_ROOT REPORT.info\n";
}

my ($source_root, $report_path) = @ARGV;
$source_root =~ s{/+$}{};
my $files = read_coverage($source_root, $report_path);
write_coverage($files);
