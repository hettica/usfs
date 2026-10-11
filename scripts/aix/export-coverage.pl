#!/usr/bin/perl
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT
# Purpose: Export merged LCOV with repository-relative paths and SonarQube XML.
# Usage: perl export-coverage.pl SOURCE INPUT-LCOV OUTPUT-LCOV OUTPUT-XML

use strict;
use warnings;

my ($source_directory, $input_path, $lcov_path, $xml_path) = @ARGV;
die "Usage: export-coverage.pl SOURCE INPUT-LCOV OUTPUT-LCOV OUTPUT-XML\n" unless @ARGV == 4;
$source_directory =~ s{/$}{};
open my $input, '<', $input_path or die "Failed to read coverage $input_path: $!\n";
open my $lcov, '>', $lcov_path or die "Failed to create LCOV $lcov_path: $!\n";
my %files;
my $current_path;

while (my $record = <$input>) {
    if ($record =~ /^SF:(.*)\r?\n$/) {
        my $path = $1;
        $path =~ s{^\Q$source_directory\E/}{} or
            die "Failed to export coverage path $path: outside source directory\n";
        die "Failed to export coverage path $path: unsafe relative path\n"
            if $path =~ m{^/|(?:^|/)\.\.(?:/|$)};
        die "Failed to export coverage path $path: missing source\n" unless -f "$source_directory/$path";
        $current_path = $path;
        $files{$path} //= { lines => {}, branches => {} };
        print $lcov "SF:$path\n";
        next;
    }
    print $lcov $record;
    if ($record =~ /^DA:(\d+),(\d+)/) {
        die "Failed to export line coverage: no source record\n" unless defined $current_path;
        $files{$current_path}{lines}{$1} += $2;
    }
    if ($record =~ /^BRDA:(\d+),([^,]+),([^,]+),([^\r\n]+)/) {
        die "Failed to export branch coverage: no source record\n" unless defined $current_path;
        my ($line, $block, $branch, $hits) = ($1, $2, $3, $4);
        $files{$current_path}{lines}{$line} //= 0;
        $files{$current_path}{branches}{$line}{"$block,$branch"} += $hits eq '-' ? 0 : $hits;
    }
    undef $current_path if $record =~ /^end_of_record/;
}
close $input or die "Failed to close coverage $input_path: $!\n";
close $lcov or die "Failed to close LCOV $lcov_path: $!\n";
die "Failed to export coverage: no source records\n" unless keys %files;

sub escape_xml {
    my ($text) = @_;
    $text =~ s/&/&amp;/g;
    $text =~ s/</&lt;/g;
    $text =~ s/>/&gt;/g;
    $text =~ s/"/&quot;/g;
    $text =~ s/'/&apos;/g;
    return $text;
}

open my $xml, '>', $xml_path or die "Failed to create XML $xml_path: $!\n";
print $xml qq{<?xml version="1.0" encoding="UTF-8"?>\n<coverage version="1">\n};
for my $path (sort keys %files) {
    print $xml '  <file path="', escape_xml($path), qq{">\n};
    my $file = $files{$path};
    for my $line (sort { $a <=> $b } keys %{$file->{lines}}) {
        my $covered = $file->{lines}{$line} > 0 ? 'true' : 'false';
        print $xml qq{    <lineToCover lineNumber="$line" covered="$covered"};
        my $branches = $file->{branches}{$line};
        if ($branches) {
            my $total = scalar keys %$branches;
            my $covered_branches = grep { $_ > 0 } values %$branches;
            print $xml qq{ branchesToCover="$total" coveredBranches="$covered_branches"};
        }
        print $xml "/>\n";
    }
    print $xml "  </file>\n";
}
print $xml "</coverage>\n";
close $xml or die "Failed to close XML $xml_path: $!\n";
