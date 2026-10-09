#!/usr/bin/perl
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

# Validate an RPM without invoking AIX's unusually expensive rpm query path.

use strict;
use warnings;

my ($path, $version, $release, $aix_version, @required) = @ARGV;
defined $aix_version or die "usage: verify-rpm.pl rpm version release aix_version [payload ...]\n";

open my $fh, '<', $path or die "cannot open $path: $!\n";
binmode $fh;
local $/;
my $rpm = <$fh>;
close $fh or die "cannot close $path: $!\n";

length($rpm) >= 112 or die "truncated RPM: $path\n";
substr($rpm, 0, 4) eq pack('C4', 0xed, 0xab, 0xee, 0xdb)
    or die "invalid RPM lead: $path\n";

sub header {
    my ($blob, $start) = @_;
    substr($blob, $start, 4) eq pack('C4', 0x8e, 0xad, 0xe8, 0x01)
        or die "invalid RPM header at offset $start\n";
    my ($entries, $bytes) = unpack('NN', substr($blob, $start + 8, 8));
    my $index = $start + 16;
    my $store = $index + $entries * 16;
    my $end = $store + $bytes;
    $end <= length($blob) or die "truncated RPM header at offset $start\n";
    my %tags;
    for my $number (0 .. $entries - 1) {
        my ($tag, $type, $offset, $count) =
            unpack('NNNN', substr($blob, $index + $number * 16, 16));
        $offset <= $bytes or die "invalid RPM tag offset: $tag\n";
        $tags{$tag} = [$type, $store + $offset, $count];
    }
    return (\%tags, $end);
}

sub strings {
    my ($blob, $entry) = @_;
    defined $entry or return ();
    my ($type, $offset, $count) = @$entry;
    ($type == 6 || $type == 8 || $type == 9)
        or die "unexpected RPM string tag type: $type\n";
    my @values;
    for (1 .. $count) {
        my $nul = index($blob, "\0", $offset);
        $nul >= 0 or die "unterminated RPM string tag\n";
        push @values, substr($blob, $offset, $nul - $offset);
        $offset = $nul + 1;
    }
    return @values;
}

sub integers {
    my ($blob, $entry) = @_;
    defined $entry or return ();
    my ($type, $offset, $count) = @$entry;
    $type == 4 or die "unexpected RPM integer tag type: $type\n";
    $offset + $count * 4 <= length($blob) or die "truncated RPM integer tag\n";
    return unpack('N' . $count, substr($blob, $offset, $count * 4));
}

my (undef, $signature_end) = header($rpm, 96);
my $main_start = ($signature_end + 7) & ~7;
my ($tags, $header_end) = header($rpm, $main_start);
$header_end < length($rpm) or die "RPM has no payload: $path\n";

my %identity = (
    1000 => 'aix-fuse',
    1001 => $version,
    1002 => $release,
    1014 => 'MIT AND LGPL-2.1-only',
    1021 => 'aix' . $aix_version,
    1022 => 'ppc',
);
for my $tag (sort keys %identity) {
    my ($actual) = strings($rpm, $tags->{$tag});
    defined $actual && $actual eq $identity{$tag}
        or die "unexpected RPM tag $tag: " . (defined $actual ? $actual : '<missing>') . "\n";
}

my @dir_indexes = integers($rpm, $tags->{1116});
my @base_names = strings($rpm, $tags->{1117});
my @dir_names = strings($rpm, $tags->{1118});
@dir_indexes == @base_names or die "inconsistent RPM file tables\n";
my %files;
for my $number (0 .. $#base_names) {
    my $directory = $dir_names[$dir_indexes[$number]];
    defined $directory or die "invalid RPM directory index\n";
    $files{$directory . $base_names[$number]} = 1;
}
for my $required (@required) {
    $files{$required} or die "RPM payload is missing $required\n";
}

for my $file (keys %files) {
    $file !~ m{^/(?:dev/usfs|usr/lib/(?:drivers|methods|objrepos)/|usr/lib/libusfs\.a$)}
        or die "adapter RPM owns a native kernel or library path: $file\n";
}
for my $requirement (strings($rpm, $tags->{1049})) {
    $requirement !~ /^(?:usfs(?:$|[\s(<=>])|libusfs(?:$|[.(]))/
        or die "adapter RPM requires the separate native package: $requirement\n";
}
for my $tag (1023, 1024, 1025, 1026) {
    for my $scriptlet (strings($rpm, $tags->{$tag})) {
        $scriptlet eq '' or die "adapter RPM contains installation lifecycle scriptlet $tag\n";
    }
}

print "verified RPM: aix-fuse-$version-$release.aix$aix_version.ppc\n";
